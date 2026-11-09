#!/usr/bin/env bash
# 在局域网 Linux 构建机上编译本仓库：本机只负责打包上传 + 取回产物。
#
# 用法：
#   POS_HOST=192.168.10.x POS_PW=... bash tools/build-remote.sh
#   POS_HOST=... POS_PW=... bash tools/build-remote.sh --fetch ../_work/build
#   POS_HOST=... POS_PW=... bash tools/build-remote.sh --clean      # 丢弃远端 build/ 全量重编
#
# 环境变量：
#   POS_HOST    构建机地址（必填）
#   POS_USER    登录用户，默认 root
#   POS_PW      登录密码（经 sshpass 传递，不写入仓库）
#   REMOTE_IDF  构建机上的 ESP-IDF 路径，默认 /root/esp-idf
#   REMOTE_DIR  构建机上的仓库目录，默认 /root/pos
#
# 一次性准备（推 IDF 源码树、工具链、managed_components）见 docs/remote-build.md。
set -euo pipefail

: "${POS_HOST:?请设置 POS_HOST（构建机地址）}"
: "${POS_PW:?请设置 POS_PW（构建机密码）}"
POS_USER="${POS_USER:-root}"
REMOTE_IDF="${REMOTE_IDF:-/root/esp-idf}"
REMOTE_DIR="${REMOTE_DIR:-/root/pos}"
TARGET="${POS_USER}@${POS_HOST}"
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15)
ssh_do() { SSHPASS="$POS_PW" sshpass -e ssh "${SSH_OPTS[@]}" "$TARGET" "$1"; }
scp_do() { SSHPASS="$POS_PW" sshpass -e scp "${SSH_OPTS[@]}" "$@"; }

FETCH_DIR=""
CLEAN=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --fetch) FETCH_DIR="${2:?--fetch 需要一个本地目录}"; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        *) echo "未知参数：$1" >&2; exit 2 ;;
    esac
done

echo "==> 打包源码"
cd "$ROOT"
tar --exclude=.git --exclude=build --exclude=build-public --exclude=build-diag \
    --exclude=managed_components --exclude='*.pyc' -czf /tmp/passport-src.tgz .
scp_do /tmp/passport-src.tgz "$TARGET:/tmp/passport-src.tgz" >/dev/null

# 保留远端的 managed_components（LVGL 等约 225 MB）与 build/ 以便增量编译；
# 源码包本身不含 managed_components，解包不会覆盖它。
echo "==> 远端编译"
ssh_do "mkdir -p '$REMOTE_DIR' \
    && { [ '$CLEAN' = 1 ] && rm -rf '$REMOTE_DIR/build'; true; } \
    && tar -xzf /tmp/passport-src.tgz -C '$REMOTE_DIR' \
    && cd '$REMOTE_DIR' && export IDF_SKIP_CHECK_SUBMODULES=1 \
    && . '$REMOTE_IDF/export.sh' >/dev/null 2>&1 \
    && idf.py set-target esp32c3 >/tmp/settarget.log 2>&1 \
    && idf.py build >/tmp/build.log 2>&1; echo build_rc=\$?; \
    grep -E 'error:|Project build complete' /tmp/build.log | head -20; \
    idf.py merge-bin -o '$REMOTE_DIR/build/FoloToy-AI-Passport-full.bin' >/dev/null 2>&1; \
    python tools/verify_firmware.py '$REMOTE_DIR/build' | tail -5; \
    ls -l build/passport-os.bin"

if [ -n "$FETCH_DIR" ]; then
    mkdir -p "$FETCH_DIR"
    scp_do "$TARGET:$REMOTE_DIR/build/passport-os.bin" "$FETCH_DIR/passport-os-app.bin" >/dev/null
    scp_do "$TARGET:$REMOTE_DIR/build/FoloToy-AI-Passport-full.bin" "$FETCH_DIR/FoloToy-AI-Passport-full.bin" >/dev/null
    echo "==> 产物已取回 $FETCH_DIR"
fi
