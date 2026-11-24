#pragma once
/*
 * Generic HTTPS transport retained from the Muse port. The WorkBuddy adapter
 * uses this HTTP CONNECT helper; the Muse pairing, Noise and chat protocol
 * implementation has been replaced by components/passport_workbuddy.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_tls.h"
#ifdef __cplusplus
extern "C" {
#endif

bool muse_proxy_enabled(void);
void muse_proxy_load_config(void);
void muse_proxy_cancel(void);
void muse_proxy_get_config(char *host, size_t cap, uint16_t *port);
bool muse_proxy_valid_config(const char *host, uint16_t port);
bool muse_proxy_save_config(const char *host, uint16_t port);
int muse_proxy_route_info(char *host, size_t cap, uint16_t *port);

esp_err_t muse_tls_connect_proxy(const char *host, int port, int timeout_ms, esp_tls_t **out);
int muse_https_exchange(const char *url, const char *method, const char *auth,
                        const char *body, int timeout_ms, int *status,
                        char **resp, size_t *resp_len);

#ifdef __cplusplus
}
#endif
