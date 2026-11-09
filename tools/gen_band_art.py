# tools/gen_band_art.py —— 生成首页底部 240×52 真彩底图（RGB565 C 数组）。
#
# 两张场景按"城市夜景 / 山湖月色"的构图自己画（不搬第三方像素资源）：
#   tools/band_art/city.png / lake.png   —— 便于预览与后续微调
#   main/apps/home/band_art.c            —— 固件用的 RGB565 数组
#
# 用法：python tools/gen_band_art.py
# 说明：LVGL 的 RGB565 按小端存储，这里直接按 <H 打包，屏幕上颜色才不会互换。
import os
import struct
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover
    sys.exit('需要 Pillow：pip install pillow')

W, H = 240, 52
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PNG = os.path.join(ROOT, 'tools', 'band_art')
OUT_C = os.path.join(ROOT, 'main', 'apps', 'home', 'band_art.c')


def lerp(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def sky(img, top, bottom, y0=0, y1=H):
    draw = ImageDraw.Draw(img)
    for y in range(y0, y0 + y1):
        draw.line([(0, y), (W - 1, y)], fill=lerp(top, bottom, (y - y0) / max(1, y1 - 1)))


def city():
    img = Image.new('RGB', (W, H))
    d = ImageDraw.Draw(img)
    horizon = 34
    sky(img, (26, 42, 74), (200, 120, 84), 0, horizon)
    # 落日与光晕
    for r, color in ((13, (120, 70, 70)), (9, (232, 138, 90)), (6, (247, 201, 72))):
        d.ellipse([196 - r, 12 - r, 196 + r, 12 + r], fill=color)
    # 远山剪影
    d.polygon([(0, 30), (34, 22), (62, 28), (96, 20), (140, 27), (180, 21), (214, 26), (240, 22), (240, 34), (0, 34)],
              fill=(46, 58, 82))
    # 楼群（高低错落）+ 亮窗
    blocks = [(2, 16, 20, 26), (24, 24, 18, 20), (44, 12, 22, 30), (68, 20, 16, 24),
              (86, 8, 24, 34), (112, 18, 18, 26), (132, 14, 22, 30), (156, 22, 16, 22)]
    for i, (x, top, w, h) in enumerate(blocks):
        d.rectangle([x, top, x + w - 1, horizon + h - 26], fill=(12, 20, 34))
        for wy in range(top + 3, horizon + h - 30, 5):
            for wx in range(x + 3, x + w - 4, 5):
                if (wx * 7 + wy * 5 + i * 3) % 11 < 4:
                    d.rectangle([wx, wy, wx + 1, wy + 2], fill=(242, 185, 59))
    # 桥与斜拉索
    d.line([(150, 26), (240, 26)], fill=(120, 134, 158), width=1)
    for cx in (168, 196, 224):
        d.line([(cx, 26), (cx + 10, 8)], fill=(96, 110, 134), width=1)
        for k in range(1, 5):
            x = cx + (k * 10) // 5
            y = 26 - (k * 18) // 5
            d.line([(x, y), (x, 26)], fill=(74, 88, 112), width=1)
    # 海面 + 反光
    d.rectangle([0, horizon, W - 1, H - 1], fill=(34, 78, 104))
    for i in range(0, W, 14):
        d.rectangle([i + 3, horizon + 2, i + 9, horizon + 3], fill=(120, 170, 192))
        d.rectangle([i + 7, horizon + 7, i + 12, horizon + 8], fill=(58, 104, 130))
    for i in range(0, W, 9):
        d.rectangle([i + 2, horizon + 12, i + 5, horizon + 13], fill=(94, 140, 166))
    d.rectangle([0, H - 3, W - 1, H - 1], fill=(26, 58, 80))
    return img


def lake():
    img = Image.new('RGB', (W, H))
    d = ImageDraw.Draw(img)
    horizon = 33
    sky(img, (198, 226, 240), (238, 246, 249), 0, horizon)
    for r, color in ((10, (246, 214, 128)), (7, (250, 232, 160)), (4, (253, 246, 208))):
        d.ellipse([38 - r, 10 - r, 38 + r, 10 + r], fill=color)
    # 云带
    for (x, y, w, h, c) in ((104, 6, 40, 4, (247, 251, 253)), (118, 3, 20, 4, (240, 247, 251)),
                            (10, 16, 34, 3, (247, 251, 253)), (186, 14, 30, 3, (240, 247, 251))):
        d.rectangle([x, y, x + w, y + h], fill=c)
    # 三层山脊
    d.polygon([(0, 26), (48, 12), (86, 24), (128, 10), (170, 23), (208, 14), (240, 24), (240, 33), (0, 33)],
              fill=(143, 179, 201))
    d.polygon([(0, 30), (36, 18), (74, 29), (110, 17), (150, 28), (196, 20), (240, 29), (240, 33), (0, 33)],
              fill=(95, 143, 168))
    d.polygon([(0, 33), (60, 24), (104, 31), (160, 25), (240, 31), (240, 33)], fill=(60, 107, 127))
    # 松树
    for i, (x, h) in enumerate(((26, 11), (34, 9), (54, 12), (66, 8), (176, 10), (188, 12), (204, 9), (216, 11))):
        base = 33
        for k in range(3):
            half = (h - k * 3) // 2 + 1
            y = base - k * 3
            d.polygon([(x - half, y), (x + half, y), (x, y - 5)], fill=(26, 74, 56))
        d.rectangle([x - 1, base - 2, x, base + 1], fill=(40, 62, 48))
    # 湖面 + 倒影 + 帆船
    d.rectangle([0, horizon, W - 1, H - 1], fill=(127, 179, 196))
    for i in range(0, W, 12):
        d.rectangle([i + 4, horizon + 3, i + 9, horizon + 4], fill=(163, 205, 219))
        d.rectangle([i + 7, horizon + 9, i + 13, horizon + 10], fill=(105, 158, 178))
    d.polygon([(150, horizon + 4), (162, horizon + 4), (156, horizon - 5)], fill=(232, 115, 74))
    d.rectangle([148, horizon + 4, 164, horizon + 6], fill=(43, 58, 74))
    d.rectangle([0, H - 4, W - 1, H - 1], fill=(74, 122, 82))
    return img


def emit_i4(name, path):
    """LVGL I4 图片格式：开头 16 个 lv_color32_t(BGRA) 调色板，随后每字节两个像素。"""
    img = Image.open(path).convert('RGB')
    assert img.size == (W, H), f'{path} 尺寸应为 {W}x{H}'
    quant = img.quantize(colors=16, method=Image.MEDIANCUT, dither=Image.FLOYDSTEINBERG)
    palette = quant.getpalette()[:16 * 3]
    indices = list(quant.getdata())
    out = bytearray()
    for i in range(16):
        r, g, b = palette[i * 3:i * 3 + 3]
        out += bytes((b, g, r, 0xFF))
    for i in range(0, len(indices), 2):
        low = indices[i + 1] if i + 1 < len(indices) else 0
        out.append(((indices[i] & 0x0F) << 4) | (low & 0x0F))
    lines = []
    for i in range(0, len(out), 16):
        lines.append('    ' + ', '.join('0x%02X' % v for v in out[i:i + 16]) + ',')
    return ('// %s: 240x52 I4（16 色调色板 + 4bpp），由 tools/gen_band_art.py 生成，勿手改\n'
            'const uint8_t %s[BAND_ART_BYTES] = {\n%s\n};\n\n'
            % (os.path.basename(path), name, '\n'.join(lines)))


def main():
    os.makedirs(OUT_PNG, exist_ok=True)
    city_path = os.path.join(OUT_PNG, 'city.png')
    lake_path = os.path.join(OUT_PNG, 'lake.png')
    city().save(city_path)
    lake().save(lake_path)
    body = ('// main/apps/home/band_art.c —— 首页底部 240×52 底图（I4 16 色调色板）。\n'
            '// 由 tools/gen_band_art.py 生成：构图见 tools/band_art/*.png，改图请改脚本重跑。\n'
            '#include "apps/home/band_art.h"\n\n')
    body += emit_i4('kBandArtCity', city_path)
    body += emit_i4('kBandArtMountain', lake_path)
    with open(OUT_C, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(body)
    print('generated', OUT_C, os.path.getsize(OUT_C), 'bytes')
    print('preview:', city_path, lake_path)


if __name__ == '__main__':
    main()
