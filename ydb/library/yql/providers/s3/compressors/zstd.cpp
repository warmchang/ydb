#include "zstd.h"
#include "output_queue_impl.h"

#include <util/generic/size_literals.h>

#include <ydb/library/yql/dq/actors/protos/dq_status_codes.pb.h>

#include <yql/essentials/utils/exceptions.h>
#include <yql/essentials/utils/yql_panic.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <contrib/libs/zstd/include/zstd.h>

#include <ydb/library/yql/udfs/common/clickhouse/client/src/IO/ReadBuffer.h>

namespace NYql::NZstd {

namespace {

class TReadBuffer : public NDB::ReadBuffer {
public:
    TReadBuffer(NDB::ReadBuffer& source);
    ~TReadBuffer();
private:
    bool nextImpl() final;

    NDB::ReadBuffer& Source_;
    std::vector<char> InBuffer, OutBuffer;
    ::ZSTD_DStream *const ZCtx_;
    size_t Offset_;
    size_t Size_;
    bool Finished_ = false;
};

TReadBuffer::TReadBuffer(NDB::ReadBuffer& source)
    : NDB::ReadBuffer(nullptr, 0ULL), Source_(source), ZCtx_(::ZSTD_createDStream())
{
    InBuffer.resize(8_KB);
    OutBuffer.resize(64_KB);
    Offset_ = InBuffer.size();
    Size_ = InBuffer.size();
}

TReadBuffer::~TReadBuffer() {
    ::ZSTD_freeDStream(ZCtx_);
}

bool TReadBuffer::nextImpl() {
    ::ZSTD_inBuffer zIn{InBuffer.data(), Size_, Offset_};
    ::ZSTD_outBuffer zOut{OutBuffer.data(), OutBuffer.size(), 0ULL};

    size_t returnCode = 0ULL;
    if (!Finished_) do {
        if (zIn.pos == zIn.size) {
            zIn.size = Source_.read(InBuffer.data(), InBuffer.size());
            Size_ = zIn.size;

            zIn.pos = Offset_ = 0;
            if (!zIn.size) {
                // end of stream, need to check that there is no uncompleted blocks
                YQL_ENSURE_CODELINE(!returnCode, NYql::NDqProto::StatusIds::BAD_REQUEST, "Incomplete block.");
                Finished_ = true;
                break;
            }
        }
        returnCode = ::ZSTD_decompressStream(ZCtx_, &zOut, &zIn);
        YQL_ENSURE_CODELINE(!::ZSTD_isError(returnCode), NYql::NDqProto::StatusIds::BAD_REQUEST, "Decompress failed: " << ::ZSTD_getErrorName(returnCode));
        if (!returnCode) {
            // The frame is over, prepare to (maybe) start a new frame
            ::ZSTD_initDStream(ZCtx_);
        }
    } while (zOut.pos != zOut.size);
    Offset_ = zIn.pos;

    if (zOut.pos) {
        working_buffer = Buffer(OutBuffer.data(), OutBuffer.data() + zOut.pos);
        return true;
    } else {
        set(nullptr, 0ULL);
        return false;
    }
}

class TCompressor : public TOutputQueue<> {
public:
    TCompressor(int level)
        : ZCtx_(::ZSTD_createCStream())
    {
        const auto ret = ::ZSTD_initCStream(ZCtx_, level);
        YQL_ENSURE_CODELINE(!::ZSTD_isError(ret), NYql::NDqProto::StatusIds::BAD_REQUEST, "code: " << ret << ", error: " << ::ZSTD_getErrorName(ret));
    }

    ~TCompressor() {
        ::ZSTD_freeCStream(ZCtx_);
    }
private:
    void Push(TString&& item) override {
        InputQueue.Push(std::move(item));
        DoCompression();
    }

    void Seal() override {
        InputQueue.Seal();
        DoCompression();
    }

    size_t Size() const override {
        return TOutputQueue::Size() + InputQueue.Size();
    }

    bool Empty() const override {
        return TOutputQueue::Empty() && InputQueue.Empty();
    }

    size_t Volume() const override {
        return TOutputQueue::Volume() + InputQueue.Volume();
    }

    void DoCompression() {
        while (!InputQueue.Empty() || !TOutputQueue::IsSealed()) {
            const auto& pop = InputQueue.Pop();
            const bool done = InputQueue.IsSealed() && InputQueue.Empty();
            if (pop.empty() && !done)
                break;

            if (const auto bound = ZSTD_compressBound(pop.size()); bound > OutputBufferSize)
                OutputBuffer = std::make_unique<char[]>(OutputBufferSize = bound);

            if (IsFirstBlock && done) {
                const auto size = ::ZSTD_compress2(ZCtx_, OutputBuffer.get(), OutputBufferSize, pop.data(), pop.size());
                YQL_ENSURE(!::ZSTD_isError(size), "code: " << size << ", error: " << ::ZSTD_getErrorName(size));
                if (size)
                    TOutputQueue::Push(TString(OutputBuffer.get(), size));
            } else {
                ::ZSTD_inBuffer zIn{pop.data(), pop.size(), 0ULL};
                ::ZSTD_outBuffer zOut{OutputBuffer.get(), OutputBufferSize, 0ULL};
                const auto code = ::ZSTD_compressStream2(ZCtx_, &zOut, &zIn, done ? ZSTD_e_end : ZSTD_e_continue);
                YQL_ENSURE(!::ZSTD_isError(code), "code: " << code << ", error: " << ::ZSTD_getErrorName(code));

                if (zOut.pos)
                    TOutputQueue::Push(TString(OutputBuffer.get(), zOut.pos));
            }

            IsFirstBlock = false;
            if (done)
                return TOutputQueue::Seal();
        };
    }

    std::size_t OutputBufferSize = 0ULL;
    std::unique_ptr<char[]> OutputBuffer;

    ::ZSTD_CStream *const ZCtx_;

    TOutputQueue<0> InputQueue;
    bool IsFirstBlock = true;
};

} // anonymous namespace

std::unique_ptr<NDB::ReadBuffer> MakeDecompressor(NDB::ReadBuffer& source) {
    return std::make_unique<TReadBuffer>(source);
}

IOutputQueue::TPtr MakeCompressor(std::optional<int> cLevel) {
    return std::make_unique<TCompressor>(cLevel.value_or(ZSTD_defaultCLevel()));
}

} // namespace NYql::NZstd
