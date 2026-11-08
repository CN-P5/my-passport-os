// main/ui/ui_appearance.h —— 全机亮/暗外观（手动 + 按日出日落自动）。
//
// 与"主题(theme)"是两条正交的轴：
//   - theme 决定色相身份（经典绿/旅行仪器/像素白/霓虹黑），影响 accent 等语义色；
//   - appearance 决定同主题的中性色走亮底还是暗底。
// ui_theme_color() 会把两者合起来取调色板，所以所有既有页面只要在"建屏时"
// 读一次颜色，就会自动跟随当前亮暗。
//
// 进入方式：设置页「亮暗模式」= 亮 / 暗 / 自动；
// 自动模式的日出日落由天气拉取写入（home_weather.c），未校时或没有天气数据时
// 退回 19:00–06:00 的固定夜间窗口。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_APPEARANCE_LIGHT = 0, /* 手动亮 */
    UI_APPEARANCE_DARK = 1,  /* 手动暗 */
    UI_APPEARANCE_AUTO = 2,  /* 按日出日落自动 */
    UI_APPEARANCE_COUNT
} ui_appearance_mode_t;

// 读 NVS（system_prefs: appearance/srise/sset）并算出当前有效亮暗。
// 由 settings_bootstrap() 在 UI 创建前调用。
void ui_appearance_init(void);

ui_appearance_mode_t ui_appearance_mode(void);
// 设置并持久化；返回"有效亮暗"是否因此改变。
bool ui_appearance_set_mode(ui_appearance_mode_t mode);
const char *ui_appearance_mode_name(ui_appearance_mode_t mode); /* 亮 / 暗 / 自动 */

// 天气拉取成功后写入本地分钟数（0..1439）；valid=false 表示坐标/数据不可用。
void ui_appearance_set_sun_times(bool valid, uint16_t sunrise_min, uint16_t sunset_min);
bool ui_appearance_sun_times(uint16_t *sunrise_min, uint16_t *sunset_min);

// 当前有效亮暗。所有绘制代码通过 ui_theme_color() 间接读取。
bool ui_appearance_is_dark(void);

// 1 Hz 调用：自动模式跨过日出/日落时切换；返回有效亮暗是否改变。
bool ui_appearance_tick(void);

// 纯函数（header-only，便于宿主测试与复用）：给定模式/本地分钟/日出日落，
// 判断应为亮还是暗。
//   - 手动模式直接返回该模式；
//   - 自动模式在未校时（local_minute < 0）时保持暗，避免首发/断网时整机突然变亮；
//   - 自动模式没有日出日落数据时退回固定 06:00–18:00 为亮。
static inline bool ui_appearance_decide(ui_appearance_mode_t mode, int local_minute,
                                        bool sun_valid, uint16_t sunrise_min,
                                        uint16_t sunset_min)
{
    if (mode == UI_APPEARANCE_DARK) return true;
    if (mode != UI_APPEARANCE_AUTO) return false;
    if (local_minute < 0 || local_minute >= 24 * 60) return true;
    /* 没有天气数据时退回固定 06:00–18:00 为亮。 */
    int rise = sun_valid ? (int)sunrise_min : 6 * 60;
    int set = sun_valid ? (int)sunset_min : 18 * 60;
    if (rise < 0) rise = 0;
    if (set < 0) set = 0;
    if (rise > 24 * 60) rise = 24 * 60 - 1;
    if (set > 24 * 60) set = 24 * 60 - 1;
    if (rise == set) return false; /* 极昼/极夜等退化输入：保持亮，避免频繁翻转 */
    if (rise < set) return local_minute < rise || local_minute >= set;
    /* 跨午夜：日落在日出之前时，日落之后到次日日出之间为暗。 */
    return local_minute < set || local_minute >= rise;
}

#ifdef __cplusplus
}
#endif
