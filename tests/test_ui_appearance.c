/* 亮暗自动模式的判定逻辑：header-only 纯函数，无固件依赖。 */
#include "ui/ui_appearance.h"

#include <assert.h>
#include <stdio.h>

static void manual_modes(void)
{
    assert(ui_appearance_decide(UI_APPEARANCE_LIGHT, 0, true, 360, 1080) == false);
    assert(ui_appearance_decide(UI_APPEARANCE_LIGHT, 1439, true, 360, 1080) == false);
    assert(ui_appearance_decide(UI_APPEARANCE_DARK, 12 * 60, true, 360, 1080) == true);
    assert(ui_appearance_decide(UI_APPEARANCE_DARK, -1, false, 0, 0) == true);
}

static void auto_with_sun(void)
{
    const uint16_t rise = 6 * 60 + 23;  /* 06:23 */
    const uint16_t set = 17 * 60 + 12;  /* 17:12 */
    /* 未校时：保持暗，避免首发/断网整机突然变亮。 */
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, -1, true, rise, set) == true);
    /* 日出前与日落后为暗。 */
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, rise - 1, true, rise, set) == true);
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, rise, true, rise, set) == false);
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, set - 1, true, rise, set) == false);
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, set, true, rise, set) == true);
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, 23 * 60, true, rise, set) == true);
}

static void auto_without_sun(void)
{
    /* 没有天气数据时退回 06:00–18:00 为亮。 */
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, 5 * 60, false, 0, 0) == true);
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, 6 * 60, false, 0, 0) == false);
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, 17 * 60 + 59, false, 0, 0) == false);
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, 18 * 60, false, 0, 0) == true);
    /* 退化输入（极昼/极夜）不翻转。 */
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, 12 * 60, true, 600, 600) == false);
}

static void out_of_range_inputs(void)
{
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, 24 * 60, true, 360, 1080) == true);
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, 16 * 60 + 39, true, 360, 1080) == false);
    /* 越界日出日落被夹到当天范围内（65535 -> 1439），退化成 rise==set 保持亮。 */
    assert(ui_appearance_decide(UI_APPEARANCE_AUTO, 12 * 60, true, 65535, 65535) == false);
}

int main(void)
{
    manual_modes();
    auto_with_sun();
    auto_without_sun();
    out_of_range_inputs();
    printf("ui-appearance: ok\n");
    return 0;
}
