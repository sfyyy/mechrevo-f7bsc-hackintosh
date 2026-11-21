# Radeon 780M 视频编解码

[PhxBringup build 37 补丁](../patches/phxbringup-vcn.patch) 基于 pristine nullarc 0.4.0-alpha，包含 build 35 的页表上限修复。selector 120 提供 VCN 4.0.2 固件及统一队列诊断，需要 root 和现有的 phxdebug=1。当前工作区的 build 37 已编译并通过依赖检查，运行中的机器仍是 build 35。

## VCN 诊断

在 nullarc 源码树运行主机侧测试。固件文件使用项目同级 vcn-ref 目录中的镜像。

    src/phxbringup/tests/vcn_selftest.sh ../vcn-ref/vcn_4_0_0.bin

在工作目录运行安装脚本并重启后执行真机测试。测试先提交四个 END，再提交六个 fence，覆盖统一环回绕，最后调用 SMU 下电。测试脚本设有四分钟的重启 watchdog。测试成功时可以用 --keep 保持引擎上电。

    sudo bash install-vcn.sh
    # 重启并进入 macOS 后执行
    sudo bash vcn-test.sh

如果已加载 build 37，但旧版 vcnctl 报 selector 120 的 0xe00002d8 或固件头太短，可单独更新用户态工具，随后直接重跑。这个入口只替换 /Users/Shared/gpu/vcnctl。

    sudo bash install-vcn.sh --tool-only
    sudo bash vcn-test.sh

如果初始化返回 code 5，可在重启前创建 /Users/Shared/gpu/vcn-fw-at-boot，使经安装脚本修改的 bringup5.sh 在 AUTOLOAD_RLC 前加载 VCN 固件。vcn-test.sh 会读取本次 fwaddr.log 中成功加载后的地址。回退使用 sudo bash rollback-vcn.sh，随后重启。

shell_sim_test.sh 在临时目录中模拟 ESP 和 IOKit，覆盖单独更新 vcnctl、HELLO 握手、固件逐字节暂存、回退、预加载及失败清理。

## VideoToolbox 验收

从本目录编译并运行。每种格式编码八帧 NV12 IOSurface，解码后核验帧数、时间戳、尺寸及亮度。硬件模式会要求硬件会话并检查 VideoToolbox 的 UsingHardwareAccelerated 属性。若硬件编码失败，工具用软件生成解码样本，以便独立验收硬件解码。任何一项失败都会返回 1。

    clang -std=c11 -O2 -Wall -Wextra -Werror vt_roundtrip.c       -framework CoreFoundation -framework CoreMedia -framework CoreVideo       -framework VideoToolbox -o /tmp/phx-vt-roundtrip
    /tmp/phx-vt-roundtrip
    /tmp/phx-vt-roundtrip --software

在有多个 GPU 的机器上用 --gpu-id ID 指定 Phoenix 的 GPU registry ID。软件模式用于对照验收。当前机器的 H.264 和 HEVC 软件往返均通过，四项硬件会话的创建均失败。

端到端硬件编解码仍缺三段实现：PhxBringup 的 MMHUB 映射与 VCN 提交、RADV Darwin 的视频队列与编解码、VideoToolbox 的会话提供方及应用进程可见性。固件 fence 的结果只覆盖命令队列和 writeback。
