from __future__ import annotations

import abc
import asyncio
import bisect
import enum
from collections import deque
from dataclasses import dataclass, field
import datetime
from typing import Union, Any, List, Dict, Deque, Optional, Tuple

from ydb._grpc.grpcwrapper.ydb_topic import OffsetsRange, Codec
from ydb._topic_reader import topic_reader_asyncio


class ICommittable(abc.ABC):
    @abc.abstractmethod
    def _commit_get_partition_session(self) -> PartitionSession:
        ...

    @abc.abstractmethod
    def _commit_get_offsets_range(self) -> OffsetsRange:
        ...


class ISessionAlive(abc.ABC):
    @property
    @abc.abstractmethod
    def alive(self) -> bool:
        pass


@dataclass
class PublicMessage(ICommittable, ISessionAlive):
    seqno: int
    created_at: datetime.datetime
    message_group_id: str
    session_metadata: Dict[str, str]
    offset: int
    written_at: datetime.datetime
    producer_id: str
    data: Union[bytes, Any]  # set as original decompressed bytes or deserialized object if deserializer set in reader
    metadata_items: Dict[str, bytes]
    _partition_session: PartitionSession
    _commit_start_offset: int
    _commit_end_offset: int

    def _commit_get_partition_session(self) -> PartitionSession:
        return self._partition_session

    def _commit_get_offsets_range(self) -> OffsetsRange:
        return OffsetsRange(self._commit_start_offset, self._commit_end_offset)

    # ISessionAlive implementation
    @property
    def alive(self) -> bool:
        return not self._partition_session.closed

    @property
    def partition_id(self) -> int:
        return self._partition_session.partition_id


@dataclass
class PartitionSession:
    id: int
    state: "PartitionSession.State"
    topic_path: str
    partition_id: int
    committed_offset: int  # last commit offset, acked from server. Processed messages up to the field-1 offset.
    reader_reconnector_id: int
    reader_stream_id: int
    _next_message_start_commit_offset: int = field(init=False)

    # todo: check if deque is optimal
    _ack_waiters: Deque["PartitionSession.CommitAckWaiter"] = field(init=False, default_factory=lambda: deque())

    _state_changed: asyncio.Event = field(init=False, default_factory=lambda: asyncio.Event(), compare=False)

    def __post_init__(self):
        self._next_message_start_commit_offset = self.committed_offset

    def add_waiter(self, end_offset: int) -> "PartitionSession.CommitAckWaiter":
        self._ensure_not_closed()

        waiter = PartitionSession.CommitAckWaiter(end_offset, asyncio.Future())
        if end_offset <= self.committed_offset:
            waiter._finish_ok()
            return waiter

        # fast way
        if self._ack_waiters and self._ack_waiters[-1].end_offset < end_offset:
            self._ack_waiters.append(waiter)
        else:
            bisect.insort(self._ack_waiters, waiter)

        return waiter

    def ack_notify(self, offset: int):
        self._ensure_not_closed()

        self.committed_offset = offset

        if not self._ack_waiters:
            # todo log warning
            # must be never receive ack for not sended request
            return

        while self._ack_waiters:
            if self._ack_waiters[0].end_offset > offset:
                break
            waiter = self._ack_waiters.popleft()
            waiter._finish_ok()

    def _update_last_commited_offset_if_needed(self, offset: int):
        self.committed_offset = max(self.committed_offset, offset)

    def close(self):
        if self.closed:
            return

        self.state = PartitionSession.State.Stopped
        exception = topic_reader_asyncio.PublicTopicReaderPartitionExpiredError()
        for waiter in self._ack_waiters:
            waiter._finish_error(exception)

    @property
    def closed(self):
        return self.state == PartitionSession.State.Stopped

    def end(self):
        if self.closed:
            return

        self.state = PartitionSession.State.Ended

    @property
    def ended(self):
        return self.state == PartitionSession.State.Ended

    def _ensure_not_closed(self):
        if self.state == PartitionSession.State.Stopped:
            raise topic_reader_asyncio.PublicTopicReaderPartitionExpiredError()

    class State(enum.Enum):
        Active = 1
        GracefulShutdown = 2
        Stopped = 3
        Ended = 4

    @dataclass(order=True)
    class CommitAckWaiter:
        end_offset: int
        future: asyncio.Future = field(compare=False)
        _done: bool = field(default=False, init=False)
        _exception: Optional[Exception] = field(default=None, init=False)

        def _finish_ok(self):
            self._done = True
            self.future.set_result(None)

        def _finish_error(self, error: Exception):
            self._exception = error
            self.future.set_exception(error)


@dataclass
class PublicBatch(ICommittable, ISessionAlive):
    messages: List[PublicMessage]
    _partition_session: PartitionSession
    _bytes_size: int
    _codec: Codec

    def _commit_get_partition_session(self) -> PartitionSession:
        return self.messages[0]._commit_get_partition_session()

    def _commit_get_offsets_range(self) -> OffsetsRange:
        return OffsetsRange(
            self.messages[0]._commit_get_offsets_range().start,
            self.messages[-1]._commit_get_offsets_range().end,
        )

    def empty(self) -> bool:
        return len(self.messages) == 0

    # ISessionAlive implementation
    @property
    def alive(self) -> bool:
        return not self._partition_session.closed

    def pop_message(self) -> PublicMessage:
        return self.messages.pop(0)

    def _extend(self, batch: PublicBatch) -> None:
        self.messages.extend(batch.messages)
        self._bytes_size += batch._bytes_size

    def _pop(self) -> Tuple[List[PublicMessage], bool]:
        msgs_left = True if len(self.messages) > 1 else False
        return self.messages.pop(0), msgs_left

    def _pop_batch(self, message_count: int) -> PublicBatch:
        initial_length = len(self.messages)

        if message_count >= initial_length:
            raise ValueError("Pop batch with size >= actual size is not supported.")

        one_message_size = self._bytes_size // initial_length

        new_batch = PublicBatch(
            messages=self.messages[:message_count],
            _partition_session=self._partition_session,
            _bytes_size=one_message_size * message_count,
            _codec=self._codec,
        )

        self.messages = self.messages[message_count:]
        self._bytes_size = self._bytes_size - new_batch._bytes_size

        return new_batch

    def _update_partition_offsets(self, tx, exc=None):
        if exc is not None:
            return
        offsets = self._commit_get_offsets_range()
        self._partition_session._update_last_commited_offset_if_needed(offsets.end)
