// 首页天气:Open-Meteo(免 API Key)当前温度+WMO 天气码,映射为中文短语。
// 坐标由上传网页在浏览器端完成地理编码后写入资料,设备端只发一次小请求。
// 上次成功结果缓存到 NVS,无网开机也能显示最近一次天气。
//
// 同一次请求顺带取回逐时(24h)/逐日(4d)预报与日出日落:天气应用直接读这份
// 快照,亮暗自动模式也从这里拿日出日落。预报只放内存(断电后重新拉取)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_FORECAST_HOURS 24
#define HOME_FORECAST_DAYS 4

typedef struct {
    bool valid;
    int16_t temp_c;    /* 四舍五入后的摄氏度 */
    uint16_t code;     /* WMO weather code */
    uint8_t humidity;  /* 相对湿度 % */
    uint8_t wind_kmh;  /* 10 米风速 km/h(取整) */
    uint16_t wind_dir; /* 10 米风向,度(0=北,顺时针) */
    uint32_t updated_at; /* 拉取成功时的 UTC 秒;0 = 未知(当时未校时) */
} home_weather_t;

// Open-Meteo/CAMS model concentration, not a Chinese AQI or indoor measurement.
// Volatile only: reuse the same city's sample for 30 minutes across app visits.
// Failed requests, city changes, expiry and device restarts invalidate it.
typedef struct {
    bool valid;
    uint32_t pm25_x10; /* tenths of micrograms per cubic metre */
} home_air_quality_t;

// 逐时/逐日预报快照（天气应用与亮暗自动模式共用）。
typedef struct {
    int8_t temp_c;
    uint16_t code;
    uint8_t precip_pct;
    uint8_t hour; /* 本地小时 0..23 */
} home_forecast_hour_t;

typedef struct {
    int8_t tmax_c;
    int8_t tmin_c;
    uint16_t code;
    uint8_t precip_h;
    uint16_t sunrise_min; /* 本地分钟 0..1439 */
    uint16_t sunset_min;
} home_forecast_day_t;

typedef struct {
    bool valid;
    uint32_t updated_at;
    uint8_t hour_count;
    home_forecast_hour_t hours[HOME_FORECAST_HOURS];
    uint8_t day_count;
    home_forecast_day_t days[HOME_FORECAST_DAYS];
    bool sun_valid;
    uint16_t sunrise_min;
    uint16_t sunset_min;
} home_forecast_t;

bool home_air_quality_get(home_air_quality_t *out);
esp_err_t home_air_quality_refresh(const char *lat, const char *lon);
void home_air_quality_label(bool connected, char *out, size_t capacity);
// Cancel bounded asynchronous HTTPS work before the Home task is stopped.
void home_weather_allow_requests(void);
void home_weather_cancel_requests(void);

// 读 NVS 缓存。UI 立即有"上次天气"可显示。
void home_weather_init(void);

bool home_weather_get(home_weather_t *out);

// 逐时(未来 24h)/逐日(未来 4d)预报 + 日出日落；只在本次开机拉取成功后有效。
bool home_weather_forecast_get(home_forecast_t *out);

// WMO 码 → 中文短语(全部在 buddy 字体字库内)。
const char *home_weather_code_text(uint16_t code);

// 蒲福风级:km/h → 0..12。
int home_weather_beaufort(uint8_t wind_kmh);

// 风向 → 中文方位("北风"/"东南风";静风返回"无风")。字形均在 buddy 字库内。
const char *home_weather_wind_dir_text(uint16_t deg);

// 阻塞拉取(lat/lon 为十进制字符串)。在首页后台任务里调用,勿持 LVGL 锁。
esp_err_t home_weather_refresh(const char *lat, const char *lon);

// 数据年龄短语:"刚刚"/"25分钟前"/"3小时前"/"2天前";updated_at 或 now 未知时
// 返回 "离线数据"。纯函数,便于宿主测试。
void home_weather_age_text(uint32_t updated_at, int64_t now, char *out, size_t capacity);

// 每次成功刷新递增,UI 据此刷新天气行。
uint32_t home_weather_version(void);

#ifdef __cplusplus
}
#endif
