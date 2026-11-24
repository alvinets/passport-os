# Only the generic HTTP CONNECT transport is retained from the Muse port.
# WorkBuddy pairing/authentication/chat live in components/passport_workbuddy.
# ESP-IDF 6.x folds esp_crt_bundle into mbedtls; 5.5.x has it as its own
# component. Keep both toolchains buildable without changing the TLS setup.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(PASSPORT_CRT_COMPONENT mbedtls)
else()
    set(PASSPORT_CRT_COMPONENT esp_crt_bundle)
endif()
idf_component_register(
    SRCS "config_store.c" "sdk/http_proxy.c" "sdk/proxy_route.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "sdk" "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES nvs_flash esp-tls esp_http_client esp_wifi esp_timer ${PASSPORT_CRT_COMPONENT}
)

# Every distributed build starts without a maintainer's network configuration.
# A local device build may opt into a LAN proxy without committing that
# machine-specific address to the source tree. Explicit -D values win; the
# environment variables keep the existing local build workflow reproducible.
if(PASSPORT_PUBLIC_RELEASE)
    set(MUSE_DEFAULT_PROXY_HOST "")
    set(MUSE_DEFAULT_PROXY_PORT 0)
endif()
if(NOT DEFINED MUSE_DEFAULT_PROXY_HOST)
    set(MUSE_DEFAULT_PROXY_HOST "$ENV{MUSE_HTTP_PROXY_HOST}")
endif()
if(NOT DEFINED MUSE_DEFAULT_PROXY_PORT)
    if(DEFINED ENV{MUSE_HTTP_PROXY_PORT} AND NOT "$ENV{MUSE_HTTP_PROXY_PORT}" STREQUAL "")
        set(MUSE_DEFAULT_PROXY_PORT "$ENV{MUSE_HTTP_PROXY_PORT}")
    else()
        set(MUSE_DEFAULT_PROXY_PORT 0)
    endif()
endif()
if(NOT MUSE_DEFAULT_PROXY_HOST MATCHES "^[0-9.]*$")
    message(FATAL_ERROR "MUSE_DEFAULT_PROXY_HOST must be an IPv4 address or empty")
endif()
if(NOT MUSE_DEFAULT_PROXY_PORT MATCHES "^[0-9]+$")
    message(FATAL_ERROR "MUSE_DEFAULT_PROXY_PORT must be numeric")
endif()
configure_file("sdk/muse_proxy_defaults.h.in" "muse_proxy_defaults.h" @ONLY)
