#!/usr/bin/env python3
"""烧写 passport-os-app.bin 到 ESP32-C3 的 factory 应用分区，烧之前先备份设备数据。

只写应用分区（默认 0x10000），不动 bootloader / 分区表 / storage，所以日常升级
不用整包，也不会清掉设备里已上传的内容。即便如此，烧写前仍会先把设备上的
「配置类」分区原样读回来存一份，出问题可以 --restore 写回：

    nvs        WiFi 账号密码 + 各种设置（音量、外观、天气城市…）
    phy_init   射频校准
    cardid     设备 ID / 卡信息
    storage    SPIFFS：上传的人像和资源
    partition-table  设备真实分区表（恢复时按它定位，不依赖仓库 csv）

用法（在 passport-os 目录下）：
    python tools/flash_app.py                    # 备份配置 → 烧写 → 复位
    python tools/flash_app.py --port COM5        # 指定串口
    python tools/flash_app.py --backup full      # 连 app/bootloader 一起整片备份(8MB，较慢)
    python tools/flash_app.py --backup none      # 不备份(不建议)
    python tools/flash_app.py --backup-only      # 只备份，不烧
    python tools/flash_app.py --restore _work/device-backup/20260101-120000
    python tools/flash_app.py --dry-run          # 只打印将执行的 esptool 命令
    python tools/flash_app.py --list-ports       # 看当前串口

依赖：pip install esptool pyserial
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

# 固件候选路径（按顺序取第一个存在的）
DEFAULT_BIN_CANDIDATES = (
    REPO_ROOT.parent / "_work" / "build-release" / "passport-os-app.bin",
    REPO_ROOT.parent / "_work" / "build-release" / "passport-os.bin",
    REPO_ROOT / "build" / "passport-os.bin",
)

# 备份默认落在工作区的 _work/device-backup/<时间戳>/
DEFAULT_BACKUP_ROOT = REPO_ROOT.parent / "_work" / "device-backup"

# 常见 USB 串口芯片 VID，ESP32-C3 原生 USB-JTAG(303a) 优先
PORT_VID_PRIORITY = (0x303A, 0x1A86, 0x10C4, 0x0403)

CHIP = "esp32c3"
CHIP_NAMES = {0: "esp32", 2: "esp32s2", 5: "esp32c3", 9: "esp32s3",
              12: "esp32c2", 13: "esp32c6", 16: "esp32h2"}
APP_DESC_MAGIC = 0xABCD5432

PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0xC00
PT_ENTRY = struct.Struct("<HBBII16sI")  # magic, type, subtype, offset, size, label, flags
PT_MAGIC = 0x50AA
PT_END_MAGIC = 0xEBEB
TYPE_APP = 0x00
TYPE_DATA = 0x01

FLASH_SIZE_8MB = 0x800000
DEFAULT_MONITOR_BAUD = 115200

# 这些分区里装的是什么，备份/恢复时给人看
PARTITION_WHAT = {
    "nvs": "WiFi 凭据 + 各项设置",
    "phy_init": "射频校准",
    "factory": "应用固件",
    "cardid": "设备 ID / 卡信息",
    "storage": "SPIFFS 人像与上传资源",
    "otadata": "OTA 选择标记",
    "coredump": "崩溃转储",
}

FAIL_HINTS = """常见原因：
  1) 串口被别的程序占着：关掉 Arduino 串口监视器 / idf.py monitor / 其他烧写工具
  2) 没进下载模式：按住 BOOT 再插线（或按住 BOOT 点一下 RESET 再松开）后重跑
  3) USB 线只供电不传数据：换一根数据线
  4) 端口选错：先 python tools/flash_app.py --list-ports，再用 --port 指定"""


@dataclass(frozen=True)
class Partition:
    label: str
    ptype: int
    subtype: int
    offset: int
    size: int

    @property
    def is_app(self) -> bool:
        return self.ptype == TYPE_APP

    @property
    def what(self) -> str:
        return PARTITION_WHAT.get(self.label, "")


# --------------------------------------------------------------------------- 基础


def _ensure_utf8_output() -> None:
    """输出被管道/重定向时统一按 UTF-8，避免 PowerShell 7 抓输出时中文乱码。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            if stream is not None and not stream.isatty():
                stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def esptool_version() -> tuple[int, ...]:
    import esptool

    parts: list[int] = []
    for chunk in str(getattr(esptool, "__version__", "0")).split("."):
        digits = "".join(ch for ch in chunk if ch.isdigit())
        parts.append(int(digits) if digits else 0)
    return tuple(parts) or (0,)


def is_v5() -> bool:
    return esptool_version() >= (5,)


def subcommand(name: str) -> str:
    """esptool v5 用连字符子命令，v4 用下划线。"""
    return name.replace("-", "_") if not is_v5() else name


def base_argv(port: str, baud: int, *, final_reset: bool, after: str = "hard-reset") -> list[str]:
    """公共参数。多段读写时前面的不动设备，最后一次才复位，省掉反复枚举 USB 端口。"""
    if final_reset:
        after_value = after if is_v5() or after != "no-reset" else "no-reset"
    else:
        after_value = "no-reset-stub" if is_v5() else "no-reset"
    return ["--chip", CHIP, "--port", port, "--baud", str(baud), "--after", after_value]


def run_esptool(argv: list[str]) -> int:
    import esptool

    print(f"[esptool] {' '.join(argv)}\n")
    try:
        esptool.main(argv)
    except SystemExit as exc:
        code = exc.code
        if code is None:
            return 0
        return code if isinstance(code, int) else 1
    except Exception as exc:  # 串口打不开 / 超时 / 握手失败
        print(f"[esptool] 出错：{exc}")
        return 1
    return 0


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# ------------------------------------------------------------------------ 分区表


def parse_partition_binary(data: bytes) -> list[Partition]:
    """解析设备里的分区表 bin（0x8000）。解析不出来就返回空列表。"""
    parts: list[Partition] = []
    for start in range(0, len(data) - PT_ENTRY.size + 1, PT_ENTRY.size):
        magic, ptype, subtype, offset, size, raw_label, _flags = PT_ENTRY.unpack_from(data, start)
        if magic == PT_END_MAGIC:
            break
        if magic != PT_MAGIC:
            break
        label = raw_label.split(b"\x00", 1)[0].decode("utf-8", "replace")
        parts.append(Partition(label, ptype, subtype, offset, size))
    return parts


def parse_partition_csv(csv_path: Path) -> list[Partition]:
    """读仓库里的 partitions.csv，拿不到设备时（比如 --dry-run）用它兜底。"""
    parts: list[Partition] = []
    try:
        text = csv_path.read_text(encoding="utf-8")
    except OSError:
        return parts
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        if len(fields) < 5:
            continue
        try:
            offset, size = int(fields[3], 0), int(fields[4], 0)
        except ValueError:
            continue
        ptype = TYPE_APP if fields[1].lower() == "app" else TYPE_DATA
        parts.append(Partition(fields[0], ptype, -1, offset, size))
    return parts


def app_partition(parts: list[Partition]) -> Partition:
    for part in parts:
        if part.is_app:
            return part
    return Partition("factory", TYPE_APP, 0x00, 0x10000, 0x400000)


def data_partitions(parts: list[Partition]) -> list[Partition]:
    """要备份的配置类分区：所有非 app 分区（nvs/phy_init/cardid/storage…）。"""
    return [part for part in parts if not part.is_app and part.size > 0]


# -------------------------------------------------------------------- 镜像与环境


def resolve_image(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.is_absolute():
            path = (Path.cwd() / path).resolve()
        if not path.is_file():
            sys.exit(f"固件不存在：{path}")
        return path

    for candidate in DEFAULT_BIN_CANDIDATES:
        if candidate.is_file():
            return candidate
    sys.exit(
        "没找到 passport-os-app.bin，请用 --bin 指定路径。已尝试：\n  "
        + "\n  ".join(str(item) for item in DEFAULT_BIN_CANDIDATES)
    )


def check_image(path: Path, max_size: int) -> None:
    """烧之前验镜像：首字节、app descriptor、chip id、是否超出分区。"""
    data = path.read_bytes()
    if not data:
        sys.exit(f"固件是空文件：{path}")
    if data[0] != 0xE9:
        sys.exit(f"不像 ESP 固件镜像（首字节 {data[0]:#04x}，应以 0xe9 开头）：{path}")

    if len(data) >= 36 and int.from_bytes(data[32:36], "little") != APP_DESC_MAGIC:
        print("[固件] 警告：没找到 app descriptor（0xabcd5432），这可能不是应用镜像。")

    if len(data) >= 14:
        chip_name = CHIP_NAMES.get(int.from_bytes(data[12:14], "little"))
        if chip_name and chip_name != CHIP:
            sys.exit(f"镜像编译目标是 {chip_name}，不是 {CHIP}，别烧。")

    if len(data) > max_size:
        sys.exit(
            f"固件 {len(data)} 字节超过应用分区上限 {max_size} 字节"
            f"（{max_size / 1048576:.1f} MB），烧进去会越界。"
        )

    print(f"[固件] {path}")
    print(f"[固件] {len(data)} 字节（{len(data) / 1048576:.2f} MB），"
          f"占分区 {len(data) * 100 / max_size:.1f}%，余量 {max_size - len(data)} 字节")
    print(f"[固件] sha256 {hashlib.sha256(data).hexdigest()[:16]}…")


def list_ports() -> list:
    from serial.tools import list_ports as lp

    ports = list(lp.comports())
    if not ports:
        print("没检测到任何串口。")
        return ports
    for port in ports:
        vid = f"{port.vid:04x}" if port.vid else "-"
        mark = " *" if (port.vid or 0) in PORT_VID_PRIORITY else ""
        print(f"  {port.device}: {port.description} (vid={vid}){mark}")
    return ports


def pick_port(explicit: str | None) -> str:
    if explicit:
        return explicit

    ports = list_ports()
    if not ports:
        sys.exit("没检测到任何串口，请插好设备或用 --port 指定（例如 --port COM5）。")

    known = [port for port in ports if (port.vid or 0) in PORT_VID_PRIORITY]
    if len(known) == 1:
        print(f"[串口] 自动选择 {known[0].device}（{known[0].description}）")
        return known[0].device
    if not known:
        sys.exit("没识别到 ESP32/CH340/CP210x 串口。确认要烧的话用 --port 指定，例如 --port COM5。")
    sys.exit("检测到多个可用串口，请用 --port 指定：\n  "
             + "\n  ".join(f"{port.device}: {port.description}" for port in known))


# ---------------------------------------------------------------------- 备份/恢复


def read_region(port: str, baud: int, offset: int, size: int, out: Path,
                final_reset: bool, after: str = "hard-reset") -> int:
    argv = base_argv(port, baud, final_reset=final_reset, after=after)
    argv += [subcommand("read-flash"), f"{offset:#x}", f"{size:#x}", str(out)]
    return run_esptool(argv)


def backup_device(port: str, baud: int, tier: str, backup_dir: Path,
                  flash_size: int, csv_parts: list[Partition]) -> tuple[bool, Path]:
    """把设备数据读回本地。tier=config 读配置分区，full 读整片 flash。"""
    backup_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = backup_dir / "manifest.json"
    images: list[dict] = []

    if tier == "full":
        target = backup_dir / f"full-{flash_size // 1024 // 1024}mb.bin"
        print(f"[备份] 整片 flash {flash_size:#x} → {target}")
        if read_region(port, baud, 0, flash_size, target, final_reset=True) != 0:
            return False, manifest_path
        images.append({
            "label": "full-flash", "offset": "0x0", "size": f"{flash_size:#x}",
            "file": target.name, "bytes": target.stat().st_size, "sha256": sha256_of(target),
            "what": "整片 flash（含 bootloader/分区表/app/配置）",
        })
    else:
        # 1) 先读设备真实分区表：备份范围按它算，恢复时也按它定位
        pt_path = backup_dir / "partition-table.bin"
        if read_region(port, baud, PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE,
                       pt_path, final_reset=False) != 0:
            return False, manifest_path
        device_parts = parse_partition_binary(pt_path.read_bytes())
        parts = device_parts or csv_parts
        if device_parts:
            print(f"[备份] 设备分区表解析到 {len(device_parts)} 个分区："
                  + ", ".join(f"{p.label}@{p.offset:#x}" for p in device_parts))
        else:
            print("[备份] 设备分区表解析失败，退回用仓库 partitions.csv 的范围")
        images.append({
            "label": "partition-table", "offset": f"{PARTITION_TABLE_OFFSET:#x}",
            "size": f"{PARTITION_TABLE_SIZE:#x}", "file": pt_path.name,
            "bytes": pt_path.stat().st_size, "sha256": sha256_of(pt_path),
            "what": "设备分区表",
        })

        # 2) 逐个读配置分区；最后一个才复位
        targets = data_partitions(parts)
        for index, part in enumerate(targets):
            out = backup_dir / f"{part.label}.bin"
            last = index == len(targets) - 1
            print(f"[备份] {part.label} {part.offset:#x}+{part.size:#x}"
                  f"（{part.size / 1024:.0f} KB，{part.what or '数据'}）→ {out.name}")
            if read_region(port, baud, part.offset, part.size, out,
                           final_reset=last) != 0:
                return False, manifest_path
            images.append({
                "label": part.label, "offset": f"{part.offset:#x}", "size": f"{part.size:#x}",
                "file": out.name, "bytes": out.stat().st_size, "sha256": sha256_of(out),
                "what": part.what,
            })

    manifest = {
        "created": time.strftime("%Y-%m-%d %H:%M:%S"),
        "port": port,
        "baud": baud,
        "chip": CHIP,
        "esptool": ".".join(str(part) for part in esptool_version()),
        "tier": tier,
        "note": "原始分区镜像，用 tools/flash_app.py --restore <本目录> 原样写回设备",
        "images": images,
    }
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")

    total = sum(item["bytes"] for item in images)
    print(f"\n[备份] 完成：{len(images)} 个镜像，共 {total / 1024:.0f} KB → {backup_dir}")
    print(f"[备份] 清单：{manifest_path}")
    return True, manifest_path


def restore_device(port: str, baud: int, source: Path) -> int:
    if source.is_file() and source.name == "manifest.json":
        manifest_path = source
    else:
        manifest_path = source / "manifest.json"
    if not manifest_path.is_file():
        sys.exit(f"找不到备份清单：{manifest_path}")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    images = manifest.get("images", [])
    if not images:
        sys.exit(f"清单里没有镜像：{manifest_path}")

    root = manifest_path.parent
    argv = base_argv(port, baud, final_reset=True)
    argv += [subcommand("write-flash"), "--flash-size", "keep"]

    print(f"[恢复] 备份时间 {manifest.get('created', '?')}，来自 {root}")
    for item in images:
        path = root / item["file"]
        if not path.is_file():
            sys.exit(f"备份文件缺失：{path}")
        if sha256_of(path) != item.get("sha256"):
            sys.exit(f"备份文件校验不通过（可能被改过/损坏）：{path}")
        print(f"[恢复] {item['label']} → {item['offset']}（{item['bytes']} 字节，"
              f"{item.get('what') or '数据'}）")
        argv += [item["offset"], str(path)]

    print("\n开始把备份写回设备（会覆盖设备上现有的同名分区）\n")
    return run_esptool(argv)


# -------------------------------------------------------------------------- 烧写


def build_flash_argv(port: str, baud: int, offset: int, image: Path,
                     erase: bool, after: str) -> list[str]:
    argv = base_argv(port, baud, final_reset=True, after=after)
    argv += [subcommand("write-flash"), "--flash-size", "keep"]
    if erase:
        argv.append("--erase-all")
    argv += [f"{offset:#x}", str(image)]
    return argv


def monitor(port: str, baud: int) -> None:
    import serial

    print(f"\n[监视] {port} @ {baud}，Ctrl-C 退出")
    try:
        with serial.Serial(port, baud, timeout=1) as ser:
            while True:
                chunk = ser.read(4096)
                if chunk:
                    sys.stdout.write(chunk.decode("utf-8", "replace"))
                    sys.stdout.flush()
                else:
                    time.sleep(0.05)
    except KeyboardInterrupt:
        print("\n[监视] 已退出")
    except serial.SerialException as exc:
        print(f"[监视] 打开 {port} 失败：{exc}")


def main() -> int:
    _ensure_utf8_output()

    parser = argparse.ArgumentParser(
        description="烧写 passport-os-app.bin 到 ESP32-C3 应用分区（烧前自动备份设备配置）",
    )
    parser.add_argument("--bin", help="固件路径，默认自动查找 _work/build-release/passport-os-app.bin")
    parser.add_argument("--port", help="串口，默认从可用串口里自动挑")
    parser.add_argument("--baud", type=int, default=921600, help="烧写/读取波特率")
    parser.add_argument("--offset", help="写入地址，默认取设备分区表里 app 分区的 offset")
    parser.add_argument("--backup", choices=["config", "full", "none"], default="config",
                        help="烧前备份范围：config=配置分区，full=整片 flash，none=不备份")
    parser.add_argument("--backup-dir", help="备份目录，默认 _work/device-backup/<时间戳>")
    parser.add_argument("--backup-only", action="store_true", help="只备份，不烧写")
    parser.add_argument("--restore", metavar="DIR", help="把某个备份目录原样写回设备（不烧固件）")
    parser.add_argument("--flash-size", type=lambda value: int(value, 0), default=FLASH_SIZE_8MB,
                        help="整片备份/擦除用的 flash 容量")
    parser.add_argument("--erase", action="store_true", help="整片擦除后再烧（会清掉配置与人像）")
    parser.add_argument("--force", action="store_true", help="配合 --erase 使用，确认整片擦除")
    parser.add_argument("--after", default="hard-reset",
                        choices=["hard-reset", "soft-reset", "no-reset"],
                        help="烧完的重置方式")
    parser.add_argument("--dry-run", action="store_true", help="只打印将执行的 esptool 命令")
    parser.add_argument("--monitor", action="store_true", help="烧完后进入串口监视")
    parser.add_argument("--list-ports", action="store_true", help="只列出当前串口后退出")
    args = parser.parse_args()

    if args.list_ports:
        list_ports()
        return 0

    if args.erase and not args.force:
        sys.exit("--erase 会整片擦除（WiFi 配置和人像一起没），确认后加 --force 再执行。")

    csv_parts = parse_partition_csv(REPO_ROOT / "partitions.csv")
    csv_app = app_partition(csv_parts)
    port = pick_port(args.port)

    # ---- 恢复模式 ----
    if args.restore:
        return restore_device(port, args.baud, Path(args.restore).expanduser())

    # ---- 只备份 ----
    if args.backup_only:
        backup_dir = Path(args.backup_dir) if args.backup_dir else \
            DEFAULT_BACKUP_ROOT / time.strftime("%Y%m%d-%H%M%S")
        ok, manifest = backup_device(port, args.baud, "config" if args.backup == "none" else args.backup,
                                     backup_dir, args.flash_size, csv_parts)
        if not ok:
            print(f"\n[失败] 备份没做完，未做任何写入。\n{FAIL_HINTS}")
            return 1
        print(f"恢复：python tools/flash_app.py --restore \"{backup_dir}\"")
        return 0

    # ---- 烧写模式 ----
    image = resolve_image(args.bin)
    offset = int(args.offset, 0) if args.offset else csv_app.offset
    print(f"[分区] 应用分区按 partitions.csv：{csv_app.label} offset={csv_app.offset:#x} "
          f"size={csv_app.size:#x}（{csv_app.size / 1048576:.1f} MB）")
    if offset != csv_app.offset:
        print(f"[警告] 本次写入地址 {offset:#x} 与分区表 app 起始 {csv_app.offset:#x} 不同。")
    check_image(image, csv_app.size)

    if args.dry_run:
        if args.backup == "config":
            print("[dry-run] 备份：partition-table.bin + "
                  + " + ".join(f"{part.label}.bin" for part in data_partitions(csv_parts)))
            print(f"[dry-run] esptool {subcommand('read-flash')} <offset> <size> <file> ×"
                  f"{len(data_partitions(csv_parts)) + 1}")
        elif args.backup == "full":
            print(f"[dry-run] 备份：整片 {args.flash_size:#x} → full-{args.flash_size // 1048576}mb.bin")
        flash_argv = build_flash_argv(port, args.baud, offset, image, args.erase, args.after)
        print(f"[dry-run] esptool {' '.join(flash_argv)}")
        return 0

    # 1) 先备份
    backup_dir = Path(args.backup_dir) if args.backup_dir else \
        DEFAULT_BACKUP_ROOT / time.strftime("%Y%m%d-%H%M%S")
    if args.backup != "none":
        print("\n=== 第一步：备份设备数据 ===")
        started = time.time()
        ok, manifest = backup_device(port, args.baud, args.backup, backup_dir,
                                     args.flash_size, csv_parts)
        if not ok:
            print(f"\n[中止] 备份失败（耗时 {time.time() - started:.1f}s），没有烧写任何东西。\n"
                  f"坚持要烧可以加 --backup none。\n{FAIL_HINTS}")
            return 1
    else:
        print("\n[跳过] --backup none，本次不备份设备数据")

    # 2) 再烧写
    print("\n=== 第二步：烧写应用分区 ===")
    flash_argv = build_flash_argv(port, args.baud, offset, image, args.erase, args.after)
    print(f"开始烧写 {image.name} → {offset:#x} @ {port}（{args.baud} baud）\n")
    started = time.time()
    code = run_esptool(flash_argv)
    elapsed = time.time() - started

    if code != 0:
        print(f"\n[失败] esptool 退出码 {code}（耗时 {elapsed:.1f}s）\n{FAIL_HINTS}")
        if args.backup != "none":
            print(f"设备数据已备份在：{backup_dir}\n恢复到烧前状态："
                  f"python tools/flash_app.py --restore \"{backup_dir}\"")
        return code

    print(f"\n[完成] 烧写成功，耗时 {elapsed:.1f}s")
    if args.backup != "none":
        print(f"[完成] 烧前数据备份：{backup_dir}")
    if args.monitor:
        monitor(port, DEFAULT_MONITOR_BAUD)
    else:
        print(f"看日志：python -m serial.tools.miniterm {port} {DEFAULT_MONITOR_BAUD}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
