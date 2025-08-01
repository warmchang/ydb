#include "ydb_service_scheme.h"

#include <ydb/public/lib/json_value/ydb_json_value.h>
#include <ydb/public/lib/ydb_cli/common/pretty_table.h>
#include <ydb/public/lib/ydb_cli/common/scheme_printers.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/topic/client.h>

#include <google/protobuf/port_def.inc>

#include <util/string/join.h>

namespace NYdb {
namespace NConsoleClient {

THashMap<NTopic::EAutoPartitioningStrategy, TString> AutoPartitioningStrategiesStrs = {
    std::pair<NTopic::EAutoPartitioningStrategy, TString>(NTopic::EAutoPartitioningStrategy::Disabled, "disabled"),
    std::pair<NTopic::EAutoPartitioningStrategy, TString>(NTopic::EAutoPartitioningStrategy::ScaleUp, "up"),
    std::pair<NTopic::EAutoPartitioningStrategy, TString>(NTopic::EAutoPartitioningStrategy::ScaleUpAndDown, "up-and-down"),
    std::pair<NTopic::EAutoPartitioningStrategy, TString>(NTopic::EAutoPartitioningStrategy::Paused, "paused"),
};

TCommandScheme::TCommandScheme()
    : TClientCommandTree("scheme", {}, "Scheme service operations")
{
    AddCommand(std::make_unique<TCommandMakeDirectory>());
    AddCommand(std::make_unique<TCommandRemoveDirectory>());
    AddCommand(std::make_unique<TCommandDescribe>());
    AddCommand(std::make_unique<TCommandList>());
    AddCommand(std::make_unique<TCommandPermissions>());
}

TCommandMakeDirectory::TCommandMakeDirectory()
    : TYdbOperationCommand("mkdir", std::initializer_list<TString>(), "Make directory")
{}

void TCommandMakeDirectory::Config(TConfig& config) {
    TYdbOperationCommand::Config(config);

    config.SetFreeArgsNum(1);
    SetFreeArgTitle(0, "<path>", "Path to create");
}

void TCommandMakeDirectory::ExtractParams(TConfig& config) {
    TClientCommand::ExtractParams(config);
    ParsePath(config, 0);
}

int TCommandMakeDirectory::Run(TConfig& config) {
    NScheme::TSchemeClient client(CreateDriver(config));
    NStatusHelpers::ThrowOnErrorOrPrintIssues(
        client.MakeDirectory(
            Path,
            FillSettings(NScheme::TMakeDirectorySettings())
        ).GetValueSync()
    );
    return EXIT_SUCCESS;
}

TCommandRemoveDirectory::TCommandRemoveDirectory()
    : TYdbOperationCommand("rmdir", std::initializer_list<TString>(), "Remove directory")
{}

void TCommandRemoveDirectory::Config(TConfig& config) {
    TYdbOperationCommand::Config(config);
    config.Opts->AddLongOption('r', "recursive", "Remove directory and its content recursively. Prompt once by default")
        .StoreTrue(&Recursive);
    config.Opts->AddLongOption('f', "force", "Never prompt")
        .NoArgument().StoreValue(&Prompt, ERecursiveRemovePrompt::Never);
    config.Opts->AddCharOption('i', "Prompt before every removal")
        .NoArgument().StoreValue(&Prompt, ERecursiveRemovePrompt::Always);
    config.Opts->AddCharOption('I', "Prompt once")
        .NoArgument().StoreValue(&Prompt, ERecursiveRemovePrompt::Once);

    config.SetFreeArgsNum(1);
    SetFreeArgTitle(0, "<path>", "Path to remove");
}

void TCommandRemoveDirectory::ExtractParams(TConfig& config) {
    TClientCommand::ExtractParams(config);
    ParsePath(config, 0);
}

int TCommandRemoveDirectory::Run(TConfig& config) {
    TDriver driver = CreateDriver(config);
    NScheme::TSchemeClient schemeClient(driver);
    const auto settings = FillSettings(NScheme::TRemoveDirectorySettings());

    if (Recursive) {
        const auto settings = TRemoveDirectoryRecursiveSettings()
            .Prompt(Prompt.GetOrElse(ERecursiveRemovePrompt::Once))
            .CreateProgressBar(true);
        NStatusHelpers::ThrowOnErrorOrPrintIssues(RemoveDirectoryRecursive(driver, Path, settings));
    } else {
        if (Prompt) {
            if (!NConsoleClient::Prompt(*Prompt, Path, NScheme::ESchemeEntryType::Directory)) {
                return EXIT_SUCCESS;
            }
        }
        NStatusHelpers::ThrowOnErrorOrPrintIssues(schemeClient.RemoveDirectory(Path, settings).GetValueSync());
    }

    return EXIT_SUCCESS;
}

namespace {
    void PrintPermissions(const std::vector<NScheme::TPermissions>& permissions) {
        if (permissions.size()) {
            for (const NScheme::TPermissions& permission : permissions) {
                Cout << permission.Subject << ":";
                for (const std::string& name : permission.PermissionNames) {
                    if (name != *permission.PermissionNames.begin()) {
                        Cout << ",";
                    }
                    Cout << name;
                }
                Cout << Endl;
            }
        } else {
            Cout << "none" << Endl;
        }
    }
}

void PrintAllPermissions(
    const std::string& owner,
    const std::vector<NScheme::TPermissions>& permissions,
    const std::vector<NScheme::TPermissions>& effectivePermissions
) {
    Cout << "Owner: " << owner << Endl << Endl << "Permissions: " << Endl;
    PrintPermissions(permissions);
    Cout << Endl << "Effective permissions: " << Endl;
    PrintPermissions(effectivePermissions);
}

int PrintPrettyDescribeConsumerResult(const NYdb::NTopic::TConsumerDescription& description, bool withPartitionsStats) {
    // Consumer info
    const NYdb::NTopic::TConsumer& consumer = description.GetConsumer();
    Cout << "Consumer " << consumer.GetConsumerName() << ": " << Endl;
    Cout << "Important: " << (consumer.GetImportant() ? "Yes" : "No") << Endl;
    if (const TInstant& readFrom = consumer.GetReadFrom()) {
        Cout << "Read from: " << readFrom.ToRfc822StringLocal() << Endl;
    } else {
        Cout << "Read from: 0" << Endl;
    }
    Cout << "Supported codecs: " << JoinSeq(", ", consumer.GetSupportedCodecs()) << Endl;

    if (const auto& attrs = consumer.GetAttributes(); !attrs.empty()) {
        TPrettyTable attrTable({ "Attribute", "Value" }, TPrettyTableConfig().WithoutRowDelimiters());
        for (const auto& [k, v] : attrs) {
            attrTable.AddRow()
                .Column(0, k)
                .Column(1, v);
        }
        Cout << "Attributes:" << Endl << attrTable;
    }

    // Partitions
    TVector<TString> columnNames = {
        "#",
        "Active",
        "ChildIds",
        "ParentIds"
    };

    size_t statsBase = columnNames.size();
    if (withPartitionsStats) {
        columnNames.insert(columnNames.end(),
            {
                "Start offset",
                "End offset",
                "Size",
                "Last write time",
                "Max write time lag",
                "Written size per minute",
                "Written size per hour",
                "Written size per day",
                "Committed offset",
                "Last read offset",
                "Reader name",
                "Read session id"
            }
        );
    }

    TPrettyTable partitionsTable(columnNames, TPrettyTableConfig().WithoutRowDelimiters());
    for (const NYdb::NTopic::TPartitionInfo& partition : description.GetPartitions()) {
        auto& row = partitionsTable.AddRow();
        row
            .Column(0, partition.GetPartitionId())
            .Column(1, partition.GetActive())
            .Column(2, JoinSeq(",", partition.GetChildPartitionIds()))
            .Column(3, JoinSeq(",", partition.GetParentPartitionIds()));
        if (withPartitionsStats) {
            if (const auto& maybeStats = partition.GetPartitionStats()) {
                row
                    .Column(statsBase + 0, maybeStats->GetStartOffset())
                    .Column(statsBase + 1, maybeStats->GetEndOffset())
                    .Column(statsBase + 2, PrettySize(maybeStats->GetStoreSizeBytes()))
                    .Column(statsBase + 3, FormatTime(maybeStats->GetLastWriteTime()))
                    .Column(statsBase + 4, FormatDuration(maybeStats->GetMaxWriteTimeLag()))
                    .Column(statsBase + 5, PrettySize(maybeStats->GetBytesWrittenPerMinute()))
                    .Column(statsBase + 6, PrettySize(maybeStats->GetBytesWrittenPerHour()))
                    .Column(statsBase + 7, PrettySize(maybeStats->GetBytesWrittenPerDay()));
            }

            if (const auto& maybeStats = partition.GetPartitionConsumerStats()) {
                row
                    .Column(statsBase + 8, maybeStats->GetCommittedOffset())
                    .Column(statsBase + 9, maybeStats->GetLastReadOffset())
                    .Column(statsBase + 10, maybeStats->GetReaderName())
                    .Column(statsBase + 11, maybeStats->GetReadSessionId());
            }
        }
    }
    Cout << "Partitions:" << Endl << partitionsTable;
    return EXIT_SUCCESS;
}

TCommandDescribe::TCommandDescribe()
    : TYdbOperationCommand("describe", std::initializer_list<TString>(), "Show information about object at given object")
{}

void TCommandDescribe::Config(TConfig& config) {
    TYdbOperationCommand::Config(config);
    // Common options
    config.Opts->AddLongOption("permissions", "Show owner and permissions").StoreTrue(&ShowPermissions);

    // Table options
    config.Opts->AddLongOption("partition-boundaries", "[Table] Show partition key boundaries").StoreTrue(&ShowKeyShardBoundaries)
        .AddLongName("shard-boundaries");
    config.Opts->AddLongOption("stats", "[Table|Topic|Replication] Show table/topic/replication statistics").StoreTrue(&ShowStats);
    config.Opts->AddLongOption("partition-stats", "[Table|Topic|Consumer] Show partition statistics").StoreTrue(&ShowPartitionStats);

    AddDeprecatedJsonOption(config, "(Deprecated, will be removed soon. Use --format option instead) [Table] Output in json format");
    AddOutputFormats(config, { EDataFormat::Pretty, EDataFormat::ProtoJsonBase64 });
    config.Opts->MutuallyExclusive("json", "format");

    config.SetFreeArgsNum(1);
    SetFreeArgTitle(0, "<path>", "Path to an object to describe. If object is topic consumer, it must be specified as <topic_path>/<consumer_name>");
}

void TCommandDescribe::Parse(TConfig& config) {
    TClientCommand::Parse(config);
    Database = config.Database;
    ParseOutputFormats();
}

void TCommandDescribe::ExtractParams(TConfig& config) {
    TClientCommand::ExtractParams(config);
    ParsePath(config, 0);
}

int TCommandDescribe::Run(TConfig& config) {
    TDriver driver = CreateDriver(config);
    NScheme::TSchemeClient client(driver);
    NScheme::TDescribePathResult result = client.DescribePath(
        Path,
        FillSettings(NScheme::TDescribePathSettings())
    ).GetValueSync();
    if (!result.IsSuccess()) {
        return TryTopicConsumerDescribeOrFail(driver, result);
    }
    NStatusHelpers::ThrowOnErrorOrPrintIssues(result);
    return PrintPathResponse(driver, result);
}

int TCommandDescribe::PrintPathResponse(TDriver& driver, const NScheme::TDescribePathResult& result) {
    NScheme::TSchemeEntry entry = result.GetEntry();
    Cout << "<" << EntryTypeToString(entry.Type) << "> " << entry.Name << Endl;
    switch (entry.Type) {
    case NScheme::ESchemeEntryType::Table:
        return DescribeTable(driver);
    case NScheme::ESchemeEntryType::ColumnTable:
        return DescribeColumnTable(driver);
    case NScheme::ESchemeEntryType::PqGroup:
    case NScheme::ESchemeEntryType::Topic:
        return DescribeTopic(driver);
    case NScheme::ESchemeEntryType::CoordinationNode:
        return DescribeCoordinationNode(driver);
    case NScheme::ESchemeEntryType::Replication:
        return DescribeReplication(driver);
    case NScheme::ESchemeEntryType::Transfer:
        return DescribeTransfer(driver);
    case NScheme::ESchemeEntryType::View:
        return DescribeView(driver);
    case NScheme::ESchemeEntryType::ExternalDataSource:
        return DescribeExternalDataSource(driver);
    case NScheme::ESchemeEntryType::ExternalTable:
        return DescribeExternalTable(driver);
    case NScheme::ESchemeEntryType::SysView:
        return DescribeSystemView(driver);
    default:
        return DescribeEntryDefault(entry);
    }
    return EXIT_SUCCESS;
}

int TCommandDescribe::DescribeEntryDefault(NScheme::TSchemeEntry entry) {
    if (ShowPermissions) {
        Cout << Endl;
        PrintAllPermissions(entry.Owner, entry.Permissions, entry.EffectivePermissions);
    }
    WarnAboutTableOptions();
    return EXIT_SUCCESS;
}

namespace {
    TString FormatCodecs(const std::vector<NYdb::NTopic::ECodec>& codecs) {
        return JoinSeq(", ", codecs);
    }

    void PrintTopicConsumers(const std::vector<NYdb::NTopic::TConsumer>& consumers) {
        if (consumers.empty()) {
            return;
        }
        TPrettyTable table({ "ConsumerName", "SupportedCodecs", "ReadFrom", "Important" });
        for (const auto& c: consumers) {
            table.AddRow()
                .Column(0, c.GetConsumerName())
                .Column(1, FormatCodecs(c.GetSupportedCodecs()))
                .Column(2, c.GetReadFrom().ToRfc822StringLocal())
                .Column(3, c.GetImportant() ? "Yes" : "No");
//                .Column(4, rule.ServiceType())
//                .Column(5, rule.Version());
        }
        Cout << Endl << "Consumers: " << Endl;
        Cout << table;
    }
}

namespace {

    void PrintStatistics(const NTopic::TTopicDescription& topicDescription) {
        Cout << Endl << "Topic stats:" << Endl;
        auto& topicStats = topicDescription.GetTopicStats();
        Cout << "Approximate size of topic: " << PrettySize(topicStats.GetStoreSizeBytes()) << Endl;
        Cout << "Max partitions write time lag: " << FormatDuration(topicStats.GetMaxWriteTimeLag()) << Endl;
        Cout << "Min partitions last write time: " << FormatTime(topicStats.GetMinLastWriteTime()) << Endl;
        Cout << "Written size per minute: " << PrettySize(topicStats.GetBytesWrittenPerMinute()) << Endl;
        Cout << "Written size per hour: " << PrettySize(topicStats.GetBytesWrittenPerHour()) << Endl;
        Cout << "Written size per day: " << PrettySize(topicStats.GetBytesWrittenPerDay()) << Endl;
    }

    void PrintMain(const NTopic::TTopicDescription& topicDescription) {
        Cout << Endl << "Main:";
        Cout << Endl << "RetentionPeriod: " << topicDescription.GetRetentionPeriod().Hours() << " hours";
        if (topicDescription.GetRetentionStorageMb().has_value()) {
            Cout << Endl << "StorageRetention: " << *topicDescription.GetRetentionStorageMb() << " MB";
        }
        Cout << Endl << "PartitionsCount: " << topicDescription.GetTotalPartitionsCount();
        Cout << Endl << "PartitionWriteSpeed: " << topicDescription.GetPartitionWriteSpeedBytesPerSecond() / 1_KB << " KB";
        Cout << Endl << "MeteringMode: " << (TStringBuilder() << topicDescription.GetMeteringMode());
        if (!topicDescription.GetSupportedCodecs().empty()) {
            Cout << Endl << "SupportedCodecs: " << FormatCodecs(topicDescription.GetSupportedCodecs()) << Endl;
        } else {
            Cout << Endl;
        }
    }

    void PrintAutopartitioning(const NTopic::TTopicDescription& topicDescription) {
        auto autoPartitioningStrategyIt = NYdb::NConsoleClient::AutoPartitioningStrategiesStrs.find(topicDescription.GetPartitioningSettings().GetAutoPartitioningSettings().GetStrategy());
        if (!autoPartitioningStrategyIt.IsEnd()) {
            Cout << Endl << "AutoPartitioning:";
            Cout << Endl << "Strategy: " << autoPartitioningStrategyIt->second;
            if (topicDescription.GetPartitioningSettings().GetAutoPartitioningSettings().GetStrategy() != NTopic::EAutoPartitioningStrategy::Disabled) {
                Cout << Endl << "MinActivePartitions: " << topicDescription.GetPartitioningSettings().GetMinActivePartitions();
                Cout << Endl << "MaxActivePartitions: " << topicDescription.GetPartitioningSettings().GetMaxActivePartitions();
                Cout << Endl << "DownUtilizationPercent: " << topicDescription.GetPartitioningSettings().GetAutoPartitioningSettings().GetDownUtilizationPercent();
                Cout << Endl << "UpUtilizationPercent: " << topicDescription.GetPartitioningSettings().GetAutoPartitioningSettings().GetUpUtilizationPercent();
                Cout << Endl << "StabilizationWindowSeconds: " << topicDescription.GetPartitioningSettings().GetAutoPartitioningSettings().GetStabilizationWindow().Seconds() << Endl;
            } else {
                Cout << Endl;
            }
        }
    }

    void PrintPartitionStatistics(const NTopic::TTopicDescription& topicDescription) {
        Cout << Endl << "Topic partitions stats:" << Endl;

        TVector<TString> columnNames = { "#" };
        columnNames.push_back("Active");
        columnNames.push_back("Start offset");
        columnNames.push_back("End offset");
        columnNames.push_back("Size");
        columnNames.push_back("Last write time");
        columnNames.push_back("Max write time lag");
        columnNames.push_back("Written size per minute");
        columnNames.push_back("Written size per hour");
        columnNames.push_back("Written size per day");

        TPrettyTable table(columnNames);
        for (const auto& part : topicDescription.GetPartitions()) {
            auto& row = table.AddRow();
            row.Column(0, part.GetPartitionId());
            row.Column(1, part.GetActive());
            const auto& partStats = part.GetPartitionStats();
            if (partStats) {
                row.Column(2, partStats->GetStartOffset());
                row.Column(3, partStats->GetEndOffset());
                row.Column(4, PrettySize(partStats->GetStoreSizeBytes()));
                row.Column(5, FormatTime(partStats->GetLastWriteTime()));
                row.Column(6, FormatDuration(partStats->GetMaxWriteTimeLag()));
                row.Column(7, PrettySize(partStats->GetBytesWrittenPerMinute()));
                row.Column(8, PrettySize(partStats->GetBytesWrittenPerHour()));
                row.Column(9, PrettySize(partStats->GetBytesWrittenPerDay()));
            }
        }
        Cout << table;
    }

}

int TCommandDescribe::PrintTopicResponsePretty(const NYdb::NTopic::TTopicDescription& description) const {
    PrintMain(description);
    PrintAutopartitioning(description);
    PrintTopicConsumers(description.GetConsumers());
    PrintPermissionsIfNeeded(description);
    if (ShowStats) {
        PrintStatistics(description);
    }
    if (ShowPartitionStats){
        PrintPartitionStatistics(description);
    }

    return EXIT_SUCCESS;
}

int TCommandDescribe::DescribeTopic(TDriver& driver) {
    NYdb::NTopic::TTopicClient topicClient(driver);
    NYdb::NTopic::TDescribeTopicSettings settings;
    settings.IncludeStats(ShowStats || ShowPartitionStats);

    auto result = topicClient.DescribeTopic(Path, settings).GetValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(result);

    const auto& desc = result.GetTopicDescription();
    return PrintDescription(this, OutputFormat, desc, &TCommandDescribe::PrintTopicResponsePretty);
}

int TCommandDescribe::DescribeTable(TDriver& driver) {
    NTable::TTableClient client(driver);
    NTable::TCreateSessionResult sessionResult = client.GetSession(NTable::TCreateSessionSettings()).GetValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(sessionResult);
    NTable::TDescribeTableResult result = sessionResult.GetSession().DescribeTable(
        Path,
        FillSettings(
            NTable::TDescribeTableSettings()
            .WithKeyShardBoundary(ShowKeyShardBoundaries)
            .WithTableStatistics(ShowStats || ShowPartitionStats)
            .WithPartitionStatistics(ShowPartitionStats)
        )
    ).GetValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(result);

    auto desc = result.GetTableDescription();
    return PrintDescription(this, OutputFormat, desc, &TCommandDescribe::PrintTableResponsePretty);
}

int TCommandDescribe::DescribeColumnTable(TDriver& driver) {
    NTable::TTableClient client(driver);
    NTable::TCreateSessionResult sessionResult = client.GetSession(NTable::TCreateSessionSettings()).GetValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(sessionResult);
    NTable::TDescribeTableResult result = sessionResult.GetSession().DescribeTable(
        Path,
        FillSettings(
            NTable::TDescribeTableSettings()
            .WithTableStatistics(ShowStats)
        )
    ).GetValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(result);

    auto desc = result.GetTableDescription();
    return PrintDescription(this, OutputFormat, desc, &TCommandDescribe::PrintTableResponsePretty);
}

int TCommandDescribe::PrintCoordinationNodeResponsePretty(const NYdb::NCoordination::TNodeDescription& result) const {
    Cout << Endl << "AttachConsistencyMode: " << result.GetAttachConsistencyMode() << Endl;
    Cout << "ReadConsistencyMode: " << result.GetReadConsistencyMode() << Endl;
    if (result.GetSessionGracePeriod().has_value()) {
        Cout << "SessionGracePeriod: " << result.GetSessionGracePeriod().value() << Endl;
    }
    if (result.GetSelfCheckPeriod().has_value()) {
        Cout << "SelfCheckPeriod: " << result.GetSelfCheckPeriod().value() << Endl;
    }
    Cout << "RatelimiterCountersMode: " << result.GetRateLimiterCountersMode() << Endl;
    return EXIT_SUCCESS;
}

int TCommandDescribe::DescribeCoordinationNode(const TDriver& driver) {
    NCoordination::TClient client(driver);
    auto result = client.DescribeNode(Path).GetValueSync();

    const auto& desc = result.GetResult();
    return PrintDescription(this, OutputFormat, desc, &TCommandDescribe::PrintCoordinationNodeResponsePretty);
}

template <typename T, typename U>
static TString ValueOr(const std::optional<T>& value, const U& orValue) {
    if (value) {
        return TStringBuilder() << *value;
    } else {
        return TStringBuilder() << orValue;
    }
}

template <typename U>
static TString ProgressOr(const std::optional<float>& value, const U& orValue) {
    if (value) {
        return TStringBuilder() << FloatToString(*value, PREC_POINT_DIGITS, 2) << "%";
    } else {
        return TStringBuilder() << orValue;
    }
}

static TStringBuf SkipDatabasePrefix(TStringBuf value, TStringBuf prefix) {
    if (value.SkipPrefix(prefix)) {
        value.Skip(1); // skip '/'
    }
    return value;
}

void PrintConnectionParams(const NReplication::TConnectionParams& connParams) {
    bool isLocal = connParams.GetDiscoveryEndpoint().empty();
    if (isLocal) {
        return;
    }

    Cout << Endl << "Endpoint: " << connParams.GetDiscoveryEndpoint();
    Cout << Endl << "Database: " << connParams.GetDatabase();

    switch (connParams.GetCredentials()) {
    case NReplication::TConnectionParams::ECredentials::Static:
        Cout << Endl << "User: " << connParams.GetStaticCredentials().User;
        Cout << Endl << "Password (SECRET): " << connParams.GetStaticCredentials().PasswordSecretName;
        break;
    case NReplication::TConnectionParams::ECredentials::OAuth:
        Cout << Endl << "OAuth token (SECRET): " << connParams.GetOAuthCredentials().TokenSecretName;
        break;
    }
}

int TCommandDescribe::PrintReplicationResponsePretty(const NYdb::NReplication::TDescribeReplicationResult& result) const {
    const auto& desc = result.GetReplicationDescription();

    Cout << Endl << "State: ";
    switch (desc.GetState()) {
    case NReplication::TReplicationDescription::EState::Running:
        if (const auto& stats = desc.GetRunningState().GetStats(); ShowStats) {
            if (const auto& progress = stats.GetInitialScanProgress(); progress && *progress < 100) {
                Cout << "Initial scan (" << FloatToString(*progress, PREC_POINT_DIGITS, 2) << "%)";
            } else if (const auto& lag = stats.GetLag()) {
                Cout << "Standby (lag: " << *lag << ")";
            } else {
                Cout << desc.GetState();
            }
        } else {
            Cout << desc.GetState();
        }
        break;
    case NReplication::TReplicationDescription::EState::Error:
        Cout << "Error: " << desc.GetErrorState().GetIssues().ToOneLineString();
        break;
    default:
        break;
    }

    const auto& connParams = desc.GetConnectionParams();
    const auto& srcDatabase = connParams.GetDatabase();
    const auto& dstDatabase = Database;

    PrintConnectionParams(connParams);

    Cout << Endl << "Consistency level: " << desc.GetConsistencyLevel();
    switch (desc.GetConsistencyLevel()) {
    case NReplication::TReplicationDescription::EConsistencyLevel::Row:
        break;
    case NReplication::TReplicationDescription::EConsistencyLevel::Global:
        Cout << Endl << "Commit interval: " << desc.GetGlobalConsistency().GetCommitInterval();
        break;
    }

    if (const auto& items = desc.GetItems(); !items.empty()) {
        TVector<TString> columnNames = { "#", "Source", "Destination", "Changefeed" };
        if (ShowStats) {
            columnNames.push_back("Lag");
            columnNames.push_back("Progress");
        }

        TPrettyTable table(columnNames, TPrettyTableConfig().WithoutRowDelimiters());
        for (const auto& item : items) {
            auto& row = table.AddRow()
                .Column(0, item.Id)
                .Column(1, SkipDatabasePrefix(TStringBuf(item.SrcPath), TStringBuf(srcDatabase)))
                .Column(2, SkipDatabasePrefix(TStringBuf(item.DstPath), TStringBuf(dstDatabase)))
                .Column(3, ValueOr(item.SrcChangefeedName, "n/a"));
            if (ShowStats) {
                row
                    .Column(4, ValueOr(item.Stats.GetLag(), "n/a"))
                    .Column(5, ProgressOr(item.Stats.GetInitialScanProgress(), "n/a"));
            }
        }
        Cout << Endl << "Items:" << Endl << table;
    }

    Cout << Endl;
    return EXIT_SUCCESS;
}

int TCommandDescribe::PrintTransferResponsePretty(const NYdb::NReplication::TDescribeTransferResult& result) const {
    const auto& desc = result.GetTransferDescription();

    Cout << Endl << "State: ";
    switch (desc.GetState()) {
    case NReplication::TTransferDescription::EState::Running:
        Cout << desc.GetState();
        break;
    case NReplication::TTransferDescription::EState::Error:
        Cout << "Error: " << desc.GetErrorState().GetIssues().ToOneLineString();
        break;
    default:
        break;
    }

    const auto& connParams = desc.GetConnectionParams();
    PrintConnectionParams(connParams);

    Cout << Endl << "Source path: " << desc.GetSrcPath();
    Cout << Endl << "Destination path: " << desc.GetDstPath();
    Cout << Endl << "Consumer: " << desc.GetConsumerName();
    Cout << Endl << "Transformation lambda: " << desc.GetTransformationLambda();
    Cout << Endl << "Batch size, bytes: " << desc.GetBatchingSettings().SizeBytes;
    Cout << Endl << "Batch flush interval: " << desc.GetBatchingSettings().FlushInterval;

    Cout << Endl;
    return EXIT_SUCCESS;
}

int TCommandDescribe::DescribeReplication(const TDriver& driver) {
    NReplication::TReplicationClient client(driver);
    auto settings = NReplication::TDescribeReplicationSettings()
        .IncludeStats(ShowStats);

    auto result = client.DescribeReplication(Path, settings).ExtractValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(result);
    return PrintDescription(this, OutputFormat, result, &TCommandDescribe::PrintReplicationResponsePretty);
}

int TCommandDescribe::DescribeTransfer(const TDriver& driver) {
    NReplication::TReplicationClient client(driver);

    auto result = client.DescribeTransfer(Path).ExtractValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(result);
    return PrintDescription(this, OutputFormat, result, &TCommandDescribe::PrintTransferResponsePretty);
}

int TCommandDescribe::PrintViewResponsePretty(const NYdb::NView::TDescribeViewResult& result) const {
    Cout << "\nQuery text:\n" << result.GetViewDescription().GetQueryText() << Endl;
    return EXIT_SUCCESS;
}

int TCommandDescribe::DescribeView(const TDriver& driver) {
    NView::TViewClient client(driver);
    auto result = client.DescribeView(Path, {}).ExtractValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(result);

    return PrintDescription(this, OutputFormat, result, &TCommandDescribe::PrintViewResponsePretty);
}

int TCommandDescribe::PrintExternalDataSourceResponsePretty(const NYdb::NTable::TExternalDataSourceDescription& description) const {
    // to do
    return EXIT_SUCCESS;
}

int TCommandDescribe::DescribeExternalDataSource(const TDriver& driver) {
    NTable::TTableClient client(driver);
    const auto sessionResult = client.CreateSession().ExtractValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(sessionResult);
    const auto description = sessionResult.GetSession().DescribeExternalDataSource(Path).ExtractValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(description);

    return PrintDescription(this, OutputFormat, description.GetExternalDataSourceDescription(), &TCommandDescribe::PrintExternalDataSourceResponsePretty);
}

int TCommandDescribe::PrintExternalTableResponsePretty(const NYdb::NTable::TExternalTableDescription& description) const {
    // to do
    return EXIT_SUCCESS;
}

int TCommandDescribe::DescribeExternalTable(const TDriver& driver) {
    NTable::TTableClient client(driver);
    const auto sessionResult = client.CreateSession().ExtractValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(sessionResult);
    const auto result = sessionResult.GetSession().DescribeExternalTable(Path).ExtractValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(result);

    return PrintDescription(this, OutputFormat, result.GetExternalTableDescription(), &TCommandDescribe::PrintExternalTableResponsePretty);
}

int TCommandDescribe::DescribeSystemView(const TDriver& driver) {
    NTable::TTableClient client(driver);
    const auto sessionResult = client.CreateSession().ExtractValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(sessionResult);
    const auto result = sessionResult.GetSession().DescribeSystemView(Path).ExtractValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(result);

    const auto desc = result.GetSystemViewDescription();
    return PrintDescription(this, OutputFormat, desc, &TCommandDescribe::PrintSystemViewResponsePretty);
}

namespace {
    template<typename TTableLikeObjectDescription>
    void PrintColumns(const TTableLikeObjectDescription& tableDescription) {
        if (!tableDescription.GetTableColumns().size()) {
            return;
        }
        Cerr << Endl;
        TPrettyTable table({ "Name", "Type", "Family", "Key" }, TPrettyTableConfig().WithoutRowDelimiters());

        const std::vector<std::string>& keyColumns = tableDescription.GetPrimaryKeyColumns();
        for (const NTable::TTableColumn& column : tableDescription.GetTableColumns()) {
            TString key = "";
            auto itKey = std::find(keyColumns.begin(), keyColumns.end(), column.Name);
            if (itKey != keyColumns.end()) {
                key = TStringBuilder() << "K" << itKey - keyColumns.begin();
            }
            TString columnType;
            try {
                columnType = FormatType(column.Type);
            } catch (yexception) {
                columnType = "<unknown_type>";
            }
            table.AddRow()
                .Column(0, column.Name)
                .Column(1, columnType)
                .Column(2, column.Family)
                .Column(3, key);
        }

        Cout << "Columns:" << Endl << table;
    }

    void PrintIndexes(const NTable::TTableDescription& tableDescription) {
        const std::vector<NTable::TIndexDescription>& indexes = tableDescription.GetIndexDescriptions();
        if (!indexes.size()) {
            return;
        }

        TPrettyTable table({ "Name", "Type", "Index columns", "Cover columns" },
            TPrettyTableConfig().WithoutRowDelimiters());

        for (const auto& index : indexes) {
            table.AddRow()
                .Column(0, index.GetIndexName())
                .Column(1, index.GetIndexType())
                .Column(2, JoinSeq(",", index.GetIndexColumns()))
                .Column(3, JoinSeq(",", index.GetDataColumns()));
        }

        Cout << Endl << "Indexes:" << Endl << table;
    }

    void PrintChangefeeds(const NTable::TTableDescription& tableDescription) {
        const auto& changefeeds = tableDescription.GetChangefeedDescriptions();
        if (changefeeds.empty()) {
            return;
        }

        TPrettyTable table({ "Name", "Mode", "Format", "State", "VirtualTimestamps" },
            TPrettyTableConfig().WithoutRowDelimiters());

        for (const auto& changefeed : changefeeds) {
            auto& row = table.AddRow()
                .Column(0, changefeed.GetName())
                .Column(1, changefeed.GetMode())
                .Column(2, changefeed.GetFormat())
                .Column(4, changefeed.GetVirtualTimestamps() ? "on" : "off");
            if (changefeed.GetState() == NTable::EChangefeedState::InitialScan && changefeed.GetInitialScanProgress()) {
                const float percentage = changefeed.GetInitialScanProgress()->GetProgress();
                row.Column(3, TStringBuilder() << changefeed.GetState()
                    << " (" << FloatToString(percentage, PREC_POINT_DIGITS, 2) << "%)");
            } else {
                row.Column(3, changefeed.GetState());
            }
        }

        Cout << Endl << "Changefeeds:" << Endl << table;
    }

    void PrintStorageSettings(const NTable::TTableDescription& tableDescription) {
        const NTable::TStorageSettings& settings = tableDescription.GetStorageSettings();
        const auto commitLog0 = settings.GetTabletCommitLog0();
        const auto commitLog1 = settings.GetTabletCommitLog1();
        const auto external = settings.GetExternal();
        const auto storeExternalBlobs = settings.GetStoreExternalBlobs();
        if (!commitLog0 && !commitLog1 && !external && !storeExternalBlobs.has_value()) {
            return;
        }
        Cout << Endl << "Storage settings: " << Endl;
        if (commitLog0) {
            Cout << "Internal channel 0 commit log storage pool: " << commitLog0.value() << Endl;
        }
        if (commitLog1) {
            Cout << "Internal channel 1 commit log storage pool: " << commitLog1.value() << Endl;
        }
        if (external) {
            Cout << "External blobs storage pool: " << external.value() << Endl;
        }
        if (storeExternalBlobs) {
            Cout << "Store large values in \"external blobs\": "
                << (storeExternalBlobs.value() ? "true" : "false") << Endl;
        }
    }

    void PrintColumnFamilies(const NTable::TTableDescription& tableDescription) {
        if (tableDescription.GetColumnFamilies().empty()) {
            return;
        }
        TPrettyTable table({ "Name", "Data", "Compression", "Keep in memory" },
            TPrettyTableConfig().WithoutRowDelimiters());

        for (const NTable::TColumnFamilyDescription& family : tableDescription.GetColumnFamilies()) {
            std::optional<std::string> data = family.GetData();
            TString compression;
            if (family.GetCompression()) {
                switch (family.GetCompression().value()) {
                case NTable::EColumnFamilyCompression::None:
                    compression = "None";
                    break;
                case NTable::EColumnFamilyCompression::LZ4:
                    compression = "LZ4";
                    break;
                default:
                    compression = TStringBuilder() << "unknown("
                        << static_cast<size_t>(family.GetCompression().value()) << ")";
                }
            }
            TStringBuilder keepInMemory;
            if (family.GetKeepInMemory().has_value()) {
                keepInMemory << keepInMemory << family.GetKeepInMemory().value();
            }
            table.AddRow()
                .Column(0, family.GetName())
                .Column(1, data ? data.value() : "")
                .Column(2, compression)
                .Column(3, keepInMemory);
        }
        Cout << Endl << "Column families: " << Endl;
        Cout << table;
    }

    template<typename TTableLikeObjectDescription>
    void PrintAttributes(const TTableLikeObjectDescription& tableDescription) {
        if (tableDescription.GetAttributes().empty()) {
            return;
        }
        TPrettyTable table({ "Name", "Value" }, TPrettyTableConfig().WithoutRowDelimiters());

        for (const auto& [name, value] : tableDescription.GetAttributes()) {
            table.AddRow()
                .Column(0, name)
                .Column(1, value);
        }
        Cout << Endl << "Attributes: " << Endl;
        Cout << table;
    }

    void PrintTtlSettings(const NTable::TTableDescription& tableDescription) {
        const auto& settings = tableDescription.GetTtlSettings();
        if (!settings) {
            return;
        }

        Cout << Endl << "Ttl settings ";
        switch (settings->GetMode()) {
        case NTable::TTtlSettings::EMode::DateTypeColumn:
        {
            Cout << "(date type column):" << Endl;
            const auto& dateTypeColumn = settings->GetDateTypeColumn();
            Cout << "Column name: " << dateTypeColumn.GetColumnName() << Endl;
            Cout << "Expire after: " << dateTypeColumn.GetExpireAfter() << Endl;
            break;
        }
        case NTable::TTtlSettings::EMode::ValueSinceUnixEpoch:
        {
            Cout << "(value since unix epoch):" << Endl;
            const auto& valueSinceEpoch = settings->GetValueSinceUnixEpoch();
            Cout << "Column name: " << valueSinceEpoch.GetColumnName() << Endl;
            Cout << "Column unit: " << valueSinceEpoch.GetColumnUnit() << Endl;
            Cout << "Expire after: " << valueSinceEpoch.GetExpireAfter() << Endl;
            break;
        }
        default:
            NColorizer::TColors colors = NColorizer::AutoColors(Cout);
            Cout << "(unknown):" << Endl
                << colors.RedColor() << "Unknown ttl settings mode. Please update your version of YDB cli"
                << colors.OldColor() << Endl;
        }

        if (settings->GetRunInterval()) {
            Cout << "Run interval: " << settings->GetRunInterval() << Endl;
        }
    }

    void PrintPartitioningSettings(const NTable::TTableDescription& tableDescription) {
        const auto& settings = tableDescription.GetPartitioningSettings();
        const auto partBySize = settings.GetPartitioningBySize();
        const auto partByLoad = settings.GetPartitioningByLoad();
        if (!partBySize.has_value() && !partByLoad.has_value()) {
            return;
        }
        const auto partitionSizeMb = settings.GetPartitionSizeMb();
        const auto minPartitions = settings.GetMinPartitionsCount();
        const auto maxPartitions = settings.GetMaxPartitionsCount();
        Cout << Endl << "Auto partitioning settings: " << Endl;
        Cout << "Partitioning by size: " << (partBySize.value() ? "true" : "false") << Endl;
        Cout << "Partitioning by load: " << (partByLoad.value() ? "true" : "false") << Endl;
        if (partBySize.has_value() && partitionSizeMb) {
            Cout << "Preferred partition size (Mb): " << partitionSizeMb << Endl;
        }
        if (minPartitions) {
            Cout << "Min partitions count: " << minPartitions << Endl;
        }
        if (maxPartitions) {
            Cout << "Max partitions count: " << maxPartitions << Endl;
        }
    }

    void PrintReadReplicasSettings(const NTable::TTableDescription& tableDescription) {
        const auto& settings = tableDescription.GetReadReplicasSettings();
        if (!settings) {
            return;
        }
        Cout << Endl << "Read replicas settings: " << Endl;
        switch (settings->GetMode()) {
        case NTable::TReadReplicasSettings::EMode::PerAz:
            Cout << "Read replicas count in each AZ: " << settings->GetReadReplicasCount() << Endl;
            break;
        case NTable::TReadReplicasSettings::EMode::AnyAz:
            Cout << "Read replicas total count in all AZs: " << settings->GetReadReplicasCount() << Endl;
            break;
        default:
            NColorizer::TColors colors = NColorizer::AutoColors(Cout);
            Cout << colors.RedColor() << "Unknown read replicas settings mode. Please update your version of YDB cli"
                << colors.OldColor() << Endl;
        }
    }

    void PrintStatistics(const NTable::TTableDescription& tableDescription) {
        Cout << Endl << "Table stats:" << Endl;
        Cout << "Partitions count: " << tableDescription.GetPartitionsCount() << Endl;
        Cout << "Approximate number of rows: " << tableDescription.GetTableRows() << Endl;
        Cout << "Approximate size of table: " << PrettySize(tableDescription.GetTableSize()) << Endl;
        Cout << "Last modified: " << FormatTime(tableDescription.GetModificationTime()) << Endl;
        Cout << "Created: " << FormatTime(tableDescription.GetCreationTime()) << Endl;
    }

    void PrintPartitionInfo(const NTable::TTableDescription& tableDescription, bool showBoundaries, bool showStats) {
        const std::vector<NTable::TKeyRange>& ranges = tableDescription.GetKeyRanges();
        const std::vector<NTable::TPartitionStats>& stats = tableDescription.GetPartitionStats();
        if (showBoundaries) {
            if (showStats) {
                Cout << Endl << "Partitions info:" << Endl;
                if (ranges.empty() && stats.empty()) {
                    Cout << "No data." << Endl;
                    return;
                }
            } else {
                Cout << Endl << "Partitions key boundaries:" << Endl;
                if (ranges.empty()) {
                    Cout << "No data." << Endl;
                    return;
                }
            }
        } else {
            Cout << Endl << "Partitions stats:" << Endl;
            if (stats.empty()) {
                Cout << "No data." << Endl;
                return;
            }
        }
        size_t rowsCount;
        if (showBoundaries && showStats && ranges.size() != stats.size()) {
            Cerr << "(!) Warning: partitions key boundaries size (" << ranges.size()
                << ") mismatches partitions stats size (" << stats.size() << ")." << Endl;
            rowsCount = Min(ranges.size(), stats.size());
        } else {
            rowsCount = Max(ranges.size(), stats.size());
        }
        TVector<TString> columnNames = { "#" };
        if (showBoundaries) {
            columnNames.push_back("");
            columnNames.push_back("From");
            columnNames.push_back("To");
            columnNames.push_back("");
        }
        if (showStats) {
            columnNames.push_back("Rows");
            columnNames.push_back("Size");
        }
        TPrettyTable table(columnNames, TPrettyTableConfig().WithoutRowDelimiters());
        for (size_t i = 0; i < rowsCount; ++i) {
            auto& row = table.AddRow();
            size_t j = 0;
            row.Column(j++, i + 1);
            if (showBoundaries) {
                const NTable::TKeyRange& keyRange = ranges[i];
                const std::optional<NTable::TKeyBound>& from = keyRange.From();
                const std::optional<NTable::TKeyBound>& to = keyRange.To();
                if (from.has_value()) {
                    const NTable::TKeyBound& bound = from.value();
                    if (bound.IsInclusive()) {
                        row.Column(j++, "[");
                    } else {
                        row.Column(j++, "(");
                    }
                    row.Column(j++, FormatValueJson(bound.GetValue(), EBinaryStringEncoding::Unicode));
                } else {
                    row.Column(j++, "(");
                    row.Column(j++, "-Inf");
                }
                if (to.has_value()) {
                    const NTable::TKeyBound& bound = to.value();
                    row.Column(j++, FormatValueJson(bound.GetValue(), EBinaryStringEncoding::Unicode));
                    if (bound.IsInclusive()) {
                        row.Column(j++, "]");
                    } else {
                        row.Column(j++, ")");
                    }
                } else {
                    row.Column(j++, "+Inf");
                    row.Column(j++, ")");
                }
            }
            if (showStats) {
                const NTable::TPartitionStats& partStats = stats[i];
                row.Column(j++, partStats.Rows);
                row.Column(j++, PrettySize(partStats.Size));
            }
        }
        Cout << table;
    }
}

int TCommandDescribe::PrintTableResponsePretty(const NTable::TTableDescription& tableDescription) const {
    PrintColumns(tableDescription);
    PrintIndexes(tableDescription);
    PrintChangefeeds(tableDescription);
    PrintStorageSettings(tableDescription);
    PrintColumnFamilies(tableDescription);
    PrintAttributes(tableDescription);
    PrintTtlSettings(tableDescription);
    PrintPartitioningSettings(tableDescription);
    if (tableDescription.GetKeyBloomFilter().has_value()) {
        Cout << Endl << "Bloom filter by key: "
            << (tableDescription.GetKeyBloomFilter().value() ? "true" : "false") << Endl;
    }
    PrintReadReplicasSettings(tableDescription);
    PrintPermissionsIfNeeded(tableDescription);
    if (ShowStats) {
        PrintStatistics(tableDescription);
    }
    if (ShowKeyShardBoundaries || ShowPartitionStats) {
        PrintPartitionInfo(tableDescription, ShowKeyShardBoundaries, ShowPartitionStats);
    }

    return EXIT_SUCCESS;
}

int TCommandDescribe::PrintSystemViewResponsePretty(const NYdb::NTable::TSystemViewDescription& result) const {
    Cout << "Id: "  << result.GetSysViewId() << " (" << result.GetSysViewName() <<  ")" << Endl;
    PrintColumns(result);
    PrintAttributes(result);

    return EXIT_SUCCESS;
}

std::pair<TString, TString> TCommandDescribe::ParseTopicConsumer() const {
    const size_t slashPos = Path.find_last_of('/');
    std::pair<TString, TString> result;
    if (slashPos != TString::npos && slashPos != Path.size() - 1) {
        result.first = Path.substr(0, slashPos);
        result.second = Path.substr(slashPos + 1);
    }
    return result;
}

int TCommandDescribe::TryTopicConsumerDescribeOrFail(TDriver& driver, const NScheme::TDescribePathResult& result) {
    auto [topic, consumer] = ParseTopicConsumer();
    if (!topic || !consumer) {
        NStatusHelpers::ThrowOnErrorOrPrintIssues(result); // no consumer can be found
    }

    NScheme::TSchemeClient client(driver);
    NScheme::TDescribePathResult topicDescribeResult = client.DescribePath(
        topic,
        FillSettings(NScheme::TDescribePathSettings())
    ).GetValueSync();
    if (!topicDescribeResult.IsSuccess() || topicDescribeResult.GetEntry().Type != NScheme::ESchemeEntryType::Topic && topicDescribeResult.GetEntry().Type != NScheme::ESchemeEntryType::PqGroup) {
        NStatusHelpers::ThrowOnErrorOrPrintIssues(result); // return previous error, this is not topic
    }

    // OK, this is topic, check the consumer
    NYdb::NTopic::TTopicClient topicClient(driver);
    auto consumerDescription = topicClient.DescribeConsumer(topic, consumer, NYdb::NTopic::TDescribeConsumerSettings().IncludeStats(ShowPartitionStats)).GetValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(consumerDescription);

    return PrintDescription(this, OutputFormat, consumerDescription.GetConsumerDescription(), &TCommandDescribe::PrintConsumerResponsePretty);
}

int TCommandDescribe::PrintConsumerResponsePretty(const NYdb::NTopic::TConsumerDescription& description) const {
    return PrintPrettyDescribeConsumerResult(description, ShowPartitionStats);
}

void TCommandDescribe::WarnAboutTableOptions() {
    if (ShowKeyShardBoundaries || ShowStats || ShowPartitionStats || OutputFormat != EDataFormat::Default) {
        TVector<TString> options;
        if (ShowKeyShardBoundaries) {
            options.emplace_back("\"partition-boundaries\"(\"shard-boundaries\")");
        }
        if (ShowStats) {
            options.emplace_back("\"stats\"");
        }
        if (ShowPartitionStats) {
            options.emplace_back("\"partition-stats\"");
        }
        if (OutputFormat != EDataFormat::Default) {
            options.emplace_back("\"json\"");
        }
        Cerr << "Note: \"" << Path << "\" is not a table. Option";
        if (options.size() > 1) {
            Cerr << 's';
        }
        for (auto& option : options) {
            if (option != *options.begin()) {
                Cerr << ',';
            }
            Cerr << ' ' << option;
        }
        Cerr << (options.size() > 1 ? " are" : " is")
            << " used only for tables and thus "
            << (options.size() > 1 ? "have" : "has")
            << " no effect for this command." << Endl;
    }
}

TCommandList::TCommandList()
    : TYdbOperationCommand("ls", std::initializer_list<TString>(), "Show information about objects inside given directory")
{}

void TCommandList::Config(TConfig& config) {
    TYdbOperationCommand::Config(config);

    config.Opts->AddCharOption('l', "List objects with detailed information")
        .StoreTrue(&AdvancedMode);
    config.Opts->AddCharOption('R', "List subdirectories recursively")
        .StoreTrue(&Recursive);
    config.Opts->AddCharOption('1', "List one object per line")
        .StoreTrue(&FromNewLine);
    config.Opts->AddCharOption('m', "Multithread recursive request")
        .StoreTrue(&Multithread);
    AddOutputFormats(config, { EDataFormat::Pretty, EDataFormat::Json });
    config.SetFreeArgsMax(1);
    SetFreeArgTitle(0, "<path>", "Path to list");
}

void TCommandList::Parse(TConfig& config) {
    TClientCommand::Parse(config);
    if (AdvancedMode && FromNewLine) {
        // TODO: add "consider using --format shell"
        throw TMisuseException() << "Options -1 and -l are incompatible";
    }
}

void TCommandList::ExtractParams(TConfig& config) {
    TClientCommand::ExtractParams(config);
    ParsePath(config, 0, true);
}

int TCommandList::Run(TConfig& config) {
    TDriver driver = CreateDriver(config);
    ISchemePrinter::TSettings settings = {
        Path,
        Recursive,
        Multithread,
        FromNewLine,
        FillSettings(NScheme::TListDirectorySettings()),
        FillSettings(NTable::TDescribeTableSettings().WithTableStatistics(true))
    };
    std::unique_ptr<ISchemePrinter> printer;

    switch (OutputFormat) {
    case EDataFormat::Default:
    case EDataFormat::Pretty:
        if (AdvancedMode) {
            printer = std::make_unique<TTableSchemePrinter>(driver, std::move(settings));
        } else {
            printer = std::make_unique<TDefaultSchemePrinter>(driver, std::move(settings));
        }
        break;
    case EDataFormat::Json:
    {
        printer = std::make_unique<TJsonSchemePrinter>(driver, std::move(settings), AdvancedMode);
        break;
    }
    default:
        throw TMisuseException() << "This command doesn't support " << OutputFormat << " output format";
    }
    printer->Print();
    return EXIT_SUCCESS;
}

TCommandPermissions::TCommandPermissions()
    : TClientCommandTree("permissions", {}, "Modify permissions")
{
    AddCommand(std::make_unique<TCommandPermissionGrant>());
    AddCommand(std::make_unique<TCommandPermissionRevoke>());
    AddCommand(std::make_unique<TCommandPermissionSet>());
    AddCommand(std::make_unique<TCommandChangeOwner>());
    AddCommand(std::make_unique<TCommandPermissionClear>());
    AddCommand(std::make_unique<TCommandPermissionSetInheritance>());
    AddCommand(std::make_unique<TCommandPermissionClearInheritance>());
    AddCommand(std::make_unique<TCommandPermissionList>());
}

TCommandPermissionGrant::TCommandPermissionGrant()
    : TYdbOperationCommand("grant", { "add" }, "Grant permission")
{}

void TCommandPermissionGrant::Config(TConfig& config) {
    TYdbOperationCommand::Config(config);

    config.SetFreeArgsNum(2);
    SetFreeArgTitle(0, "<path>", "Path to grant permissions to");
    SetFreeArgTitle(1, "<subject>", "Subject to grant permissions");

    config.Opts->AddLongOption('p', "permission", "[At least one] Permission(s) to grant")
        .RequiredArgument("NAME").AppendTo(&PermissionsToGrant);
}

void TCommandPermissionGrant::Parse(TConfig& config) {
    TClientCommand::Parse(config);
    Subject = config.ParseResult->GetFreeArgs()[1];
    if (Subject.empty()) {
        throw TMisuseException() << "Missing required argument <subject>";
    }
    if (!PermissionsToGrant.size()) {
        throw TMisuseException() << "At least one permission to grant should be provided";
    }
}

void TCommandPermissionGrant::ExtractParams(TConfig& config) {
    TClientCommand::ExtractParams(config);
    ParsePath(config, 0);
}

int TCommandPermissionGrant::Run(TConfig& config) {
    NScheme::TSchemeClient client(CreateDriver(config));
    NStatusHelpers::ThrowOnErrorOrPrintIssues(
        client.ModifyPermissions(
            Path,
            FillSettings(
                NScheme::TModifyPermissionsSettings()
                .AddGrantPermissions({ Subject, PermissionsToGrant })
            )
        ).GetValueSync()
    );
    return EXIT_SUCCESS;
}

TCommandPermissionRevoke::TCommandPermissionRevoke()
    : TYdbOperationCommand("revoke", { "remove" }, "Revoke permission")
{}

void TCommandPermissionRevoke::Config(TConfig& config) {
    TYdbOperationCommand::Config(config);

    config.SetFreeArgsNum(2);
    SetFreeArgTitle(0, "<path>", "Path to revoke permissions to");
    SetFreeArgTitle(1, "<subject>", "Subject to revoke permissions");

    config.Opts->AddLongOption('p', "permission", "[At least one] Permission(s) to revoke")
        .RequiredArgument("NAME").AppendTo(&PermissionsToRevoke);
}

void TCommandPermissionRevoke::Parse(TConfig& config) {
    TClientCommand::Parse(config);
    Subject = config.ParseResult->GetFreeArgs()[1];
    if (Subject.empty()) {
        throw TMisuseException() << "Missing required argument <subject>";
    }
    if (!PermissionsToRevoke.size()) {
        throw TMisuseException() << "At least one permission to revoke should be provided";
    }
}

void TCommandPermissionRevoke::ExtractParams(TConfig& config) {
    TClientCommand::ExtractParams(config);
    ParsePath(config, 0);
}

int TCommandPermissionRevoke::Run(TConfig& config) {
    NScheme::TSchemeClient client(CreateDriver(config));
    NStatusHelpers::ThrowOnErrorOrPrintIssues(
        client.ModifyPermissions(
            Path,
            FillSettings(
                NScheme::TModifyPermissionsSettings()
                .AddRevokePermissions({ Subject, PermissionsToRevoke })
            )
        ).GetValueSync()
    );
    return EXIT_SUCCESS;
}

TCommandPermissionSet::TCommandPermissionSet()
    : TYdbOperationCommand("set", std::initializer_list<TString>(), "Set permissions")
{}

void TCommandPermissionSet::Config(TConfig& config) {
    TYdbOperationCommand::Config(config);

    config.SetFreeArgsNum(2);
    SetFreeArgTitle(0, "<path>", "Path to set permissions to");
    SetFreeArgTitle(1, "<subject>", "Subject to set permissions");

    config.Opts->AddLongOption('p', "permission", "[At least one] Permission(s) to set")
        .RequiredArgument("NAME").AppendTo(&PermissionsToSet);
}

void TCommandPermissionSet::Parse(TConfig& config) {
    TClientCommand::Parse(config);
    Subject = config.ParseResult->GetFreeArgs()[1];
    if (Subject.empty()) {
        throw TMisuseException() << "Missing required argument <subject>";
    }
    if (!PermissionsToSet.size()) {
        throw TMisuseException() << "At least one permission to set should be provided";
    }
}

void TCommandPermissionSet::ExtractParams(TConfig& config) {
    TClientCommand::ExtractParams(config);
    ParsePath(config, 0);
}

int TCommandPermissionSet::Run(TConfig& config) {
    NScheme::TSchemeClient client(CreateDriver(config));
    NStatusHelpers::ThrowOnErrorOrPrintIssues(
        client.ModifyPermissions(
            Path,
            FillSettings(
                NScheme::TModifyPermissionsSettings()
                .AddSetPermissions({ Subject, PermissionsToSet })
            )
        ).GetValueSync()
    );
    return EXIT_SUCCESS;
}

TCommandChangeOwner::TCommandChangeOwner()
    : TYdbOperationCommand("chown", std::initializer_list<TString>(), "Change owner")
{}

void TCommandChangeOwner::Config(TConfig& config) {
    TYdbOperationCommand::Config(config);

    config.SetFreeArgsNum(2);
    SetFreeArgTitle(0, "<path>", "Path to change owner for");
    SetFreeArgTitle(1, "<owner>", "Owner to set");
}

void TCommandChangeOwner::Parse(TConfig& config) {
    TClientCommand::Parse(config);
    Owner = config.ParseResult->GetFreeArgs()[1];
    if (!Owner){
        throw TMisuseException() << "Missing required argument <owner>";
    }
}

void TCommandChangeOwner::ExtractParams(TConfig& config) {
    TClientCommand::ExtractParams(config);
    ParsePath(config, 0);
}

int TCommandChangeOwner::Run(TConfig& config) {
    NScheme::TSchemeClient client(CreateDriver(config));
    NStatusHelpers::ThrowOnErrorOrPrintIssues(
        client.ModifyPermissions(
            Path,
            FillSettings(
                NScheme::TModifyPermissionsSettings()
                .AddChangeOwner(Owner)
            )
        ).GetValueSync()
    );
    return EXIT_SUCCESS;
}

TCommandPermissionClear::TCommandPermissionClear()
    : TYdbOperationCommand("clear", std::initializer_list<TString>(), "Clear permissions")
{}

void TCommandPermissionClear::Config(TConfig& config) {
    TYdbOperationCommand::Config(config);

    config.SetFreeArgsNum(1);
    SetFreeArgTitle(0, "<path>", "Path to clear permissions to");
}

void TCommandPermissionClear::ExtractParams(TConfig& config) {
    TClientCommand::ExtractParams(config);
    ParsePath(config, 0);
}

int TCommandPermissionClear::Run(TConfig& config) {
    NScheme::TSchemeClient client(CreateDriver(config));
    NStatusHelpers::ThrowOnErrorOrPrintIssues(
        client.ModifyPermissions(
            Path,
            FillSettings(
                NScheme::TModifyPermissionsSettings()
                .AddClearAcl()
            )
        ).GetValueSync()
    );
    return EXIT_SUCCESS;
}

TCommandPermissionSetInheritance::TCommandPermissionSetInheritance()
    : TYdbOperationCommand("set-inheritance", std::initializer_list<TString>(), "Set to inherit permissions from the parent")
{}

void TCommandPermissionSetInheritance::Config(TConfig& config) {
    TYdbOperationCommand::Config(config);

    config.SetFreeArgsNum(1);
    SetFreeArgTitle(0, "<path>", "Path to set interrupt-inheritance flag for");
}

void TCommandPermissionSetInheritance::ExtractParams(TConfig& config) {
    TClientCommand::ExtractParams(config);
    ParsePath(config, 0);
}

int TCommandPermissionSetInheritance::Run(TConfig& config) {
    NScheme::TSchemeClient client(CreateDriver(config));
    NStatusHelpers::ThrowOnErrorOrPrintIssues(
        client.ModifyPermissions(
            Path,
            FillSettings(
                NScheme::TModifyPermissionsSettings()
                .AddInterruptInheritance(false)
            )
        ).GetValueSync()
    );
    return EXIT_SUCCESS;
}

TCommandPermissionClearInheritance::TCommandPermissionClearInheritance()
    : TYdbOperationCommand("clear-inheritance", std::initializer_list<TString>(), "Set to do not inherit permissions from the parent")
{}

void TCommandPermissionClearInheritance::Config(TConfig& config) {
    TYdbOperationCommand::Config(config);

    config.SetFreeArgsNum(1);
    SetFreeArgTitle(0, "<path>", "Path to set interrupt-inheritance flag for");
}

void TCommandPermissionClearInheritance::ExtractParams(TConfig& config) {
    TClientCommand::ExtractParams(config);
    ParsePath(config, 0);
}

int TCommandPermissionClearInheritance::Run(TConfig& config) {
    NScheme::TSchemeClient client(CreateDriver(config));
    NStatusHelpers::ThrowOnErrorOrPrintIssues(
        client.ModifyPermissions(
            Path,
            FillSettings(
                NScheme::TModifyPermissionsSettings()
                .AddInterruptInheritance(true)
            )
        ).GetValueSync()
    );
    return EXIT_SUCCESS;
}

TCommandPermissionList::TCommandPermissionList()
    : TYdbOperationCommand("list", std::initializer_list<TString>(), "List permissions")
{}

void TCommandPermissionList::Config(TConfig& config) {
    TYdbOperationCommand::Config(config);

    config.SetFreeArgsNum(1);
    SetFreeArgTitle(0, "<path>", "Path to list permissions for");
}

void TCommandPermissionList::ExtractParams(TConfig& config) {
    TClientCommand::ExtractParams(config);
    ParsePath(config, 0);
}

int TCommandPermissionList::Run(TConfig& config) {
    TDriver driver = CreateDriver(config);
    NScheme::TSchemeClient client(driver);
    NScheme::TDescribePathResult result = client.DescribePath(
        Path,
        FillSettings(NScheme::TDescribePathSettings())
    ).GetValueSync();
    NStatusHelpers::ThrowOnErrorOrPrintIssues(result);
    NScheme::TSchemeEntry entry = result.GetEntry();
    Cout << Endl;
    PrintAllPermissions(entry.Owner, entry.Permissions, entry.EffectivePermissions);
    return EXIT_SUCCESS;
}

}
}
