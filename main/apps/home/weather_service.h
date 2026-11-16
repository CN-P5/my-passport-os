// main/apps/home/weather_service.h —— 系统级天气/空气质量定时刷新
//
// 天气数据只由这里刷新：首页、天气应用进入时都只读缓存；
// 应用页的"确定刷新"也只是请求这里立刻跑一次，与定时刷新完全同一条路径。
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 启动刷新任务（幂等，开机调用一次）。
void weather_service_start(void);
// 请求立刻刷新一次；返回 false 表示服务未启动。
bool weather_service_request_refresh(void);
// 是否正在拉取（UI 用来显示"刷新中…"与转圈）。
bool weather_service_busy(void);

#ifdef __cplusplus
}
#endif
