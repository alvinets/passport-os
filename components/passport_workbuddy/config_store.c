/* WorkBuddy adapter settings. NVS namespace is isolated from Muse. */
#include "workbuddy_config.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *NS = "passport_workbuddy";

bool wb_get_str(const char *key, char *out, size_t cap) {
    if (cap) out[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = nvs_get_str(h, key, out, &cap);
    nvs_close(h);
    return e == ESP_OK;
}
bool wb_set_str(const char *key, const char *value) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, key, value);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}
static bool erase_key(const char *key) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_erase_key(h, key);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

bool passport_workbuddy_valid_relay_url(const char *url) {
    if (!url || strncmp(url, "https://", 8) != 0) return false;
    const char *host = url + 8;
    if (!*host || strchr(host, '/') == host) return false;
    if (strchr(url, '?') || strchr(url, '#') || strchr(url, ' ')) return false;
    size_t len = strlen(url);
    return len >= 12 && len <= 160;
}
bool passport_workbuddy_valid_token(const char *token) {
    if (!token) return false;
    size_t len = strlen(token);
    if (len < 16 || len > 128) return false;
    for (const char *p = token; *p; ++p) if (*p <= 32 || *p >= 127) return false;
    return true;
}

esp_err_t passport_workbuddy_configure(const char *relay_url, const char *token,
                                       const char *proxy_host, uint16_t proxy_port) {
    if (!passport_workbuddy_valid_relay_url(relay_url) || !passport_workbuddy_valid_token(token))
        return ESP_ERR_INVALID_ARG;
    if ((!proxy_host || !*proxy_host) != (proxy_port == 0)) return ESP_ERR_INVALID_ARG;
    if (!wb_set_str("relay_url", relay_url) || !wb_set_str("device_token", token)) return ESP_FAIL;
    char proxy[40] = {0};
    if (proxy_host && *proxy_host) snprintf(proxy, sizeof(proxy), "%s:%u", proxy_host, (unsigned)proxy_port);
    if (!wb_set_str("proxy", proxy) || !wb_set_str("setup_complete", "yes")) return ESP_FAIL;
    return ESP_OK;
}

void passport_workbuddy_config_status(bool *setup, char *url, size_t url_cap,
                                      char *host, size_t host_cap, uint16_t *port) {
    char flag[8] = {0};
    if (setup) *setup = wb_get_str("setup_complete", flag, sizeof(flag)) && !strcmp(flag, "yes");
    if (url && url_cap) wb_get_str("relay_url", url, url_cap);
    char stored[40] = {0};
    wb_get_str("proxy", stored, sizeof(stored));
    const char *colon = strchr(stored, ':');
    if (host && host_cap) {
        if (colon && (size_t)(colon - stored) < host_cap) {
            memcpy(host, stored, (size_t)(colon - stored));
            host[colon - stored] = 0;
        } else host[0] = 0;
    }
    if (port) *port = colon ? (uint16_t)strtoul(colon + 1, NULL, 10) : 0;
}

int passport_workbuddy_proxy_route_info(char *host, size_t cap, uint16_t *port) {
    passport_workbuddy_config_status(NULL, NULL, 0, host, cap, port);
    return (port && *port && host && host[0]) ? 1 : 0;
}
