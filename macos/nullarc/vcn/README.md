# Radeon 780M 视频编解码

2026-11-22，本机重启加载 PhxBringup build 64 后，普通用户通过 VideoToolbox 的 Phoenix 编码器完成了八帧 160×128 H.264 硬编码。独立软件解码器按时间戳逐帧核对了各帧特有的亮度和色度图案。驱动诊断还通过了 864×480 H.264 固定帧硬解和 136 字节 IDR 的正常导出。初始化和底层诊断使用 selector 120，需要 root 和启动参数 `phxdebug=1`。[真机记录](NEXT.md)

build 65 在 build 64 的固定帧接口之外增加了按会话尺寸提交 NV12 的接口。驱动源码与参考向量见 [PhxBringup 补丁](../patches/phxbringup-vcn.patch)。

## 应用接入 H.264 编码

在 x86_64 应用中一同编译 `phx_vt.c`、`phx_avc.c` 和 `phx_avc_io.c`。启动时调用 `PhxRegisterAVCEncoder()`，成功后使用 `VTCompressionSessionCreate` 创建 H.264 会话。编码器说明字典中将 `kVTVideoEncoderSpecification_EncoderID` 设为 `PhxAVCEncoderID`，将 `kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder` 设为 `kCFBooleanTrue`。可运行示例见 [vt_roundtrip.c](vt_roundtrip.c) 的 `--phx-avc` 入口。

客户端按原尺寸提交双平面 NV12（`420v` 或 `420f`）。适配层会把全范围输入映射到码流使用的视频范围。build 64 支持 160×128。build 65 接受两边均为偶数、每边至少 128、长边最多 2560、短边最多 1440 的尺寸，包含 960×540、1920×1080、2048×1152 和 2560×1440。每帧生成一张独立 IDR，回调返回带输入时间戳和时长的 H.264 样本。当前提供器使用 x86_64 macOS 26 的私有插件 ABI。HEVC 硬编码、应用硬解码和会议吞吐量尚未验收。

以下命令从本目录构建进程内注册的验收程序。

    clang -std=c11 -O2 -Wall -Wextra -Werror -Wl,-adhoc_codesign -DPHX_VT_ENCODER \
        vt_roundtrip.c phx_vt.c phx_avc.c phx_avc_io.c \
        -framework VideoToolbox -framework CoreMedia -framework CoreVideo \
        -framework CoreFoundation -framework IOKit -o /tmp/phx-vt-roundtrip

在工作目录运行 `sudo bash install-vcn.sh` 部署本地候选 kext。重启后确认 `phx-build` 为 65，再运行以下命令。初始化使用现有的 root 诊断入口，随后以普通用户运行应用。休眠后需要重新初始化。

    ioreg -r -c PhxBringup -d 1 | grep phx-build
    sudo bash vcn-test.sh --keep
    /tmp/phx-vt-roundtrip --phx-avc
    /tmp/phx-vt-roundtrip --phx-avc --size 2048x1152 --full-range
    /tmp/phx-vt-roundtrip --phx-avc --size 2560x1440 --full-range
    sudo /Users/Shared/gpu/vcnctl down

验收程序检查会话实际选择的编码器 ID 和硬件属性，再将八帧编码结果交给独立软件解码器，检查时间戳、亮度和 UV。build 64 真机运行输出 `Phoenix H.264 VT hardware encode: PASS (8/8)` 和 `Independent software decode and Y/UV check: PASS`，退出码为 0。

开发时可用参考码流验证 VideoToolbox 回调。在本目录运行以下命令。测试替换了 IOKit 后端，保留真实 VideoToolbox 会话。

    clang -std=c11 -O2 -Wall -Wextra -Werror -Wl,-adhoc_codesign phx_vt_test.c phx_vt.c phx_avc.c \
        -framework VideoToolbox -framework CoreMedia -framework CoreVideo \
        -framework CoreFoundation -framework IOKit -o /tmp/phx-vt-test
    /tmp/phx-vt-test vcn_quadrants.h264
    /tmp/phx-vt-test fixture-2048x1152.h264 2048 1152
    /tmp/phx-vt-test fixture-2560x1440.h264 2560 1440

两份会议尺寸的参考文件各含一帧由系统软件 H.264 编码器生成的测试图案。上述回放用例以 1/25 秒为间隔提交时间戳，并核对八次回调中的画幅、时间戳和帧时长。持续编码帧率需要在硬件上另行测量。

## 系统发现与会议尺寸验收

**本机 macOS 26.7.1 的默认编码器列表仍不包含 Phoenix。** 用 [构建脚本](build-vt-bundle.sh) 生成插件，安装到 `/Library/Video/Plug-Ins` 后在独立进程中测试，CoreMedia 的系统扫描会过滤其签名。Apple 自带的 H.264 插件通过了同一签名检查。普通 Developer ID 也无法让 `org.nullarc.phoenix.avc` 满足这项要求。build 65 的尺寸支持可以通过上面的进程内会话单独验收。

腾讯会议在本机共享屏幕时记录了 2048×1152 和 2560×1440 的 r264 编码，屏幕采集为 25 fps 的全范围 NV12。部分 2560×1440 会话选择了 v265 软件 HEVC。以下独立程序只链接 Apple 框架，可检查普通应用首次打开 VideoToolbox 时的结果。

    clang -std=c11 -O2 -Wall -Wextra -Werror -Wl,-adhoc_codesign vt_roundtrip.c \
        -framework VideoToolbox -framework CoreMedia -framework CoreVideo \
        -framework CoreFoundation -o /tmp/phx-vt-system
    /tmp/phx-vt-system --system-phx --size 2048x1152
    /tmp/phx-vt-system --system-phx --size 2560x1440

要复现原版应用按编码格式自动选择硬件的行为，运行下面两项。每项都列出该格式的系统候选，直接要求硬件编码，成功时打印实际编码器 ID 并独立解码。它们也能验收 Apple GVA 命名的编码器。

    /tmp/phx-vt-system --system-encode h264 --size 160x128
    /tmp/phx-vt-system --system-encode hevc --size 1920x1080

本机的原版 FFmpeg 对照、GVA 注册入口和 AMD 驱动对 Phoenix 设备 ID 的检查见[系统接口实测](NEXT.md)。

`--system-phx` 查询系统编码器列表，再以 H.264 和“要求硬件”的选项创建会话。通过要求为系统列出 Phoenix、自动选中 Phoenix 的编码器 ID、硬件标志为真，并且独立软件解码得到原尺寸、时间戳及每帧特有的 Y/UV 图案。测试输入为 8 帧 `420f`，时间戳间隔为 1/25 秒。软件对照可用 `--software --size 2048x1152 --full-range`。[本机测试记录](NEXT.md)

## 从原版源码复现

在工作目录中给一份干净的 nullarc 源码应用补丁。补丁内包含 kext、命令行工具、寄存器对照表、测试向量及测试入口。

    BUILD_DIR=$(mktemp -d)
    PATCH="$PWD/mechrevo-f7bsc-hackintosh/macos/nullarc/patches/phxbringup-vcn.patch"
    cp -R nullarc-0.4.0-alpha.orig/. "$BUILD_DIR/"
    (cd "$BUILD_DIR" && git apply --check "$PATCH" && git apply "$PATCH")
    "$BUILD_DIR/src/phxbringup/build.sh" "$PWD/sdk/MacKernelSDK-master"
    "$BUILD_DIR/src/phxbringup/tests/vcn_selftest.sh" "$PWD/vcn-ref/vcn_4_0_0.bin"
    clang -O2 -Wall -Wextra -Werror "$BUILD_DIR/tools/phxctl/vcnctl.c" -framework IOKit -framework CoreFoundation -o "$BUILD_DIR/vcnctl"

    clang -std=c11 -O2 -Wall -Wextra -Werror "$BUILD_DIR/tools/phxctl/vcn_frame_check.c" -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation -o "$BUILD_DIR/vcn_frame_check"
    "$BUILD_DIR/vcn_frame_check" "$BUILD_DIR/src/phxbringup/tests/vcn_quadrants.h264"

vcn_quadrants.h264 来自 macOS 软件 AVC 编码器，输入为 160×128、Y=48/200、UV=128 的 NV12 四象限图案，用于独立验证像素检查工具。

## VCN 诊断

在 nullarc 源码树运行主机侧测试。固件文件使用项目同级 vcn-ref 目录中的镜像。

    src/phxbringup/tests/vcn_selftest.sh ../vcn-ref/vcn_4_0_0.bin

部署脚本归档在 deploy/。复现部署时，将这三个脚本复制到含 nullarc-0.4.0-alpha 和 vcn-ref 的工作目录，将构建出的 PhxBringup.kext 放在工作目录并命名为 PhxBringup-vcn.kext，再运行 install-vcn.sh。脚本备份现有 kext，记录 ESP 分区 UUID，并安装 vcnctl 和 vcn_frame_check。

重启进入 macOS 后，在工作目录执行：

    ioreg -r -c PhxBringup -d 1 | grep phx-build
    # 测试多分辨率应用接口时确认已加载 65
    sudo bash vcn-test.sh

脚本验证固件、四个 END、跨回绕 fence、两种会话，再分别运行 decode-frame 和 encode-frame。解码目标为 864×480 NV12，行跨度 1024，对完整 737280 字节求和应为 0x020345d8。编码目标为 160×128 的单张 IDR 图，独立软件解码器核验四个象限的亮度和中性色度。每次运行在 /Users/Shared/gpu/vcn-run.XXXXXXXX 留存 run.log 和已通过驱动校验的 frame.nv12、frame.h264。失败时另存 crumbs.log。脚本会启动四分钟重启 watchdog，结束时撤销。测试成功可用 --keep 保持 VCN 上电。

如果初始化返回 code 5，可在重启前创建 /Users/Shared/gpu/vcn-fw-at-boot，使经安装脚本修改的 bringup5.sh 在 AUTOLOAD_RLC 前加载 VCN 固件。vcn-test.sh 会读取本次 fwaddr.log 中成功加载后的地址。回退使用 sudo bash rollback-vcn.sh，随后重启。

shell_sim_test.sh 在临时目录模拟 ESP 和 IOKit，检查 BO 映射、原子文件输出、旧驱动版本、陈旧 fence、阶段错误、坏长度和坏内容，同时验证安装、回退和失败清理。

## VideoToolbox 验收

从本目录编译并运行。每种格式编码八帧 NV12 IOSurface，解码后核验帧数、时间戳、尺寸及亮度。硬件模式会要求硬件会话并检查 VideoToolbox 的 UsingHardwareAccelerated 属性。若硬件编码失败，工具用软件生成解码样本，以便独立验收硬件解码。任何一项失败都会返回 1。

    clang -std=c11 -O2 -Wall -Wextra -Werror vt_roundtrip.c       -framework CoreFoundation -framework CoreMedia -framework CoreVideo       -framework VideoToolbox -o /tmp/phx-vt-roundtrip
    /tmp/phx-vt-roundtrip
    /tmp/phx-vt-roundtrip --software

在有多个 GPU 的机器上用 --gpu-id ID 指定 Phoenix 的 GPU registry ID。软件模式用于对照验收。当前机器的 H.264 和 HEVC 软件往返均通过，四项硬件会话的创建均失败。

默认模式验收系统可发现的编码器和解码器，最终要求 H.264、HEVC 各完成 8 帧硬件编码和解码。进程内 H.264 验收单独使用上文的 `--phx-avc` 模式。
