#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PASSPORT_WORKBUDDY_NEEDS_SETUP,
    PASSPORT_WORKBUDDY_CONNECTING,
    PASSPORT_WORKBUDDY_READY,
    PASSPORT_WORKBUDDY_LISTENING,
    PASSPORT_WORKBUDDY_WORKING,
    PASSPORT_WORKBUDDY_REPLY,
    PASSPORT_WORKBUDDY_ERROR,
} passport_workbuddy_stage_t;

typedef struct {
    passport_workbuddy_stage_t stage;
    bool setup_complete;
    uint16_t level;
    uint32_t reply_revision;
    char agent[32];
    char detail[96];
    char reply[1024];
} passport_workbuddy_snapshot_t;

/* Relay URL must be https://<host>[:port][/path] with no query or fragment. */
bool passport_workbuddy_valid_relay_url(const char *url);
bool passport_workbuddy_valid_token(const char *token);
esp_err_t passport_workbuddy_configure(const char *relay_url, const char *token,
                                       const char *proxy_host, uint16_t proxy_port);
void passport_workbuddy_config_status(bool *setup_complete, char *relay_url, size_t url_cap,
                                      char *proxy_host, size_t host_cap, uint16_t *proxy_port);

esp_err_t passport_workbuddy_start(void);
bool passport_workbuddy_stop(void);
void passport_workbuddy_key(bool pressed, bool cancel);
void passport_workbuddy_confirm(void);
void passport_workbuddy_reset(void);
void passport_workbuddy_snapshot(passport_workbuddy_snapshot_t *out);
void passport_workbuddy_log_diagnostics(void);
/* Current route: 0 direct, 1 HTTP CONNECT proxy configured for the relay. */
int passport_workbuddy_proxy_route_info(char *host, size_t cap, uint16_t *port);

/* UTF-8-safe reply paging: 5 lines x 26 half-width columns. */
size_t workbuddy_reply_page(const char *text, size_t at, char *out, size_t cap);
size_t workbuddy_base64(const uint8_t *in, size_t n, char *out);
void workbuddy_wav_header(uint8_t h[44], uint32_t rate);

#ifdef __cplusplus
}
#endif
