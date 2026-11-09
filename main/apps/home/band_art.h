// main/apps/home/band_art.h —— 内置底图(I4 16 色)。见 band_art.c 与 tools/gen_band_art.py。
#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BAND_ART_W 240
#define BAND_ART_H 52
/* 64 字节调色板(16×BGRA) + 4bpp 像素 */
#define BAND_ART_BYTES (64 + BAND_ART_W * BAND_ART_H / 2)

extern const uint8_t kBandArtCity[BAND_ART_BYTES];
extern const uint8_t kBandArtMountain[BAND_ART_BYTES];

#ifdef __cplusplus
}
#endif
