#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/table/table.h>

#include <ydb/core/base/table_index.h>
#include <ydb/core/metering/metering.h>
#include <ydb/core/protos/schemeshard/operations.pb.h>
#include <ydb/core/testlib/actors/block_events.h>
#include <ydb/core/testlib/actors/wait_events.h>
#include <ydb/core/testlib/tablet_helpers.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/scheme_board/events.h>
#include <ydb/core/tx/schemeshard/schemeshard_billing_helpers.h>
#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>

using namespace NKikimr;
using namespace NSchemeShard;
using namespace NSchemeShardUT_Private;

Y_UNIT_TEST_SUITE(IndexBuildTest) {
    Y_UNIT_TEST(ShadowDataNotAllowedByDefault) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "IndexTable"
              Columns { Name: "index"   Type: "Uint32" }
              Columns { Name: "key"     Type: "Uint32" }
              KeyColumnNames: ["index", "key"]
              PartitionConfig {
                  CompactionPolicy {
                      KeepEraseMarkers: true
                  }
                  ShadowData: true
              }
        )", {NKikimrScheme::StatusInvalidParameter});
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "IndexTable"
              Columns { Name: "index"   Type: "Uint32" }
              Columns { Name: "key"     Type: "Uint32" }
              KeyColumnNames: ["index", "key"]
              PartitionConfig {
                  CompactionPolicy {
                      KeepEraseMarkers: true
                  }
                  ShadowData: false
              }
        )", {NKikimrScheme::StatusInvalidParameter});
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "IndexTable"
              Columns { Name: "index"   Type: "Uint32" }
              Columns { Name: "key"     Type: "Uint32" }
              KeyColumnNames: ["index", "key"]
              PartitionConfig {
                  CompactionPolicy {
                      KeepEraseMarkers: true
                  }
              }
        )");
        env.TestWaitNotification(runtime, txId);
        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
              Name: "IndexTable"
              PartitionConfig {
                  ShadowData: false
              }
        )", {NKikimrScheme::StatusInvalidParameter});
        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
              Name: "IndexTable"
              PartitionConfig {
                  ShadowData: true
              }
        )", {NKikimrScheme::StatusInvalidParameter});
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "CopyTable"
              CopyFromTable: "/MyRoot/IndexTable"
              PartitionConfig {
                  ShadowData: true
              }
        )", {NKikimrScheme::StatusInvalidParameter});
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "CopyTable"
              CopyFromTable: "/MyRoot/IndexTable"
              PartitionConfig {
                  ShadowData: false
              }
        )", {NKikimrScheme::StatusInvalidParameter});
    }

    Y_UNIT_TEST(ShadowDataEdgeCases) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        // Allow manipulating shadow data using normal schemeshard operations
        runtime.GetAppData().AllowShadowDataInSchemeShardForTests = true;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "IndexTable"
              Columns { Name: "index"   Type: "Uint32" }
              Columns { Name: "key"     Type: "Uint32" }
              KeyColumnNames: ["index", "key"]
              PartitionConfig {
                  CompactionPolicy {
                      KeepEraseMarkers: true
                  }
                  ShadowData: true
              }
        )");
        env.TestWaitNotification(runtime, txId);

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "CopyTable"
              CopyFromTable: "/MyRoot/IndexTable"
        )", {NKikimrScheme::StatusPreconditionFailed});

        // This is basically a no-op alter, not filtered at the moment
        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
              Name: "IndexTable"
              PartitionConfig {
                  ShadowData: true
              }
        )");
        env.TestWaitNotification(runtime, txId);

        // This removes shadow data, should be allowed
        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
              Name: "IndexTable"
              PartitionConfig {
                  ShadowData: false
              }
        )");
        env.TestWaitNotification(runtime, txId);

        // Shadow data cannot be re-enabled
        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
              Name: "IndexTable"
              PartitionConfig {
                  ShadowData: true
              }
        )", {NKikimrScheme::StatusInvalidParameter});

        // This is basically a no-op alter, not filtered at the moment
        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
              Name: "IndexTable"
              PartitionConfig {
                  ShadowData: false
              }
        )");
        env.TestWaitNotification(runtime, txId);

        // Copy should work after shadow data is disabled
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "CopyTable1"
              CopyFromTable: "/MyRoot/IndexTable"
        )");
        env.TestWaitNotification(runtime, txId);

        // Should we prohibit creation of shadow data in a copy?
        // Technically it's safe, even if not backwards compatible.
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "CopyTable2"
              CopyFromTable: "/MyRoot/IndexTable"
              PartitionConfig {
                  ShadowData: true
              }
        )");
        env.TestWaitNotification(runtime, txId);

        // This should remove shadow data correctly
        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
              Name: "CopyTable2"
              PartitionConfig {
                  ShadowData: false
              }
        )");
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(Metering_Documentation_Formula) {
        for (ui64 dataSizeMB : {1500, 500, 100, 0}) {
            const ui64 rowCount = 500'000;

            TMeteringStats stats;
            stats.SetReadRows(rowCount);
            stats.SetReadBytes(dataSizeMB * 1_MB);
            stats.SetUploadRows(rowCount);
            stats.SetUploadBytes(dataSizeMB * 1_MB);

            TString explain;
            const ui64 result = TRUCalculator::Calculate(stats, explain);

            // Note: in case of any cost changes, documentation is needed to be updated correspondingly.
            // https://yandex.cloud/ru/docs/ydb/pricing/ru-special#secondary-index
            UNIT_ASSERT_VALUES_EQUAL_C(result, Max<ui64>(dataSizeMB * 640, dataSizeMB * 128 + rowCount * 0.5), explain);
        }
    }

    Y_UNIT_TEST(BaseCase) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        ui64 tenantSchemeShard = 0;
        TestCreateServerLessDb(runtime, env, txId, tenantSchemeShard);

        // Just create main table
        TestCreateTable(runtime, tenantSchemeShard, ++txId, "/MyRoot/ServerLessDB", R"(
              Name: "Table"
              Columns { Name: "key"     Type: "Uint32" }
              Columns { Name: "index"   Type: "Uint32" }
              Columns { Name: "value"   Type: "Utf8"   }
              KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId, tenantSchemeShard);

        auto fnWriteRow = [&] (ui64 tabletId, ui32 key, ui32 index, TString value, const char* table) {
            // What is __user__?
            TString writeQuery = Sprintf(R"(
                (
                    (let key   '( '('key   (Uint32 '%u ) ) ) )
                    (let row   '( '('index (Uint32 '%u ) )  '('value (Utf8 '%s) ) ) )
                    (return (AsList (UpdateRow '__user__%s key row) ))
                )
            )", key, index, value.c_str(), table);
            NKikimrMiniKQL::TResult result;
            TString err;
            NKikimrProto::EReplyStatus status = LocalMiniKQL(runtime, tabletId, writeQuery, result, err);
            UNIT_ASSERT_VALUES_EQUAL(err, "");
            UNIT_ASSERT_VALUES_EQUAL(status, NKikimrProto::EReplyStatus::OK);;
        };
        for (ui32 delta = 0; delta < 101; ++delta) {
            // What is TTestTxConfig::FakeHiveTablets + 6?
            fnWriteRow(TTestTxConfig::FakeHiveTablets + 6, 1 + delta, 1000 + delta, "aaaa", "Table");
        }

        TestDescribeResult(DescribePath(runtime, tenantSchemeShard, "/MyRoot/ServerLessDB/Table"),
                           {NLs::PathExist,
                            NLs::IndexesCount(0),
                            NLs::PathVersionEqual(3)});

        TStringBuilder meteringMessages;
        auto grabMeteringMessage = [&meteringMessages](TAutoPtr<IEventHandle>& ev) -> auto {
            if (ev->Type == NMetering::TEvMetering::TEvWriteMeteringJson::EventType) {
                auto *msg = ev->Get<NMetering::TEvMetering::TEvWriteMeteringJson>();
                Cerr << "grabMeteringMessage has happened" << Endl;
                meteringMessages << msg->MeteringJson;
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };

        runtime.SetObserverFunc(grabMeteringMessage);

        TestBuildIndex(runtime, ++txId, tenantSchemeShard, "/MyRoot/ServerLessDB", "/MyRoot/ServerLessDB/Table", "index1", {"index"});
        ui64 buildIndexId = txId;

        auto listing = TestListBuildIndex(runtime, tenantSchemeShard, "/MyRoot/ServerLessDB");
        UNIT_ASSERT_VALUES_EQUAL(listing.EntriesSize(), 1);

        env.TestWaitNotification(runtime, txId, tenantSchemeShard);

        auto descr = TestGetBuildIndex(runtime, tenantSchemeShard, "/MyRoot/ServerLessDB", txId);
        UNIT_ASSERT_VALUES_EQUAL(descr.GetIndexBuild().GetState(), Ydb::Table::IndexBuildState::STATE_DONE);

        // Note: in case of any cost changes, documentation is needed to be updated correspondingly.
        // https://yandex.cloud/ru/docs/ydb/pricing/ru-special#secondary-index
        const TString expectedMetering = R"({"usage":{"start":0,"quantity":179,"finish":0,"unit":"request_unit","type":"delta"},"tags":{},"id":"106-72075186233409549-2-0-0-0-0-101-101-1818-1818","cloud_id":"CLOUD_ID_VAL","source_wt":0,"source_id":"sless-docapi-ydb-ss","resource_id":"DATABASE_ID_VAL","schema":"ydb.serverless.requests.v1","folder_id":"FOLDER_ID_VAL","version":"1.0.0"})";
        MeteringDataEqual(meteringMessages, expectedMetering);

        TestDescribeResult(DescribePath(runtime, tenantSchemeShard, "/MyRoot/ServerLessDB/Table"),
                           {NLs::PathExist,
                            NLs::IndexesCount(1),
                            NLs::PathVersionEqual(6)});

        TestDescribeResult(DescribePath(runtime, tenantSchemeShard, "/MyRoot/ServerLessDB/Table/index1", true, true, true),
                           {NLs::PathExist,
                            NLs::IndexState(NKikimrSchemeOp::EIndexState::EIndexStateReady)});

        TestForgetBuildIndex(runtime, ++txId, tenantSchemeShard, "/MyRoot/ServerLessDB", buildIndexId);
        listing = TestListBuildIndex(runtime, tenantSchemeShard, "/MyRoot/ServerLessDB");
        UNIT_ASSERT_VALUES_EQUAL(listing.EntriesSize(), 0);

        TestDropTableIndex(runtime, tenantSchemeShard, ++txId, "/MyRoot/ServerLessDB", R"(
            TableName: "Table"
            IndexName: "index1"
        )");
        env.TestWaitNotification(runtime, txId, tenantSchemeShard);

        RebootTablet(runtime, tenantSchemeShard, runtime.AllocateEdgeActor());

        TestDescribeResult(DescribePath(runtime, tenantSchemeShard, "/MyRoot/ServerLessDB/Table"),
                           {NLs::PathExist,
                            NLs::IndexesCount(0),
                            NLs::PathVersionEqual(8)});

        // Test that index build succeeds on recreated columns

        TestAlterTable(runtime, tenantSchemeShard, ++txId, "/MyRoot/ServerLessDB", R"(
              Name: "Table"
              DropColumns { Name: "index" }
        )");
        env.TestWaitNotification(runtime, txId, tenantSchemeShard);

        TestAlterTable(runtime, tenantSchemeShard, ++txId, "/MyRoot/ServerLessDB", R"(
              Name: "Table"
              Columns { Name: "index"   Type: "Uint32" }
        )");
        env.TestWaitNotification(runtime, txId, tenantSchemeShard);

        TestBuildIndex(runtime, ++txId, tenantSchemeShard, "/MyRoot/ServerLessDB", "/MyRoot/ServerLessDB/Table", "index2", {"index"});
        env.TestWaitNotification(runtime, txId, tenantSchemeShard);

        // CommonDB
        TestCreateExtSubDomain(runtime, ++txId,  "/MyRoot",
                               "Name: \"CommonDB\"");
        env.TestWaitNotification(runtime, txId);

        TestAlterExtSubDomain(runtime, ++txId,  "/MyRoot",
                              "StoragePools { "
                              "  Name: \"pool-3\" "
                              "  Kind: \"pool-kind-3\" "
                              "} "
                              "PlanResolution: 50 "
                              "Coordinators: 1 "
                              "Mediators: 1 "
                              "TimeCastBucketsPerMediator: 2 "
                              "ExternalSchemeShard: true "
                              "Name: \"CommonDB\"");
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/CommonDB"),
                           {NLs::PathExist,
                            NLs::IsExternalSubDomain("CommonDB"),
                            NLs::ExtractTenantSchemeshard(&tenantSchemeShard)});

        TestCreateTable(runtime, tenantSchemeShard, ++txId, "/MyRoot/CommonDB", R"(
              Name: "Table"
              Columns { Name: "key"     Type: "Uint32" }
              Columns { Name: "index"   Type: "Uint32" }
              Columns { Name: "value"   Type: "Utf8"   }
              KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId, tenantSchemeShard);

        for (ui32 delta = 0; delta < 101; ++delta) {
            // What is TTestTxConfig::FakeHiveTablets + 12?
            fnWriteRow(TTestTxConfig::FakeHiveTablets + 12, 1 + delta, 1000 + delta, "aaaa", "Table");
        }

        TVector<TString> billRecords;
        TVector<bool> shadowData;
        TVector<bool> keepEraseMarkers;
        runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->Type == NMetering::TEvMetering::TEvWriteMeteringJson::EventType) {
                auto* msg = ev->Get<NMetering::TEvMetering::TEvWriteMeteringJson>();
                billRecords.push_back(msg->MeteringJson);
            } else if (ev->Type == TSchemeBoardEvents::TEvNotifyUpdate::EventType) {
                auto* msg = ev->Get<TSchemeBoardEvents::TEvNotifyUpdate>();
                if (msg->Path.EndsWith(NTableIndex::ImplTable)) {
                    auto& desc = msg->DescribeSchemeResult.GetPathDescription().GetTable().GetPartitionConfig();
                    shadowData.push_back(desc.GetShadowData());
                    keepEraseMarkers.push_back(desc.GetCompactionPolicy().GetKeepEraseMarkers());
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TestBuildIndex(runtime, ++txId, tenantSchemeShard, "/MyRoot/CommonDB", "/MyRoot/CommonDB/Table", "index1", {"index"});
        buildIndexId = txId;

        listing = TestListBuildIndex(runtime, tenantSchemeShard, "/MyRoot/CommonDB");
        UNIT_ASSERT_VALUES_EQUAL(listing.EntriesSize(), 1);

        env.TestWaitNotification(runtime, txId, tenantSchemeShard);

        descr = TestGetBuildIndex(runtime, tenantSchemeShard, "/MyRoot/CommonDB", txId);
        UNIT_ASSERT_VALUES_EQUAL(descr.GetIndexBuild().GetState(), Ydb::Table::IndexBuildState::STATE_DONE);

        UNIT_ASSERT_VALUES_EQUAL(billRecords.size(), 0);

        UNIT_ASSERT_VALUES_EQUAL(shadowData.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(shadowData[0], true);
        UNIT_ASSERT_VALUES_EQUAL(shadowData[1], false);

        UNIT_ASSERT_VALUES_EQUAL(keepEraseMarkers.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(keepEraseMarkers[0], true);
        UNIT_ASSERT_VALUES_EQUAL(keepEraseMarkers[1], false);
    }

    Y_UNIT_TEST(CancellationNotEnoughRetries) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        SetSplitMergePartCountLimit(&runtime, -1);

        // Just create main table
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "Table"
              Columns { Name: "key"     Type: "Uint32" }
              Columns { Name: "value"   Type: "Utf8"   }
              Columns { Name: "index"   Type: "Uint32" }
              KeyColumnNames: ["key", "value"]
        )");
        env.TestWaitNotification(runtime, txId);

        auto fnWriteRow = [&](ui64 tabletId, ui32 key, ui32 index, const TString& value, const char *table) {
            TString writeQuery = Sprintf(R"(
                (
                    (let key   '( '('key   (Uint32 '%u ) ) '('value (Utf8 '%s) ) ) )
                    (let row   '( '('index (Uint32 '%u ) ) ) )
                    (return (AsList (UpdateRow '__user__%s key row) ))
                )
            )", key, value.c_str(), index, table);
            NKikimrMiniKQL::TResult result;
            TString err;
            NKikimrProto::EReplyStatus status = LocalMiniKQL(runtime, tabletId, writeQuery, result, err);
            UNIT_ASSERT_VALUES_EQUAL(err, "");
            UNIT_ASSERT_VALUES_EQUAL(status, NKikimrProto::EReplyStatus::OK);;
        };
        TVector<char> longStrData(100000, 'a');
        TString longString(longStrData.begin(), longStrData.end());
        for (ui32 delta = 0; delta < 1000; ++delta) {
            fnWriteRow(TTestTxConfig::FakeHiveTablets, 1 + delta, 1000 + delta, longString, "Table");
        }

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"),
                           {NLs::PathExist,
                            NLs::IndexesCount(0),
                            NLs::PathVersionEqual(3)});

        // Force stats reporting without delays
        NDataShard::gDbStatsReportInterval = TDuration::Seconds(0);
        NDataShard::gDbStatsDataSizeResolution = 80000;

        auto upgradeEvent = [&](TAutoPtr<IEventHandle>& ev) -> auto {
            if (ev->Type == TEvSchemeShard::EvModifySchemeTransaction) {
                auto *msg = ev->Get<TEvSchemeShard::TEvModifySchemeTransaction>();
                if (msg->Record.GetTransaction(0).GetOperationType() == NKikimrSchemeOp::EOperationType::ESchemeOpCreateIndexBuild) {
                    auto& tx = *msg->Record.MutableTransaction(0);
                    auto& config = *tx.MutableInitiateIndexBuild();
                    NKikimrSchemeOp::TIndexCreationConfig& indexConfig = *config.MutableIndex();
                    NKikimrSchemeOp::TTableDescription& indexTableDescr = indexConfig.MutableIndexImplTableDescriptions()->at(0);

                    indexTableDescr.MutablePartitionConfig()->MutablePartitioningPolicy()->SetSizeToSplit(10);
                    indexTableDescr.MutablePartitionConfig()->MutablePartitioningPolicy()->SetMaxPartitionsCount(10);

                    Cerr << "upgradeEvent has happened" << Endl;
                }
            }

            return TTestActorRuntime::EEventAction::PROCESS;
        };

        runtime.SetObserverFunc(upgradeEvent);

        {
            NKikimrIndexBuilder::TIndexBuildSettings settings;
            settings.set_source_path("/MyRoot/Table");
            settings.MutableScanSettings()->SetMaxBatchRows(0); // row by row
            settings.MutableScanSettings()->SetMaxBatchBytes(1<<10);
            settings.MutableScanSettings()->SetMaxBatchRetries(0);
            settings.set_max_shards_in_flight(1);

            Ydb::Table::TableIndex& index = *settings.mutable_index();
            index.set_name("index1");
            index.add_index_columns("index");
            *index.mutable_global_index() = Ydb::Table::GlobalIndex();

            auto request = new TEvIndexBuilder::TEvCreateRequest(++txId, "/MyRoot", std::move(settings));
            auto sender = runtime.AllocateEdgeActor();

            ForwardToTablet(runtime, TTestTxConfig::SchemeShard, sender, request);

            TAutoPtr<IEventHandle> handle;
            TEvIndexBuilder::TEvCreateResponse* event = runtime.GrabEdgeEvent<TEvIndexBuilder::TEvCreateResponse>(handle);
            UNIT_ASSERT(event);

            Cerr << "BUILDINDEX RESPONSE CREATE: " << event->ToString() << Endl;
            UNIT_ASSERT_EQUAL_C(event->Record.GetStatus(), Ydb::StatusIds::SUCCESS,
                                "status mismatch"
                                        << " got " << Ydb::StatusIds::StatusCode_Name(event->Record.GetStatus())
                                        << " expected "  << Ydb::StatusIds::StatusCode_Name(Ydb::StatusIds::SUCCESS));
        }
        ui64 buildIndexId = txId;

        auto listing = TestListBuildIndex(runtime, TTestTxConfig::SchemeShard, "/MyRoot");
        UNIT_ASSERT_VALUES_EQUAL(listing.EntriesSize(), 1);

        env.TestWaitNotification(runtime, txId);

        auto descr = TestGetBuildIndex(runtime, TTestTxConfig::SchemeShard, "/MyRoot", txId);
        UNIT_ASSERT_EQUAL(descr.GetIndexBuild().GetState(), Ydb::Table::IndexBuildState::STATE_REJECTED);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"),
                           {NLs::PathExist,
                            NLs::IndexesCount(0),
                            NLs::PathVersionEqual(6)});

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table/index1", true, true, true),
                           {NLs::PathNotExist});

        TestForgetBuildIndex(runtime, ++txId, TTestTxConfig::SchemeShard, "/MyRoot", buildIndexId);
        listing = TestListBuildIndex(runtime, TTestTxConfig::SchemeShard, "/MyRoot");
        UNIT_ASSERT_VALUES_EQUAL(listing.EntriesSize(), 0);
    }

    Y_UNIT_TEST(CancellationNoTable) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "index1", {"index"}, Ydb::StatusIds::BAD_REQUEST);
        env.TestWaitNotification(runtime, txId);

        NKikimrIndexBuilder::TEvListResponse listing = TestListBuildIndex(runtime, TTestTxConfig::SchemeShard, "/MyRoot");
        UNIT_ASSERT_VALUES_EQUAL(listing.EntriesSize(), 0);
    }

    Y_UNIT_TEST(WithFollowers) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "WithFollowers"
            Columns { Name: "key"   Type: "Uint64" }
            Columns { Name: "value0" Type: "Utf8" }
            Columns { Name: "value1" Type: "Utf8" }
            Columns { Name: "valueFloat" Type: "Float" }
            KeyColumnNames: ["key"]
            PartitionConfig {
              FollowerGroups {
                FollowerCount: 1
              }
            }
        )");
        env.TestWaitNotification(runtime, txId);

        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/WithFollowers", "UserDefinedIndexByValue0", {"value0"});
        env.TestWaitNotification(runtime, txId);

        ui64 buildId = txId;

        auto descr = TestGetBuildIndex(runtime, TTestTxConfig::SchemeShard, "/MyRoot", buildId);
        UNIT_ASSERT_VALUES_EQUAL(descr.GetIndexBuild().GetState(), Ydb::Table::IndexBuildState::STATE_DONE);
        UNIT_ASSERT(descr.GetIndexBuild().HasStartTime());
        UNIT_ASSERT(descr.GetIndexBuild().HasEndTime());

        TestDescribeResult(DescribePath(runtime, "/MyRoot/WithFollowers"),
                           {NLs::PathExist,
                            NLs::IndexesCount(1),
                            NLs::PathVersionEqual(6)});

        TestDescribeResult(DescribePath(runtime, "/MyRoot/WithFollowers/UserDefinedIndexByValue0", true, true, true),
                           {NLs::PathExist,
                            NLs::IndexState(NKikimrSchemeOp::EIndexState::EIndexStateReady)});

        TestForgetBuildIndex(runtime, ++txId, TTestTxConfig::SchemeShard, "/MyRoot", buildId);
        auto listing = TestListBuildIndex(runtime, TTestTxConfig::SchemeShard, "/MyRoot");
        UNIT_ASSERT_VALUES_EQUAL(listing.EntriesSize(), 0);

        TestDropTableIndex(runtime, ++txId, "/MyRoot", R"(
            TableName: "WithFollowers"
            IndexName: "UserDefinedIndexByValue0"
        )");
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/WithFollowers"),
                           {NLs::PathExist,
                            NLs::IndexesCount(0),
                            NLs::PathVersionEqual(8)});
    }

    Y_UNIT_TEST(RejectsCreate) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableProtoSourceIdInfo(true));
        ui64 txId = 100;

        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/NotExist", "index1", {"index"}, Ydb::StatusIds::BAD_REQUEST);
        env.TestWaitNotification(runtime, txId);

        TestMkDir(runtime, TTestTxConfig::SchemeShard, ++txId, "/MyRoot", "DIR");
        env.TestWaitNotification(runtime, txId);

        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/DIR", "index1", {"index"}, Ydb::StatusIds::BAD_REQUEST);
        env.TestWaitNotification(runtime, txId);

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
              Name: "Table"
              Columns { Name: "key"   Type: "Uint64" }
              Columns { Name: "value0" Type: "Utf8" }
              Columns { Name: "value1" Type: "Utf8" }
              Columns { Name: "valueFloat" Type: "Float" }
              KeyColumnNames: ["key"]
            }
            IndexDescription {
              Name: "UserDefinedIndexByValue0"
              KeyColumnNames: ["value0"]
            }
            IndexDescription {
              Name: "UserDefinedIndexByValue1"
              KeyColumnNames: ["value1"]
            }
        )");
        env.TestWaitNotification(runtime, txId);

        // should not affect index limits
        TestCreateCdcStream(runtime, ++txId, "/MyRoot", R"(
            TableName: "Table"
            StreamDescription {
              Name: "Stream"
              Mode: ECdcStreamModeKeysOnly
              Format: ECdcStreamFormatProto
            }
        )");
        env.TestWaitNotification(runtime, txId);

        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "UserDefinedIndexByValue0", {"value0"}, Ydb::StatusIds::BAD_REQUEST);
        env.TestWaitNotification(runtime, txId);

        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "UserDefinedIndexByValue0", {"value1"}, Ydb::StatusIds::BAD_REQUEST);
        env.TestWaitNotification(runtime, txId);

        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "nameOK", {"NotExist"}, Ydb::StatusIds::BAD_REQUEST);
        env.TestWaitNotification(runtime, txId);

        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "nameOK", {"valueFloat"}, Ydb::StatusIds::BAD_REQUEST);
        env.TestWaitNotification(runtime, txId);

        TSchemeLimits lowLimits;

        lowLimits.ExtraPathSymbolsAllowed = "_-.";
        lowLimits.MaxTableIndices = 2;
        SetSchemeshardSchemaLimits(runtime, lowLimits);
        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "!name!", {"value0"}, Ydb::StatusIds::BAD_REQUEST);
        env.TestWaitNotification(runtime, txId);

        lowLimits.MaxTableIndices = 2;
        SetSchemeshardSchemaLimits(runtime, lowLimits);
        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "nameOK", {"value0", "value1"}, Ydb::StatusIds::PRECONDITION_FAILED);
        env.TestWaitNotification(runtime, txId);

        lowLimits.MaxTableIndices = 3;
        lowLimits.MaxChildrenInDir = 3;
        SetSchemeshardSchemaLimits(runtime, lowLimits);
        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "nameOK", {"value0", "value1"}, Ydb::StatusIds::PRECONDITION_FAILED);
        env.TestWaitNotification(runtime, txId);

        lowLimits.MaxTableIndices = 3;
        lowLimits.MaxChildrenInDir = 4;
        SetSchemeshardSchemaLimits(runtime, lowLimits);
        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "nameOK", {"value0", "value1"}, Ydb::StatusIds::SUCCESS);
        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "nameOK", {"value0", "value1"}, Ydb::StatusIds::OVERLOADED);
        env.TestWaitNotification(runtime, {txId, txId - 1});
    }

    Y_UNIT_TEST(CheckLimitWithDroppedIndex) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TSchemeLimits lowLimits;
        lowLimits.MaxTableIndices = 1;
        SetSchemeshardSchemaLimits(runtime, lowLimits);

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key"   Type: "Uint64" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestBuildIndex(runtime, ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "Index1", {"value"}, Ydb::StatusIds::SUCCESS);
        env.TestWaitNotification(runtime, txId);

        TestDropTableIndex(runtime, ++txId, "/MyRoot", R"(
            TableName: "Table"
            IndexName: "Index1"
        )");
        env.TestWaitNotification(runtime, txId);

        TestBuildIndex(runtime, ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "Index2", {"value"}, Ydb::StatusIds::SUCCESS);
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(Lock) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        // Just create main table
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "Table"
              Columns { Name: "key"     Type: "Uint32" }
              Columns { Name: "index"   Type: "Uint32" }
              Columns { Name: "value"   Type: "Utf8"   }
              KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "nameOK", {"index"});
        ui64 buildIndexId = txId;

        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
                                Name: "Table"
                                DropColumns { Name: "index" }
                           )",
                       {NKikimrScheme::StatusMultipleModifications});
        env.TestWaitNotification(runtime, txId);

        TestDropTableIndex(runtime, ++txId, "/MyRoot", R"(
            TableName: "Table"
            IndexName: "nameOK"
        )", {NKikimrScheme::StatusMultipleModifications});
        env.TestWaitNotification(runtime, txId);

        TestDropTable(runtime, ++txId, "/MyRoot", "Table",
                      {NKikimrScheme::StatusMultipleModifications});
        env.TestWaitNotification(runtime, txId);


        env.TestWaitNotification(runtime, buildIndexId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"),
                           {NLs::PathExist,
                            NLs::IndexesCount(1)});

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table/nameOK", true, true, true),
                           {NLs::PathExist,
                            NLs::IndexState(NKikimrSchemeOp::EIndexState::EIndexStateReady)});

        NKikimrIndexBuilder::TEvGetResponse descr = TestGetBuildIndex(runtime, TTestTxConfig::SchemeShard, "/MyRoot", buildIndexId);
        UNIT_ASSERT_VALUES_EQUAL(descr.GetIndexBuild().GetState(), Ydb::Table::IndexBuildState::STATE_DONE);

//        KIKIMR-9945
        TestAlterTable(runtime, ++txId, "/MyRoot", R"(
                                Name: "Table"
                                DropColumns { Name: "index" }
                           )",
                       {NKikimrScheme::StatusPreconditionFailed});
        env.TestWaitNotification(runtime, txId);

        TestDropTable(runtime, ++txId, "/MyRoot", "Table");
        env.TestWaitNotification(runtime, txId);

    }

    Y_UNIT_TEST(MergeIndexTableShardsOnlyWhenReady) {
        TTestBasicRuntime runtime;

        TTestEnvOptions opts;
        opts.EnableBackgroundCompaction(false);
        opts.DisableStatsBatching(true);
        TTestEnv env(runtime, opts);

        NDataShard::gDbStatsReportInterval = TDuration::Seconds(0);

        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "value" Type: "Uint64" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        Ydb::Table::GlobalIndexSettings settings;
        UNIT_ASSERT(google::protobuf::TextFormat::ParseFromString(R"(
            partition_at_keys {
                split_points {
                    type { tuple_type { elements { optional_type { item { type_id: UINT64 } } } } }
                    value { items { uint64_value: 10 } }
                }
                split_points {
                    type { tuple_type { elements { optional_type { item { type_id: UINT64 } } } } }
                    value { items { uint64_value: 20 } }
                }
                split_points {
                    type { tuple_type { elements { optional_type { item { type_id: UINT64 } } } } }
                    value { items { uint64_value: 30 } }
                }
            }
        )", &settings));

        TBlockEvents<TEvSchemeShard::TEvModifySchemeTransaction> indexApplicationBlocker(runtime, [](const auto& ev) {
            const auto& modifyScheme = ev->Get()->Record.GetTransaction(0);
            return modifyScheme.GetOperationType() == NKikimrSchemeOp::ESchemeOpApplyIndexBuild;
        });

        ui64 indexInitializationTx = 0;
        TWaitForFirstEvent<TEvSchemeShard::TEvModifySchemeTransaction> indexInitializationWaiter(runtime,
            [&indexInitializationTx](const auto& ev){
                const auto& record = ev->Get()->Record;
                if (record.GetTransaction(0).GetOperationType() == NKikimrSchemeOp::ESchemeOpCreateIndexBuild) {
                    indexInitializationTx = record.GetTxId();
                    return true;
                }
                return false;
            }
        );

        const ui64 buildIndexTx = ++txId;
        TestBuildIndex(runtime,  buildIndexTx, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", TBuildIndexConfig{
            "ByValue", NKikimrSchemeOp::EIndexTypeGlobal, { "value" }, {},
            { NYdb::NTable::TGlobalIndexSettings::FromProto(settings) }
        });

        indexInitializationWaiter.Wait();
        UNIT_ASSERT_VALUES_UNEQUAL(indexInitializationTx, 0);
        env.TestWaitNotification(runtime, indexInitializationTx);

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/ByValue"), {
            NLs::PathExist,
            NLs::IndexState(NKikimrSchemeOp::EIndexStateWriteOnly)
        });

        TVector<ui64> indexShards;
        auto shardCollector = [&indexShards](const NKikimrScheme::TEvDescribeSchemeResult& record) {
            UNIT_ASSERT_VALUES_EQUAL(record.GetStatus(), NKikimrScheme::StatusSuccess);
            const auto& partitions = record.GetPathDescription().GetTablePartitions();
            indexShards.clear();
            indexShards.reserve(partitions.size());
            for (const auto& partition : partitions) {
                indexShards.emplace_back(partition.GetDatashardId());
            }
        };
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/ByValue/indexImplTable", true), {
            NLs::PathExist,
            NLs::PartitionCount(4),
            shardCollector
        });
        UNIT_ASSERT_VALUES_EQUAL(indexShards.size(), 4);

        {
            // make sure no shards are merged
            TBlockEvents<TEvSchemeShard::TEvModifySchemeTransaction> mergeBlocker(runtime, [](const auto& ev) {
                const auto& modifyScheme = ev->Get()->Record.GetTransaction(0);
                return modifyScheme.GetOperationType() == NKikimrSchemeOp::ESchemeOpSplitMergeTablePartitions;
            });

            {
                // wait for all index shards to send statistics
                THashSet<ui64> shardsWithStats;
                using TEvType = TEvDataShard::TEvPeriodicTableStats;
                auto statsObserver = runtime.AddObserver<TEvType>([&shardsWithStats](const TEvType::TPtr& ev) {
                    shardsWithStats.emplace(ev->Get()->Record.GetDatashardId());
                });

                runtime.WaitFor("all index shards to send statistics", [&]{
                    return AllOf(indexShards, [&shardsWithStats](ui64 indexShard) {
                        return shardsWithStats.contains(indexShard);
                    });
                });
            }

            // we expect to not have observed any attempts to merge
            UNIT_ASSERT(mergeBlocker.empty());

            // wait for 1 minute to ensure that no merges have been started by SchemeShard
            env.SimulateSleep(runtime, TDuration::Minutes(1));
            UNIT_ASSERT(mergeBlocker.empty());
        }

        // splits are allowed even if the index is not ready
        TestSplitTable(runtime, ++txId, "/MyRoot/Table/ByValue/indexImplTable", Sprintf(R"(
                    SourceTabletId: %lu
                    SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 5 } } } }
                )",
                indexShards.front()
            )
        );
        env.TestWaitNotification(runtime, txId);

        indexApplicationBlocker.Stop().Unblock();
        env.TestWaitNotification(runtime, buildIndexTx);

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/ByValue"), {
            NLs::IndexState(NKikimrSchemeOp::EIndexStateReady)
        });

        // wait until all index impl table shards are merged into one
        while (true) {
            TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/ByValue/indexImplTable", true), {
                shardCollector
            });
            if (indexShards.size() > 1) {
                // If a merge happens, old shards are deleted and replaced with a new one.
                // That is why we need to wait for * all * the shards to be deleted.
                env.TestWaitTabletDeletion(runtime, indexShards);
            } else {
                break;
            }
        }
    }

    Y_UNIT_TEST(IndexPartitioningIsPersisted) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table"
            Columns { Name: "key" Type: "Uint64" }
            Columns { Name: "value" Type: "Utf8" }
            KeyColumnNames: [ "key" ]
        )");
        env.TestWaitNotification(runtime, txId);

        Ydb::Table::GlobalIndexSettings settings;
        UNIT_ASSERT(google::protobuf::TextFormat::ParseFromString(R"(
            partition_at_keys {
                split_points {
                    type { tuple_type { elements { optional_type { item { type_id: UTF8 } } } } }
                    value { items { text_value: "alice" } }
                }
                split_points {
                    type { tuple_type { elements { optional_type { item { type_id: UTF8 } } } } }
                    value { items { text_value: "bob" } }
                }
            }
            partitioning_settings {
                min_partitions_count: 3
                max_partitions_count: 3
            }
        )", &settings));

        TBlockEvents<TEvSchemeShard::TEvModifySchemeTransaction> indexCreationBlocker(runtime, [](const auto& ev) {
            const auto& modifyScheme = ev->Get()->Record.GetTransaction(0);
            return modifyScheme.GetOperationType() == NKikimrSchemeOp::ESchemeOpCreateIndexBuild;
        });

        const ui64 buildIndexTx = ++txId;
        TestBuildIndex(runtime, buildIndexTx, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", TBuildIndexConfig{
            "Index", NKikimrSchemeOp::EIndexTypeGlobal, { "value" }, {},
            { NYdb::NTable::TGlobalIndexSettings::FromProto(settings) }
        });

        RebootTablet(runtime, TTestTxConfig::SchemeShard, runtime.AllocateEdgeActor());

        indexCreationBlocker.Stop().Unblock();
        env.TestWaitNotification(runtime, buildIndexTx);

        auto buildIndexOperation = TestGetBuildIndex(runtime, TTestTxConfig::SchemeShard, "/MyRoot", buildIndexTx);
        UNIT_ASSERT_VALUES_EQUAL_C(
            buildIndexOperation.GetIndexBuild().GetState(), Ydb::Table::IndexBuildState::STATE_DONE,
            buildIndexOperation.DebugString()
        );

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"), {
            NLs::IsTable,
            NLs::IndexesCount(1)
        });

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/Index"), {
            NLs::PathExist,
            NLs::IndexState(NKikimrSchemeOp::EIndexState::EIndexStateReady)
        });

        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/Index/indexImplTable", true, true), {
            NLs::IsTable,
            NLs::PartitionCount(3),
            NLs::MinPartitionsCountEqual(3),
            NLs::MaxPartitionsCountEqual(3),
            NLs::PartitionKeys({"alice", "bob", ""})
        });
    }

    Y_UNIT_TEST(DropIndex) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
              Name: "Table"
              Columns { Name: "key"   Type: "Uint64" }
              Columns { Name: "value0" Type: "Utf8" }
              Columns { Name: "value1" Type: "Utf8" }
              KeyColumnNames: ["key"]
            }
            IndexDescription {
              Name: "UserDefinedIndexByValue0"
              KeyColumnNames: ["value0"]
            }
            IndexDescription {
              Name: "UserDefinedIndexByValue1"
              KeyColumnNames: ["value1"]
            }
        )");
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"),
                           {NLs::Finished,
                            NLs::PathVersionEqual(3),
                            NLs::IndexesCount(2)});
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/UserDefinedIndexByValue0"),
                           {NLs::Finished,
                            NLs::IndexType(NKikimrSchemeOp::EIndexTypeGlobal),
                            NLs::IndexState(NKikimrSchemeOp::EIndexStateReady),
                            NLs::IndexKeys({"value0"})});
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/UserDefinedIndexByValue0/indexImplTable"),
                           {NLs::Finished,
                            NLs::PathVersionEqual(3)});

        TestDropTableIndex(runtime, ++txId, "/MyRoot", R"(
            TableName: "Table"
            IndexName: "UserDefinedIndexByValue0"
        )");
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"),
                           {NLs::Finished,
                            NLs::PathVersionEqual(5),
                            NLs::IndexesCount(1)});
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/UserDefinedIndexByValue0"),
                           {NLs::PathNotExist});
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/UserDefinedIndexByValue0/indexImplTable"),
                           {NLs::PathNotExist});

        TestCopyTable(runtime, ++txId, "/MyRoot", "Copy", "/MyRoot/Table");
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"),
                           {NLs::Finished,
                            NLs::PathVersionEqual(5),
                            NLs::IndexesCount(1)});
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/UserDefinedIndexByValue1"),
                           {NLs::PathExist});
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/UserDefinedIndexByValue1/indexImplTable"),
                           {NLs::PathExist});


        TestDropTable(runtime, ++txId, "/MyRoot", "Table");
        env.TestWaitNotification(runtime, txId);

        TestDropTable(runtime, ++txId, "/MyRoot", "Copy");
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(RejectsDropIndex) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        TestCreateIndexedTable(runtime, ++txId, "/MyRoot", R"(
            TableDescription {
              Name: "Table"
              Columns { Name: "key"   Type: "Uint64" }
              Columns { Name: "value0" Type: "Utf8" }
              Columns { Name: "value1" Type: "Utf8" }
              KeyColumnNames: ["key"]
            }
            IndexDescription {
              Name: "UserDefinedIndexByValue0"
              KeyColumnNames: ["value0"]
            }
        )");
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"),
                           {NLs::Finished,
                            NLs::PathVersionEqual(3),
                            NLs::IndexesCount(1)});
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/UserDefinedIndexByValue0"),
                           {NLs::Finished,
                            NLs::IndexType(NKikimrSchemeOp::EIndexTypeGlobal),
                            NLs::IndexState(NKikimrSchemeOp::EIndexStateReady),
                            NLs::IndexKeys({"value0"})});
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/UserDefinedIndexByValue0/indexImplTable"),
                           {NLs::Finished,
                            NLs::PathVersionEqual(3)});

        TestDropTableIndex(runtime, ++txId, "/MyRoot/NotExist", R"(
            TableName: "Table"
            IndexName: "UserDefinedIndexByValue0"
        )", {NKikimrScheme::StatusPathDoesNotExist});
        env.TestWaitNotification(runtime, txId);

        TestDropTableIndex(runtime, ++txId, "/MyRoot", R"(
            TableName: "NotExist"
            IndexName: "UserDefinedIndexByValue0"
        )", {NKikimrScheme::StatusPathDoesNotExist});
        env.TestWaitNotification(runtime, txId);

        TestDropTableIndex(runtime, ++txId, "/MyRoot", R"(
            TableName: "Table"
            IndexName: "NotExist"
        )", {NKikimrScheme::StatusPathDoesNotExist});
        env.TestWaitNotification(runtime, txId);

        TestDropTableIndex(runtime, ++txId, "/MyRoot", R"(
            TableName: "Table"
            IndexName: "UserDefinedIndexByValue0"
        )");
        TestDropTableIndex(runtime, ++txId, "/MyRoot", R"(
            TableName: "Table"
            IndexName: "UserDefinedIndexByValue0"
        )", {NKikimrScheme::StatusMultipleModifications});
        env.TestWaitNotification(runtime, {txId, txId - 1});

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"),
                           {NLs::Finished,
                            NLs::PathVersionEqual(5),
                            NLs::IndexesCount(0)});
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/UserDefinedIndexByValue0"),
                           {NLs::PathNotExist});
        TestDescribeResult(DescribePrivatePath(runtime, "/MyRoot/Table/UserDefinedIndexByValue0/indexImplTable"),
                           {NLs::PathNotExist});

        TestDropTable(runtime, ++txId, "/MyRoot", "Table");
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(CancelBuild) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        // Just create main table
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "Table"
              Columns { Name: "key"     Type: "Uint32" }
              Columns { Name: "index"   Type: "Uint32" }
              Columns { Name: "value"   Type: "Utf8"   }
              KeyColumnNames: ["key"]
              UniformPartitionsCount: 10
        )");
        env.TestWaitNotification(runtime, txId);

        auto fnWriteRow = [&] (ui64 tabletId, ui32 key, ui32 index, TString value, const char* table) {
            TString writeQuery = Sprintf(R"(
                (
                    (let key   '( '('key   (Uint32 '%u ) ) ) )
                    (let row   '( '('index (Uint32 '%u ) )  '('value (Utf8 '%s) ) ) )
                    (return (AsList (UpdateRow '__user__%s key row) ))
                )
            )", key, index, value.c_str(), table);
            NKikimrMiniKQL::TResult result;
            TString err;
            NKikimrProto::EReplyStatus status = LocalMiniKQL(runtime, tabletId, writeQuery, result, err);
            UNIT_ASSERT_VALUES_EQUAL(err, "");
            UNIT_ASSERT_VALUES_EQUAL(status, NKikimrProto::EReplyStatus::OK);;
        };
        for (ui32 delta = 0; delta < 101; ++delta) {
            fnWriteRow(TTestTxConfig::FakeHiveTablets, 1 + delta, 1000 + delta, "aaaa", "Table");
        }

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"),
                           {NLs::PathExist,
                            NLs::IndexesCount(0),
                            NLs::PathVersionEqual(3)});

        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "index1", {"index"});
        ui64 buildIndexId = txId;

        auto listing = TestListBuildIndex(runtime, TTestTxConfig::SchemeShard, "/MyRoot");
        UNIT_ASSERT_VALUES_EQUAL(listing.EntriesSize(), 1);

        TestCancelBuildIndex(runtime, ++txId, TTestTxConfig::SchemeShard, "/MyRoot", buildIndexId);

        env.TestWaitNotification(runtime, buildIndexId);

        auto descr = TestGetBuildIndex(runtime, TTestTxConfig::SchemeShard, "/MyRoot", buildIndexId);
        UNIT_ASSERT_VALUES_EQUAL(descr.GetIndexBuild().GetState(), Ydb::Table::IndexBuildState::STATE_CANCELLED);
        UNIT_ASSERT(descr.GetIndexBuild().HasStartTime());
        UNIT_ASSERT(descr.GetIndexBuild().HasEndTime());

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"),
                           {NLs::PathExist,
                            NLs::IndexesCount(0),
                            NLs::PathVersionEqual(6)});

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table/index1", true, true, true),
                           {NLs::PathNotExist});
    }

    Y_UNIT_TEST(RejectsCancel) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime);
        ui64 txId = 100;

        runtime.SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::BUILD_INDEX, NLog::PRI_TRACE);

        // Just create main table
        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
              Name: "Table"
              Columns { Name: "key"     Type: "Uint32" }
              Columns { Name: "index"   Type: "Uint32" }
              Columns { Name: "value"   Type: "Utf8"   }
              KeyColumnNames: ["key"]
              UniformPartitionsCount: 10
        )");
        env.TestWaitNotification(runtime, txId);

        auto fnWriteRow = [&] (ui64 tabletId, ui32 key, ui32 index, TString value, const char* table) {
            TString writeQuery = Sprintf(R"(
                (
                    (let key   '( '('key   (Uint32 '%u ) ) ) )
                    (let row   '( '('index (Uint32 '%u ) )  '('value (Utf8 '%s) ) ) )
                    (return (AsList (UpdateRow '__user__%s key row) ))
                )
            )", key, index, value.c_str(), table);
            NKikimrMiniKQL::TResult result;
            TString err;
            NKikimrProto::EReplyStatus status = LocalMiniKQL(runtime, tabletId, writeQuery, result, err);
            UNIT_ASSERT_VALUES_EQUAL(err, "");
            UNIT_ASSERT_VALUES_EQUAL(status, NKikimrProto::EReplyStatus::OK);
        };
        for (ui32 delta = 0; delta < 101; ++delta) {
            fnWriteRow(TTestTxConfig::FakeHiveTablets, 1 + delta, 1000 + delta, "aaaa", "Table");
        }

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"),
                           {NLs::PathExist,
                            NLs::IndexesCount(0),
                            NLs::PathVersionEqual(3)});

        TestBuildIndex(runtime,  ++txId, TTestTxConfig::SchemeShard, "/MyRoot", "/MyRoot/Table", "index1", {"index"});
        ui64 buildIndexId = txId;

        {
            auto descr = TestGetBuildIndex(runtime, TTestTxConfig::SchemeShard, "/MyRoot", buildIndexId);
            UNIT_ASSERT_VALUES_EQUAL(descr.GetIndexBuild().GetState(), Ydb::Table::IndexBuildState::STATE_PREPARING);
            UNIT_ASSERT(descr.GetIndexBuild().HasStartTime());
            UNIT_ASSERT(!descr.GetIndexBuild().HasEndTime());
        }

        runtime.AdvanceCurrentTime(TDuration::Seconds(30)); // building index

        //
        TestCancelBuildIndex(runtime, ++txId, TTestTxConfig::SchemeShard, "/MyRoot", buildIndexId + 1, TVector<Ydb::StatusIds::StatusCode>{Ydb::StatusIds::NOT_FOUND});
        TestCancelBuildIndex(runtime, ++txId, TTestTxConfig::SchemeShard, "/MyRoot/DirNoExist", buildIndexId, TVector<Ydb::StatusIds::StatusCode>{Ydb::StatusIds::NOT_FOUND});

        env.TestWaitNotification(runtime, buildIndexId);

        TestCancelBuildIndex(runtime, ++txId, TTestTxConfig::SchemeShard, "/MyRoot", buildIndexId, TVector<Ydb::StatusIds::StatusCode>{Ydb::StatusIds::PRECONDITION_FAILED});

        {
            auto descr = TestGetBuildIndex(runtime, TTestTxConfig::SchemeShard, "/MyRoot", buildIndexId);
            UNIT_ASSERT_VALUES_EQUAL(descr.GetIndexBuild().GetState(), Ydb::Table::IndexBuildState::STATE_DONE);
            UNIT_ASSERT(descr.GetIndexBuild().HasStartTime());
            UNIT_ASSERT(descr.GetIndexBuild().HasEndTime());
            UNIT_ASSERT_LT(descr.GetIndexBuild().GetStartTime().seconds(), descr.GetIndexBuild().GetEndTime().seconds());
        }

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table"),
                           {NLs::PathExist,
                            NLs::IndexesCount(1),
                            NLs::PathVersionEqual(6)});

        TestDescribeResult(DescribePath(runtime, "/MyRoot/Table/index1", true, true, true),
                           {NLs::PathExist});
    }
}
