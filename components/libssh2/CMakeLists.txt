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

# Explicit mbedTLS include paths, for robustness against `REQUIRES mbedtls`
# propagation differences between IDF 6.0 snapshots. Listing every plausible
# location is harmless where a path doesn't exist. Same pattern as
# components/tailscale/CMakeLists.txt.
#
# NOTE: this does NOT make libssh2 build on IDF 6.0.x stable. Those releases
# ship mbedTLS 4 / TF-PSA-Crypto, which made the low-level crypto headers
# libssh2's backend includes directly — rsa.h, bignum.h, cipher.h, ecdh.h,
# ecdsa.h, entropy.h, ctr_drbg.h — *private* (they now live under
# tf-psa-crypto/drivers/builtin/include/mbedtls/private/). Verified against a
# v6.0.2 checkout: only mbedtls/private/rsa.h exists, and the build fails at
# src/mbedtls.h:62. Moving to stable needs libssh2 to gain PSA support
# upstream; until then CI pins the pre-mbedTLS-4 master snapshot that the
# project is developed against. See .github/workflows/release.yml.
idf_component_get_property(mbedtls_dir mbedtls COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE
    ${mbedtls_dir}/mbedtls/include
    ${mbedtls_dir}/mbedtls/tf-psa-crypto/include
    ${mbedtls_dir}/mbedtls/tf-psa-crypto/drivers/builtin/include
    ${mbedtls_dir}/port/include
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
