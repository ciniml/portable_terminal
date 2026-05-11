# libssh2 (vendored as git submodule) — built against mbedTLS 3.6 / IDF 6.0.
# Excludes backends we don't use (openssl, libgcrypt, wincng, os400qc3),
# the win32 agent transport, and the os/400 quartz crypto bridge.
file(GLOB LIBSSH2_SRCS "libssh2/src/*.c")
list(REMOVE_ITEM LIBSSH2_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/libssh2/src/openssl.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/libssh2/src/libgcrypt.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/libssh2/src/wincng.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/libssh2/src/os400qc3.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/libssh2/src/agent_win.c"
)

idf_component_register(
    SRCS ${LIBSSH2_SRCS}
    INCLUDE_DIRS "libssh2/include"
    PRIV_INCLUDE_DIRS "libssh2/src" "."
    REQUIRES mbedtls lwip
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LIBSSH2_MBEDTLS=1
    HAVE_CONFIG_H=1
)

# libssh2 1.11.1 throws a few -Wunused-but-set-variable and stringop-truncation
# warnings under IDF's strict flags; silence them rather than patch the source.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-but-set-variable
    -Wno-stringop-truncation
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-format-truncation
)
