LIBRARY()

WITHOUT_LICENSE_TEXTS()

LICENSE(
    Apache-2.0 WITH LLVM-exception
)

VERSION(19.1.7)

ORIGINAL_SOURCE(https://github.com/llvm/llvm-project/archive/llvmorg-19.1.7.tar.gz)

ADDINCL(
    contrib/libs/cxxsupp/libcxxabi/include
    contrib/libs/cxxsupp/libcxx/include
    contrib/libs/cxxsupp/libcxx
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

NO_UTIL()

CFLAGS(
    -D_LIBCPP_BUILDING_LIBRARY
    -D_LIBCXXABI_BUILDING_LIBRARY
)

IF (EXPORT_CMAKE)
    # TODO(YMAKE-91) keep flags required for libc++ vendoring in a separate core.conf variable
    CXXFLAGS(GLOBAL -nostdinc++)
ENDIF()

SRCDIR(contrib/libs/cxxsupp/libcxxabi)

SRCS(
    src/abort_message.cpp
    src/cxa_demangle.cpp
    src/cxa_thread_atexit.cpp
)

IF (NOT MUSL)
    CFLAGS(
        -DHAVE___CXA_THREAD_ATEXIT_IMPL
    )
ENDIF()

END()
