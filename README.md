<div align="center">

# My Passport OS

**FoloToy AI Passport 第三方固件 · ESP32-C3 · 8 MB Flash · 240×320**

基于 [FoloToy AI Passport OS](https://github.com/manchunx7-bit/passport-os) 二次开发
界面与图形参考社区固件 **1944「口袋百宝箱」**

**简体中文** · [English](docs/README.en.md)

</div>

---

## 这个仓库是什么

一块小屏幕硬件（FoloToy AI Passport，ESP32-C3 / 8 MB Flash / 无 PSRAM / 240×320 ST7789）上的个人向固件。

在上游 Passport OS 的基础上，我把日常用着不顺手的地方逐项改掉了：加了亮暗切换、加了天气应用、重画了首页底图与人像显示、把手机设置页改成同网段直连、补齐了启动器彩色图标，并合入了上游 rc.59 的小智语音控制与几个新应用。

> 这是**个人自用**的改动分支，不承诺稳定性，也不替代上游。想跟进主线请以上游仓库为准。

## 代码来源与授权

本仓库是 [**FoloToy AI Passport OS**](https://github.com/manchunx7-bit/passport-os) 的衍生作品，上游以 **MIT** 许可发布。

| 项 | 说明 |
| --- | --- |
| 上游仓库 | <https://github.com/manchunx7-bit/passport-os> |
| 本次改动的起点 | `b70879b`（`v0.1.0-8`，rc.56 一线） |
| 合并的上游版本 | `5101891` — Passport OS 0.2.0-rc.59 |
| 当前版本号 | `0.2.0-rc.59`（见 `CMakeLists.txt`） |
| 许可证 | MIT，原始版权声明完整保留于 [`LICENSE`](LICENSE) |

上游版权归 **FoloToy**（`Copyright (c) 2026 FoloToy`）及各位贡献者所有。本仓库在原作基础上修改，同样以 MIT 发布；再分发时请保留 `LICENSE` 中的原始版权声明。

第三方组件（LVGL、ESP-IDF、cJSON、NimBLE 等）遵循各自的原始许可证，未做改动。

## UI 与图形参考

界面布局、配色、图形语言参考了社区固件 **1944「口袋百宝箱」** —— 同款硬件的另一支社区版本。

需要明确说明的是：

- **1944 没有公开源码**，本仓库**没有复制它的任何代码、字模或二进制资源**。
- 参考方式是对照实机界面与截图，在本仓库内**独立重写**绘制代码（LVGL 矩形块 / 像素图形 / 调色板）。
- 首页底部双底图、启动器彩色图标、人像预设均为按同一视觉风格**自绘**：
  - `tools/gen_band_art.py` —— 底图（16 色 I4）生成与预览
  - `tools/gen_launcher_icon_preview.py` —— 应用图标离线预览
- 若 1944 作者认为有不妥之处，欢迎开 issue，会立即调整。

## 相比上游改了什么

### 界面与个性化

- **亮 / 暗模式**：设置里可切换「亮 / 暗 / 跟随日出日落」，4 套主题 × 明暗共 8 种配色，跨主题统一生效。
- **启动器彩色图标**：每个应用一套主色/辅色/点缀色，不再跟随主题单色；新增应用同步补齐图标。
- **人像 6 种预设**：点阵、平涂、平涂＋局部点阵、漫画线稿、剪影、网点印刷。手机端上传时切换，固件只认第 5 字节的预设号，新增预设不需要改固件。
- **首页底部双底图**：城市夜景 / 山湖月色，16 色（I4）打包，暗色模式自动压暗；长按上键切换。
- **品牌名可配置**：首页品牌字样可在手机设置页改写。

### 天气

- 新增「天气」应用：当前天气、未来 24 小时、未来 3 天、日出日落、空气质量。
- **系统级定时刷新**（`main/apps/home/weather_service.cc`）：天气 15 分钟、PM2.5 30 分钟；**失败一律 2 分钟重试**；城市坐标变更立即刷新。
- 前台是收音机 / 网易云 / 语音 / 小智等音频应用时**让路**，不抢 TLS 连续堆。
- 首页与天气页**只读缓存**，进出页面秒开；手动刷新与定时刷新走同一条路径。
- 缓存落 NVS，重启后先显示旧数据，再静默更新。

### 手机设置页

- **局域网直连**：设备已联网时，页面显示当前 SSID 与 IP，手机连同一个 Wi-Fi 直接打开该 IP 即可，不必切到设备的配网热点。
- 未联网时仍回落到 `192.168.4.1` 热点提示。
- 修复了「已连热点时打开设备 IP 被 302 重定向回 `192.168.4.1`」的问题。

### 合入的上游 rc.59 能力

- **小智 MCP 设备控制**：音量、亮度、打开应用、番茄钟、电台、点歌等由语音调度。
- **双击确定键**进入小智对话；熄屏时双击先唤醒再进入。
- **小智表情动画**与语音等级指示。
- 新应用：**网易云音乐**、**备忘提醒**、**文字清单**、**AI 热点**（新闻能力，供小智调用）。

### 移除

- 「你在狗叫什么」「随身 Muse」两个应用下架（`APP_ID` 保留，避免 NVS / RTC 里存的旧值错位）。

## 应用列表

启动器里共 12 个应用，首页（`APP_ID_HOME`）与手机设置（`APP_ID_WIFI`）不占卡片位。

| 应用 | 说明 |
| --- | --- |
| 天气 | 当前天气、24 小时 / 3 天预报、空气质量 |
| 小智 | 语音对话，可调度设备功能 |
| 收音机 | 在线电台，按省市目录选台 |
| 网易云音乐 | 扫码登录，歌单 / 搜索 / 歌词 |
| 语音 | 语音备忘与输入 |
| 番茄钟 | 25 分钟专注计时 |
| 秒表 | 计时与分段 |
| 备忘提醒 | 定时提醒，到点唤醒屏幕 |
| 文字清单 | 待办清单 |
| 俄罗斯方块 | 小游戏 |
| 声控跳跃 | 小游戏（麦克风） |
| 设置 | 音量、亮度、亮暗、主题、按键音、屏幕超时、重置 |

## 构建

需要 **ESP-IDF 5.5.x**（本项目在 5.5.3 上验证）与 LVGL 9.5（由 `main/idf_component.yml` 拉取）。

```bash
idf.py set-target esp32c3
idf.py build
python tools/verify_firmware.py      # 固件布局 / 首堆预算检查
```

也可以把编译放到局域网 Linux 构建机上跑：

```bash
POS_HOST=192.168.1.10 POS_PW=<密码> python tools/build-remote.py --fetch ../_work/build
```

详见 [`docs/MAINTENANCE.md`](docs/MAINTENANCE.md) 与 [`docs/remote-build.md`](docs/remote-build.md)。

## 烧录

只刷 app 分区即可，**保留 NVS**（否则配置、名片、Wi-Fi 会丢）：

```bash
python -m esptool --chip esp32c3 -p COM5 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x10000 build/passport-os.bin
```

完整固件（含 bootloader / 分区表）从 `0x0` 烧写，会覆盖全部数据。

## 宿主测试

不接硬件也能跑大部分逻辑测试（LVGL 与 ESP-IDF 用桩替代）：

```bash
bash tools/test-host.sh
```

覆盖天气解析与调度、人像预设、小智协议、设备控制、提醒 / 清单存储、启动器注册表、手机页面脚本等。

## 目录结构

```
main/
  apps/           应用（home / weather / xiaozhi / radio / netease / reminders / lists ...）
  launcher/       启动器、应用注册表、设置、配网
  ui/             主题、调色板、像素控件
  fonts/          字模（含按需裁剪的数字子集）
components/       bsp（显示/音频/电池/按键）、passport_muse 等
tools/            构建、校验与生成脚本（底图、图标、字模、远程构建）
tests/            宿主测试
docs/             构建与设计文档
```

## 许可

[MIT](LICENSE)。原始版权归 FoloToy 所有，本仓库的修改同样以 MIT 发布。

## 致谢

- [FoloToy](https://github.com/manchunx7-bit/passport-os) —— 上游 Passport OS 与硬件设计
- 1944「口袋百宝箱」作者 —— 界面与图形参考
- [LVGL](https://lvgl.io/)、[ESP-IDF](https://github.com/espressif/esp-idf) 及所有上游依赖的作者
