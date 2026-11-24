#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

// NVS 初始化后、Launcher 创建前调用，恢复整机设置并立即应用。
void settings_bootstrap(void);
void settings_tick(void);

// Shared by Settings and app volume keys; applies and persists only volume.
void settings_set_volume(uint8_t percent);
// Input-task APIs: persist first; failure leaves the applied setting unchanged.
bool settings_save_volume(uint8_t percent);
bool settings_save_brightness(uint8_t percent);
uint8_t settings_get_volume(void);
uint8_t settings_get_brightness(void);

// 手机网页切换「头像风格」时改主题(彩色人像 ↔ 像素风),与设置页同一份持久化。
// 传入 ui_theme_t 的枚举值;非法值返回 false,沿用当前主题,不静默降级。
bool settings_save_theme(uint8_t theme);

extern const passport_app_t g_settings_app;

#ifdef __cplusplus
}
#endif
