// main/apps/weather/app_weather.cc —— 天气应用。
//
// 版面：顶栏(信号/标题/电池) → 城市 → 当前温度+天气图形 → 日出日落 →
// 湿度/风/PM2.5 → 未来 24 小时(5 列) → 未来 3 天(3 行) → 底部提示。
// 数据来自 home_weather 的共享快照（一次 HTTPS 同时填充 current/hourly/daily，
// 并给亮暗自动模式提供日出日落），本应用不另发请求。
#include "apps/app_base.h"
#include "apps/home/home_profile.h"
#include "apps/home/home_weather.h"
#include "launcher/app_registry.h"

#include "app_fonts.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui_pixel.h"
#include "wifi_manager.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr const char *kTag = "weather_app";
constexpr int kHourColumns = 5;

// 天气图形配色：与主题无关，亮暗两种底色下都能看清。
constexpr uint32_t kSun = 0xF7C948;
constexpr uint32_t kCloud = 0xA8BECC;
constexpr uint32_t kCloudDeep = 0x7593AC;
constexpr uint32_t kRain = 0x3B72B8;
constexpr uint32_t kSnow = 0xEAF2FA;
constexpr uint32_t kBolt = 0xF2A93B;
constexpr uint32_t kFog = 0x9AA6B2;

lv_obj_t *s_screen;
lv_obj_t *s_city;
lv_obj_t *s_temp;
lv_obj_t *s_cond;
lv_obj_t *s_sun;
lv_obj_t *s_metrics[3];
lv_obj_t *s_art;
lv_obj_t *s_hour_time[kHourColumns];
lv_obj_t *s_hour_temp[kHourColumns];
lv_obj_t *s_day_name[3];
lv_obj_t *s_day_cond[3];
lv_obj_t *s_day_temp[3];
lv_obj_t *s_wifi[3];
ui_pixel_battery_t s_battery;
lv_timer_t *s_timer;
TaskHandle_t s_task;
volatile bool s_busy;
volatile bool s_dirty = true;
uint32_t s_seen_version = 0xFFFFFFFFu;

void art_block(int x, int y, int w, int h, uint32_t color)
{
    if (!s_art) return;
    lv_obj_t *part = lv_obj_create(s_art);
    lv_obj_remove_flag(part, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(part, x, y);
    lv_obj_set_size(part, w, h);
    lv_obj_set_style_radius(part, 0, 0);
    lv_obj_set_style_border_width(part, 0, 0);
    lv_obj_set_style_pad_all(part, 0, 0);
    lv_obj_set_style_bg_color(part, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
}

void art_sun(int cx, int cy)
{
    art_block(cx - 7, cy - 7, 14, 14, kSun);
    art_block(cx - 9, cy - 2, 2, 4, kSun);
    art_block(cx + 8, cy - 2, 2, 4, kSun);
    art_block(cx - 2, cy - 9, 4, 2, kSun);
    art_block(cx - 2, cy + 8, 4, 2, kSun);
}

void art_cloud(int x, int y, bool deep)
{
    const uint32_t color = deep ? kCloudDeep : kCloud;
    art_block(x, y, 30, 10, color);
    art_block(x + 6, y - 6, 14, 8, color);
    art_block(x + 18, y - 3, 10, 6, color);
}

void art_rain(int x, int y, int drops)
{
    for (int i = 0; i < drops; ++i) art_block(x + 4 + i * 9, y, 3, 7, kRain);
}

void art_snow(int x, int y, int flakes)
{
    for (int i = 0; i < flakes; ++i) art_block(x + 4 + i * 9, y, 4, 4, kSnow);
}

void art_fog(int x, int y)
{
    art_block(x, y, 26, 3, kFog);
    art_block(x + 4, y + 6, 26, 3, kFog);
}

// 按 WMO 码画 40x34 的像素图形（坐标都在 s_art 的局部空间内）。
void draw_art(uint16_t code)
{
    if (!s_art) return;
    lv_obj_clean(s_art);
    if (code == 0 || code == 1) {
        art_sun(20, 16);
        return;
    }
    if (code == 2) {
        art_sun(28, 10);
        art_cloud(2, 16, false);
        return;
    }
    if (code == 3) {
        art_cloud(4, 12, false);
        return;
    }
    if (code == 45 || code == 48) {
        art_cloud(4, 6, false);
        art_fog(4, 20);
        return;
    }
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        art_cloud(4, 4, false);
        art_rain(4, 18, 3);
        return;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        art_cloud(4, 4, false);
        art_snow(4, 18, 3);
        return;
    }
    if (code >= 95) {
        art_cloud(4, 4, true);
        art_block(18, 18, 3, 6, kBolt);
        art_block(15, 23, 3, 5, kBolt);
        return;
    }
    art_cloud(4, 12, false);
}

const char *cond_text(uint16_t code) { return home_weather_code_text(code); }

void fmt_clock(int minutes, char *out, size_t capacity)
{
    if (minutes < 0 || minutes >= 24 * 60) {
        snprintf(out, capacity, "--:--");
        return;
    }
    snprintf(out, capacity, "%02d:%02d", minutes / 60, minutes % 60);
}

void build_ui()
{
    s_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    ui_pixel_background(s_screen);

    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(top, 12, 18, s_wifi);
    lv_obj_t *title = ui_pixel_label(top, "天气", &buddy_font_16, UI_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 42, 0);
    s_battery = ui_pixel_battery_create(top, 206, 6, UI_TEXT, &lv_font_montserrat_14);
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());

    s_city = ui_pixel_label(s_screen, "", &buddy_font_16, UI_SUCCESS);
    lv_obj_set_pos(s_city, 132, 28);
    lv_obj_set_width(s_city, 100);
    lv_obj_set_style_text_align(s_city, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_city, LV_LABEL_LONG_MODE_DOTS);

    s_temp = ui_pixel_label(s_screen, "--°", &lv_font_montserrat_32, UI_TEXT);
    lv_obj_set_pos(s_temp, 12, 36);

    s_cond = ui_pixel_label(s_screen, "暂无数据", &buddy_font_16, UI_DANGER);
    lv_obj_set_pos(s_cond, 12, 72);

    s_art = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_art);
    lv_obj_remove_flag(s_art, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_art, 150, 34);
    lv_obj_set_size(s_art, 44, 36);

    s_sun = ui_pixel_label(s_screen, "", &home_font_12, UI_TEXT_DIM);
    lv_obj_set_pos(s_sun, 12, 92);
    lv_obj_set_width(s_sun, 216);
    lv_label_set_long_mode(s_sun, LV_LABEL_LONG_MODE_DOTS);

    for (int i = 0; i < 3; ++i) {
        s_metrics[i] = ui_pixel_label(s_screen, "--", &home_font_12, UI_TEXT_DIM);
        lv_obj_set_pos(s_metrics[i], 12 + i * 72, 108);
        lv_obj_set_width(s_metrics[i], 72);
        lv_label_set_long_mode(s_metrics[i], LV_LABEL_LONG_MODE_DOTS);
    }

    ui_pixel_panel_create(s_screen, 12, 122, 216, 1, UI_BORDER);

    lv_obj_t *hour_hdr = ui_pixel_label(s_screen, "未来 24 小时", &buddy_font_16, UI_SECONDARY);
    lv_obj_set_pos(hour_hdr, 12, 126);

    for (int i = 0; i < kHourColumns; ++i) {
        const int x = 12 + i * 44;
        s_hour_time[i] = ui_pixel_label(s_screen, "--:--", &lv_font_montserrat_14, UI_TEXT_DIM);
        lv_obj_set_pos(s_hour_time[i], x, 148);
        s_hour_temp[i] = ui_pixel_label(s_screen, "--°", &lv_font_montserrat_14, UI_TEXT);
        lv_obj_set_pos(s_hour_temp[i], x, 164);
    }

    ui_pixel_panel_create(s_screen, 12, 184, 216, 1, UI_BORDER);

    lv_obj_t *day_hdr = ui_pixel_label(s_screen, "未来 3 天", &buddy_font_16, UI_SECONDARY);
    lv_obj_set_pos(day_hdr, 12, 188);

    static const char *kDayNames[3] = {"今天", "明天", "后天"};
    for (int i = 0; i < 3; ++i) {
        const int y = 210 + i * 19;
        s_day_name[i] = ui_pixel_label(s_screen, kDayNames[i], &buddy_font_16, UI_TEXT_DIM);
        lv_obj_set_pos(s_day_name[i], 12, y);
        lv_obj_set_width(s_day_name[i], 44);
        s_day_cond[i] = ui_pixel_label(s_screen, "--", &buddy_font_16, UI_TEXT);
        lv_obj_set_pos(s_day_cond[i], 58, y);
        lv_obj_set_width(s_day_cond[i], 88);
        lv_label_set_long_mode(s_day_cond[i], LV_LABEL_LONG_MODE_DOTS);
        s_day_temp[i] = ui_pixel_label(s_screen, "--°/--°", &lv_font_montserrat_14, UI_TEXT);
        lv_obj_set_pos(s_day_temp[i], 160, y + 1);
        lv_obj_set_width(s_day_temp[i], 68);
        lv_obj_set_style_text_align(s_day_temp[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    ui_pixel_footer(s_screen, "确定刷新", "长按返回", &buddy_font_16);
}

void update_ui();

void refresh_done()
{
    s_busy = false;
    s_dirty = true; /* 下一次 tick 在 LVGL 线程里刷新界面 */
}

void refresh_task(void *)
{
    home_profile_t profile;
    home_profile_get(&profile);
    if (profile.lat[0] && profile.lon[0]) {
        const esp_err_t weather = home_weather_refresh(profile.lat, profile.lon);
        if (weather != ESP_OK) ESP_LOGW(kTag, "Weather refresh failed: %s", esp_err_to_name(weather));
        const esp_err_t air = home_air_quality_refresh(profile.lat, profile.lon);
        if (air != ESP_OK) ESP_LOGW(kTag, "Air quality refresh failed: %s", esp_err_to_name(air));
    } else {
        ESP_LOGW(kTag, "No coordinates: set the city on the phone setup page");
    }
    refresh_done();
    s_task = nullptr;
    vTaskDelete(nullptr);
}

void start_refresh()
{
    if (s_task || s_busy) return;
    s_busy = true;
    if (xTaskCreate(refresh_task, "weather_fetch", 4096, nullptr, 5, &s_task) != pdPASS) {
        s_busy = false;
        s_task = nullptr;
        ESP_LOGW(kTag, "Unable to start refresh task");
    }
}

void set_text(lv_obj_t *label, const char *text)
{
    if (label) ui_pixel_label_set_text(label, text);
}

void update_ui()
{
    home_profile_t profile;
    home_profile_get(&profile);
    set_text(s_city, profile.city[0] ? profile.city : "未设城市");

    home_weather_t current = {};
    const bool online = WifiManager::GetInstance().IsConnected();
    if (home_weather_get(&current)) {
        char temp[16];
        snprintf(temp, sizeof(temp), "%d°", (int)current.temp_c);
        set_text(s_temp, temp);
        set_text(s_cond, cond_text(current.code));
        draw_art(current.code);

        char metric[32];
        snprintf(metric, sizeof(metric), "湿度 %u%%", (unsigned)current.humidity);
        set_text(s_metrics[0], metric);
        snprintf(metric, sizeof(metric), "风 %d级", home_weather_beaufort(current.wind_kmh));
        set_text(s_metrics[1], metric);

        char air_text[24];
        home_air_quality_label(online, air_text, sizeof(air_text));
        set_text(s_metrics[2], air_text);
    } else {
        set_text(s_temp, "--°");
        set_text(s_cond, "暂无数据");
        lv_obj_clean(s_art);
        set_text(s_metrics[0], "湿度 --");
        set_text(s_metrics[1], "风 --");
        set_text(s_metrics[2], "PM2.5 --");
    }

    home_forecast_t forecast = {};
    if (home_weather_forecast_get(&forecast)) {
        char clock[40];
        if (forecast.sun_valid) {
            char rise[8], set[8];
            fmt_clock(forecast.sunrise_min, rise, sizeof(rise));
            fmt_clock(forecast.sunset_min, set, sizeof(set));
            snprintf(clock, sizeof(clock), "日出 %s  日落 %s", rise, set);
        } else {
            snprintf(clock, sizeof(clock), "日出 --:--  日落 --:--");
        }
        set_text(s_sun, clock);

        for (int col = 0; col < kHourColumns; ++col) {
            char text[12];
            if (col < forecast.hour_count) {
                const home_forecast_hour_t *hour = &forecast.hours[col];
                snprintf(text, sizeof(text), "%02d:00", (int)hour->hour);
                set_text(s_hour_time[col], text);
                snprintf(text, sizeof(text), "%d°", (int)hour->temp_c);
                set_text(s_hour_temp[col], text);
            } else {
                set_text(s_hour_time[col], "--:--");
                set_text(s_hour_temp[col], "--°");
            }
        }

        for (int i = 0; i < 3; ++i) {
            if (i < forecast.day_count) {
                const home_forecast_day_t *day = &forecast.days[i];
                set_text(s_day_cond[i], cond_text(day->code));
                char text[20];
                snprintf(text, sizeof(text), "%d°/%d°", (int)day->tmin_c, (int)day->tmax_c);
                set_text(s_day_temp[i], text);
            } else {
                set_text(s_day_cond[i], "--");
                set_text(s_day_temp[i], "--°/--°");
            }
        }
    } else {
        set_text(s_sun, "日出 --:--  日落 --:--");
        for (int col = 0; col < kHourColumns; ++col) {
            set_text(s_hour_time[col], "--:--");
            set_text(s_hour_temp[col], "--°");
        }
        for (int i = 0; i < 3; ++i) {
            set_text(s_day_cond[i], "--");
            set_text(s_day_temp[i], "--°/--°");
        }
    }
}

void tick(lv_timer_t *)
{
    if (!s_screen || !app_registry_screen_is_on()) return;
    const uint32_t version = home_weather_version();
    if (s_dirty || version != s_seen_version) {
        s_dirty = false;
        s_seen_version = version;
        update_ui();
    }
    if (s_wifi[0]) {
        const int level = WifiManager::GetInstance().IsConnected() ? 3 : 0;
        ui_pixel_wifi_bars_set(s_wifi, level, UI_TEXT);
    }
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());
}

}  // namespace

static void weather_init(void) {}

static void weather_start(void)
{
    bsp_lvgl_lock(-1);
    if (!s_screen) {
        build_ui();
        s_seen_version = 0xFFFFFFFFu;
        s_dirty = true;
        update_ui();
    }
    lv_scr_load(s_screen);
    if (!s_timer) s_timer = lv_timer_create(tick, 500, nullptr);
    bsp_lvgl_unlock();
    start_refresh();
}

static void weather_stop(void)
{
    home_weather_cancel_requests();
    bsp_lvgl_lock(-1);
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
        s_city = s_temp = s_cond = s_sun = s_art = nullptr;
        for (int i = 0; i < 3; ++i) {
            s_metrics[i] = nullptr;
            s_day_name[i] = s_day_cond[i] = s_day_temp[i] = nullptr;
        }
        for (int i = 0; i < kHourColumns; ++i) s_hour_time[i] = s_hour_temp[i] = nullptr;
        s_wifi[0] = s_wifi[1] = s_wifi[2] = nullptr;
    }
    bsp_lvgl_unlock();
    home_weather_allow_requests();
}

static void weather_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv)
{
    (void)mv;
    if (btn == BSP_BTN_OK && event == BTN_EVT_LONG) {
        app_registry_switch_to(APP_ID_LAUNCHER);
        return;
    }
    if (btn == BSP_BTN_OK && event == BTN_EVT_CLICK) {
        start_refresh();
        return;
    }
    if (event == BTN_EVT_CLICK) start_refresh();
}

static bool weather_prepare_stop(void) { return !s_busy; }

// extern：C++ 里命名空间作用域的 const 默认内部链接，注册表在另一个翻译单元
// 引用它，必须显式外部链接（与 app_pomodoro.cc 同一写法）。
extern const passport_app_t g_weather_app = {
    .id = APP_ID_WEATHER,
    .name = "天气",
    .en_name = "WEATHER",
    .desc = "当前天气与未来预报\n日出日落与空气质量",
    .tag = "网络",
    .theme_color = 0x3B82C4,
    .init = weather_init,
    .start = weather_start,
    .stop = weather_stop,
    .on_key = weather_on_key,
    .prepare_stop = weather_prepare_stop,
};
