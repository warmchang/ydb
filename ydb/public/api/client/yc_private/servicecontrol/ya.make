PROTO_LIBRARY()
PROTOC_FATAL_WARNINGS()

EXCLUDE_TAGS(GO_PROTO)

GRPC()
SRCS(
    access_service.proto
    resource.proto
)

USE_COMMON_GOOGLE_APIS(
    api/annotations
)

END()
