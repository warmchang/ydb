#include "schemeshard__operation_drop_cdc_stream.h"

#include "schemeshard__operation_common.h"
#include "schemeshard__operation_part.h"
#include "schemeshard_utils.h"  // for TransactionTemplate

#define LOG_D(stream) LOG_DEBUG_S (context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)
#define LOG_I(stream) LOG_INFO_S  (context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)
#define LOG_N(stream) LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << context.SS->TabletID() << "] " << stream)

namespace NKikimr::NSchemeShard {

namespace NCdc {

namespace {

class TPropose: public TSubOperationState {
    TString DebugHint() const override {
        return TStringBuilder()
            << "DropCdcStream TPropose"
            << " opId# " << OperationId << " ";
    }

public:
    explicit TPropose(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {});
    }

    bool ProgressState(TOperationContext& context) override {
        LOG_I(DebugHint() << "ProgressState");

        const auto* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxDropCdcStream);

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, TStepId(0));
        return false;
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        const auto step = TStepId(ev->Get()->StepId);

        LOG_I(DebugHint() << "HandleReply TEvOperationPlan"
            << ": step# " << step);

        const auto* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxDropCdcStream);
        const auto& pathId = txState->TargetPathId;

        Y_ABORT_UNLESS(context.SS->PathsById.contains(pathId));
        auto path = context.SS->PathsById.at(pathId);

        Y_ABORT_UNLESS(!path->Dropped());
        path->SetDropped(step, OperationId.GetTxId());

        NIceDb::TNiceDb db(context.GetDB());

        context.SS->PersistDropStep(db, pathId, step, OperationId);
        context.SS->PersistRemoveCdcStream(db, pathId);

        Y_ABORT_UNLESS(context.SS->PathsById.contains(path->ParentPathId));
        auto parent = context.SS->PathsById.at(path->ParentPathId);

        context.SS->ResolveDomainInfo(pathId)->DecPathsInside(context.SS);
        DecAliveChildrenDirect(OperationId, parent, context); // for correct discard of ChildrenExist prop

        context.SS->ClearDescribePathCaches(path);
        context.OnComplete.PublishToSchemeBoard(OperationId, pathId);

        context.SS->ChangeTxState(db, OperationId, TTxState::Done);
        return true;
    }

private:
    const TOperationId OperationId;

}; // TPropose

class TDropCdcStream: public TSubOperation {
    static TTxState::ETxState NextState() {
        return TTxState::Propose;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) const override {
        switch (state) {
        case TTxState::Propose:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) override {
        switch (state) {
        case TTxState::Propose:
            return MakeHolder<TPropose>(OperationId);
        case TTxState::Done:
            return MakeHolder<TDone>(OperationId);
        default:
            return nullptr;
        }
    }

public:
    using TSubOperation::TSubOperation;

    THolder<TProposeResponse> Propose(const TString&, TOperationContext& context) override {
        const auto& workingDir = Transaction.GetWorkingDir();
        const auto& op = Transaction.GetDrop();
        const auto& streamName = op.GetName();

        LOG_N("TDropCdcStream Propose"
            << ": opId# " << OperationId
            << ", stream# " << workingDir << "/" << streamName);

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), context.SS->TabletID());

        const auto streamPath = op.HasId()
            ? TPath::Init(context.SS->MakeLocalId(op.GetId()), context.SS)
            : TPath::Resolve(workingDir, context.SS).Dive(streamName);
        {
            const auto checks = streamPath.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .IsCdcStream();

            // Allow processing streams that are being deleted/operated on by the same transaction
            // (coordinated multi-stream drop within same transaction)
            bool isSameTransaction = false;
            
            if (streamPath.Base()->PathState == TPathElement::EPathState::EPathStateDrop) {
                // Check if the stream is being dropped by the same transaction
                isSameTransaction = (streamPath.Base()->DropTxId == OperationId.GetTxId());
                if (!isSameTransaction) {
                    checks.NotUnderDeleting();
                }
            }
            
            // Check if stream is under operation by same transaction
            // Allow if it's any suboperation of the same transaction
            if (streamPath.Base()->LastTxId != InvalidTxId) {
                if (streamPath.Base()->LastTxId != OperationId.GetTxId()) {
                    checks.NotUnderOperation();
                }
            } else {
                // No LastTxId set, check normal operation state
                if (!isSameTransaction) {
                    checks.NotUnderOperation();
                }
            }

            if (!checks) {
                result->SetError(checks.GetStatus(), checks.GetError());
                return result;
            }
        }

        const auto tablePath = streamPath.Parent();
        {
            const auto checks = tablePath.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .IsTable()
                .NotAsyncReplicaTable()
                .IsUnderOperation()
                .IsUnderTheSameOperation(OperationId.GetTxId());

            if (checks && !tablePath.IsInsideTableIndexPath()) {
                checks.IsCommonSensePath();
            }

            if (!checks) {
                result->SetError(checks.GetStatus(), checks.GetError());
                return result;
            }
        }

        auto guard = context.DbGuard();
        context.DbChanges.PersistTxState(OperationId);

        Y_ABORT_UNLESS(!context.SS->FindTx(OperationId));
        auto& txState = context.SS->CreateTx(OperationId, TTxState::TxDropCdcStream, streamPath.Base()->PathId);
        txState.State = TTxState::Propose;
        txState.MinStep = TStepId(1);

        streamPath.Base()->PathState = TPathElement::EPathState::EPathStateDrop;
        streamPath.Base()->DropTxId = OperationId.GetTxId();
        streamPath.Base()->LastTxId = OperationId.GetTxId();

        context.SS->TabletCounters->Simple()[COUNTER_CDC_STREAMS_COUNT].Sub(1);
        context.SS->ClearDescribePathCaches(streamPath.Base());
        context.OnComplete.PublishToSchemeBoard(OperationId, streamPath.Base()->PathId);
        context.OnComplete.ActivateTx(OperationId);

        SetState(NextState());
        return result;
    }

    void AbortPropose(TOperationContext&) override {
        Y_ABORT("no AbortPropose for TDropCdcStream");
    }

    void AbortUnsafe(TTxId txId, TOperationContext& context) override {
        LOG_N("TDropCdcStream AbortUnsafe"
            << ": opId# " << OperationId
            << ", txId# " << txId);
        context.OnComplete.DoneOperation(OperationId);
    }

}; // TDropCdcStream

class TConfigurePartsAtTable: public NCdcStreamState::TConfigurePartsAtTable {
protected:
    void FillNotice(const TPathId& pathId, NKikimrTxDataShard::TFlatSchemeTransaction& tx, TOperationContext& context) const override {
        Y_ABORT_UNLESS(context.SS->PathsById.contains(pathId));
        auto path = context.SS->PathsById.at(pathId);

        Y_ABORT_UNLESS(context.SS->Tables.contains(pathId));
        auto table = context.SS->Tables.at(pathId);

        auto& notice = *tx.MutableDropCdcStreamNotice();
        pathId.ToProto(notice.MutablePathId());
        notice.SetTableSchemaVersion(table->AlterVersion + 1);

        // Collect all streams planned for drop on this table
        TVector<TPathId> streamPathIds;
        for (const auto& [_, childPathId] : path->GetChildren()) {
            Y_ABORT_UNLESS(context.SS->PathsById.contains(childPathId));
            auto childPath = context.SS->PathsById.at(childPathId);

            if (!childPath->IsCdcStream() || !childPath->PlannedToDrop()) {
                continue;
            }
            streamPathIds.push_back(childPathId);
        }
        
        Y_VERIFY_S(!streamPathIds.empty(), "No CDC streams planned for drop");
        
        for (const auto& streamId : streamPathIds) {
            streamId.ToProto(notice.AddStreamPathId());
        }
    }

public:
    using NCdcStreamState::TConfigurePartsAtTable::TConfigurePartsAtTable;

}; // TConfigurePartsAtTable

class TConfigurePartsAtTableDropSnapshot: public TConfigurePartsAtTable {
protected:
    void FillNotice(const TPathId& pathId, NKikimrTxDataShard::TFlatSchemeTransaction& tx, TOperationContext& context) const override {
        TConfigurePartsAtTable::FillNotice(pathId, tx, context);

        Y_ABORT_UNLESS(context.SS->TablesWithSnapshots.contains(pathId));
        const auto snapshotTxId = context.SS->TablesWithSnapshots.at(pathId);

        Y_ABORT_UNLESS(context.SS->SnapshotsStepIds.contains(snapshotTxId));
        const auto snapshotStep = context.SS->SnapshotsStepIds.at(snapshotTxId);

        Y_ABORT_UNLESS(tx.HasDropCdcStreamNotice());
        auto& notice = *tx.MutableDropCdcStreamNotice();

        notice.MutableDropSnapshot()->SetStep(ui64(snapshotStep));
        notice.MutableDropSnapshot()->SetTxId(ui64(snapshotTxId));
    }

public:
    using TConfigurePartsAtTable::TConfigurePartsAtTable;

}; // TConfigurePartsAtTableDropSnapshot

class TDropCdcStreamAtTable: public TSubOperation {
    static TTxState::ETxState NextState() {
        return TTxState::ConfigureParts;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) const override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::ConfigureParts:
            return TTxState::Propose;
        case TTxState::Propose:
            return TTxState::ProposedWaitParts;
        case TTxState::ProposedWaitParts:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::ConfigureParts:
            if (DropSnapshot) {
                return MakeHolder<TConfigurePartsAtTableDropSnapshot>(OperationId);
            } else {
                return MakeHolder<TConfigurePartsAtTable>(OperationId);
            }
        case TTxState::Propose:
            if (DropSnapshot) {
                return MakeHolder<NCdcStreamState::TProposeAtTableDropSnapshot>(OperationId);
            } else {
                return MakeHolder<NCdcStreamState::TProposeAtTable>(OperationId);
            }
        case TTxState::ProposedWaitParts:
            return MakeHolder<NTableState::TProposedWaitParts>(OperationId);
        case TTxState::Done:
            return MakeHolder<TDone>(OperationId);
        default:
            return nullptr;
        }
    }

public:
    explicit TDropCdcStreamAtTable(TOperationId id, const TTxTransaction& tx, bool dropSnapshot)
        : TSubOperation(id, tx)
        , DropSnapshot(dropSnapshot)
    {
        // Extract all stream names from transaction
        const auto& op = tx.GetDropCdcStream();
        for (const auto& name : op.GetStreamName()) {
            StreamNames.push_back(name);
        }
    }

    explicit TDropCdcStreamAtTable(TOperationId id, TTxState::ETxState state, bool dropSnapshot)
        : TSubOperation(id, state)
        , DropSnapshot(dropSnapshot)
    {
    }

    THolder<TProposeResponse> Propose(const TString&, TOperationContext& context) override {
        const auto& workingDir = Transaction.GetWorkingDir();
        const auto& op = Transaction.GetDropCdcStream();
        const auto& tableName = op.GetTableName();

        LOG_N("TDropCdcStreamAtTable Propose"
            << ": opId# " << OperationId
            << ", table# " << workingDir << "/" << tableName 
            << ", streams# " << StreamNames.size());

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, ui64(OperationId.GetTxId()), context.SS->TabletID());

        const auto tablePath = TPath::Resolve(workingDir, context.SS).Dive(tableName);
        {
            const auto checks = tablePath.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .IsTable()
                .NotAsyncReplicaTable()
                .NotUnderDeleting()
                .NotUnderOperation();

            if (checks && !tablePath.IsInsideTableIndexPath()) {
                checks.IsCommonSensePath();
            }

            if (!checks) {
                result->SetError(checks.GetStatus(), checks.GetError());
                return result;
            }
        }

        // Validate all streams exist and are on same table
        TVector<TPath> streamPaths;
        for (const auto& streamName : StreamNames) {
            const auto streamPath = tablePath.Child(streamName);
            const auto checks = streamPath.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .IsCdcStream()
                .NotUnderDeleting()
                .NotUnderOperation();

            if (!checks) {
                result->SetError(checks.GetStatus(), checks.GetError());
                return result;
            }
            streamPaths.push_back(streamPath);
        }

        TString errStr;
        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }

        if (!context.SS->CheckLocks(tablePath.Base()->PathId, Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusMultipleModifications, errStr);
            return result;
        }

        if (DropSnapshot && !context.SS->TablesWithSnapshots.contains(tablePath.Base()->PathId)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, TStringBuilder() << "Table has no snapshots"
                << ": pathId# " << tablePath.Base()->PathId);
            return result;
        }

        auto guard = context.DbGuard();
        context.MemChanges.GrabPath(context.SS, tablePath.Base()->PathId);
        context.MemChanges.GrabNewTxState(context.SS, OperationId);

        context.DbChanges.PersistPath(tablePath.Base()->PathId);
        context.DbChanges.PersistTxState(OperationId);

        Y_ABORT_UNLESS(context.SS->Tables.contains(tablePath.Base()->PathId));
        auto table = context.SS->Tables.at(tablePath.Base()->PathId);

        Y_ABORT_UNLESS(table->AlterVersion != 0);
        Y_ABORT_UNLESS(!table->AlterData);

        // Validate and mark all streams for drop in single transaction
        for (const auto& streamPath : streamPaths) {
            Y_ABORT_UNLESS(context.SS->CdcStreams.contains(streamPath.Base()->PathId));
            auto stream = context.SS->CdcStreams.at(streamPath.Base()->PathId);

            Y_ABORT_UNLESS(stream->AlterVersion != 0);
            Y_ABORT_UNLESS(!stream->AlterData);

            streamPath.Base()->PathState = TPathElement::EPathState::EPathStateDrop;
            streamPath.Base()->DropTxId = OperationId.GetTxId();
            streamPath.Base()->LastTxId = OperationId.GetTxId();
            
            context.SS->TabletCounters->Simple()[COUNTER_CDC_STREAMS_COUNT].Sub(1);
            context.SS->ClearDescribePathCaches(streamPath.Base());
            context.OnComplete.PublishToSchemeBoard(OperationId, streamPath.Base()->PathId);
        }

        const auto txType = DropSnapshot
            ? TTxState::TxDropCdcStreamAtTableDropSnapshot
            : TTxState::TxDropCdcStreamAtTable;

        Y_ABORT_UNLESS(!context.SS->FindTx(OperationId));
        auto& txState = context.SS->CreateTx(OperationId, txType, tablePath.Base()->PathId);
        txState.State = TTxState::ConfigureParts;

        tablePath.Base()->PathState = NKikimrSchemeOp::EPathStateAlter;
        tablePath.Base()->LastTxId = OperationId.GetTxId();

        for (const auto& splitOpId : table->GetSplitOpsInFlight()) {
            context.OnComplete.Dependence(splitOpId.GetTxId(), OperationId.GetTxId());
        }

        context.OnComplete.ActivateTx(OperationId);

        SetState(NextState());
        return result;
    }

    void AbortPropose(TOperationContext& context) override {
        LOG_N("TDropCdcStreamAtTable AbortPropose"
            << ": opId# " << OperationId);
    }

    void AbortUnsafe(TTxId txId, TOperationContext& context) override {
        LOG_N("TDropCdcStreamAtTable AbortUnsafe"
            << ": opId# " << OperationId
            << ", txId# " << txId);
        context.OnComplete.DoneOperation(OperationId);
    }

private:
    TVector<TString> StreamNames;  // All streams in this operation
    const bool DropSnapshot;

}; // TDropCdcStreamAtTable

} // anonymous

std::variant<TStreamPaths, ISubOperation::TPtr> DoDropStreamPathChecks(
        const TOperationId& opId,
        const TPath& workingDirPath,
        const TString& tableName,
        const TString& streamName)
{
    const auto tablePath = workingDirPath.Child(tableName);
    {
        const auto checks = tablePath.Check();
        checks
            .NotEmpty()
            .NotUnderDomainUpgrade()
            .IsAtLocalSchemeShard()
            .IsResolved()
            .NotDeleted()
            .IsTable()
            .NotAsyncReplicaTable()
            .NotUnderDeleting()
            .NotUnderOperation();

        if (checks && !tablePath.IsInsideTableIndexPath()) {
            checks.IsCommonSensePath();
        }

        if (!checks) {
            return CreateReject(opId, checks.GetStatus(), checks.GetError());
        }
    }

    const auto streamPath = tablePath.Child(streamName);
    {
        const auto checks = streamPath.Check();
        checks
            .NotEmpty()
            .NotUnderDomainUpgrade()
            .IsAtLocalSchemeShard()
            .IsResolved()
            .NotDeleted()
            .IsCdcStream()
            .NotUnderDeleting()
            .NotUnderOperation();

        if (!checks) {
            return CreateReject(opId, checks.GetStatus(), checks.GetError());
        }
    }

    return TStreamPaths{tablePath, streamPath};
}

ISubOperation::TPtr DoDropStreamChecks(
        const TOperationId& opId,
        const TPath& tablePath,
        const TTxId lockTxId,
        TOperationContext& context)
{

    TString errStr;
    if (!context.SS->CheckLocks(tablePath.Base()->PathId, lockTxId, errStr)) {
        return CreateReject(opId, NKikimrScheme::StatusMultipleModifications, errStr);
    }

    return nullptr;
}

void DoDropStream(
        TVector<ISubOperation::TPtr>& result,
        const NKikimrSchemeOp::TDropCdcStream& op,
        const TOperationId& opId,
        const TPath& workingDirPath,
        const TPath& tablePath,
        const TVector<TPath>& streamPaths,  // Now handles multiple streams
        const TTxId lockTxId,
        TOperationContext& context)
{
    {
        auto outTx = TransactionTemplate(workingDirPath.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpDropCdcStreamAtTable);
        outTx.MutableDropCdcStream()->CopyFrom(op);  // Preserves all stream names

        if (lockTxId != InvalidTxId) {
            outTx.MutableLockGuard()->SetOwnerTxId(ui64(lockTxId));
        }

        result.push_back(CreateDropCdcStreamAtTable(NextPartId(opId, result), outTx, lockTxId != InvalidTxId));
    }

    if (lockTxId != InvalidTxId) {
        auto outTx = TransactionTemplate(workingDirPath.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpDropLock);
        outTx.SetFailOnExist(true);
        outTx.SetInternal(true);
        outTx.MutableLockConfig()->SetName(tablePath.LeafName());
        outTx.MutableLockGuard()->SetOwnerTxId(ui64(lockTxId));

        result.push_back(DropLock(NextPartId(opId, result), outTx));
    }

    if (workingDirPath.IsTableIndex()) {
        auto outTx = TransactionTemplate(workingDirPath.Parent().PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpAlterTableIndex);
        outTx.MutableAlterTableIndex()->SetName(workingDirPath.LeafName());
        outTx.MutableAlterTableIndex()->SetState(NKikimrSchemeOp::EIndexState::EIndexStateReady);

        result.push_back(CreateAlterTableIndex(NextPartId(opId, result), outTx));
    }

    for (const auto& streamPath : streamPaths) {
        auto outTx = TransactionTemplate(tablePath.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpDropCdcStreamImpl);
        outTx.MutableDrop()->SetName(streamPath.Base()->Name);

        if (lockTxId != InvalidTxId) {
            outTx.MutableLockGuard()->SetOwnerTxId(ui64(lockTxId));
        }

        result.push_back(CreateDropCdcStreamImpl(NextPartId(opId, result), outTx));
    }

    for (const auto& streamPath : streamPaths) {
        for (const auto& [name, pathId] : streamPath.Base()->GetChildren()) {
            Y_ABORT_UNLESS(context.SS->PathsById.contains(pathId));
            auto implPath = context.SS->PathsById.at(pathId);

            if (implPath->Dropped()) {
                continue;
            }

            auto streamImpl = context.SS->PathsById.at(pathId);
            Y_ABORT_UNLESS(streamImpl->IsPQGroup());

            auto outTx = TransactionTemplate(streamPath.PathString(), NKikimrSchemeOp::EOperationType::ESchemeOpDropPersQueueGroup);
            outTx.MutableDrop()->SetName(name);

            result.push_back(CreateDropPQ(NextPartId(opId, result), outTx));
        }
    }
}

} // namespace NCdc

using namespace NCdc;

ISubOperation::TPtr CreateDropCdcStreamImpl(TOperationId id, const TTxTransaction& tx) {
    return MakeSubOperation<TDropCdcStream>(id, tx);
}

ISubOperation::TPtr CreateDropCdcStreamImpl(TOperationId id, TTxState::ETxState state) {
    return MakeSubOperation<TDropCdcStream>(id, state);
}

ISubOperation::TPtr CreateDropCdcStreamAtTable(TOperationId id, const TTxTransaction& tx, bool dropSnapshot) {
    return MakeSubOperation<TDropCdcStreamAtTable>(id, tx, dropSnapshot);
}

ISubOperation::TPtr CreateDropCdcStreamAtTable(TOperationId id, TTxState::ETxState state, bool dropSnapshot) {
    return MakeSubOperation<TDropCdcStreamAtTable>(id, state, dropSnapshot);
}

bool CreateDropCdcStream(TOperationId opId, const TTxTransaction& tx, TOperationContext& context, TVector<ISubOperation::TPtr>& result) {
    Y_ABORT_UNLESS(tx.GetOperationType() == NKikimrSchemeOp::EOperationType::ESchemeOpDropCdcStream);

    const auto& op = tx.GetDropCdcStream();
    const auto& tableName = op.GetTableName();
    
    TVector<TString> streamNames;
    for (const auto& name : op.GetStreamName()) {
        streamNames.push_back(name);
    }
    
    LOG_D("CreateDropCdcStream"
        << ": opId# " << opId
        << ", table# " << tableName
        << ", streams# " << streamNames.size()
        << ", tx# " << tx.ShortDebugString());

    const auto workingDirPath = TPath::Resolve(tx.GetWorkingDir(), context.SS);

    // Validate all streams exist on the same table
    TVector<TPath> streamPaths;
    
    if (streamNames.empty()) {
        result = {CreateReject(opId, NKikimrScheme::StatusInvalidParameter, 
                             "At least one StreamName must be specified")};
        return false;
    }
    
    const auto firstStreamChecksResult = DoDropStreamPathChecks(opId, workingDirPath, tableName, streamNames[0]);
    if (std::holds_alternative<ISubOperation::TPtr>(firstStreamChecksResult)) {
        result = {std::get<ISubOperation::TPtr>(firstStreamChecksResult)};
        return false;
    }
    
    const auto [tablePath, firstStreamPath] = std::get<TStreamPaths>(firstStreamChecksResult);
    streamPaths.push_back(firstStreamPath);
    
    for (size_t i = 1; i < streamNames.size(); ++i) {
        const auto checksResult = DoDropStreamPathChecks(opId, workingDirPath, tableName, streamNames[i]);
        if (std::holds_alternative<ISubOperation::TPtr>(checksResult)) {
            result = {std::get<ISubOperation::TPtr>(checksResult)};
            return false;
        }

        const auto [currentTablePath, streamPath] = std::get<TStreamPaths>(checksResult);
        streamPaths.push_back(streamPath);
    }

    TString errStr;
    if (!context.SS->CheckApplyIf(tx, errStr)) {
        result = {CreateReject(opId, NKikimrScheme::StatusPreconditionFailed, errStr)};
        return false;
    }

    // Check lock consistency across all streams
    TTxId lockTxId = InvalidTxId;
    for (const auto& streamPath : streamPaths) {
        Y_ABORT_UNLESS(context.SS->CdcStreams.contains(streamPath.Base()->PathId));
        auto stream = context.SS->CdcStreams.at(streamPath.Base()->PathId);

        const auto streamLockTxId = stream->State == TCdcStreamInfo::EState::ECdcStreamStateScan
            ? streamPath.Base()->CreateTxId : InvalidTxId;
            
        if (lockTxId == InvalidTxId) {
            lockTxId = streamLockTxId;
        } else if (lockTxId != streamLockTxId) {
            result = {CreateReject(opId, NKikimrScheme::StatusPreconditionFailed, 
                                 "Cannot drop CDC streams with different lock states in single operation")};
            return false;
        }
    }

    if (const auto reject = DoDropStreamChecks(opId, tablePath, lockTxId, context); reject) {
        result = {reject};
        return false;
    }

    DoDropStream(result, op, opId, workingDirPath, tablePath, streamPaths, lockTxId, context);

    return true;
}

TVector<ISubOperation::TPtr> CreateDropCdcStream(TOperationId opId, const TTxTransaction& tx, TOperationContext& context) {
    TVector<ISubOperation::TPtr> result;

    CreateDropCdcStream(opId, tx, context, result);

    return result;
}

}
