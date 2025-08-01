#pragma once

#include "public.h"

#include <yt/yt/library/codegen_api/execution_backend.h>

#include <yt/yt/client/hydra/public.h>

#include <yt/yt/client/misc/workload.h>

#include <yt/yt/client/table_client/versioned_io_options.h>

#include <yt/yt/client/tablet_client/public.h>

#include <yt/yt/core/rpc/public.h>

namespace NYT::NApi {

////////////////////////////////////////////////////////////////////////////////

struct TMutatingOptions
{
    NRpc::TMutationId MutationId;
    bool Retry = false;

    NRpc::TMutationId GetOrGenerateMutationId() const;
};

struct TMultiplexingBandOptions
{
    NRpc::EMultiplexingBand MultiplexingBand = NRpc::EMultiplexingBand::Default;
};

struct TSuppressableAccessTrackingOptions
{
    bool SuppressAccessTracking = false;
    bool SuppressModificationTracking = false;
    bool SuppressExpirationTimeoutRenewal = false;
};

struct TUserWorkloadDescriptor
{
    EUserWorkloadCategory Category = EUserWorkloadCategory::Interactive;
    int Band = 0;

    operator TWorkloadDescriptor() const;
};

void Serialize(const TUserWorkloadDescriptor& workloadDescriptor, NYson::IYsonConsumer* consumer);
void Deserialize(TUserWorkloadDescriptor& workloadDescriptor, NYTree::INodePtr node);
void Deserialize(TUserWorkloadDescriptor& workloadDescriptor, NYson::TYsonPullParserCursor* cursor);

struct TTransactionalOptions
{
    //! Ignored when queried via transaction.
    NObjectClient::TTransactionId TransactionId;
    bool Ping = false;
    bool PingAncestors = false;
    //! For internal use only.
    //! Setting it to |true| may result in loss of consistency.
    bool SuppressTransactionCoordinatorSync = false;
    //! For internal use only.
    //! Setting it to |true| may result in loss of consistency .
    bool SuppressUpstreamSync = false;
};

struct TMasterReadOptions
{
    EMasterChannelKind ReadFrom = EMasterChannelKind::Follower;
    bool DisablePerUserCache = false;
    TDuration ExpireAfterSuccessfulUpdateTime = TDuration::Seconds(15);
    TDuration ExpireAfterFailedUpdateTime = TDuration::Seconds(15);
    std::optional<int> CacheStickyGroupSize;
    bool EnableClientCacheStickiness = false;

    // When staleness bound is non-zero, master cache is allowed to
    // return successful expired response, with staleness not exceeding the bound.
    // This allows non-blocking master cache responses, with async on-demand updates.
    TDuration SuccessStalenessBound;
};

struct TSerializableMasterReadOptions
    : public TMasterReadOptions
    , public NYTree::TYsonStruct
{
    REGISTER_YSON_STRUCT(TSerializableMasterReadOptions);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TSerializableMasterReadOptions)

struct TPrerequisiteRevisionConfig
    : public NYTree::TYsonStruct
{
    NYTree::TYPath Path;
    NHydra::TRevision Revision;

    REGISTER_YSON_STRUCT(TPrerequisiteRevisionConfig);

    static void Register(TRegistrar registrar);
};

DEFINE_REFCOUNTED_TYPE(TPrerequisiteRevisionConfig)

struct TPrerequisiteOptions
{
    std::vector<NTransactionClient::TTransactionId> PrerequisiteTransactionIds;
    std::vector<TPrerequisiteRevisionConfigPtr> PrerequisiteRevisions;
};

struct TSyncReplicaCacheOptions
{
    std::optional<TDuration> CachedSyncReplicasTimeout;
};

struct TTabletReadOptionsBase
    : public TSyncReplicaCacheOptions
{
    NHydra::EPeerKind ReadFrom = NHydra::EPeerKind::Leader;
    std::optional<TDuration> RpcHedgingDelay;

    //! Ignored when queried via transaction.
    NTransactionClient::TTimestamp Timestamp = NTransactionClient::SyncLastCommittedTimestamp;

    NTransactionClient::TTimestamp RetentionTimestamp = NTransactionClient::NullTimestamp;

    EReplicaConsistency ReplicaConsistency = EReplicaConsistency::None;
};

struct TTabletReadOptions
    : public TTimeoutOptions
    , public TTabletReadOptionsBase
{ };

struct TSelectRowsOptionsBase
    : public TTabletReadOptions
    , public TSuppressableAccessTrackingOptions
{
    //! Expected schemas for tables in a query (used for replica fallback in replicated tables).
    using TExpectedTableSchemas = THashMap<NYPath::TYPath, NTableClient::TTableSchemaPtr>;
    TExpectedTableSchemas ExpectedTableSchemas;
    //! Add |$timestamp:columnName| to result if ReadMode is latest_timestamp.
    NTableClient::TVersionedReadOptions VersionedReadOptions;
    //! Limits range expanding.
    ui64 RangeExpansionLimit = 200'000;
    //! Limits parallel subqueries by row count.
    i64 MinRowCountPerSubquery = 100'000;
    //! Path in Cypress with UDFs.
    std::optional<NYPath::TYPath> UdfRegistryPath;
    //! Limits maximum parallel subqueries.
    int MaxSubqueries = std::numeric_limits<int>::max();
    //! Query language syntax version.
    int SyntaxVersion = 1;
    //! If |true| then logging is more verbose.
    bool VerboseLogging = false;
    // COMPAT(lukyan)
    //! Use fixed and rewritten range inference.
    bool NewRangeInference = true;
    //! Typed expression builder version.
    int ExpressionBuilderVersion = 1;
};

struct TSelectRowsOptions
    : public TSelectRowsOptionsBase
{
    //! If null then connection defaults are used.
    std::optional<i64> InputRowLimit;
    //! If null then connection defaults are used.
    std::optional<i64> OutputRowLimit;
    //! Execution pool.
    std::optional<TString> ExecutionPool;
    //! Used to prioritize requests.
    TUserWorkloadDescriptor WorkloadDescriptor;
    //! Memory limit per execution node.
    size_t MemoryLimitPerNode = std::numeric_limits<size_t>::max();
    //! Info on detailed profiling.
    TDetailedProfilingInfoPtr DetailedProfilingInfo;
    //! YSON map with placeholder values for parameterized queries.
    NYson::TYsonString PlaceholderValues;
    //! Native or WebAssembly execution backend.
    std::optional<NCodegen::EExecutionBackend> ExecutionBackend;
    //! Explicitly allow or forbid the usage of row cache.
    std::optional<bool> UseLookupCache;
    //! Tune batch sizes for row processing.
    std::optional<i64> RowsetProcessingBatchSize;
    //! Tune write row batch size.
    std::optional<i64> WriteRowsetSize;
    //! Tune join row batch size.
    std::optional<i64> MaxJoinBatchSize;
    //! Allow queries without any condition on key columns.
    bool AllowFullScan = true;
    //! Allow queries with join condition which implies foreign query with IN operator.
    bool AllowJoinWithoutIndex = false;
    //! If |true| then incomplete result would lead to a failure.
    bool FailOnIncompleteResult = true;
    //! Enables generated code caching.
    bool EnableCodeCache = true;
    //! Enables canonical SQL behaviour for relational operators, i.e. null </=/> value -> null.
    bool UseCanonicalNullRelations = false;
    //! Merge versioned rows from different stores when reading.
    bool MergeVersionedRows = true;
    //! For internal use only.
    //! Use original table schema in result rowset.
    bool UseOriginalTableSchema = false;
    //! Minimizes request rate to dictionary tables when executing joins.
    bool UseOrderByInJoinSubqueries = false;
};

struct TFallbackReplicaOptions
{
    NTableClient::TTableSchemaPtr FallbackTableSchema;
    NTabletClient::TTableReplicaId FallbackReplicaId;
};

struct TCreateObjectOptions
    : public TTimeoutOptions
    , public TMutatingOptions
    , public TPrerequisiteOptions
{
    bool IgnoreExisting = false;
    bool Sync = true;
    NYTree::IAttributeDictionaryPtr Attributes;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi
