#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "passport_workbuddy.h"
#include "workbuddy_config.h"
#include "nvs.h"

typedef struct { char key[24], value[176]; } entry_t;
static entry_t durable[16], pending[16];
static bool opened, writable;

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle) {
    assert(strcmp(ns, "passport_workbuddy") == 0 && !opened);
    opened = true; writable = mode == NVS_READWRITE;
    memcpy(pending, durable, sizeof(pending)); *handle = 1; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1 && opened); opened = false; }
static entry_t *find(entry_t *entries, const char *key) {
    for (unsigned i = 0; i < 16; ++i) if (!strcmp(entries[i].key, key)) return &entries[i];
    return NULL;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *size) {
    assert(handle == 1 && opened);
    entry_t *e = find(durable, key); if (!e) return ESP_ERR_NVS_NOT_FOUND;
    size_t needed = strlen(e->value) + 1;
    if (!out) { *size = needed; return ESP_OK; }
    if (*size < needed) return ESP_ERR_INVALID_SIZE;
    memcpy(out, e->value, needed); *size = needed; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
    assert(handle == 1 && opened && writable);
    entry_t *e = find(pending, key);
    if (!e) for (unsigned i = 0; i < 16; ++i) if (!pending[i].key[0]) { e = &pending[i]; break; }
    assert(e && strlen(key) < sizeof(e->key) && strlen(value) < sizeof(e->value));
    strcpy(e->key, key); strcpy(e->value, value); return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    assert(handle == 1 && opened && writable);
    entry_t *e = find(pending, key); if (!e) return ESP_ERR_NVS_NOT_FOUND;
    memset(e, 0, sizeof(*e)); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    assert(handle == 1 && opened && writable);
    memcpy(durable, pending, sizeof(durable)); return ESP_OK;
}

int main(void) {
    assert(passport_workbuddy_valid_relay_url("https://relay.example.com"));
    assert(passport_workbuddy_valid_relay_url("https://192.168.1.20:8787/path"));
    assert(!passport_workbuddy_valid_relay_url("http://relay.example.com"));
    assert(!passport_workbuddy_valid_relay_url("https://relay.example.com?x=1"));
    assert(!passport_workbuddy_valid_relay_url("https://"));
    assert(passport_workbuddy_valid_token("0123456789abcdefdeviceToken"));
    assert(!passport_workbuddy_valid_token("short"));
    assert(!passport_workbuddy_valid_token("0123456789abcdef with space"));

    bool setup = true;
    char url[168] = {0}, host[16] = {0};
    uint16_t port = 0;
    passport_workbuddy_config_status(&setup, url, sizeof(url), host, sizeof(host), &port);
    assert(!setup && !url[0] && !host[0] && port == 0);

    assert(passport_workbuddy_configure("https://relay.example.com", "0123456789abcdefdeviceToken",
                                        "192.168.1.20", 7897) == ESP_OK);
    passport_workbuddy_config_status(&setup, url, sizeof(url), host, sizeof(host), &port);
    assert(setup && !strcmp(url, "https://relay.example.com"));
    assert(!strcmp(host, "192.168.1.20") && port == 7897);
    assert(passport_workbuddy_proxy_route_info(host, sizeof(host), &port) == 1);

    assert(passport_workbuddy_configure("https://relay.example.com", "bad", NULL, 0) == ESP_ERR_INVALID_ARG);
    assert(passport_workbuddy_configure("https://relay.example.com", "0123456789abcdefdeviceToken",
                                        "192.168.1.20", 0) == ESP_ERR_INVALID_ARG);
    return 0;
}
