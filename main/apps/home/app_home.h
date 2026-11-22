// 首页(个人主页):头像 + 姓名/签名 + 时钟 + 天气 + 两张统计卡(占位,恒 --)。
// 这是一个完整 App(而非 Launcher 的一部分),遵守 registry 的"stop 即删屏"
// 约定 —— 进应用时 Launcher 已释放界面,退出时本应用同样把 LVGL 对象全部删除。
#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif
extern const passport_app_t g_home_app;
// 亮暗主题翻转后整屏重建首页(create_ui 里颜色是创建期求值的，只 invalidate 会花屏)。
void home_ui_rebuild(void);
extern const passport_app_t g_home_app;

#ifdef __cplusplus
}
#endif
