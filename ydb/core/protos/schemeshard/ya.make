PROTO_LIBRARY()
PROTOC_FATAL_WARNINGS()

SET(PROTOC_TRANSITIVE_HEADERS "no")

GRPC()

IF (OS_WINDOWS)
    NO_OPTIMIZE_PY_PROTOS()
ENDIF()

SRCS(
    operations.proto
)

EXCLUDE_TAGS(GO_PROTO)

END()
