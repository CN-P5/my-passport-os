// main/apps/weather/app_weather.cc —— 天气应用。
//
// 版面（240×320）：顶栏 → 城市 → 当前温度+大图标 → 日出日落 →
// 湿度/风/PM2.5 → 未来 24 小时(图标在上、小时数字在下) →
// 未来 3 天(名称+图标+天气文字+高低温度) → 底部提示。
// 数据来自 home_weather 的共享快照（一次 HTTPS 同时填充 current/hourly/daily，
// 并给亮暗自动模式提供日出日落），本应用不另发请求。
//
// 字体：中文一律用 buddy_font_16（4713 字形，覆盖本页所有文案）。
// home_font_12 只有 119 个字形，之前用它画「日出/日落」直接变成方块。
#include "apps/app_base.h"
#include "apps/home/home_profile.h"
#include "apps/home/home_weather.h"
#include "apps/home/weather_service.h"
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
#include <ctime>
#include <cstring>

namespace {

constexpr const char *kTag = "weather_app";
constexpr int kHourColumns = 5;

// 天气图形配色：与主题无关，亮暗两种底色下都能看清。
constexpr uint32_t kSun = 0xF7C948;
constexpr uint32_t kCloud = 0x7E97AC;
constexpr uint32_t kCloudDeep = 0x5A7387;
constexpr uint32_t kRain = 0x3B72B8;
constexpr uint32_t kSnow = 0x8FB6D8;
constexpr uint32_t kBolt = 0xE08B1E;
constexpr uint32_t kFog = 0x8A97A3;

lv_obj_t *s_screen;
lv_obj_t *s_city;
lv_obj_t *s_temp;
lv_obj_t *s_cond;
lv_obj_t *s_sun;
lv_obj_t *s_metrics[3];
lv_obj_t *s_art;
lv_obj_t *s_hour_icon[kHourColumns];
lv_obj_t *s_hour_hour[kHourColumns];
lv_obj_t *s_hour_temp[kHourColumns];
lv_obj_t *s_day_name[3];
lv_obj_t *s_day_icon[3];
lv_obj_t *s_day_cond[3];
lv_obj_t *s_day_temp[3];
lv_obj_t *s_wifi[3];
ui_pixel_battery_t s_battery;
lv_timer_t *s_timer;
lv_obj_t *s_footer;          // 底部提示（刷新中/操作提示）
bool s_refreshing_shown;     // 当前 footer 是否已显示"刷新中"
volatile bool s_dirty = true;
uint32_t s_seen_version = 0xFFFFFFFFu;

// 一个像素块：所有天气图形都由它拼出来，尺寸/位置都在父容器的局部坐标里。
void block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    if (!parent) return;
    lv_obj_t *part = lv_obj_create(parent);
    lv_obj_remove_flag(part, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(part, x, y);
    lv_obj_set_size(part, w, h);
    lv_obj_set_style_radius(part, 0, 0);
    lv_obj_set_style_border_width(part, 0, 0);
    lv_obj_set_style_pad_all(part, 0, 0);
    lv_obj_set_style_bg_color(part, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
}

// 40×34 的大图形（当前天气）。
void art_sun_big(lv_obj_t *parent, int cx, int cy)
{
    block(parent, cx - 7, cy - 7, 14, 14, kSun);
    block(parent, cx - 9, cy - 2, 2, 4, kSun);
    block(parent, cx + 8, cy - 2, 2, 4, kSun);
    block(parent, cx - 2, cy - 9, 4, 2, kSun);
    block(parent, cx - 2, cy + 8, 4, 2, kSun);
}

void draw_art_big(lv_obj_t *parent, uint16_t code)
{
    if (!parent) return;
    lv_obj_clean(parent);
    if (code == 0 || code == 1) {
        art_sun_big(parent, 20, 16);
        return;
    }
    if (code == 2) {
        art_sun_big(parent, 28, 10);
        block(parent, 2, 16, 30, 10, kCloud);
        block(parent, 8, 10, 14, 8, kCloud);
        block(parent, 20, 13, 10, 6, kCloud);
        return;
    }
    if (code == 3) {
        block(parent, 4, 12, 30, 10, kCloud);
        block(parent, 10, 6, 14, 8, kCloud);
        block(parent, 22, 9, 10, 6, kCloud);
        return;
    }
    if (code == 45 || code == 48) {
        block(parent, 4, 6, 30, 9, kCloud);
        block(parent, 10, 1, 14, 7, kCloud);
        block(parent, 4, 20, 26, 3, kFog);
        block(parent, 8, 26, 26, 3, kFog);
        return;
    }
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        block(parent, 4, 4, 30, 10, kCloud);
        block(parent, 10, -1, 14, 8, kCloud);
        for (int i = 0; i < 3; ++i) block(parent, 8 + i * 9, 18, 3, 7, kRain);
        return;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        block(parent, 4, 4, 30, 10, kCloud);
        block(parent, 10, -1, 14, 8, kCloud);
        for (int i = 0; i < 3; ++i) block(parent, 8 + i * 9, 18, 4, 4, kSnow);
        return;
    }
    if (code >= 95) {
        block(parent, 4, 2, 30, 10, kCloudDeep);
        block(parent, 10, -3, 14, 8, kCloudDeep);
        block(parent, 16, 16, 4, 7, kBolt);
        block(parent, 13, 22, 4, 6, kBolt);
        return;
    }
    block(parent, 6, 12, 30, 10, kCloud);
    block(parent, 12, 6, 14, 8, kCloud);
}

// 18×14 的小图形（逐小时 / 逐日）。
void art_sun_small(lv_obj_t *parent)
{
    block(parent, 6, 4, 6, 6, kSun);
    block(parent, 4, 6, 2, 2, kSun);
    block(parent, 12, 6, 2, 2, kSun);
    block(parent, 8, 2, 2, 2, kSun);
    block(parent, 8, 10, 2, 2, kSun);
}

void draw_art_small(lv_obj_t *parent, uint16_t code)
{
    if (!parent) return;
    lv_obj_clean(parent);
    if (code == 0 || code == 1) {
        art_sun_small(parent);
        return;
    }
    if (code == 2) {
        art_sun_small(parent);
        block(parent, 1, 8, 15, 5, kCloud);
        return;
    }
    if (code == 3) {
        block(parent, 1, 5, 16, 5, kCloud);
        block(parent, 5, 2, 8, 4, kCloud);
        return;
    }
    if (code == 45 || code == 48) {
        block(parent, 1, 2, 16, 4, kCloud);
        block(parent, 1, 8, 12, 2, kFog);
        block(parent, 4, 11, 12, 2, kFog);
        return;
    }
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        block(parent, 1, 2, 16, 4, kCloud);
        block(parent, 3, 8, 2, 4, kRain);
        block(parent, 9, 8, 2, 4, kRain);
        block(parent, 14, 8, 2, 3, kRain);
        return;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        block(parent, 1, 2, 16, 4, kCloud);
        block(parent, 4, 8, 3, 3, kSnow);
        block(parent, 11, 8, 3, 3, kSnow);
        return;
    }
    if (code >= 95) {
        block(parent, 1, 2, 16, 4, kCloudDeep);
        block(parent, 7, 7, 3, 5, kBolt);
        return;
    }
    block(parent, 2, 5, 14, 4, kCloud);
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

// 底部提示是一整块可重建对象：切换"刷新中…"与"确定刷新"只需重建这两行。
void set_footer(const char *action)
{
    if (!s_screen) {
        s_footer = nullptr;
        return;
    }
    if (s_footer) {
        lv_obj_delete(s_footer);
        s_footer = nullptr;
    }
    s_footer = ui_pixel_footer(s_screen, action, "长按返回", &buddy_font_16);
}

// footer 只在"忙/闲"切换时重建，避免每 500ms 抖动一次。
// 刷新统一由系统级 weather_service 负责：这里只请求一次立即刷新。
void request_refresh()
{
    if (!weather_service_request_refresh())
        ESP_LOGW(kTag, "Weather service is not running");
}

void sync_refresh_footer()
{
    const bool busy = weather_service_busy();
    if (busy == s_refreshing_shown) return;
    s_refreshing_shown = busy;
    set_footer(busy ? "刷新中…" : "确定刷新");
}

lv_obj_t *icon_cell(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(cell, x, y);
    lv_obj_set_size(cell, w, h);
    return cell;
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
    lv_obj_set_pos(s_city, 120, 28);
    lv_obj_set_width(s_city, 108);
    lv_obj_set_style_text_align(s_city, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_city, LV_LABEL_LONG_MODE_DOTS);

    s_temp = ui_pixel_label(s_screen, "--°", &lv_font_montserrat_32, UI_TEXT);
    lv_obj_set_pos(s_temp, 12, 30);

    s_cond = ui_pixel_label(s_screen, "暂无数据", &buddy_font_16, UI_DANGER);
    lv_obj_set_pos(s_cond, 12, 64);
    lv_obj_set_width(s_cond, 132);
    lv_label_set_long_mode(s_cond, LV_LABEL_LONG_MODE_DOTS);

    s_art = icon_cell(s_screen, 150, 28, 44, 36);

    s_sun = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT_DIM);
    lv_obj_set_pos(s_sun, 12, 86);
    lv_obj_set_width(s_sun, 216);
    lv_label_set_long_mode(s_sun, LV_LABEL_LONG_MODE_DOTS);

    for (int i = 0; i < 3; ++i) {
        s_metrics[i] = ui_pixel_label(s_screen, "--", &buddy_font_16, UI_TEXT_DIM);
        lv_obj_set_pos(s_metrics[i], 12 + i * 72, 106);
        lv_obj_set_width(s_metrics[i], 72);
        lv_label_set_long_mode(s_metrics[i], LV_LABEL_LONG_MODE_DOTS);
    }

    ui_pixel_panel_create(s_screen, 12, 128, 216, 1, UI_BORDER);

    lv_obj_t *hour_hdr = ui_pixel_label(s_screen, "未来 24 小时", &buddy_font_16, UI_SECONDARY);
    lv_obj_set_pos(hour_hdr, 12, 130);

    for (int i = 0; i < kHourColumns; ++i) {
        const int x = 12 + i * 44;
        s_hour_icon[i] = icon_cell(s_screen, x, 150, 18, 14);
        s_hour_hour[i] = ui_pixel_label(s_screen, "--", &lv_font_montserrat_14, UI_TEXT);
        lv_obj_set_pos(s_hour_hour[i], x, 164);
        s_hour_temp[i] = ui_pixel_label(s_screen, "--°", &lv_font_montserrat_14, UI_TEXT_DIM);
        lv_obj_set_pos(s_hour_temp[i], x + 20, 164);
    }

    ui_pixel_panel_create(s_screen, 12, 182, 216, 1, UI_BORDER);

    lv_obj_t *day_hdr = ui_pixel_label(s_screen, "未来 3 天", &buddy_font_16, UI_SECONDARY);
    lv_obj_set_pos(day_hdr, 12, 184);

    static const char *kDayNames[3] = {"今天", "明天", "后天"};
    for (int i = 0; i < 3; ++i) {
        const int y = 204 + i * 20;
        s_day_name[i] = ui_pixel_label(s_screen, kDayNames[i], &buddy_font_16, UI_TEXT_DIM);
        lv_obj_set_pos(s_day_name[i], 12, y);
        lv_obj_set_width(s_day_name[i], 34);
        s_day_icon[i] = icon_cell(s_screen, 50, y + 3, 18, 14);
        s_day_cond[i] = ui_pixel_label(s_screen, "--", &buddy_font_16, UI_TEXT);
        lv_obj_set_pos(s_day_cond[i], 74, y);
        lv_obj_set_width(s_day_cond[i], 76);
        lv_label_set_long_mode(s_day_cond[i], LV_LABEL_LONG_MODE_DOTS);
        s_day_temp[i] = ui_pixel_label(s_screen, "--°/--°", &lv_font_montserrat_14, UI_TEXT);
        lv_obj_set_pos(s_day_temp[i], 152, y + 2);
        lv_obj_set_width(s_day_temp[i], 68);
        lv_obj_set_style_text_align(s_day_temp[i], LV_TEXT_ALIGN_RIGHT, 0);
    }

    set_footer("确定刷新");
}

void update_ui()
{
    home_profile_t profile;
    home_profile_get(&profile);
    ui_pixel_label_set_text(s_city, profile.city[0] ? profile.city : "未设城市");

    const bool online = WifiManager::GetInstance().IsConnected();
    home_weather_t current = {};
    if (home_weather_get(&current)) {
        char temp[16];
        snprintf(temp, sizeof(temp), "%d°", (int)current.temp_c);
        ui_pixel_label_set_text(s_temp, temp);
        ui_pixel_label_set_text(s_cond, cond_text(current.code));
        draw_art_big(s_art, current.code);

        char metric[32];
        snprintf(metric, sizeof(metric), "湿度 %u%%", (unsigned)current.humidity);
        ui_pixel_label_set_text(s_metrics[0], metric);
        snprintf(metric, sizeof(metric), "风 %d级", home_weather_beaufort(current.wind_kmh));
        ui_pixel_label_set_text(s_metrics[1], metric);

        char air_text[24];
        home_air_quality_label(online, air_text, sizeof(air_text));
        ui_pixel_label_set_text(s_metrics[2], air_text);
    } else {
        ui_pixel_label_set_text(s_temp, "--°");
        ui_pixel_label_set_text(s_cond, "暂无数据");
        lv_obj_clean(s_art);
        ui_pixel_label_set_text(s_metrics[0], "湿度 --");
        ui_pixel_label_set_text(s_metrics[1], "风 --");
        ui_pixel_label_set_text(s_metrics[2], "PM2.5 --");
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
        ui_pixel_label_set_text(s_sun, clock);

        for (int col = 0; col < kHourColumns; ++col) {
            char text[12];
            if (col < forecast.hour_count) {
                const home_forecast_hour_t *hour = &forecast.hours[col];
                draw_art_small(s_hour_icon[col], hour->code);
                snprintf(text, sizeof(text), "%02d", (int)hour->hour);
                ui_pixel_label_set_text(s_hour_hour[col], text);
                snprintf(text, sizeof(text), "%d°", (int)hour->temp_c);
                ui_pixel_label_set_text(s_hour_temp[col], text);
            } else {
                lv_obj_clean(s_hour_icon[col]);
                ui_pixel_label_set_text(s_hour_hour[col], "--");
                ui_pixel_label_set_text(s_hour_temp[col], "--°");
            }
        }

        for (int i = 0; i < 3; ++i) {
            if (i < forecast.day_count) {
                const home_forecast_day_t *day = &forecast.days[i];
                draw_art_small(s_day_icon[i], day->code);
                ui_pixel_label_set_text(s_day_cond[i], cond_text(day->code));
                char text[20];
                // 高/低温：先高后低。
                snprintf(text, sizeof(text), "%d°/%d°", (int)day->tmax_c, (int)day->tmin_c);
                ui_pixel_label_set_text(s_day_temp[i], text);
            } else {
                lv_obj_clean(s_day_icon[i]);
                ui_pixel_label_set_text(s_day_cond[i], "--");
                ui_pixel_label_set_text(s_day_temp[i], "--°/--°");
            }
        }
    } else {
        ui_pixel_label_set_text(s_sun, "日出 --:--  日落 --:--");
        for (int col = 0; col < kHourColumns; ++col) {
            lv_obj_clean(s_hour_icon[col]);
            ui_pixel_label_set_text(s_hour_hour[col], "--");
            ui_pixel_label_set_text(s_hour_temp[col], "--°");
        }
        for (int i = 0; i < 3; ++i) {
            lv_obj_clean(s_day_icon[i]);
            ui_pixel_label_set_text(s_day_cond[i], "--");
            ui_pixel_label_set_text(s_day_temp[i], "--°/--°");
        }
    }
}

// 进入页面时调用：缓存新鲜就完全不联网，保证"秒开"；
// 有别的入口正在拉（首页后台刷新）则等它，不重复请求。
void tick(lv_timer_t *)
{
    if (!s_screen || !app_registry_screen_is_on()) return;
    sync_refresh_footer();
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
    sync_refresh_footer();
    bsp_lvgl_unlock();
    request_refresh();
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
            s_day_name[i] = s_day_cond[i] = s_day_temp[i] = s_day_icon[i] = nullptr;
        }
        for (int i = 0; i < kHourColumns; ++i) {
            s_hour_icon[i] = s_hour_hour[i] = s_hour_temp[i] = nullptr;
        }
        s_wifi[0] = s_wifi[1] = s_wifi[2] = nullptr;
        // 屏幕销毁后 footer 等控件指针一律失效：不清掉的话下次进入会 delete 已释放对象，
        // LVGL 断言直接 trap（串口表现为 MCAUSE 3 Breakpoint 重启）。
        s_footer = nullptr;
        s_refreshing_shown = false;
        s_battery = {};
        s_seen_version = 0xFFFFFFFFu;
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
    if (event == BTN_EVT_CLICK) request_refresh();
}

// 离开不等网络：stop() 会取消在途请求，后台任务自行收尾。
static bool weather_prepare_stop(void) { return true; }

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
