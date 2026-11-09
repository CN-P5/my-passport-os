# 在局域网 Linux 机器上编译

Windows 侧没有 ESP-IDF 工具链，WSL 里编译虽然可行，但把构建放到一台常开的小机器上
更省事：源码从这边打包上传，编译、`merge-bin`、`verify_firmware.py` 全在那边跑，
产物再取回来烧写。

## 日常用法

```bash
# 只看编译与校验结果
POS_HOST=192.168.10.204 POS_PW=<密码> bash tools/build-remote.sh

# 顺便把 app 镜像与整包镜像取回到本地目录
POS_HOST=192.168.10.204 POS_PW=<密码> bash tools/build-remote.sh --fetch ../_work/build
```

密码只通过环境变量经 `sshpass` 传递，不写进仓库、不写进脚本。

## 一次性准备

构建机是 Debian 12（2 核 / 8 GB / 16 GB 空闲磁盘）时，按下面顺序准备一次即可。
关键动机：**那台机器访问 GitHub 不稳定**（子模块与 release 下载都会超时），所以
ESP-IDF 源码树、工具链和组件依赖都从已有安装直接推过去，而不是让远端去 clone。

1. 系统依赖

   ```bash
   apt-get install -y ninja-build python3-venv build-essential ccache \
       libffi-dev libssl-dev git
   ```

2. ESP-IDF 源码树（v5.5.3，不含 `.git`）

   ```bash
   tar -cf /tmp/idf.tar --exclude=.git esp-idf        # 在已有 IDF 的机器上
   scp /tmp/idf.tar build-host:/root/ && ssh build-host 'tar -xf /root/idf.tar -C /root'
   ```

   因为没有 `.git`，CMake 会以为子模块未初始化并尝试 `git submodule update`，
   **构建时必须 `IDF_SKIP_CHECK_SUBMODULES=1`**（`tools/build-remote.sh` 已设置）。
   升级 IDF 版本时重做这一步。

3. 工具链（GitHub release 下载被墙，直接拷贝）

   ```bash
   cd /root/.espressif && tar -cf - tools/riscv32-esp-elf tools/esp-rom-elfs \
       tools/openocd-esp32 tools/riscv32-esp-elf-gdb \
       | ssh build-host 'mkdir -p /root/.espressif/tools && tar -xf - -C /root/.espressif'
   ```

4. Python 环境

   虚拟环境不能跨 Python 版本复制，所以在远端用本机 python3 现场创建，
   并按 `constraints` 钉住版本（不钉会装成最新版，`export.sh` 自检直接失败）：

   ```bash
   python3 -m venv /root/.espressif/python_env/idf5.5_py3.11_env
   /root/.espressif/python_env/idf5.5_py3.11_env/bin/pip install -r \
       /root/esp-idf/tools/requirements/requirements.core.txt \
       -c /root/.espressif/espidf.constraints.v5.5.txt      # 约束文件从已有安装复制过去
   ```

5. 项目依赖 `managed_components/`

   LVGL、`esp_lvgl_port` 等约 225 MB，第一次从已有构建拷贝到构建机的项目目录，
   之后 `tools/build-remote.sh` 不会覆盖它（脚本打包时排除了该目录）。

6. 验证

   ```bash
   ssh build-host 'cd /root/esp-idf && . ./export.sh >/dev/null && idf.py --version'
   ```

## 已知差异

同一份源码在两台机器上编出的镜像可能差十几字节（绝对路径长度不同会体现在
调试字符串里），分区占用、首块连续堆与 `verify_firmware.py` 的结论一致。
