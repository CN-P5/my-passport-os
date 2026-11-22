#!/usr/bin/env python
"""在局域网 Linux 构建机上编译本仓库（Windows/Linux/macOS 都能直接跑，不用 WSL）。

用法（默认走密钥免密，见 ~/.ssh/config 的 pos-build 别名）：
    python tools/build-remote.py                      # 增量编译
    python tools/build-remote.py --fetch ../_work/build
    python tools/build-remote.py --clean
    POS_HOST=192.168.10.204 POS_KEY=~/.ssh/id_pos_build python tools/build-remote.py

环境变量：
    POS_HOST    构建机地址或 ssh config 别名，默认 pos-build
    POS_KEY     私钥路径；不设则用 ssh config 里该别名指定的私钥
    POS_PW      登录密码（可选，仅在没有可用密钥时使用）
    POS_USER    登录用户，默认取 ssh config，其次 root
    POS_PORT    SSH 端口，默认取 ssh config，其次 22
    REMOTE_IDF  构建机上的 ESP-IDF 路径，默认 /root/esp-idf
    REMOTE_DIR  构建机上的仓库目录，默认 /root/pos

一次性准备（推 IDF 源码树、工具链、managed_components）见 docs/remote-build.md。
"""
import argparse
import io
import os
import sys
import tarfile
import time

try:
    import paramiko
except ImportError:  # pragma: no cover
    sys.exit('需要 paramiko：pip install paramiko')

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXCLUDES = ('.git', 'build', 'build-public', 'build-diag', 'managed_components', '__pycache__')
EXCLUDE_SUFFIX = ('.pyc',)


def pack_source() -> io.BytesIO:
    """把工作区源码打成一个内存里的 tar.gz，跳过构建产物与已解析依赖。"""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w:gz') as tar:
        for name in sorted(os.listdir(REPO)):
            if name in EXCLUDES:
                continue
            path = os.path.join(REPO, name)
            if os.path.isfile(path) and name.endswith(EXCLUDE_SUFFIX):
                continue
            tar.add(path, arcname=name, filter=lambda info: _filter(info))
    buffer.seek(0)
    return buffer


def _filter(info: tarfile.TarInfo):
    """跳过嵌套目录里的构建产物（例如 tools/build 之类）。"""
    parts = info.name.replace('\\', '/').split('/')
    if any(part in EXCLUDES for part in parts) or info.name.endswith(EXCLUDE_SUFFIX):
        return None
    return info


def ssh_config(alias: str):
    """读 ~/.ssh/config，返回 (hostname, user, identityfile, port)；域名不存在返回 None。

    paramiko 不会解析 ssh config 的 Host 别名，所以这里手工查一次，
    这样 POS_HOST 既能写 IP，也能直接写 `pos-build` 这种别名。
    """
    path = os.path.expanduser('~/.ssh/config')
    if not os.path.isfile(path):
        return None
    config = paramiko.SSHConfig()
    try:
        with open(path, encoding='utf-8') as handle:
            config.parse(handle)
    except OSError:
        return None
    entry = config.lookup(alias)
    # 未知别名时 lookup 只会回一个 hostname=alias，据此判断命中。
    if not entry or set(entry) <= {'hostname'}:
        return None
    identity = entry.get('identityfile') or []
    key = os.path.expanduser(identity[0]) if identity else None
    return entry.get('hostname') or alias, entry.get('user'), key, entry.get('port')


def connect(host: str, user: str, password=None, key=None, port: int = 22) -> paramiko.SSHClient:
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(hostname=host, port=port, username=user,
                   password=password or None, key_filename=key,
                   look_for_keys=True, allow_agent=True,
                   timeout=20, banner_timeout=30)
    return client


def run(client: paramiko.SSHClient, command: str, label: str) -> int:
    """流式执行远端命令，逐行打印，返回退出码。"""
    channel = client.get_transport().open_session()
    channel.exec_command(command)
    pending = b''
    while True:
        chunk = channel.recv(4096)
        if not chunk:
            break
        pending += chunk
        while b'\n' in pending:
            line, pending = pending.split(b'\n', 1)
            print(line.decode('utf-8', 'replace').rstrip())
    if pending:
        print(pending.decode('utf-8', 'replace').rstrip())
    code = channel.recv_exit_status()
    print(f'[{label}] exit={code}')
    return code


def main() -> int:
    parser = argparse.ArgumentParser(description='在局域网构建机上编译 passport-os')
    parser.add_argument('--fetch', metavar='DIR', help='编译后把产物取回到该目录')
    parser.add_argument('--clean', action='store_true', help='丢弃远端 build/ 后全量重编')
    args = parser.parse_args()

    # 认证：优先 ~/.ssh/config 里的别名（含密钥），也可以用 POS_KEY 指定私钥；
    # 两者都没有时才回退到 POS_PW 密码。paramiko 自己不读 ssh config，这里手工解析。
    alias = os.environ.get('POS_HOST', 'pos-build')
    password = os.environ.get('POS_PW') or None
    key = os.environ.get('POS_KEY') or None
    if key:
        key = os.path.expanduser(key)

    looked_up = ssh_config(alias)
    host, cfg_user, cfg_key, cfg_port = looked_up if looked_up else (alias, None, None, None)
    user = os.environ.get('POS_USER') or cfg_user or 'root'
    key = key or cfg_key
    port = int(os.environ.get('POS_PORT') or cfg_port or 22)

    remote_idf = os.environ.get('REMOTE_IDF', '/root/esp-idf')
    remote_dir = os.environ.get('REMOTE_DIR', '/root/pos')

    started = time.time()
    auth = f'密钥 {key}' if key else ('密码' if password else '默认密钥/agent')
    print(f'==> 连接 {user}@{host}:{port}（{auth}）')
    try:
        client = connect(host, user, password, key, port)
    except paramiko.AuthenticationException:
        sys.exit('认证失败：密钥没装上或密码不对。可设置 POS_KEY=<私钥路径> 或 POS_PW=<密码>。')
    try:
        archive = pack_source()
        size = len(archive.getvalue())
        print(f'==> 上传源码 {size // 1024} KB')
        sftp = client.open_sftp()
        sftp.putfo(archive, '/tmp/passport-src.tar.gz')
        sftp.close()

        # 保留远端 managed_components 与 build/，默认增量编译。
        script = (
            f"set -eo pipefail; mkdir -p '{remote_dir}'; "
            + (f"rm -rf '{remote_dir}/build'; " if args.clean else '')
            + f"tar -xzf /tmp/passport-src.tar.gz -C '{remote_dir}'; "
            f"cd '{remote_dir}'; export IDF_SKIP_CHECK_SUBMODULES=1; "
            f". '{remote_idf}/export.sh' >/dev/null 2>&1; "
            "idf.py set-target esp32c3 >/tmp/settarget.log 2>&1; "
            "if ! idf.py build >/tmp/build.log 2>&1; then tail -40 /tmp/build.log; exit 1; fi; "
            "grep -E 'error:|Project build complete' /tmp/build.log | tail -3; "
            f"idf.py merge-bin -o '{remote_dir}/build/FoloToy-AI-Passport-full.bin' >/dev/null 2>&1; "
            f"python tools/verify_firmware.py '{remote_dir}/build' | tail -5; "
            "ls -l build/passport-os.bin"
        )
        # --norc/--noprofile：构建机的 .bashrc 里有无关报错，别让它污染输出；
        # export.sh 是普通脚本，不需要登录 shell 也能改好 PATH。
        code = run(client, f"bash --noprofile --norc -c {shell_quote(script)}", 'build')
        if code != 0:
            print('!! 构建失败，完整日志：远端 /tmp/build 输出见上（或 /root/pos/build/log）')
            return code

        if args.fetch:
            local = os.path.abspath(args.fetch)
            os.makedirs(local, exist_ok=True)
            sftp = client.open_sftp()
            for remote_name, local_name in (
                ('passport-os.bin', 'passport-os-app.bin'),
                ('FoloToy-AI-Passport-full.bin', 'FoloToy-AI-Passport-full.bin'),
            ):
                sftp.get(f'{remote_dir}/build/{remote_name}', os.path.join(local, local_name))
            sftp.close()
            print(f'==> 产物已取回 {local}')
    finally:
        client.close()
    print(f'==> 完成，用时 {time.time() - started:.0f}s')
    return 0


def shell_quote(text: str) -> str:
    return "'" + text.replace("'", "'\\''") + "'"


if __name__ == '__main__':
    raise SystemExit(main())
