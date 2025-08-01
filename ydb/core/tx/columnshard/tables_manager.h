#pragma once

#include "columnshard_schema.h"

#include "blobs_action/abstract/storages_manager.h"
#include "data_accessor/manager.h"
#include "engines/column_engine.h"
#include "engines/metadata_accessor.h"

#include <ydb/core/base/row_version.h>
#include <ydb/core/protos/tx_columnshard.pb.h>
#include <ydb/core/tx/columnshard/blobs_action/abstract/storage.h>
#include <ydb/core/tx/columnshard/common/path_id.h>
#include <ydb/core/tx/columnshard/counters/portion_index.h>
#include <ydb/core/tx/columnshard/engines/scheme/tiering/tier_info.h>

#include <ydb/library/accessor/accessor.h>

#include <util/digest/numeric.h>

namespace NKikimr::NColumnShard {

template <class TVersionData>
class TVersionedSchema {
private:
    TMap<NOlap::TSnapshot, ui64> Versions;
    TMap<ui64, TVersionData> VersionsById;
    TMap<ui64, NOlap::TSnapshot> MinVersionById;

public:
    bool IsEmpty() const {
        return VersionsById.empty();
    }

    const TMap<ui64, TVersionData>& GetVersionsById() const {
        return VersionsById;
    }

    TMap<ui64, TVersionData>& MutableVersionsById() {
        return VersionsById;
    }

    NOlap::TSnapshot GetMinVersionForId(const ui64 sVersion) const {
        auto it = MinVersionById.find(sVersion);
        Y_ABORT_UNLESS(it != MinVersionById.end());
        return it->second;
    }

    void AddVersion(const NOlap::TSnapshot& snapshot, const TVersionData& versionInfo) {
        ui64 ssVersion = 0;
        if (versionInfo.HasSchema()) {
            ssVersion = versionInfo.GetSchema().GetVersion();
        }
        VersionsById.emplace(ssVersion, versionInfo);
        Y_ABORT_UNLESS(Versions.emplace(snapshot, ssVersion).second);

        auto it = MinVersionById.find(ssVersion);
        if (it == MinVersionById.end()) {
            MinVersionById.emplace(ssVersion, snapshot);
        } else {
            it->second = std::min(snapshot, it->second);
        }
    }
};

class TSchemaPreset: public TVersionedSchema<NKikimrTxColumnShard::TSchemaPresetVersionInfo> {
public:
    using TSchemaPresetVersionInfo = NKikimrTxColumnShard::TSchemaPresetVersionInfo;
    ui32 Id = 0;
    TString Name;

public:
    bool IsStandaloneTable() const {
        return Id == 0;
    }

    const TString& GetName() const {
        return Name;
    }

    ui32 GetId() const {
        return Id;
    }

    void Deserialize(const NKikimrSchemeOp::TColumnTableSchemaPreset& presetProto);

    template <class TRow>
    bool InitFromDB(const TRow& rowset) {
        Id = rowset.template GetValue<Schema::SchemaPresetInfo::Id>();
        if (!IsStandaloneTable()) {
            Name = rowset.template GetValue<Schema::SchemaPresetInfo::Name>();
        }
        Y_ABORT_UNLESS(!Id || Name == "default", "Unsupported preset at load time");
        return true;
    }
};

class TTableInfo {
    const TInternalPathId InternalPathId;
    TSchemeShardLocalPathId SchemeShardLocalPathId;   //path id the table is known as at SchemeShard
    std::optional<NOlap::TSnapshot> DropVersion;
    YDB_READONLY_DEF(TSet<NOlap::TSnapshot>, Versions);

public:
    bool IsEmpty() const {
        return Versions.empty();
    }

    TUnifiedPathId GetPathId() const {
        return TUnifiedPathId::BuildValid(InternalPathId, SchemeShardLocalPathId);
    }

    const NOlap::TSnapshot& GetDropVersionVerified() const {
        AFL_VERIFY(DropVersion);
        return *DropVersion;
    }

    void SetDropVersion(const NOlap::TSnapshot& version) {
        AFL_VERIFY(!DropVersion)("exists", DropVersion->DebugString())("version", version.DebugString());
        DropVersion = version;
    }

    void AddVersion(const NOlap::TSnapshot& snapshot) {
        Versions.insert(snapshot);
    }

    void UpdateLocalPathId(NIceDb::TNiceDb& db, const TSchemeShardLocalPathId newPathId) {
        Schema::SaveTableSchemeShardLocalPathId(db, InternalPathId, newPathId);
        SchemeShardLocalPathId = newPathId;
    }

    bool IsDropped(const std::optional<NOlap::TSnapshot>& minReadSnapshot = std::nullopt) const {
        if (!DropVersion) {
            return false;
        }
        if (!minReadSnapshot) {
            return true;
        }
        return *DropVersion < *minReadSnapshot;
    }

    TTableInfo(const TUnifiedPathId& pathId)
        : InternalPathId(pathId.InternalPathId)
        , SchemeShardLocalPathId(pathId.SchemeShardLocalPathId) {
    }

    template <class TRow>
    static TTableInfo InitFromDB(const TRow& rowset) {
        const auto internalPathId = TInternalPathId::FromRawValue(rowset.template GetValue<Schema::TableInfo::PathId>());
        AFL_VERIFY(internalPathId);
        const auto& schemeShardLocalPathId =
            TSchemeShardLocalPathId::FromRawValue(rowset.template HaveValue<Schema::TableInfo::SchemeShardLocalPathId>()
                                                      ? rowset.template GetValue<Schema::TableInfo::SchemeShardLocalPathId>()
                                                      : internalPathId.GetRawValue());
        AFL_VERIFY(schemeShardLocalPathId);
        TTableInfo result(TUnifiedPathId::BuildValid(internalPathId, schemeShardLocalPathId));
        if (rowset.template HaveValue<Schema::TableInfo::DropStep>() && rowset.template HaveValue<Schema::TableInfo::DropTxId>()) {
            result.DropVersion.emplace(
                rowset.template GetValue<Schema::TableInfo::DropStep>(), rowset.template GetValue<Schema::TableInfo::DropTxId>());
        }
        return result;
    }
};

class TTtlVersions {
private:
    THashMap<TInternalPathId, std::map<NOlap::TSnapshot, std::optional<NOlap::TTiering>>> Ttl;

    void AddVersion(const TInternalPathId pathId, const NOlap::TSnapshot& snapshot, std::optional<NOlap::TTiering> ttl) {
        AFL_VERIFY(Ttl[pathId].emplace(snapshot, ttl).second)("snapshot", snapshot);
    }

public:
    void AddVersionFromProto(
        const TInternalPathId pathId, const NOlap::TSnapshot& snapshot, const NKikimrSchemeOp::TColumnDataLifeCycle& ttlSettings) {
        std::optional<NOlap::TTiering> ttlVersion;
        if (ttlSettings.HasEnabled()) {
            NOlap::TTiering deserializedTtl;
            AFL_VERIFY(deserializedTtl.DeserializeFromProto(ttlSettings.GetEnabled()).IsSuccess());
            ttlVersion.emplace(std::move(deserializedTtl));
        }
        AddVersion(pathId, snapshot, ttlVersion);
    }

    std::optional<NOlap::TTiering> GetTableTtl(const TInternalPathId pathId, const NOlap::TSnapshot& snapshot = NOlap::TSnapshot::Max()) const {
        auto findTable = Ttl.FindPtr(pathId);
        if (!findTable) {
            return std::nullopt;
        }
        const auto findTtl = findTable->upper_bound(snapshot);
        if (findTtl == findTable->begin()) {
            return std::nullopt;
        }
        return std::prev(findTtl)->second;
    }

    ui64 GetMemoryUsage() const {
        ui64 memory = 0;
        for (const auto& [_, ttlVersions] : Ttl) {
            memory += ttlVersions.size() * sizeof(NOlap::TTiering);
        }
        return memory;
    }
};

class TTablesManager: public NOlap::IPathIdTranslator {
private:
    THashMap<TInternalPathId, TTableInfo> Tables;
    THashMap<TSchemeShardLocalPathId, TInternalPathId> SchemeShardLocalToInternal;
    THashMap<TSchemeShardLocalPathId, TInternalPathId> RenamingLocalToInternal;   // Paths that are being renamed
    THashSet<ui32> SchemaPresetsIds;
    THashMap<ui32, NKikimrSchemeOp::TColumnTableSchema> ActualSchemaForPreset;
    std::map<NOlap::TSnapshot, THashSet<TInternalPathId>> PathsToDrop;
    TTtlVersions Ttl;
    std::unique_ptr<NOlap::IColumnEngine> PrimaryIndex;
    std::shared_ptr<NOlap::IStoragesManager> StoragesManager;
    NOlap::NDataAccessorControl::TDataAccessorsManagerContainer DataAccessorsManager;
    std::unique_ptr<TTableLoadTimeCounters> LoadTimeCounters;
    YDB_READONLY_DEF(NBackgroundTasks::TControlInterfaceContainer<NOlap::TSchemaObjectsCache>, SchemaObjectsCache);
    std::shared_ptr<TPortionIndexStats> PortionsStats;
    ui64 TabletId = 0;
    bool GenerateInternalPathId;
    std::optional<TUnifiedPathId> TabletPathId;
    TInternalPathId MaxInternalPathId;

    friend class TTxInit;

public:   //IPathIdTranslator
    virtual std::optional<NColumnShard::TSchemeShardLocalPathId> ResolveSchemeShardLocalPathIdOptional(
        const TInternalPathId internalPathId) const override;
    virtual std::optional<TInternalPathId> ResolveInternalPathIdOptional(
        const NColumnShard::TSchemeShardLocalPathId schemeShardLocalPathId, const bool withTabletPathId) const override;

public:
    TTablesManager(const std::shared_ptr<NOlap::IStoragesManager>& storagesManager,
        const std::shared_ptr<NOlap::NDataAccessorControl::IDataAccessorsManager>& dataAccessorsManager,
        const std::shared_ptr<TPortionIndexStats>& portionsStats, const ui64 tabletId);

    TConclusion<std::shared_ptr<NOlap::ITableMetadataAccessor>> BuildTableMetadataAccessor(
        const TString& tablePath, const TSchemeShardLocalPathId externalPathId);
    TConclusion<std::shared_ptr<NOlap::ITableMetadataAccessor>> BuildTableMetadataAccessor(
        const TString& tablePath, const TInternalPathId internalPathId);

    class TSchemaAddress {
    private:
        YDB_READONLY(ui32, PresetId, 0);
        YDB_READONLY(NOlap::TSnapshot, Snapshot, NOlap::TSnapshot::Zero());

    public:
        TString DebugString() const {
            return TStringBuilder() << PresetId << "," << Snapshot.DebugString();
        }

        TSchemaAddress(const ui32 presetId, const NOlap::TSnapshot& snapshot)
            : PresetId(presetId)
            , Snapshot(snapshot) {
        }

        explicit operator size_t() const {
            return CombineHashes<size_t>((size_t)PresetId, (size_t)Snapshot);
        }

        bool operator==(const TSchemaAddress& item) const {
            return std::tie(PresetId, Snapshot) == std::tie(item.PresetId, item.Snapshot);
        }

        bool operator<(const TSchemaAddress& item) const {
            AFL_VERIFY(PresetId == item.PresetId);
            return Snapshot < item.Snapshot;
        }
    };

    class TSchemasChain {
    private:
        YDB_READONLY_DEF(std::set<TSchemaAddress>, ToRemove);
        TSchemaAddress Finish;

    public:
        const TSchemaAddress& GetFinish() const {
            return Finish;
        }

        void FillAddressesTo(std::set<TSchemaAddress>& addresses) const {
            addresses.insert(ToRemove.begin(), ToRemove.end());
            addresses.emplace(Finish);
        }

        TSchemasChain(const std::set<TSchemaAddress>& toRemove, const TSchemaAddress& finish)
            : ToRemove(toRemove)
            , Finish(finish) {
            AFL_VERIFY(toRemove.size());
            AFL_VERIFY(*ToRemove.rbegin() < Finish);
        }
    };

    std::vector<TSchemasChain> ExtractSchemasToClean() const;

    std::optional<TUnifiedPathId> GetTabletPathIdOptional() const {
      return TabletPathId;
    }

    TUnifiedPathId GetTabletPathIdVerified() const {
      AFL_VERIFY(TabletPathId.has_value());
      AFL_VERIFY(TabletPathId->InternalPathId.IsValid());
      AFL_VERIFY(TabletPathId->SchemeShardLocalPathId.IsValid());
      return *TabletPathId;
    }

    const std::unique_ptr<TTableLoadTimeCounters>& GetLoadTimeCounters() const {
        return LoadTimeCounters;
    }

    bool TryFinalizeDropPathOnExecute(NTable::TDatabase& dbTable, const TInternalPathId pathId) const;
    bool TryFinalizeDropPathOnComplete(const TInternalPathId pathId);

    THashMap<TInternalPathId, NOlap::TTiering> GetTtl(const NOlap::TSnapshot& snapshot = NOlap::TSnapshot::Max()) const {
        THashMap<TInternalPathId, NOlap::TTiering> ttl;
        for (const auto& [pathId, info] : Tables) {
            if (info.IsDropped(snapshot)) {
                continue;
            }
            if (auto tableTtl = Ttl.GetTableTtl(pathId, snapshot)) {
                ttl.emplace(pathId, std::move(*tableTtl));
            }
        }
        return ttl;
    }

    std::optional<NOlap::TTiering> GetTableTtl(const TInternalPathId pathId, const NOlap::TSnapshot& snapshot = NOlap::TSnapshot::Max()) const {
        return Ttl.GetTableTtl(pathId, snapshot);
    }

    const std::map<NOlap::TSnapshot, THashSet<TInternalPathId>>& GetPathsToDrop() const {
        return PathsToDrop;
    }

    THashSet<TInternalPathId> GetPathsToDrop(const NOlap::TSnapshot& minReadSnapshot) const {
        THashSet<TInternalPathId> result;
        for (auto&& i : PathsToDrop) {
            if (minReadSnapshot < i.first) {
                break;
            }
            result.insert(i.second.begin(), i.second.end());
        }
        return result;
    }

    const THashMap<TInternalPathId, TTableInfo>& GetTables() const {
        return Tables;
    }

    const THashSet<ui32>& GetSchemaPresets() const {
        return SchemaPresetsIds;
    }

    bool HasPrimaryIndex() const {
        return !!PrimaryIndex;
    }

    void MoveTablePropose(const TSchemeShardLocalPathId schemeShardLocalPathId);
    void MoveTableProgress(
        NIceDb::TNiceDb& db, const TSchemeShardLocalPathId oldSchemeShardLocalPathId, const TSchemeShardLocalPathId newSchemeShardLocalPathId);

    NOlap::IColumnEngine& MutablePrimaryIndex() const {
        Y_ABORT_UNLESS(!!PrimaryIndex);
        return *PrimaryIndex;
    }

    const NOlap::TIndexInfo& GetIndexInfo(const NOlap::TSnapshot& version) const {
        Y_ABORT_UNLESS(!!PrimaryIndex);
        return PrimaryIndex->GetVersionedIndex().GetSchemaVerified(version)->GetIndexInfo();
    }

    const std::unique_ptr<NOlap::IColumnEngine>& GetPrimaryIndex() const {
        return PrimaryIndex;
    }

    const NOlap::IColumnEngine& GetPrimaryIndexSafe() const {
        Y_ABORT_UNLESS(!!PrimaryIndex);
        return *PrimaryIndex;
    }

    template <class TIndex>
    TIndex& MutablePrimaryIndexAsVerified() const {
        AFL_VERIFY(!!PrimaryIndex);
        auto result = dynamic_cast<TIndex*>(PrimaryIndex.get());
        AFL_VERIFY(result);
        return *result;
    }

    template <class TIndex>
    const TIndex& GetPrimaryIndexAsVerified() const {
        AFL_VERIFY(!!PrimaryIndex);
        auto result = dynamic_cast<const TIndex*>(PrimaryIndex.get());
        AFL_VERIFY(result);
        return *result;
    }

    template <class TIndex>
    const TIndex* GetPrimaryIndexAsOptional() const {
        if (!PrimaryIndex) {
            return nullptr;
        }
        auto result = dynamic_cast<const TIndex*>(PrimaryIndex.get());
        AFL_VERIFY(result);
        return result;
    }

    template <class TIndex>
    TIndex* MutablePrimaryIndexAsOptional() const {
        if (!PrimaryIndex) {
            return nullptr;
        }
        auto result = dynamic_cast<TIndex*>(PrimaryIndex.get());
        AFL_VERIFY(result);
        return result;
    }

    bool InitFromDB(NIceDb::TNiceDb& db);
    void Init(NIceDb::TNiceDb& db, const TSchemeShardLocalPathId tabletSchemeShardLocalPathId, const TTabletStorageInfo* info);
    bool InitFromDB(NIceDb::TNiceDb& db, const TTabletStorageInfo* info);

    const TTableInfo& GetTable(const TInternalPathId pathId) const;
    ui64 GetMemoryUsage() const;
    TInternalPathId GetOrCreateInternalPathId(const TSchemeShardLocalPathId schemShardLocalPathId);
    THashMap<TSchemeShardLocalPathId, TInternalPathId> ResolveInternalPathIds(
        const TSchemeShardLocalPathId from, const TSchemeShardLocalPathId to) const;
    bool HasTable(const TInternalPathId pathId, const bool withDeleted = false,
        const std::optional<NOlap::TSnapshot> minReadSnapshot = std::nullopt) const;
    bool IsReadyForStartWrite(const TInternalPathId pathId, const bool withDeleted) const;
    bool IsReadyForFinishWrite(const TInternalPathId pathId, const NOlap::TSnapshot& minReadSnapshot) const;
    bool HasPreset(const ui32 presetId) const;

    void DropTable(const TInternalPathId pathId, const NOlap::TSnapshot& version, NIceDb::TNiceDb& db);
    void DropPreset(const ui32 presetId, const NOlap::TSnapshot& version, NIceDb::TNiceDb& db);

    void RegisterTable(TTableInfo&& table, NIceDb::TNiceDb& db);
    bool RegisterSchemaPreset(const TSchemaPreset& schemaPreset, NIceDb::TNiceDb& db);

    void AddSchemaVersion(
        const ui32 presetId, const NOlap::TSnapshot& version, const NKikimrSchemeOp::TColumnTableSchema& schema, NIceDb::TNiceDb& db);
    void AddTableVersion(const TInternalPathId pathId, const NOlap::TSnapshot& version,
        const NKikimrTxColumnShard::TTableVersionInfo& versionInfo, const std::optional<NKikimrSchemeOp::TColumnTableSchema>& schema,
        NIceDb::TNiceDb& db);
    bool FillMonitoringReport(NTabletFlatExecutor::TTransactionContext& txc, NJson::TJsonValue& json);

    [[nodiscard]] std::unique_ptr<NTabletFlatExecutor::ITransaction> CreateAddShardingInfoTx(TColumnShard& owner,
        const NColumnShard::TSchemeShardLocalPathId pathId, const ui64 versionId,
        const NSharding::TGranuleShardingLogicContainer& tabletShardingLogic) const;
};

}   // namespace NKikimr::NColumnShard
