// main/ui/ui_appearance.c —— 全机亮/暗外观状态与持久化。见 ui_appearance.h。
#include "ui/ui_appearance.h"

#include "apps/voice/time_sync.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

#include <time.h>

#define TAG "appearance"
#define NVS_NAMESPACE "system_prefs"
#define KEY_MODE "appearance"
#define KEY_SUN_OK "sunok"
#define KEY_RISE "srise"
#define KEY_SET "sset"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ui_appearance_mode_t s_mode = UI_APPEARANCE_AUTO;
static bool s_sun_valid;
static uint16_t s_sunrise_min;
static uint16_t s_sunset_min;
static bool s_dark = true;

static void persist_mode(ui_appearance_mode_t mode)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u8(handle, KEY_MODE, (uint8_t)mode) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

static void persist_sun_times(bool valid, uint16_t rise, uint16_t set)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    esp_err_t result = nvs_set_u8(handle, KEY_SUN_OK, valid ? 1 : 0);
    if (result == ESP_OK) result = nvs_set_u16(handle, KEY_RISE, rise);
    if (result == ESP_OK) result = nvs_set_u16(handle, KEY_SET, set);
    if (result == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

// 本地分钟数（0..1439）。未校时返回 -1：自动模式据此保持暗。
static int local_minute(void)
{
    if (!time_sync_valid()) return -1;
    int64_t utc = (int64_t)time(NULL);
    const int64_t base = time_sync_last_epoch();
    if (utc < base) utc = base; /* settimeofday 未生效时退回校时基线 */
    const int64_t local = utc + time_sync_tz_offset_sec();
    const int day = (int)(((local % 86400) + 86400) % 86400);
    return day / 60;
}

// 重新计算有效亮暗；返回是否改变。
static bool recompute(void)
{
    bool sun_valid;
    uint16_t rise;
    uint16_t set;
    portENTER_CRITICAL(&s_mux);
    sun_valid = s_sun_valid;
    rise = s_sunrise_min;
    set = s_sunset_min;
    portEXIT_CRITICAL(&s_mux);

    const bool dark = ui_appearance_decide(ui_appearance_mode(), local_minute(), sun_valid, rise, set);
    bool changed = false;
    portENTER_CRITICAL(&s_mux);
    if (dark != s_dark) {
        s_dark = dark;
        changed = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return changed;
}

void ui_appearance_init(void)
{
    uint8_t mode = (uint8_t)UI_APPEARANCE_AUTO;
    uint8_t sun_ok = 0;
    uint16_t rise = 0;
    uint16_t set = 0;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, KEY_MODE, &mode) != ESP_OK || mode >= UI_APPEARANCE_COUNT)
            mode = (uint8_t)UI_APPEARANCE_AUTO;
        if (nvs_get_u8(handle, KEY_SUN_OK, &sun_ok) != ESP_OK) sun_ok = 0;
        if (nvs_get_u16(handle, KEY_RISE, &rise) != ESP_OK) rise = 0;
        if (nvs_get_u16(handle, KEY_SET, &set) != ESP_OK) set = 0;
        nvs_close(handle);
    }
    portENTER_CRITICAL(&s_mux);
    s_mode = (ui_appearance_mode_t)mode;
    s_sun_valid = sun_ok != 0 && rise < 24 * 60 && set < 24 * 60 && rise != set;
    s_sunrise_min = rise;
    s_sunset_min = set;
    portEXIT_CRITICAL(&s_mux);
    (void)recompute();
    ESP_LOGI(TAG, "Appearance mode=%s dark=%d sun=%d..%d", ui_appearance_mode_name(s_mode),
             (int)ui_appearance_is_dark(), (int)rise, (int)set);
}

ui_appearance_mode_t ui_appearance_mode(void)
{
    portENTER_CRITICAL(&s_mux);
    const ui_appearance_mode_t mode = s_mode;
    portEXIT_CRITICAL(&s_mux);
    return mode;
}

bool ui_appearance_set_mode(ui_appearance_mode_t mode)
{
    if ((unsigned)mode >= UI_APPEARANCE_COUNT) mode = UI_APPEARANCE_AUTO;
    portENTER_CRITICAL(&s_mux);
    const bool same = s_mode == mode;
    s_mode = mode;
    portEXIT_CRITICAL(&s_mux);
    if (!same) persist_mode(mode);
    const bool changed = recompute();
    ESP_LOGI(TAG, "Mode -> %s (dark=%d)", ui_appearance_mode_name(ui_appearance_mode()),
             (int)ui_appearance_is_dark());
    return changed;
}

const char *ui_appearance_mode_name(ui_appearance_mode_t mode)
{
    switch (mode) {
        case UI_APPEARANCE_LIGHT: return "亮";
        case UI_APPEARANCE_DARK: return "暗";
        default: return "自动";
    }
}

void ui_appearance_set_sun_times(bool valid, uint16_t sunrise_min, uint16_t sunset_min)
{
    if (valid && (sunrise_min >= 24 * 60 || sunset_min >= 24 * 60 || sunrise_min == sunset_min))
        valid = false;
    portENTER_CRITICAL(&s_mux);
    s_sun_valid = valid;
    if (valid) {
        s_sunrise_min = sunrise_min;
        s_sunset_min = sunset_min;
    }
    portEXIT_CRITICAL(&s_mux);
    if (valid) persist_sun_times(true, sunrise_min, sunset_min);
    (void)recompute(); /* 由 1 Hz tick 通知 UI，避免在天气任务里碰 LVGL */
}

bool ui_appearance_sun_times(uint16_t *sunrise_min, uint16_t *sunset_min)
{
    bool valid;
    portENTER_CRITICAL(&s_mux);
    valid = s_sun_valid;
    if (sunrise_min) *sunrise_min = s_sunrise_min;
    if (sunset_min) *sunset_min = s_sunset_min;
    portEXIT_CRITICAL(&s_mux);
    return valid;
}

bool ui_appearance_is_dark(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool dark = s_dark;
    portEXIT_CRITICAL(&s_mux);
    return dark;
}

bool ui_appearance_tick(void) { return recompute(); }
