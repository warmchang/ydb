LIBRARY()

SRCS(
    nodes_health_check.cpp
    rate_limiter_resources.cpp
    response_tasks.cpp
    task_get.cpp
    task_ping.cpp
    task_result_write.cpp
    utils.cpp
)

PEERDIR(
    contrib/libs/fmt
    library/cpp/lwtrace/mon
    library/cpp/monlib/service/pages
    ydb/core/base
    ydb/core/fq/libs/common
    ydb/core/fq/libs/compute/common
    ydb/core/fq/libs/config
    ydb/core/fq/libs/control_plane_storage/proto
    ydb/core/fq/libs/metrics
    ydb/core/fq/libs/quota_manager
    ydb/core/fq/libs/quota_manager/events
    ydb/core/fq/libs/rate_limiter/events
    ydb/core/fq/libs/ydb
    ydb/core/kqp/opt
    ydb/core/kqp/proxy_service
    ydb/core/metering
    ydb/core/mon
    ydb/library/actors/core
    ydb/library/protobuf_printer
    ydb/library/security
    ydb/public/lib/fq
    ydb/public/sdk/cpp/src/client/scheme
    ydb/public/sdk/cpp/src/client/value
    yql/essentials/public/issue
    yql/essentials/utils
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)
