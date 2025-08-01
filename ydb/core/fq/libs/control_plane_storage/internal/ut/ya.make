UNITTEST_FOR(ydb/core/fq/libs/control_plane_storage/internal)

SIZE(MEDIUM)

SRCS(utils_ut.cpp)

PEERDIR(
    library/cpp/testing/unittest
    library/cpp/json/yson
    yql/essentials/public/udf/service/stub
    yql/essentials/sql/pg_dummy
)

YQL_LAST_ABI_VERSION()

RESOURCE(
    resources/v1_output.json      v1_output.json
    resources/v2_output.json      v2_output.json
    resources/v1_s3source.json    v1_s3source.json
    resources/v2_s3source.json    v2_s3source.json
    resources/v1_two_results.json v1_two_results.json
    resources/v2_two_results.json v2_two_results.json
)

END()

