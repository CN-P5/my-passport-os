# tools/gen_launcher_icon_preview.py —— 应用图标设计预览（88×88，纯矩形块，与 launcher.cc 同画法）
# 每个应用两个方案，每方案 ≤10 块、只用三色（primary/detail/warm）。用 ASCII 标签避免缺中文字形。
import os
from PIL import Image, ImageDraw

SIZE, SCALE, PAD, LABEL_H = 88, 3, 10, 22
P = {
    'netease':   (0xD8382F, 0x1E1616, 0xF7EFE6),   # 红 / 近黑 / 米白
    'reminders': (0xF2B233, 0x6B4410, 0xF6F2E7),   # 琥珀 / 深褐 / 纸白
    'lists':     (0x4FA8E0, 0x1F3F5C, 0xEEF4F9),   # 蓝 / 深蓝 / 纸白
    'aihot':     (0x8B5CF6, 0x241242, 0xFFB257),   # 紫 / 深紫 / 橙
}
D = {
    'netease': [
        [('warm', 8, 8, 72, 72), ('primary', 8, 8, 72, 9), ('primary', 8, 71, 72, 9),
         ('primary', 8, 8, 9, 72), ('primary', 71, 8, 9, 72),
         ('detail', 36, 36, 16, 16), ('detail', 58, 20, 8, 30), ('detail', 46, 44, 16, 8)],
    ],
    'reminders': [
        [('warm', 10, 12, 68, 66), ('primary', 10, 12, 8, 66),
         ('detail', 28, 26, 34, 7), ('detail', 28, 40, 34, 7), ('detail', 28, 54, 22, 7),
         ('primary', 54, 44, 18, 16), ('primary', 49, 60, 28, 7), ('detail', 60, 38, 6, 6)],
    ],
    'lists': [
        [('primary', 12, 14, 18, 18), ('primary', 12, 38, 18, 18), ('primary', 12, 62, 18, 18),
         ('detail', 38, 19, 38, 8), ('detail', 38, 43, 38, 8), ('detail', 38, 67, 38, 8)],
    ],
}


def render(parts, palette):
    img = Image.new('RGB', (SIZE, SIZE), (0xF6, 0xF5, 0xEF))
    draw = ImageDraw.Draw(img)
    for role, x, y, w, h in parts:
        c = palette[{'primary': 0, 'detail': 1, 'warm': 2}[role]]
        draw.rectangle([x, y, x + w - 1, y + h - 1], fill=((c >> 16) & 255, (c >> 8) & 255, c & 255))
    return img


def main():
    cell = SIZE * SCALE
    keys = list(D)
    width = PAD + 1 * (cell + PAD)
    height = PAD + len(keys) * (cell + LABEL_H + PAD)
    sheet = Image.new('RGB', (width, height), (0x2B, 0x2B, 0x2B))
    draw = ImageDraw.Draw(sheet)
    for row, key in enumerate(keys):
        for col, parts in enumerate(D[key]):
            x = PAD + col * (cell + PAD)
            y = PAD + row * (cell + LABEL_H + PAD)
            sheet.paste(render(parts, P[key]).resize((cell, cell), Image.NEAREST), (x, y))
            draw.text((x + 2, y + cell + 4), f'{key.upper()} (implemented)', fill=(0x9E, 0xE6, 0xB0))
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'icon_preview.png')
    sheet.save(out)
    print('saved', out, sheet.size)


if __name__ == '__main__':
    main()
