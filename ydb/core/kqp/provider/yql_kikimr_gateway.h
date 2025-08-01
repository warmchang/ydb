#pragma once

#include <ydb/library/aclib/aclib.h>
#include <yql/essentials/providers/common/gateway/yql_provider_gateway.h>
#include <yql/essentials/providers/result/expr_nodes/yql_res_expr_nodes.h>
#include <yql/essentials/public/udf/udf_data_type.h>
#include <yql/essentials/public/udf/udf_value.h>
#include <yql/essentials/minikql/mkql_alloc.h>
#include <yql/essentials/minikql/mkql_node.h>
#include <ydb/library/mkql_proto/mkql_proto.h>
#include <ydb/library/yql/dq/runtime/dq_transport.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <yql/essentials/utils/resetable_setting.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/topic/client.h>
#include <ydb/services/metadata/abstract/kqp_common.h>
#include <ydb/services/metadata/manager/abstract.h>
#include <ydb/services/persqueue_v1/actors/events.h>

#include <ydb/core/base/table_index.h>
#include <ydb/core/external_sources/external_source_factory.h>
#include <ydb/core/kqp/query_data/kqp_prepared_query.h>
#include <ydb/core/kqp/query_data/kqp_query_data.h>
#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/protos/kqp.pb.h>
#include <ydb/core/protos/kqp_stats.pb.h>
#include <ydb/core/protos/subdomains.pb.h>
#include <ydb/core/protos/sys_view_types.pb.h>
#include <ydb/core/protos/yql_translation_settings.pb.h>
#include <ydb/core/scheme/scheme_types_proto.h>

#include <library/cpp/json/json_reader.h>
#include <library/cpp/protobuf/json/proto2json.h>
#include <library/cpp/threading/future/future.h>

#include <util/string/join.h>

namespace NKikimr {
    namespace NMiniKQL {
        class IFunctionRegistry;
    }

    namespace NKqp {
        class TKqpPhyTxHolder;
    }
}

namespace NKikimrReplication {
    class TOAuthToken;
    class TStaticCredentials;
    class TConsistencySettings_TGlobalConsistency;
}

namespace NYql {

using NUdf::EDataSlot;

struct TKikimrQueryPhaseLimits {
    ui32 AffectedShardsLimit = 0;
    ui32 ReadsetCountLimit = 0;
    ui64 ComputeNodeMemoryLimitBytes = 0;
    ui64 TotalReadSizeLimitBytes = 0;
};

struct TKikimrQueryLimits {
    TKikimrQueryPhaseLimits PhaseLimits;
};

struct TIndexDescription {
    enum class EType : ui32 {
        GlobalSync = 0,
        GlobalAsync = 1,
        GlobalSyncUnique = 2,
        GlobalSyncVectorKMeansTree = 3,
    };

    // Index states here must be in sync with NKikimrSchemeOp::EIndexState protobuf
    enum class EIndexState : ui32 {
        Invalid = 0,  // this state should not be used
        Ready = 1,    // index is ready to use
        NotReady = 2, // index is visible but not ready to use
        WriteOnly = 3 // index is visible only write operations to index are allowed
    };

    const TString Name;
    const TVector<TString> KeyColumns;
    const TVector<TString> DataColumns;
    const EType Type;
    const EIndexState State;
    const ui64 SchemaVersion;
    const ui64 LocalPathId;
    const ui64 PathOwnerId;

    using TSpecializedIndexDescription = std::variant<std::monostate, NKikimrKqp::TVectorIndexKmeansTreeDescription>;
    TSpecializedIndexDescription SpecializedIndexDescription;

    TIndexDescription(const TString& name, const TVector<TString>& keyColumns, const TVector<TString>& dataColumns,
        EType type, EIndexState state, ui64 schemaVersion, ui64 localPathId, ui64 pathOwnerId,
        const TSpecializedIndexDescription& specializedIndexDescription)
        : Name(name)
        , KeyColumns(keyColumns)
        , DataColumns(dataColumns)
        , Type(type)
        , State(state)
        , SchemaVersion(schemaVersion)
        , LocalPathId(localPathId)
        , PathOwnerId(pathOwnerId)
        , SpecializedIndexDescription(specializedIndexDescription)
    {}

    TIndexDescription(const NKikimrSchemeOp::TIndexDescription& index)
        : Name(index.GetName())
        , KeyColumns(index.GetKeyColumnNames().begin(), index.GetKeyColumnNames().end())
        , DataColumns(index.GetDataColumnNames().begin(), index.GetDataColumnNames().end())
        , Type(ConvertIndexType(index.GetType()))
        , State(static_cast<EIndexState>(index.GetState()))
        , SchemaVersion(index.GetSchemaVersion())
        , LocalPathId(index.GetLocalPathId())
        , PathOwnerId(index.HasPathOwnerId() ? index.GetPathOwnerId() : 0ul)
    {
        if (Type == TIndexDescription::EType::GlobalSyncVectorKMeansTree) {
            NKikimrKqp::TVectorIndexKmeansTreeDescription vectorIndexDescription;
            *vectorIndexDescription.MutableSettings() = index.GetVectorIndexKmeansTreeDescription().GetSettings();
            SpecializedIndexDescription = vectorIndexDescription;
        }
    }

    TIndexDescription(const NKikimrKqp::TIndexDescriptionProto* message)
        : Name(message->GetName())
        , KeyColumns(message->GetKeyColumns().begin(), message->GetKeyColumns().end())
        , DataColumns(message->GetDataColumns().begin(), message->GetDataColumns().end())
        , Type(static_cast<EType>(message->GetType()))
        , State(static_cast<EIndexState>(message->GetState()))
        , SchemaVersion(message->GetSchemaVersion())
        , LocalPathId(message->GetLocalPathId())
        , PathOwnerId(message->GetPathOwnerId())
    {
        if (Type == TIndexDescription::EType::GlobalSyncVectorKMeansTree) {
            SpecializedIndexDescription = message->GetVectorIndexKmeansTreeDescription();
        }
    }

    static TIndexDescription::EType ConvertIndexType(const NKikimrSchemeOp::EIndexType indexType) {
        switch (indexType) {
            case NKikimrSchemeOp::EIndexType::EIndexTypeGlobal:
                return TIndexDescription::EType::GlobalSync;
            case NKikimrSchemeOp::EIndexType::EIndexTypeGlobalAsync:
                return TIndexDescription::EType::GlobalAsync;
            case NKikimrSchemeOp::EIndexType::EIndexTypeGlobalUnique:
                return TIndexDescription::EType::GlobalSyncUnique;
            case NKikimrSchemeOp::EIndexType::EIndexTypeGlobalVectorKmeansTree:
                return TIndexDescription::EType::GlobalSyncVectorKMeansTree;
            default:
                YQL_ENSURE(false, "Unexpected NKikimrSchemeOp::EIndexType::EIndexTypeInvalid");
        }
    }

    static NKikimrSchemeOp::EIndexType ConvertIndexType(const TIndexDescription::EType indexType) {
        switch (indexType) {
            case TIndexDescription::EType::GlobalSync:
                return NKikimrSchemeOp::EIndexType::EIndexTypeGlobal;
            case TIndexDescription::EType::GlobalAsync:
                return NKikimrSchemeOp::EIndexType::EIndexTypeGlobalAsync;
            case TIndexDescription::EType::GlobalSyncUnique:
                return NKikimrSchemeOp::EIndexType::EIndexTypeGlobalUnique;
            case NYql::TIndexDescription::EType::GlobalSyncVectorKMeansTree:
                return NKikimrSchemeOp::EIndexType::EIndexTypeGlobalVectorKmeansTree;
        }
    }

    void ToMessage(NKikimrKqp::TIndexDescriptionProto* message) const {
        message->SetName(Name);
        message->SetType(static_cast<ui32>(Type));
        message->SetState(static_cast<ui32>(State));
        message->SetSchemaVersion(SchemaVersion);
        message->SetLocalPathId(LocalPathId);
        message->SetPathOwnerId(PathOwnerId);

        for(auto& key: KeyColumns) {
            message->AddKeyColumns(key);
        }

        for(auto& data: DataColumns) {
            message->AddDataColumns(data);
        }

        if (Type == TIndexDescription::EType::GlobalSyncVectorKMeansTree) {
            *message->MutableVectorIndexKmeansTreeDescription() = std::get<NKikimrKqp::TVectorIndexKmeansTreeDescription>(SpecializedIndexDescription);
        }

    }

    bool IsSameIndex(const TIndexDescription& other) const {
        return Name == other.Name &&
            KeyColumns == other.KeyColumns &&
            DataColumns == other.DataColumns &&
            Type == other.Type;
    }

    bool ItUsedForWrite() const {
        switch (Type) {
            case EType::GlobalSync:
                return true;
            case EType::GlobalSyncUnique:
                return true;
            case EType::GlobalAsync:
                return false;
            case EType::GlobalSyncVectorKMeansTree:
                return false;
        }
    }

    std::span<const std::string_view> GetImplTables() const {
        return NKikimr::NTableIndex::GetImplTables(NYql::TIndexDescription::ConvertIndexType(Type), KeyColumns);
    }
};

struct TColumnFamily {
    TString Name;
    TMaybe<TString> Data;
    TMaybe<TString> Compression;
    TMaybe<i32> CompressionLevel;
};

struct TTtlSettings {
    enum class EUnit: ui32 {
        Seconds = 1,
        Milliseconds = 2,
        Microseconds = 3,
        Nanoseconds = 4,
    };

    struct TTier {
        TDuration ApplyAfter;
        std::optional<TString> StorageName;
    };

    TString ColumnName;
    TMaybe<EUnit> ColumnUnit;
    std::vector<TTier> Tiers;

    static bool TryParse(const NNodes::TCoNameValueTupleList& node, TTtlSettings& settings, TString& error);
};

struct TTableSettings {
    TMaybe<TString> CompactionPolicy;
    TVector<TString> PartitionBy;
    TMaybe<TString> AutoPartitioningBySize;
    TMaybe<ui64> PartitionSizeMb;
    TMaybe<TString> AutoPartitioningByLoad;
    TMaybe<ui64> MinPartitions;
    TMaybe<ui64> MaxPartitions;
    TMaybe<ui64> UniformPartitions;
    TVector<TVector<std::pair<EDataSlot, TString>>> PartitionAtKeys;
    TMaybe<TString> KeyBloomFilter;
    TMaybe<TString> ReadReplicasSettings;
    TResetableSetting<TTtlSettings, void> TtlSettings;
    TMaybe<TString> PartitionByHashFunction;
    TMaybe<TString> StoreExternalBlobs;

    // These parameters are only used for external sources
    TMaybe<TString> DataSourcePath;
    TMaybe<TString> Location;
    TVector<std::pair<TString, TString>> ExternalSourceParameters;

    bool IsSet() const;
};

struct TKikimrPathId {

    static const ui64 InvalidOwnerId = Max<ui64>();
    static const ui64 InvalidTableId = Max<ui64>();

    explicit TKikimrPathId(const std::pair<ui64, ui64>& raw)
        : Raw(raw) {}

    TKikimrPathId(ui64 ownerId, ui64 tableId)
        : TKikimrPathId(std::make_pair(ownerId, tableId)) {}

    TKikimrPathId(const NKikimrKqp::TKqpPathIdProto* message)
        : TKikimrPathId(std::make_pair(message->GetOwnerId(), message->GetTableId())) {}

    TKikimrPathId()
        : TKikimrPathId(InvalidOwnerId, InvalidTableId) {}

    ui64 OwnerId() const { return Raw.first; }
    ui64 TableId() const { return Raw.second; }

    TString ToString() const {
        return ::ToString(OwnerId()) + ':' + ::ToString(TableId());
    }

    bool operator==(const TKikimrPathId& x) const {
        return Raw == x.Raw;
    }

    bool operator!=(const TKikimrPathId& x) const {
        return !operator==(x);
    }

    ui64 Hash() const noexcept {
        return THash<decltype(Raw)>()(Raw);
    }

    static TKikimrPathId Parse(const TStringBuf& str);

    std::pair<ui64, ui64> Raw;

    void ToMessage(NKikimrKqp::TKqpPathIdProto* message) const {
        message->SetOwnerId(OwnerId());
        message->SetTableId(TableId());
    }
};

struct TKikimrColumnMetadata {

    TString Name;
    ui32 Id = 0;
    TString Type;
    bool NotNull = false;
    NKikimr::NScheme::TTypeInfo TypeInfo;
    TString TypeMod;
    TVector<TString> Families;
    NKikimrKqp::TKqpColumnMetadataProto::EDefaultKind DefaultKind = NKikimrKqp::TKqpColumnMetadataProto::DEFAULT_KIND_UNSPECIFIED;
    TString DefaultFromSequence;
    TKikimrPathId DefaultFromSequencePathId;
    Ydb::TypedValue DefaultFromLiteral;
    bool IsBuildInProgress = false;

    TKikimrColumnMetadata() = default;

    TKikimrColumnMetadata(const TString& name, ui32 id, const TString& type, bool notNull,
        NKikimr::NScheme::TTypeInfo typeInfo = {}, const TString& typeMod = {}, const TString& defaultFromSequence = {},
        const TKikimrPathId& defaultFromSequencePathId = {}, NKikimrKqp::TKqpColumnMetadataProto::EDefaultKind defaultKind = NKikimrKqp::TKqpColumnMetadataProto::DEFAULT_KIND_UNSPECIFIED,
        const Ydb::TypedValue& defaultFromLiteral = {}, bool isBuildInProgress = false)
        : Name(name)
        , Id(id)
        , Type(type)
        , NotNull(notNull)
        , TypeInfo(typeInfo)
        , TypeMod(typeMod)
        , DefaultKind(defaultKind)
        , DefaultFromSequence(defaultFromSequence)
        , DefaultFromSequencePathId(defaultFromSequencePathId)
        , DefaultFromLiteral(defaultFromLiteral)
        , IsBuildInProgress(isBuildInProgress)
    {}

    explicit TKikimrColumnMetadata(const NKikimrKqp::TKqpColumnMetadataProto* message)
        : Name(message->GetName())
        , Id(message->GetId())
        , Type(message->GetType())
        , NotNull(message->GetNotNull())
        , Families(message->GetFamily().begin(), message->GetFamily().end())
        , DefaultKind(message->GetDefaultKind())
        , DefaultFromSequence(message->GetDefaultFromSequence())
        , DefaultFromSequencePathId(&message->GetDefaultFromSequencePathId())
        , DefaultFromLiteral(message->GetDefaultFromLiteral())
        , IsBuildInProgress(message->GetIsBuildInProgress())
    {
        auto typeInfoMod = NKikimr::NScheme::TypeInfoModFromProtoColumnType(message->GetTypeId(),
            message->HasTypeInfo() ? &message->GetTypeInfo() : nullptr);
        TypeInfo = typeInfoMod.TypeInfo;
        TypeMod = typeInfoMod.TypeMod;
    }

    void SetDefaultFromSequence() {
        DefaultKind = NKikimrKqp::TKqpColumnMetadataProto::DEFAULT_KIND_SEQUENCE;
    }

    void SetDefaultFromLiteral() {
        DefaultKind = NKikimrKqp::TKqpColumnMetadataProto::DEFAULT_KIND_LITERAL;
    }

    bool IsDefaultFromSequence() const {
        return DefaultKind == NKikimrKqp::TKqpColumnMetadataProto::DEFAULT_KIND_SEQUENCE;
    }

    bool IsDefaultFromLiteral() const {
        return DefaultKind == NKikimrKqp::TKqpColumnMetadataProto::DEFAULT_KIND_LITERAL;
    }

    bool IsDefaultKindDefined() const {
        return DefaultKind != NKikimrKqp::TKqpColumnMetadataProto::DEFAULT_KIND_UNSPECIFIED;
    }

    void ToMessage(NKikimrKqp::TKqpColumnMetadataProto* message) const {
        message->SetName(Name);
        message->SetId(Id);
        message->SetType(Type);
        message->SetNotNull(NotNull);
        auto columnType = NKikimr::NScheme::ProtoColumnTypeFromTypeInfoMod(TypeInfo, TypeMod);
        message->SetTypeId(columnType.TypeId);
        message->SetDefaultFromSequence(DefaultFromSequence);
        DefaultFromSequencePathId.ToMessage(message->MutableDefaultFromSequencePathId());
        message->SetDefaultKind(DefaultKind);
        message->MutableDefaultFromLiteral()->CopyFrom(DefaultFromLiteral);
        message->SetIsBuildInProgress(IsBuildInProgress);
        if (columnType.TypeInfo) {
            *message->MutableTypeInfo() = *columnType.TypeInfo;
        }
        for(auto& family: Families) {
            message->AddFamily(family);
        }
    }

    bool IsSameScheme(const TKikimrColumnMetadata& other) const {
        return Name == other.Name && Type == other.Type && NotNull == other.NotNull;
    }

    void SetNotNull() {
        NotNull = true;
    }
};

enum class EKikimrTableKind : ui32 {
    Unspecified = 0,
    Datashard = 1,
    SysView = 2,
    Olap = 3,
    External = 4,
    View = 5,
};

enum class ETableType : ui32 {
    Unknown = 0,
    Table = 1,
    TableStore = 2,
    ExternalTable = 3
};

ETableType GetTableTypeFromString(const TStringBuf& tableType);

bool GetTopicMeteringModeFromString(const TString& meteringMode,
                                                        Ydb::Topic::MeteringMode& result);
TVector<Ydb::Topic::Codec> GetTopicCodecsFromString(const TStringBuf& codecsStr);
bool GetTopicAutoPartitioningStrategyFromString(const TString& strategy, Ydb::Topic::AutoPartitioningStrategy& result);


enum class EStoreType : ui32 {
    Row = 0,
    Column = 1
};

enum class ESourceType : ui32 {
    Unknown = 0,
    ExternalTable = 1,
    ExternalDataSource = 2
};

struct TExternalSource {
    ESourceType SourceType = ESourceType::Unknown;
    TString Type;
    TString TableLocation;
    TString TableContent;
    TString DataSourcePath;
    TString DataSourceLocation;
    TString DataSourceInstallation;
    TString ServiceAccountIdSignature;
    TString Password;
    TString AwsAccessKeyId;
    TString AwsSecretAccessKey;
    TString Token;
    NKikimrSchemeOp::TAuth DataSourceAuth;
    NKikimrSchemeOp::TExternalDataSourceProperties Properties;
};

enum EMetaSerializationType : ui64 {
    EncodedProto = 1,
    Json = 2
};

struct TViewPersistedData {
    TString QueryText;
    NYql::NProto::TTranslationSettings CapturedContext;
};

struct TKikimrTableMetadata : public TThrRefBase {
    TIntrusivePtr<TKikimrTableMetadata> Next;

    bool DoesExist = false;
    TString Cluster;
    TString Name;
    std::optional<TString> QueryName = std::nullopt;
    bool Temporary = false;
    TKikimrPathId PathId;
    TString SysView;
    ui64 SchemaVersion = 0;
    THashMap<TString, TString> Attributes;
    EKikimrTableKind Kind = EKikimrTableKind::Unspecified;
    ETableType TableType = ETableType::Table;
    EStoreType StoreType = EStoreType::Row;
    TMaybe<NKikimrSysView::TSysViewDescription> SysViewInfo;
    bool IsIndexImplTable = false;

    // If writes are disabled, query that writes to table must finish with error.
    bool WritesToTableAreDisabled = false;
    TString DisableWritesReason;

    ui64 RecordsCount = 0;
    ui64 DataSize = 0;
    ui64 MemorySize = 0;
    ui32 ShardsCount = 0;
    bool StatsLoaded = false;

    TInstant LastAccessTime;
    TInstant LastUpdateTime;

    TMap<TString, TKikimrColumnMetadata> Columns;
    TVector<TString> KeyColumnNames;
    TVector<TString> ColumnOrder;

    // Indexes and ImplTables must be in same order
    TVector<TIndexDescription> Indexes;
    TVector<TIntrusivePtr<TKikimrTableMetadata>> ImplTables;

    TVector<TColumnFamily> ColumnFamilies;
    TTableSettings TableSettings;

    TExternalSource ExternalSource;
    TViewPersistedData ViewPersistedData;

    TVector<TString> PartitionedByColumns;

    TKikimrTableMetadata(const TString& cluster, const TString& table)
        : Cluster(cluster)
        , Name(table)
        , PathId(std::make_pair(0, 0)) {}

    TKikimrTableMetadata()
        : TKikimrTableMetadata("", "") {}

    TKikimrTableMetadata(const NKikimrKqp::TKqpTableMetadataProto* message)
        : DoesExist(message->GetDoesExist())
        , Cluster(message->GetCluster())
        , Name(message->GetName())
        , PathId(&message->GetPathId())
        , SysView(message->GetSysView())
        , SchemaVersion(message->GetSchemaVersion())
        , Kind(static_cast<EKikimrTableKind>(message->GetKind()))
        , RecordsCount(message->GetRecordsCount())
        , DataSize(message->GetDataSize())
        , StatsLoaded(message->GetStatsLoaded())
        , KeyColumnNames(message->GetKeyColunmNames().begin(), message->GetKeyColunmNames().end())

    {
        for(auto& attr: message->GetAttributes()) {
            Attributes.emplace(attr.GetKey(), attr.GetValue());
        }

        std::map<ui32, TString> orderMap;
        for(auto& col: message->GetColumns()) {
            Columns.emplace(col.GetName(), TKikimrColumnMetadata(&col));
            orderMap.emplace(col.GetId(), col.GetName());
        }

        const auto indexesCount = message->GetIndexes().size();
        Indexes.reserve(indexesCount);
        for(auto& index: message->GetIndexes()) {
            Indexes.emplace_back(&index);
        }

        auto it = message->GetSecondaryGlobalIndexMetadata().begin();
        ImplTables.reserve(indexesCount);
        for(int i = 0; i < indexesCount; ++i) {
            decltype(ImplTables)::value_type* implTable = nullptr;
            for (const auto& _ : Indexes[i].GetImplTables()) {
                YQL_ENSURE(it != message->GetSecondaryGlobalIndexMetadata().end());
                if (implTable) {
                    (*implTable)->Next = MakeIntrusive<TKikimrTableMetadata>(&*it++);
                    implTable = &(*implTable)->Next;
                } else {
                    implTable = &ImplTables.emplace_back(MakeIntrusive<TKikimrTableMetadata>(&*it++));
                }
            }
        }
        YQL_ENSURE(it == message->GetSecondaryGlobalIndexMetadata().end());

        ColumnOrder.reserve(Columns.size());
        for(auto& [_, name]: orderMap) {
            ColumnOrder.emplace_back(name);
        }
    }

    bool IsSameTable(const TKikimrTableMetadata& other) {
        if (!DoesExist) {
            return false;
        }

        if (Cluster != other.Cluster || Name != other.Name || Columns.size() != other.Columns.size() ||
                KeyColumnNames != other.KeyColumnNames || Indexes.size() != other.Indexes.size()) {
            return false;
        }

        for (auto& [name, column]: Columns) {
            auto otherColumn = other.Columns.FindPtr(name);
            if (!otherColumn) {
                return false;
            }

            if (!column.IsSameScheme(*otherColumn)) {
                return false;
            }
        }

        for (size_t i = 0; i < Indexes.size(); i++) {
            if (!Indexes[i].IsSameIndex(other.Indexes[i])) {
                return false;
            }
        }

        return true;
    }

    void ToMessage(NKikimrKqp::TKqpTableMetadataProto* message) const {
        message->SetDoesExist(DoesExist);
        message->SetCluster(Cluster);
        message->SetName(Name);
        message->SetSysView(SysView);
        PathId.ToMessage(message->MutablePathId());
        message->SetSchemaVersion(SchemaVersion);
        message->SetKind(static_cast<ui32>(Kind));
        message->SetStatsLoaded(StatsLoaded);
        message->SetRecordsCount(RecordsCount);
        message->SetDataSize(DataSize);
        for(auto& [key, value] : Attributes) {
            message->AddAttributes()->SetKey(key);
            message->AddAttributes()->SetValue(value);
        }

        for(auto& [name, column] : Columns) {
            column.ToMessage(message->AddColumns());
        }

        for(auto& key: KeyColumnNames) {
            message->AddKeyColunmNames(key);
        }

        for(auto& index: Indexes) {
            index.ToMessage(message->AddIndexes());
        }

        for(auto implTable: ImplTables) {
            YQL_ENSURE(implTable);
            do {
                implTable->ToMessage(message->AddSecondaryGlobalIndexMetadata());
                implTable = implTable->Next;
            } while (implTable);
        }
    }

    TString SerializeToString() const {
        NKikimrKqp::TKqpTableMetadataProto proto;
        ToMessage(&proto);
        return proto.SerializeAsString();
    }

    std::pair<TIntrusivePtr<TKikimrTableMetadata>, const TIndexDescription*> GetIndex(std::string_view indexName) const {
        YQL_ENSURE(Indexes.size(), "GetIndexMetadata called for table without indexes");
        YQL_ENSURE(Indexes.size() == ImplTables.size(), "index metadata has not been loaded yet");
        for (size_t i = 0; i < Indexes.size(); i++) {
            if (Indexes[i].Name == indexName) {
                auto implTable = ImplTables[i];
                YQL_ENSURE(implTable, "unexpected empty metadata for index " << indexName);
                return {std::move(implTable), &Indexes[i]};
            }
        }
        return {nullptr, nullptr};
    }

    std::pair<TIntrusivePtr<TKikimrTableMetadata>, TIndexDescription::EIndexState> GetIndexMetadata(std::string_view indexName) const {
        auto [implTable, index] = GetIndex(indexName);
        return {std::move(implTable), index ? index->State : TIndexDescription::EIndexState::Invalid};
    }

    bool IsOlap() const {
        return Kind == EKikimrTableKind::Olap;
    }
};

struct TAlterDatabaseSettings {
    TString DatabasePath;
    std::optional<TString> Owner;
    std::optional<NKikimrSubDomains::TSchemeLimits> SchemeLimits;
};

struct TCreateUserSettings {
    TString UserName;
    TString Password;
    bool IsHashedPassword = false;
    bool CanLogin;
};

struct TModifyPermissionsSettings {
    enum class EAction : ui32 {
        Grant,
        Revoke
    };

    EAction Action = EAction::Grant;
    THashSet<TString> Permissions;
    THashSet<TString> Paths;
    THashSet<TString> Roles;
    bool IsPermissionsClear = false;
};

struct TAlterUserSettings {
    TString UserName;
    std::optional<TString> Password;
    bool IsHashedPassword = false;
    std::optional<bool> CanLogin;
};

struct TDropUserSettings {
    TString UserName;
    bool MissingOk = false;
};

struct TCreateGroupSettings {
    TString GroupName;
    std::vector<TString> Roles;
};

struct TAlterGroupSettings {
    enum class EAction : ui32 {
        AddRoles = 0,
        RemoveRoles = 1,
    };

    TString GroupName;
    EAction Action;
    std::vector<TString> Roles;
};

struct TRenameGroupSettings {
    TString GroupName;
    TString NewName;
};

struct TDropGroupSettings {
    TString GroupName;
    bool MissingOk = false;
};

struct TAlterColumnTableSettings {
    TString Table;
};

struct TCreateTableStoreSettings {
    TString TableStore;
    ui32 ShardsCount = 0;
    TMap<TString, TKikimrColumnMetadata> Columns;
    TVector<TString> KeyColumnNames;
    TVector<TString> ColumnOrder;
    TVector<TIndexDescription> Indexes;
    TVector<TColumnFamily> ColumnFamilies;
};

struct TAlterTableStoreSettings {
    TString TableStore;
};

struct TDropTableSettings {
    TString Table;
    bool SuccessOnNotExist;
};

struct TDropTableStoreSettings {
    TString TableStore;
};

struct TCreateExternalTableSettings {
    TString ExternalTable;
    TString DataSourcePath;
    TString Location;
    TVector<TString> ColumnOrder;
    TMap<TString, TKikimrColumnMetadata> Columns;
    TVector<std::pair<TString, TString>> SourceTypeParameters;
};

struct TAlterTopicSettings {
    Ydb::Topic::AlterTopicRequest Request;
    TString Name;
    TString WorkDir;
    bool MissingOk;
};

struct TSequenceSettings {
    TMaybe<i64> MinValue;
    TMaybe<i64> MaxValue;
    TMaybe<i64> StartValue;
    TMaybe<ui64> Cache;
    TMaybe<i64> Increment;
    TMaybe<bool> Cycle;
    TMaybe<TString> OwnedBy;
    TMaybe<TString> DataType;
    TMaybe<bool> Restart;
    TMaybe<i64> RestartValue;
};

struct TCreateSequenceSettings {
    TString Name;
    bool Temporary = false;
    TSequenceSettings SequenceSettings;
};

struct TDropSequenceSettings {
    TString Name;
};

struct TAlterSequenceSettings {
    TString Name;
    TSequenceSettings SequenceSettings;
};

struct TAlterExternalTableSettings {
    TString ExternalTable;
};

struct TDropExternalTableSettings {
    TString ExternalTable;
};

struct TReplicationSettingsBase {
    struct TOAuthToken {
        TString Token;
        TString TokenSecretName;

        void Serialize(NKikimrReplication::TOAuthToken& proto) const;
    };

    struct TStaticCredentials {
        TString UserName;
        TString Password;
        TString PasswordSecretName;

        void Serialize(NKikimrReplication::TStaticCredentials& proto) const;
    };

    struct TStateDone {
        enum class EFailoverMode: ui32 {
            Consistent = 1,
            Force = 2,
        };

        EFailoverMode FailoverMode;
    };

    TMaybe<TString> ConnectionString;
    TMaybe<TString> Endpoint;
    TMaybe<TString> Database;
    TMaybe<TOAuthToken> OAuthToken;
    TMaybe<TStaticCredentials> StaticCredentials;
    TMaybe<TString> CaCert;
    TMaybe<TStateDone> StateDone;
    bool StatePaused = false;
    bool StateStandBy = false;

    using EFailoverMode = TStateDone::EFailoverMode;
    TStateDone& EnsureStateDone(EFailoverMode mode = EFailoverMode::Consistent) {
        if (!StateDone) {
            StateDone = TStateDone{
                .FailoverMode = mode,
            };
        }

        return *StateDone;
    }

    TOAuthToken& EnsureOAuthToken() {
        if (!OAuthToken) {
            OAuthToken = TOAuthToken();
        }

        return *OAuthToken;
    }

    TStaticCredentials& EnsureStaticCredentials() {
        if (!StaticCredentials) {
            StaticCredentials = TStaticCredentials();
        }

        return *StaticCredentials;
    }
};

struct TReplicationSettings : public TReplicationSettingsBase {

    struct TRowConsistency {};

    struct TGlobalConsistency {
        TDuration CommitInterval;

        void Serialize(NKikimrReplication::TConsistencySettings_TGlobalConsistency& proto) const;
    };

    TMaybe<TRowConsistency> RowConsistency;
    TMaybe<TGlobalConsistency> GlobalConsistency;

    TRowConsistency& EnsureRowConsistency() {
        if (!RowConsistency) {
            RowConsistency = TRowConsistency();
        }

        return *RowConsistency;
    }

    TGlobalConsistency& EnsureGlobalConsistency() {
        if (!GlobalConsistency) {
            GlobalConsistency = TGlobalConsistency();
        }

        return *GlobalConsistency;
    }
};

struct TCreateReplicationSettings {
    TString Name;
    TVector<std::pair<TString, TString>> Targets;
    TReplicationSettings Settings;
};

struct TAlterReplicationSettings {
    TString Name;
    TReplicationSettings Settings;
};

struct TDropReplicationSettings {
    TString Name;
    bool Cascade = false;
};

struct TTransferSettings : public TReplicationSettingsBase {

    struct TBatching {
        TDuration FlushInterval;
        std::optional<ui64> BatchSizeBytes;
    };

    TMaybe<TString> ConsumerName;
    TMaybe<TBatching> Batching;

    TBatching& EnsureBatching() {
        if (!Batching) {
            Batching = TBatching();
        }

        return *Batching;
    }

    TMaybe<TString> DirectoryPath;
};

struct TCreateTransferSettings {
    TString Name;
    std::tuple<TString, TString, TString> Target;
    TTransferSettings Settings;
};

struct TAlterTransferSettings {
    TString Name;
    TString TranformLambda;
    TTransferSettings Settings;
};

struct TDropTransferSettings {
    TString Name;
    bool Cascade = false;
};

struct TAnalyzeSettings {
    TString TablePath;
    TVector<TString> Columns;
};

struct TBackupCollectionSettings {
    bool IncrementalBackupEnabled;
};

struct TCreateBackupCollectionSettings {
    struct TDatabase {};

    struct TTable {
        TString Path;
    };

    TString Name;
    TString Prefix;
    std::variant<TDatabase, TVector<TTable>> Entries;
    TBackupCollectionSettings Settings;
};

struct TAlterBackupCollectionSettings {
    TString Name;
    TString Prefix;
    TBackupCollectionSettings Settings;
};

struct TDropBackupCollectionSettings {
    TString Name;
    TString Prefix;
    bool Cascade = false;
};

struct TBackupSettings {
    TString Name;
};

struct TKikimrListPathItem {
    TKikimrListPathItem(TString name, bool isDirectory) {
        Name = name;
        IsDirectory = isDirectory;
    }

    TString Name;
    bool IsDirectory;
};

typedef TIntrusivePtr<TKikimrTableMetadata> TKikimrTableMetadataPtr;

template<typename TResult>
class IKikimrAsyncResult : public TThrRefBase {
public:
    virtual bool HasResult() const = 0;
    virtual TResult GetResult() = 0;
    virtual NThreading::TFuture<bool> Continue() = 0;

    virtual ~IKikimrAsyncResult() {}
};

template<typename TResult>
class TKikimrResultHolder : public IKikimrAsyncResult<TResult> {
public:
    TKikimrResultHolder(TResult&& result)
        : Result(std::move(result)) {}

    bool HasResult() const override {
        return Full;
    }

    TResult GetResult() override {
        Full = false;
        return std::move(Result);
    }

    NThreading::TFuture<bool> Continue() override {
        return NThreading::MakeFuture<bool>(true);
    }

private:
    TResult Result;
    bool Full = true;
};

template<typename TResult>
static TIntrusivePtr<TKikimrResultHolder<TResult>> MakeKikimrResultHolder(TResult&& result) {
    return MakeIntrusive<TKikimrResultHolder<TResult>>(std::move(result));
}

class IKikimrGateway : public TThrRefBase {
public:
    using TPtr = TIntrusivePtr<IKikimrGateway>;

    struct TGenericResult : public NCommon::TOperationResult {
    };

    struct TListPathResult : public TGenericResult {
        TString Path;
        TVector<TKikimrListPathItem> Items;
    };

    struct TTableMetadataResult : public TGenericResult {
        TKikimrTableMetadataPtr Metadata;
    };

    struct TQueryResult : public TGenericResult {
        TQueryResult()
            : ProtobufArenaPtr(MakeIntrusive<NActors::TProtoArenaHolder>())
        {}

        TString SessionId;
        TVector<Ydb::ResultSet*> Results;
        NKqpProto::TKqpStatsQuery QueryStats;
        std::unique_ptr<NKikimrKqp::TPreparedQuery> PreparingQuery;
        std::shared_ptr<const NKikimrKqp::TPreparedQuery> PreparedQuery;
        TString QueryAst;
        TString QueryPlan;
        TIntrusivePtr<NActors::TProtoArenaHolder> ProtobufArenaPtr;
        TMaybe<ui16> SqlVersion;
        google::protobuf::RepeatedPtrField<NKqpProto::TResultSetMeta> ResultSetsMeta;
        bool NeedToSplit = false;
        bool AllowCache = true;
        TMaybe<TString> CommandTagName = {};
    };

    struct TExecuteLiteralResult : public TGenericResult {
        TString BinaryResult;
        NKikimrMiniKQL::TResult Result;
    };

    struct TLoadTableMetadataSettings {
        TLoadTableMetadataSettings& WithTableStats(bool enable) {
            RequestStats_ = enable;
            return *this;
        }

        TLoadTableMetadataSettings& WithPrivateTables(bool enable) {
            WithPrivateTables_ = enable;
            return *this;
        }

        TLoadTableMetadataSettings& WithExternalDatasources(bool enable) {
            WithExternalDatasources_ = enable;
            return *this;
        }

        TLoadTableMetadataSettings& WithAuthInfo(bool enable) {
            RequestAuthInfo_ = enable;
            return *this;
        }

        TLoadTableMetadataSettings& WithExternalSourceFactory(NKikimr::NExternalSource::IExternalSourceFactory::TPtr factory) {
            ExternalSourceFactory = std::move(factory);
            return *this;
        }

        TLoadTableMetadataSettings& WithReadAttributes(THashMap<TString, TString> options) {
            ReadAttributes = std::move(options);
            return *this;
        }

        TLoadTableMetadataSettings& WithSysViewRewritten(bool enable) {
            SysViewRewritten_ = enable;
            return *this;
        }

        NKikimr::NExternalSource::IExternalSourceFactory::TPtr ExternalSourceFactory;
        THashMap<TString, TString> ReadAttributes;
        bool RequestStats_ = false;
        bool WithPrivateTables_ = false;
        bool WithExternalDatasources_ = false;
        bool RequestAuthInfo_ = true;
        bool SysViewRewritten_ = false;
    };

    class IKqpTableMetadataLoader : public std::enable_shared_from_this<IKqpTableMetadataLoader> {
    public:
        virtual NThreading::TFuture<TTableMetadataResult> LoadTableMetadata(
            const TString& cluster, const TString& table, const TLoadTableMetadataSettings& settings, const TString& database,
            const TIntrusiveConstPtr<NACLib::TUserToken>& userToken) = 0;

        virtual TVector<NKikimrKqp::TKqpTableMetadataProto> GetCollectedSchemeData() = 0;

        virtual ~IKqpTableMetadataLoader() = default;
    };

public:
    virtual bool HasCluster(const TString& cluster) = 0;
    virtual TVector<TString> GetClusters() = 0;
    virtual TString GetDefaultCluster() = 0;
    virtual TMaybe<TString> GetSetting(const TString& cluster, const TString& name) = 0;

    virtual void SetToken(const TString& cluster, const TIntrusiveConstPtr<NACLib::TUserToken>& token) = 0;
    virtual void SetClientAddress(const TString& clientAddress) = 0;

    virtual NThreading::TFuture<TListPathResult> ListPath(const TString& cluster, const TString& path) = 0;

    virtual NThreading::TFuture<TTableMetadataResult> LoadTableMetadata(
        const TString& cluster, const TString& table, TLoadTableMetadataSettings settings) = 0;

    virtual NThreading::TFuture<TGenericResult> AlterDatabase(const TString& cluster, const TAlterDatabaseSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> CreateTable(TKikimrTableMetadataPtr metadata, bool createDir, bool existingOk = false, bool replaceIfExists = false) = 0;

    virtual NThreading::TFuture<TGenericResult> SendSchemeExecuterRequest(const TString& cluster,
        const TMaybe<TString>& requestType,
        const std::shared_ptr<const NKikimr::NKqp::TKqpPhyTxHolder> &phyTx) = 0;

    virtual NThreading::TFuture<TGenericResult> AlterTable(const TString& cluster, Ydb::Table::AlterTableRequest&& req,
        const TMaybe<TString>& requestType, ui64 flags, NKikimrIndexBuilder::TIndexBuildSettings&& buildSettings) = 0;

    virtual NThreading::TFuture<TGenericResult> RenameTable(const TString& src, const TString& dst, const TString& cluster) = 0;

    virtual NThreading::TFuture<TGenericResult> DropTable(const TString& cluster, const TDropTableSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> CreateTopic(const TString& cluster, Ydb::Topic::CreateTopicRequest&& request, bool existingOk) = 0;

    virtual NThreading::TFuture<TGenericResult> AlterTopic(const TString& cluster, Ydb::Topic::AlterTopicRequest&& request, bool missingOk) = 0;

    virtual NThreading::TFuture<NKikimr::NGRpcProxy::V1::TAlterTopicResponse> AlterTopicPrepared(TAlterTopicSettings&& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> DropTopic(const TString& cluster, const TString& topic, bool missingOk) = 0;

    virtual NThreading::TFuture<TGenericResult> CreateReplication(const TString& cluster, const TCreateReplicationSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> AlterReplication(const TString& cluster, const TAlterReplicationSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> DropReplication(const TString& cluster, const TDropReplicationSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> CreateTransfer(const TString& cluster, const TCreateTransferSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> AlterTransfer(const TString& cluster, const TAlterTransferSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> DropTransfer(const TString& cluster, const TDropTransferSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> ModifyPermissions(const TString& cluster, const TModifyPermissionsSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> CreateBackupCollection(const TString& cluster, const TCreateBackupCollectionSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> AlterBackupCollection(const TString& cluster, const TAlterBackupCollectionSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> DropBackupCollection(const TString& cluster, const TDropBackupCollectionSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> Backup(const TString& cluster, const TBackupSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> BackupIncremental(const TString& cluster, const TBackupSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> Restore(const TString& cluster, const TBackupSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> CreateUser(const TString& cluster, const TCreateUserSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> AlterUser(const TString& cluster, const TAlterUserSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> DropUser(const TString& cluster, const TDropUserSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> UpsertObject(const TString& cluster, const TUpsertObjectSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> CreateObject(const TString& cluster, const TCreateObjectSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> AlterObject(const TString& cluster, const TAlterObjectSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> DropObject(const TString& cluster, const TDropObjectSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> CreateGroup(const TString& cluster, const TCreateGroupSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> AlterGroup(const TString& cluster, TAlterGroupSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> RenameGroup(const TString& cluster, TRenameGroupSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> DropGroup(const TString& cluster, const TDropGroupSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> CreateSequence(const TString& cluster,
        const TCreateSequenceSettings& settings, bool existingOk) = 0;
    virtual NThreading::TFuture<TGenericResult> DropSequence(const TString& cluster,
        const TDropSequenceSettings& settings, bool missingOk) = 0;
    virtual NThreading::TFuture<TGenericResult> AlterSequence(const TString& cluster,
        const TAlterSequenceSettings& settings, bool missingOk) = 0;

    virtual NThreading::TFuture<TGenericResult> CreateColumnTable(
        TKikimrTableMetadataPtr metadata, bool createDir, bool existingOk = false) = 0;

    virtual NThreading::TFuture<TGenericResult> AlterColumnTable(const TString& cluster, Ydb::Table::AlterTableRequest&& req) = 0;

    virtual NThreading::TFuture<TGenericResult> CreateTableStore(const TString& cluster,
        const TCreateTableStoreSettings& settings, bool existingOk = false) = 0;

    virtual NThreading::TFuture<TGenericResult> AlterTableStore(const TString& cluster, const TAlterTableStoreSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> DropTableStore(const TString& cluster,
        const TDropTableStoreSettings& settings, bool missingOk) = 0;

    virtual NThreading::TFuture<TGenericResult> CreateExternalTable(const TString& cluster, const TCreateExternalTableSettings& settings, bool createDir, bool existingOk, bool replaceIfExists) = 0;

    virtual NThreading::TFuture<TGenericResult> AlterExternalTable(const TString& cluster, const TAlterExternalTableSettings& settings) = 0;

    virtual NThreading::TFuture<TGenericResult> DropExternalTable(const TString& cluster, const TDropExternalTableSettings& settings, bool missingOk) = 0;

    virtual NThreading::TFuture<TGenericResult> Analyze(const TString& cluster, const TAnalyzeSettings& settings) = 0;

    virtual TVector<NKikimrKqp::TKqpTableMetadataProto> GetCollectedSchemeData() = 0;

    virtual TExecuteLiteralResult ExecuteLiteralInstant(const TString& program, ui32 langVer, const NKikimrMiniKQL::TType& resultType, NKikimr::NKqp::TTxAllocatorState::TPtr txAlloc) = 0;

public:
    using TCreateDirFunc = std::function<void(const TString&, const TString&, NThreading::TPromise<TGenericResult>)>;

    static NThreading::TFuture<TGenericResult> CreatePath(const TString& path, TCreateDirFunc createDir);
};

EYqlIssueCode YqlStatusFromYdbStatus(ui32 ydbStatus);
Ydb::FeatureFlag::Status GetFlagValue(const TMaybe<bool>& value);

bool SetColumnType(const TTypeAnnotationNode* typeNode, bool notNull, Ydb::Type& protoType, TString& error);
bool ConvertReadReplicasSettingsToProto(const TString settings, Ydb::Table::ReadReplicasSettings& proto,
    Ydb::StatusIds::StatusCode& code, TString& error);
void ConvertTtlSettingsToProto(const NYql::TTtlSettings& settings, Ydb::Table::TtlSettings& proto);

} // namespace NYql

template<>
struct THash<NYql::TKikimrPathId> {
    inline ui64 operator()(const NYql::TKikimrPathId& x) const noexcept {
        return x.Hash();
    }
};
