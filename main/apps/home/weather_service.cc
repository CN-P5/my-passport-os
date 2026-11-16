// main/apps/home/weather_service.cc —— 系统级天气/空气质量定时刷新
//
// 从首页工作线程里抽出来的独立服务：不管当前停在哪一页，只要到点就刷新；
// 首页/天气应用只负责显示缓存，手动刷新也只是请求这里立刻跑一次。
#include "apps/home/weather_service.h"
#include "apps/home/home_profile.h"
#include "apps/home/home_weather.h"
#include "launcher/app_registry.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char *kTag = "weather_service";
// 天气 15 分钟、空气质量 30 分钟；失败 2 分钟重试，避免长时间停在旧数据上。
constexpr int64_t kWeatherFirstDelayMs = 15000;
constexpr int64_t kWeatherPeriodMs = 900000;
constexpr int64_t kAirPeriodMs = 1800000;
constexpr int64_t kWeatherRetryMs = 120000;
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(2000);

// 跨首页/天气页复用同一份截止时间：换页面不会产生额外请求。
struct WeatherSchedule {
    static constexpr unsigned Weather = 1, Air = 2, Both = Weather | Air;
    int64_t weather_due_ms = -1, air_due_ms = -1;
    char lat[HOME_COORD_MAX]{}, lon[HOME_COORD_MAX]{};

    unsigned due(int64_t now, int64_t entered, bool force, const home_profile_t &profile) const {
        if (!profile.lat[0] || !profile.lon[0]) return 0;
        if (force || (lat[0] && (std::strcmp(lat, profile.lat) || std::strcmp(lon, profile.lon)))) return Both;
        if (weather_due_ms < 0 && air_due_ms < 0) return now - entered >= kWeatherFirstDelayMs ? Both : 0;
        return (weather_due_ms <= now ? Weather : 0u) | (air_due_ms <= now ? Air : 0u);
    }

    void begin(unsigned requests, int64_t now, const home_profile_t &profile) {
        std::snprintf(lat, sizeof(lat), "%s", profile.lat);
        std::snprintf(lon, sizeof(lon), "%s", profile.lon);
        // 请求被中断时保持"到期"，下一轮继续补。
        if (requests & Weather) weather_due_ms = now;
        if (requests & Air) air_due_ms = now;
    }

    void finish(unsigned request, int64_t now, esp_err_t result) {
        // 失败一律 2 分钟后再试：空气质量若按成功周期退避，首次失败会让 PM2.5
        // 空等 30 分钟，用户看到的就是"一直没数据"。
        const int64_t period = result != ESP_OK
                                   ? kWeatherRetryMs
                                   : (request == Air ? kAirPeriodMs : kWeatherPeriodMs);
        const int64_t next = now + period;
        if (request == Weather) weather_due_ms = next;
        else air_due_ms = next;
    }
};

WeatherSchedule s_schedule;
std::atomic<bool> s_force{false};
std::atomic<bool> s_busy{false};
TaskHandle_t s_task = nullptr;
int64_t s_started_ms = 0;

// 音频类应用在前台时让路：TLS 握手要一块连续堆，别和流媒体/语音抢内存。
bool audio_app_active() {
    const auto id = app_registry_get_active();
    return id == APP_ID_RADIO || id == APP_ID_NETEASE || id == APP_ID_VOICE || id == APP_ID_XIAOZHI;
}

void service_task(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, kPollTicks);   // 手动刷新可立刻唤醒
        if (!WifiManager::GetInstance().IsConnected()) continue;
        if (audio_app_active()) continue;
        home_profile_t profile;
        home_profile_get(&profile);
        const int64_t now = esp_timer_get_time() / 1000;
        const bool force = s_force.exchange(false);
        const unsigned requests = s_schedule.due(now, s_started_ms, force, profile);
        if (!requests) continue;
        s_schedule.begin(requests, now, profile);
        s_busy = true;
        if (requests & WeatherSchedule::Weather) {
            const esp_err_t result = home_weather_refresh(profile.lat, profile.lon);
            if (result != ESP_OK) ESP_LOGW(kTag, "weather refresh failed: %s", esp_err_to_name(result));
            s_schedule.finish(WeatherSchedule::Weather, esp_timer_get_time() / 1000, result);
        }
        // 天气请求失败时，本轮不再继续拉空气质量，等下个重试点一起补。
        if (requests & WeatherSchedule::Air) {
            const esp_err_t result = home_air_quality_refresh(profile.lat, profile.lon);
            if (result != ESP_OK) ESP_LOGW(kTag, "air refresh failed: %s", esp_err_to_name(result));
            s_schedule.finish(WeatherSchedule::Air, esp_timer_get_time() / 1000, result);
        }
        s_busy = false;
    }
}

}  // namespace

void weather_service_start(void) {
    if (s_task) return;
    s_started_ms = esp_timer_get_time() / 1000;
    if (xTaskCreate(service_task, "weather_svc", 4096, nullptr, 4, &s_task) != pdPASS) {
        s_task = nullptr;
        ESP_LOGW(kTag, "unable to start weather service task");
    }
}

bool weather_service_request_refresh(void) {
    if (!s_task) return false;
    s_force = true;
    xTaskNotifyGive(s_task);
    return true;
}

bool weather_service_busy(void) { return s_busy.load(); }
