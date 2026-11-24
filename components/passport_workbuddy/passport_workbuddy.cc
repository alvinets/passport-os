// WorkBuddy adapter for Passport OS.
// The device never holds an OAuth client_secret. It talks to a relay that owns
// the WorkBuddy authorization and exposes a bounded, streaming device protocol:
//   GET  /v1/status                  -> {"ok":true,"agent":"..."}
//   POST /v1/turn/begin              -> {"turn_id":"..."}
//   POST /v1/turn/{id}/audio         -> {"chunk":"<base64 WAV bytes>"}
//   POST /v1/turn/{id}/end           -> {"state":"working"}
//   GET  /v1/turn/{id}               -> {"state":"working|done|error","text":"..."}
//   POST /v1/turn/{id}/cancel
#include "passport_workbuddy.h"
#include "workbuddy_config.h"
#include "passport_muse.h"
#include "wifi_manager.h"
#include "bsp_audio.h"
#include "bsp_memory.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
const char *TAG = "passport_wb";
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
passport_workbuddy_snapshot_t s_view{};
std::atomic<bool> s_active{false}, s_exit{false}, s_worker{false};
std::atomic<bool> s_pressed{false}, s_cancel{false}, s_reset{false};
char s_url[168], s_token[136];

void view(passport_workbuddy_stage_t stage, const char *detail) {
    portENTER_CRITICAL(&s_lock);
    s_view.stage = stage;
    strlcpy(s_view.detail, detail, sizeof(s_view.detail));
    portEXIT_CRITICAL(&s_lock);
}

bool build_url(const char *path, char *out, size_t cap) {
    size_t n = strlen(s_url);
    while (n && s_url[n - 1] == '/') --n;
    int written = snprintf(out, cap, "%.*s%s", (int)n, s_url, path);
    return written > 0 && (size_t)written < cap;
}

struct reply_t {
    int status = 0;
    cJSON *json = nullptr;
};

reply_t request(const char *method, const char *path, const char *body, int timeout_ms) {
    reply_t r;
    char url[256], auth[160];
    if (!build_url(path, url, sizeof(url))) return r;
    snprintf(auth, sizeof(auth), "Bearer %s", s_token);
    char *response = nullptr;
    size_t len = 0;
    if (muse_https_exchange(url, method, auth, body, timeout_ms, &r.status, &response, &len) != 0 || !response) {
        free(response);
        return r;
    }
    r.json = cJSON_ParseWithLength(response, len);
    free(response);
    return r;
}

const char *json_string(cJSON *obj, const char *key) {
    if (!obj) return nullptr;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}

bool json_ok(cJSON *obj) {
    cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, "ok") : nullptr;
    return cJSON_IsTrue(v);
}

bool send_chunk(const char *turn_id, const uint8_t *pcm, size_t frames) {
    const size_t bytes = frames * 2;
    char *b64 = nullptr;
    cJSON *obj = nullptr;
    char *printed = nullptr;
    b64 = (char *)malloc(bytes * 4 / 3 + 8);
    if (!b64) return false;
    size_t n = workbuddy_base64((const uint8_t *)pcm, bytes, b64);
    b64[n] = 0;
    obj = cJSON_CreateObject();
    if (obj) cJSON_AddStringToObject(obj, "chunk", b64);
    printed = obj ? cJSON_PrintUnformatted(obj) : nullptr;
    cJSON_Delete(obj);
    free(b64);
    if (!printed) return false;
    char path[192];
    snprintf(path, sizeof(path), "/v1/turn/%s/audio", turn_id);
    reply_t r = request("POST", path, printed, 8000);
    free(printed);
    bool ok = r.status == 200;
    cJSON_Delete(r.json);
    return ok;
}

bool turn_begin(char *turn_id, size_t cap) {
    reply_t r = request("POST", "/v1/turn/begin", "{}", 8000);
    const char *id = json_string(r.json, "turn_id");
    bool ok = r.status == 200 && id && *id && strlen(id) < cap;
    if (ok) strlcpy(turn_id, id, cap);
    else view(PASSPORT_WORKBUDDY_ERROR, r.status == 401 ? "WorkBuddy 授权失效，请用手机重新设置" :
              r.status ? "WorkBuddy 中继暂不可用，稍后重试" : "网络不可用，请检查 Wi-Fi 或代理");
    cJSON_Delete(r.json);
    return ok;
}

bool turn_end(const char *turn_id) {
    char path[192];
    snprintf(path, sizeof(path), "/v1/turn/%s/end", turn_id);
    reply_t r = request("POST", path, "{}", 8000);
    bool ok = r.status == 200;
    cJSON_Delete(r.json);
    return ok;
}

void turn_cancel(const char *turn_id) {
    if (!*turn_id) return;
    char path[200];
    snprintf(path, sizeof(path), "/v1/turn/%s/cancel", turn_id);
    reply_t r = request("POST", path, "{}", 5000);
    cJSON_Delete(r.json);
}

void worker(void *) {
    char turn_id[96] = {0};
    int64_t began = 0, next_check = 0, next_poll = 0, poll_until = 0;
    bool recording = false, receiving = false, capture_wifi = false;
    static uint8_t stage[8192];
    size_t staged = 0;
    muse_proxy_load_config();

    while (!s_exit) {
        if (s_reset.exchange(false)) {
            wb_set_str("setup_complete", "");
            wb_set_str("device_token", "");
            wb_set_str("relay_url", "");
            view(PASSPORT_WORKBUDDY_NEEDS_SETUP, "请在手机设置页重新配置 WorkBuddy");
            break;
        }
        if (!WifiManager::GetInstance().IsConnected()) {
            view(PASSPORT_WORKBUDDY_ERROR, "Wi-Fi 未连接，连上网络后自动重试");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_check) {
            next_check = now + 10000000;
            reply_t r = request("GET", "/v1/status", nullptr, 6000);
            const char *agent = json_string(r.json, "agent");
            bool ok = r.status == 200 && json_ok(r.json);
            if (ok) {
                portENTER_CRITICAL(&s_lock);
                if (agent) strlcpy(s_view.agent, agent, sizeof(s_view.agent));
                else strlcpy(s_view.agent, "WorkBuddy", sizeof(s_view.agent));
                portEXIT_CRITICAL(&s_lock);
                if (!receiving && !recording) view(PASSPORT_WORKBUDDY_READY, "按住上键说话");
            } else {
                view(PASSPORT_WORKBUDDY_ERROR, r.status == 401 ? "WorkBuddy 授权失效，请用手机重新设置" :
                     r.status ? "WorkBuddy 中继暂不可用，稍后重试" : "网络不可用，请检查 Wi-Fi 或代理");
            }
            cJSON_Delete(r.json);
        }
        if (s_cancel.exchange(false) && (recording || receiving)) {
            if (recording) { recording = false; bsp_audio_deinit(); }
            turn_cancel(turn_id);
            receiving = false; turn_id[0] = 0;
            view(PASSPORT_WORKBUDDY_READY, "已停止等待");
        }
        bool pressed = s_pressed;
        if (pressed && !recording && !receiving && !s_exit) {
            if (turn_begin(turn_id, sizeof(turn_id))) {
                bsp_memory_phase("wb.record");
                WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
                capture_wifi = true;
                if (bsp_audio_init_capture() == ESP_OK && bsp_audio_set_format(16000, 16, 1) == ESP_OK) {
                    uint8_t wav[44];
                    workbuddy_wav_header(wav, 16000);
                    staged = 0;
                    memcpy(stage, wav, sizeof(wav));
                    staged = sizeof(wav);
                    recording = true;
                    began = esp_timer_get_time();
                    view(PASSPORT_WORKBUDDY_LISTENING, "松开上键发送");
                } else {
                    bsp_audio_deinit();
                    turn_cancel(turn_id); turn_id[0] = 0;
                    view(PASSPORT_WORKBUDDY_ERROR, "麦克风未就绪，请重试");
                }
            }
        }
        if (recording) {
            int16_t pcm[256];
            if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
                bsp_audio_deinit(); recording = false;
                turn_cancel(turn_id); turn_id[0] = 0;
                view(PASSPORT_WORKBUDDY_ERROR, "录音失败，请重试");
            } else {
                unsigned peak = 0;
                for (int16_t s : pcm) { unsigned v = s < 0 ? (unsigned)-s : (unsigned)s; if (v > peak) peak = v; }
                portENTER_CRITICAL(&s_lock); s_view.level = peak; portEXIT_CRITICAL(&s_lock);
                size_t bytes = sizeof(pcm);
                if (staged + bytes > sizeof(stage)) {
                    if (!send_chunk(turn_id, stage, staged / 2)) {
                        bsp_audio_deinit(); recording = false;
                        turn_cancel(turn_id); turn_id[0] = 0;
                        view(PASSPORT_WORKBUDDY_ERROR, "语音上传失败，请检查网络后重试");
                    } else staged = 0;
                }
                if (recording) { memcpy(stage + staged, pcm, bytes); staged += bytes; }
            }
            if (recording && (!pressed || esp_timer_get_time() - began >= 30000000)) {
                recording = false;
                bsp_audio_deinit();
                if (staged) {
                    if (send_chunk(turn_id, stage, staged / 2)) staged = 0;
                    else { view(PASSPORT_WORKBUDDY_ERROR, "语音上传失败，请重试"); turn_cancel(turn_id); turn_id[0] = 0; }
                }
                if (staged == 0 && *turn_id) {
                    if (turn_end(turn_id)) {
                        receiving = true;
                        bsp_memory_phase("wb.reply");
                        view(PASSPORT_WORKBUDDY_WORKING, "正在思考");
                        next_poll = 0;
                        poll_until = esp_timer_get_time() + 60000000;
                    } else {
                        turn_cancel(turn_id); turn_id[0] = 0;
                        view(PASSPORT_WORKBUDDY_ERROR, "WorkBuddy 未接受语音，请重试");
                    }
                }
            }
        }
        if (receiving && esp_timer_get_time() >= next_poll) {
            next_poll = esp_timer_get_time() + 500000;
            char path[192];
            snprintf(path, sizeof(path), "/v1/turn/%s", turn_id);
            reply_t r = request("GET", path, nullptr, 6000);
            const char *state = json_string(r.json, "state");
            const char *text = json_string(r.json, "text");
            if (r.status == 200 && state && !strcmp(state, "done")) {
                portENTER_CRITICAL(&s_lock);
                if (text && *text) {
                    strlcpy(s_view.reply, text, sizeof(s_view.reply));
                    ++s_view.reply_revision;
                }
                portEXIT_CRITICAL(&s_lock);
                receiving = false; turn_id[0] = 0;
                view(PASSPORT_WORKBUDDY_REPLY, "继续说话或翻看回复");
            } else if (r.status == 200 && state && !strcmp(state, "error")) {
                const char *err = json_string(r.json, "error");
                view(PASSPORT_WORKBUDDY_ERROR, err && *err ? err : "WorkBuddy 回复失败，请重试");
                receiving = false; turn_id[0] = 0;
            } else if (r.status != 200 || esp_timer_get_time() >= poll_until) {
                view(PASSPORT_WORKBUDDY_ERROR, "WorkBuddy 回复超时，请在手机查看");
                receiving = false; turn_id[0] = 0;
            }
            cJSON_Delete(r.json);
        }
        if (!recording && capture_wifi) {
            WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::BALANCED);
            capture_wifi = false;
        }
        vTaskDelay(pdMS_TO_TICKS(recording || receiving ? 20 : 200));
    }
    if (recording) bsp_audio_deinit();
    if (capture_wifi) WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::BALANCED);
    muse_proxy_cancel();
    s_worker = false;
    vTaskDelete(nullptr);
}
}  // namespace

extern "C" esp_err_t passport_workbuddy_start(void) {
    if (s_active.exchange(true)) return ESP_ERR_INVALID_STATE;
    bool setup = false;
    char host[16] = {0};
    uint16_t port = 0;
    passport_workbuddy_config_status(&setup, s_url, sizeof(s_url), host, sizeof(host), &port);
    wb_get_str("device_token", s_token, sizeof(s_token));
    if (!setup || !passport_workbuddy_valid_relay_url(s_url) || !passport_workbuddy_valid_token(s_token)) {
        view(PASSPORT_WORKBUDDY_NEEDS_SETUP, "请在手机设置页填写中继地址与令牌");
        return ESP_OK;
    }
    if (muse_proxy_save_config(host, port) && muse_proxy_valid_config(host, port)) muse_proxy_load_config();
    s_exit = false; s_pressed = false; s_cancel = false; s_reset = false;
    portENTER_CRITICAL(&s_lock);
    s_view = {};
    s_view.setup_complete = true;
    strlcpy(s_view.agent, "WorkBuddy", sizeof(s_view.agent));
    portEXIT_CRITICAL(&s_lock);
    view(PASSPORT_WORKBUDDY_CONNECTING, "正在连接 WorkBuddy");
    s_worker = true;
    TaskHandle_t task = nullptr;
    if (xTaskCreate(worker, "passport_workbuddy", 10240, nullptr, 4, &task) != pdPASS) {
        s_worker = false;
        view(PASSPORT_WORKBUDDY_ERROR, "内存不足，请返回重试");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

extern "C" bool passport_workbuddy_stop(void) {
    if (!s_active) return true;
    s_exit = true;
    s_pressed = false;
    while (s_worker) vTaskDelay(pdMS_TO_TICKS(10));
    s_active = false;
    return true;
}

extern "C" void passport_workbuddy_key(bool pressed, bool cancel) {
    s_pressed = pressed;
    if (cancel) s_cancel = true;
}
extern "C" void passport_workbuddy_confirm(void) { s_cancel = true; }
extern "C" void passport_workbuddy_reset(void) { s_reset = true; }
extern "C" void passport_workbuddy_snapshot(passport_workbuddy_snapshot_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_view;
    portEXIT_CRITICAL(&s_lock);
}
extern "C" void passport_workbuddy_log_diagnostics(void) {
    passport_workbuddy_snapshot_t s;
    passport_workbuddy_snapshot(&s);
    char host[16] = {0};
    uint16_t port = 0;
    passport_workbuddy_config_status(nullptr, nullptr, 0, host, sizeof(host), &port);
    ESP_LOGI(TAG, "stage=%d reply=%u revision=%u proxy=%s:%u", (int)s.stage,
             (unsigned)strlen(s.reply), (unsigned)s.reply_revision, host, (unsigned)port);
}
