#pragma once

#include <util/stream/output.h>
#include <util/generic/fwd.h>
#include <contrib/libs/protobuf/src/google/protobuf/map.h>

#include <ydb/core/resource_pools/resource_pool_settings.h>
#include <ydb/library/actors/core/actorid.h>

namespace NKikimr::NKqp {

    struct TUserRequestContext : public TAtomicRefCount<TUserRequestContext> {
        TString TraceId;
        TString Database;
        TString DatabaseId;
        TString SessionId;
        TString CurrentExecutionId;
        TString CustomerSuppliedId;
        NActors::TActorId RunScriptActorId;
        TString PoolId;
        std::optional<NResourcePool::TPoolSettings> PoolConfig;

        TUserRequestContext() = default;

        TUserRequestContext(const TString& traceId, const TString& database, const TString& sessionId)
            : TraceId(traceId)
            , Database(database)
            , SessionId(sessionId) {}

        TUserRequestContext(const TString& traceId, const TString& database, const TString& sessionId, const TString& currentExecutionId, const TString& customerSuppliedId, NActors::TActorId runScriptActorId)
            : TraceId(traceId)
            , Database(database)
            , SessionId(sessionId)
            , CurrentExecutionId(currentExecutionId)
            , CustomerSuppliedId(customerSuppliedId)
            , RunScriptActorId(runScriptActorId) {}

        void Out(IOutputStream& o) const;
    };

    void SerializeCtxToMap(const TUserRequestContext& ctx, google::protobuf::Map<TString, TString>& resultMap);
}

template<>
inline void Out<NKikimr::NKqp::TUserRequestContext>(IOutputStream& o, const NKikimr::NKqp::TUserRequestContext &x) {
    return x.Out(o);
}
