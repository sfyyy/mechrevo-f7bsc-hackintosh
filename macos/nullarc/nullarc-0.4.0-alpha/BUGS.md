# 已知问题

基准机上的缺陷与待完善事项动态清单：Firebat U6C、Ryzen 7 H 255、Radeon 780M、macOS Tahoe 26.6.2。
按严重度处理：先 S1，再 S2，依此类推。新发现会直接补充到这里。

**严重度**

- **S1 — 致命。** 系统卡死、重启，或无法使用。
- **S2 — 严重。** 重要功能不可用，或问题持续妨碍使用。
- **S3 — 中等。** 现象明显，但有规避方式或很少发生。
- **S4 — 轻微。** 仅影响外观。

**状态：** 未解决 · 处理中 · 待验证（已有修复，等待在机器上确认） ·
已规避（根因尚未解决，但问题不再出现） · 已修复 · 无法修复（硬件或固件限制）。

## 开放项

| 编号 | 严重度 | 区域 | 现象 | 状态 | 已知情况 |
|---|---|---|---|---|---|
| B-32 | S1 | 内存 | Telegram 在打开聊天时崩溃三次（SIGBUS “FS pagein error: 94 Bad message”），UserNotificationCenter 也一同崩溃；之后所有动画都卡顿（2026-10-26 14:39）。 | 未解决 | 崩溃并非磁盘损坏：dyld 共享缓存中的所有文件都能完整读取，只是 `liblangid.dylib` 的一个代码页未能加载（文本语言检测会使用该页）。原因是内存不足：交换空间 8959/9216 MB，kernel_task 14 GB：其中 6.4 GB 预留给 BIOS 中的 UMA 图形内存（配置为 6 GB，驱动程序几乎不使用），3.9 GB 是 PhxBringup 的 GPU 缓冲区（已固定，每个 Metal 应用约 70–80 MB，共 50 个客户端），1.4 GB 是页表（867 个进程，其中数百个是 iOS 模拟器），0.5 GB 是 PhxAccel 池。处理措施：将 BIOS 中的 UMA 减至 1 GB（PhxBringup 从寄存器读取大小，自身只使用前约 128 MB：PSP 环、20 MB 的 TMR、64 MB 的固件）。插件中已完成：池块从 4 MB 开始，按倍数增至 32 MB（此前每个进程直接为 32 MB），调用之间的中间加载缓冲区在 WindowServer 之外保持最多 4 MB（此前每个线程 32 MB）。旧插件和新插件上的 SceneKit 表现相同：93/92 帧/秒。接下来：继续查清 RADV 还会为每个进程创建或初始化什么。 |
| B-30 | S1 | 驱动：计算 | Geekbench 6 GPU 在 OpenCL 模式下进行 Feature Matching 测试时使 GPU 挂起：WindowServer 被看门狗终止，phxboot 重启了 Mac（2026-10-26 00:13）。 | 待验证 | 前五项 OpenCL 测试（Background Blur、Face Detection、Horizon/Edge Detection、Gaussian Blur）均已通过。任何进程中的挂起都会导致整个会话终止：GPU 无法重置，且队列是共享的。需要：发生挂起时的 kernel（通过 `NVMTL_LINK_DUMP` 导出 Feature Matching kernel 的转储），以及挂起后的恢复机制（重置队列或上下文、终止肇事进程）。软恢复与 Linux 中的方式相同（`gfx_v11_0_ring_soft_recovery`）：在 RLC safe mode 下写 `SQ_CMD`（kill、broadcast）。已于 2026-10-26 手动验证（`tools/mtlprobe/hangprobe` 使一个 wave 挂起）：GPU 已释放，命令缓冲区在 2.9 秒后完成，WindowServer 仍存活。在 PhxBringup build 33 中加入了看门狗：当有工作队列的 fence 在超过 `debug.phx_hang_ms`（3000）后仍未推进时 → 终止挂起提交的 VMID wave，然后终止所有 wave；计数器为 `debug.phx_hang_kills`。已构建，需在机主在场时安装到 EFI。已于 2026-10-26 02:01 安装（PhxBringup 34）：`hangprobe` —— wave 在 3.3 秒后被终止（`debug.phx_hang_kills` 从 0 变为 1），WindowServer 仍是同一个，没有生成报告。剩余工作：运行 Geekbench OpenCL（Feature Matching）。 |
| B-01 | S3 | USB-C | 连接 USB-C 设备后系统会卡顿。以前只要设备保持连接就会一直卡顿；2026-10-25（iPhone）约半分钟后卡顿自行消失。 | 已修复 | 设备连接时 EC 会发送电源事件，固件调用 `\_SB.ALIB(0x0C)`（APU 功耗限制包），随后 ACPICA 崩溃（“Result stack is empty”，AE_AML_NO_RETURN_VALUE）。已在 `hardware/acpi/SSDT-ALIB.dsl`（.aml）中修复：函数 0x0C 在 Darwin 下返回 Zero，避免 ACPICA 栈故障。 |
| B-02 | S2 | 驱动：显示输出 | 闪烁、白色“泛光”以及屏幕顶部的杂讯条纹。在按 Ctrl+C、点击被模态窗口阻塞的窗口，以及尝试进入睡眠后会发生（移动光标后消失）。 | 待验证 | 关闭 `NVMTL_RP_LIGHT` 后仍然存在。假设（2026-10-26）：未选中文本时按 Ctrl+C，以及点击模态窗口下的窗口，会触发系统提示音；没有音频设备时，macOS 会用屏幕闪烁代替声音，而这些伪影就是该闪烁留下的。现在已有声音（B-03）——需要检查泛光和条纹是否仍然存在。“尝试睡眠后”的情况是显示器睡眠，已在 B-26 中修复。 |
| B-24 | S2 | 驱动：3D | 3D 场景很卡：就连标准的 Chess 也严重卡顿。 | 处理中 | Chess 通过 OpenGL 渲染。测试 `tools/anim/scenetest`：SceneKit（Metal）——118 帧/秒，每帧 CPU 1.4 毫秒；OpenGL——60 帧/秒，每帧 6.4 毫秒。原因在 OpenGL。第一：OpenGL 的 IOSurface 行跨距为 9600 字节，不是 256 的整数倍，因此插件会渲染到单独的图像，并在每一帧用 CPU 复制 14 MB 数据往返。现在复制由 GPU 直接写入表面页：读取一帧从约 13 毫秒加速到 0.45 毫秒，每帧 CPU 从 6.4 毫秒降至 3.6 毫秒。剩余问题：OpenGL 有 43% 的时间在等待 WindowServer 的同步响应（`SLSFlushSurface` → `_CGSWindowIsOrderedIn`），而 WindowServer 只在自己的帧之间响应——这与 B-06 的原因相同。 |
| B-25 | S2 | 网络：UE200 | 只要 UE200 插着，它就会持续占用约 1.5 个 CPU 核心：AppleUserECM 约 79%，kernel_task 约 67%，XHC0 每秒约 14,000 次中断。这会让笔记本发热、消耗电池，并挤占其他所有任务的 CPU。 | 未解决 | 规避方式：拔掉 UE200，Wi-Fi 现在工作正常。 |
| B-29 | S2 | 驱动：计算 | Geekbench 6 GPU（Metal）在第一项 Background Blur 测试中就崩溃。 | 未解决 | MPSGraph 编译 MPS 内核 `depthwiseConv_3x3`（570 KB AIR）；转换器（闭源 NullMoth metal2vulkan）拒绝并报错：“pointer merge storage mismatch Function vs Private”，MPS 因断言失败而崩溃。现在，带 visible 引用的函数会直接推迟到链接阶段，不再尝试预先转换（7d1965c）：此前每次运行都会白白耗时 4.5 分钟。对于链接后的内核，转换器运行约 5 分钟后拒绝；对于清理过（重复的 `@llvm.*`、函数、元数据）并展开的 IR（在 `NVMTL_LINK_DUMP` 转储上运行 `irparse`、`xlatefc`），转换器也会在大约 4.6 分钟后因另一个 private-phi 指针问题而拒绝——需要修改转换器本身。GPU 性能评估的规避方式：通过 OpenCL 运行 Geekbench（不使用 MPSGraph）。 |
| B-33 | S3 | iOS 模拟器 | iOS 模拟器中的 `MercuryPosterExtension`（壁纸）每隔几分钟就会崩溃：`MTLSimBlitCommandEncoder copyFromBuffer:sourceBytesPerRow:…`（MTLSimDriver）中发生 SIGFPE。之后 ReportCrash 会占用约 96% 的 CPU。 | 已修复 | 由于缺少选择器 `minimumTextureBufferAlignmentForPixelFormat:`，`MTLSimDriver` 内部发生除零。已在 `src/nmplugin/NVMTLDevice.m` 中修复（返回 256）。已构建并部署到 `NVMTLDriver.bundle`。 |
| B-06 | S3 | 驱动：FPS | 拖动窗口时，Safari 和“电话”只能达到约 65–70 帧/秒，而不是 120。 | 处理中 | WindowServer 主线程每帧消耗 7–9 毫秒 CPU。主要开销包括：描述符绑定、每帧创建纹理、将帧同步复制到屏幕。2026-10-26 测量（`wintest` heavy，约 75 帧/秒）：WindowServer 主线程有 12.6% 的时间在 `commitAndWaitUntilSubmitted` 中等待 GPU（合成到目标表面以及 `DisplaySurface::AccessComplete`），约 7% 用于描述符（`radv_UpdateDescriptorSets`、`nvmtl_snapshot_set_md`）。NullMoth 中的合成等待通过为 NMXAccel 设置“进行中/已完成”标记来消除（切换帧前调用 `nvSurfWaitReady`）；我们的这些标记此前通过 NVK 导出传递，而 RADV 没有该导出。PhxBringup build 34 —— 选择器 `PHXN_SEL_SURF_GPU_MARK`（22）会把它们传给 NMXAccel；插件使用了该机制（已部署，与旧 kext 搭配时不会产生任何改变）。需要在机主在场时把 PhxBringup 安装到 EFI，然后检查 `wintest` 和画面。已于 2026-10-26 02:01 安装：WindowServer 输出“the swap waits in NVAccel (no composite wait here)”，等待超时为 0。在一次 `wintest` heavy 运行中做 A/B（切换 `debug.nvaccel_surface_gpu_on`）：80/79 帧/秒，对比旧等待方式的 61/61。`scenetest`：GL 63.5 → 68.4，SceneKit 101 → 109 帧/秒。 |
| B-07 | S3 | Wi-Fi | Wi-Fi 通过 AirportItlwm 2.4.0 工作，但 macOS 认为网络未连接（“Network Service Inactive”），且 Wi-Fi 服务名称为“Ethernet”。AirDrop 和 Handoff 无法使用。 | 未解决 | 网络本身工作正常：SSH 通过 Wi-Fi 运行，ping 约 1 毫秒。用于在网络中查找 Mac 的 MAC 地址不适用：在此网络上，macOS 会提供私有 MAC 地址。在 26.5+ 上，Intel AX210 无法使用 AirDrop（没有 awdl0），需要 Broadcom 网卡。 |
| B-08 | S3 | 睡眠、合盖 | 没有真正的睡眠，只有关闭显示器。合盖时的行为尚未配置。 | 处理中 | 固件中的 S3 已关闭（XS3、SS3=0）。已支持 S4（休眠）架构：PhxBringup 已注册到 IOKit 电源树（PMinit、registerPowerDriver、setPowerState）。进入睡眠时 GPU 会进入停泊（parked）状态，唤醒时会重新创建环和队列。 |
| B-09 | S3 | 网络：UE200 | USB 以太网 UE200 启动后无法获取地址，只有重新插拔才有效。 | 未解决 | ue200watch 会重复 DHCP 并重新连接设备，但没有帮助。 |
| B-10 | S3 | 视频 | 没有硬件视频解码和编码，视频由 CPU 解码。 | 未解决 | VCN 4.0.2 尚未启用。 |
| B-11 | S3 | 外接显示器 | 通过 HDMI 和 USB-C 连接的显示器无法工作。 | 未解决 | 已集成来自 Kreytus 的 DCN 3.1.4 和 DMUB 模块（`src/amd/phx_dcn314_*`、`phx_dmub_*`）。正在校准时序和 OTG。 |
| B-12 | S3 | 驱动 | CoreAnimation 的新效果首次出现时会使屏幕卡住 1–4 秒。 | 已规避 | 这是着色器的转换和编译。转换始终会缓存到磁盘，但 RADV 中 WindowServer 的编译此前没有缓存，直到 B-22 修复。在完成 B-22 并使用磁盘缓存 fc 后：重启后未命中 0 次，WindowServer 最长管线耗时 31 毫秒（2026-10-25）。只有该效果第一次出现时需要付出此代价。 |
| B-28 | S3 | 驱动：Night Shift | 开启 Night Shift 时，光标会抖动几秒。 | 已修复 | 原因：“Attempting to get capabilities from plane with no gamma”（每次切换发生 250 次）以及 `CoreDisplay::CombineLUTs` 中的 LUT 软件计算。已在 `src/phxaccel/accel/phxfb.cpp` 修复：实现了 `getGammaTableCapabilities`（3 个通道、256 个条目、8 位）和 `getGammaTable`。 |
| B-13 | S3 | 驱动 | Spotify 在登录系统时启动，有一次连续 3 小时只显示背景。重新启动后恢复正常。 | 待验证 | 看起来像登录时的竞态。仅发生一次，2026-10-21。 |
| B-14 | S4 | 传感器 | CPU 温度和频率尚未检查。 | 未解决 | |
| B-31 | S4 | 关机 | 每次重启都会在 `/Library/Logs/DiagnosticReports` 中留下一个 `shutdown_stall_*.shutdownStall` 报告：关机在某处发生延迟。 | 无法修复 | 01:04 的报告（`spindump -i`）：关机时 audioaccessoryd 仍存活（正在等待已经结束的 coreaudiod），还有 AppleUserHIDDrivers、IOUserDockChannelSerial、SafeBrowsing Safari；约 3 秒后 launchd 将它们强制杀死。这是 macOS 的行为，我们的栈没有参与。 |

## 已规避

| 编号 | 严重度 | 区域 | 原现象 | 规避方式 |
|---|---|---|---|---|
| B-15 | S1 | 驱动 | Apple 神经网络内核（照片分析、XProtect）曾导致 GPU 挂起，进而使 WindowServer 崩溃并触发整机重启。 | 驱动当前拒绝加载 Espresso 和 MPSNeuralNetwork。mediaanalysisd、photoanalysisd 和 Spotlight 不会获得 GPU。phxboot 会在 GPU 挂起时重启机器。2026-11-20：Vision 二维码复现所用 SPIR-V 声明了 Int16 与无格式存储图像能力。RADV 报告支持这些能力，但原 VkDevice 创建时未启用。`src/nmplugin/nvmtl_vk.c` 的候选改动仅启用设备实际报告支持的能力，并在管线创建前校验 SPIR-V 声明。已对 `cnnNeuron` 成功创建管线，未提交计算命令。共享 GPU 上的内核执行尚未验证，不能确认运行安全。候选驱动未安装。2026-11-20：另行修复了因拒绝加载而导致的中止。`src/nmplugin/nvmtl_nn_hide.h` 让 Core ML、Vision、Espresso 查询不到 GPU，推理改在 CPU 上进行，不再加载被拒绝的库；App 本身仍使用 GPU。详见 mechrevo 仓库 `docs/known-issues.md`。 |

## 已修复

| 编号 | 严重度 | 区域 | 原有现象 | 修复措施 |
|---|---|---|---|---|
| B-34 | S2 | 驱动：环境 | Ghostty 1.4.0 每次创建 surface 时退出，包括恢复窗口时。 | Ghostty 1.4 会在首次 Metal 调用前缓存 `environ` 的指针和长度；旧 `NVMTLDriver` 加载时因 `nvrm610.conf` 调用 `unsetenv()`/`setenv()`，随后 `defaultTermioEnv()` 扫描到失效的旧切片。修复：用 `nvmtl_env_get()` 读取配置，不改写宿主环境；移除 `NVRM_FLUSH_TIMEOUT_MS`、Mesa 缓存和 Vulkan loader 的运行时 `setenv()`；仅 Chrome/ANGLE 保留为明确例外。2026-11-23 20:23–20:24 恢复旧驱动后，Ghostty 每次启动都生成新的 fatal 记录，日志出现 `2 inherited conf knob(s) forgotten first`。20:26 安装修复版驱动后，20:26:51 启动的 Ghostty 持续运行，`lsof` 确认映射新驱动，无新 fatal 记录。机主于 20:32 确认双击可正常打开；20:37 重启后 20:58 再次打开正常。提交 `ea8ab6c` 已推送，含补丁、中文记录和回归测试。 |
| B-27 | S4 | 音频 | 设备名曾为“DevShortNameOut” / “DevShortNameIn”。 | PhxHDA 将 `IOAudioDeviceLocalizedBundle` 指向 `/Library/Extensions/AppleHDA.kext`；现在显示为“Built-in Output” / “Built-in Microphone”，2026-10-26。 |
| B-03 | S2 | 音频 | 没有声音。 | 三个原因：AppleALC 找不到来自 `/Library/Extensions` 的 AppleHDA 符号（AppleALC 中的路径、`alc_le_paths.py`）；kernelmanagerd 不响应系统内核缓存（KC）之外 kext 的资源请求——Lilu 插件 PhxHDA 在内核中响应（f48ad38）；layout 7 输出到节点 0x1B，而扬声器在 0x14——`alcid=18`（转储 `hdadump`，137d508）。机主已于 2026-10-25 确认。 |
| B-26 | S2 | 屏幕：显示器睡眠 | 显示器睡眠时，黑色或撕裂画面上方的背光仍然亮着。 | 来自 AppleBacklightDisplay 的亮度 0 现在对应 PWM 0，已关闭的连接也会关闭背光（f48ad38）。测量：睡眠时 PWM 为 0%，唤醒后亮度恢复；机主确认：合盖时屏幕关闭，开盖时亮起。 |
| B-04 | S2 | 屏幕 | 亮度无法调节。 | PhxAMDFB 发布背光处理程序（AppleBacklightDisplay、用于 Fn 的 BrightnessKeys）；亮度值写入两个 PWM 发生器 PWRSEQ0 和 PWRSEQ1（9c168d8）；CoreBrightness 设置 `linear-brightness`，而该参数此前不在参数列表中（3af9781）。机主已于 2026-10-26 确认。 |
| B-22 | S1 | 驱动：着色器 | WindowServer 在启动 40 多秒后卡死，看门狗将其终止，会话重新启动（使用电池且处于节能模式时）。 | Mesa 将 WindowServer（真实 UID 为 root、有效 UID 为 _windowserver）判定为 setuid 程序，并关闭了着色器磁盘缓存。插件向 RADV 提供有效 ID（df01d18）。已在 2026-10-25 启动时验证：WindowServer 的所有管线均来自缓存，最长耗时 31 毫秒，未命中 0 次。 |
| B-05 | S2 | 驱动：文本 | 在 Spotlight 和“信息”中输入的文本会消失并再次出现。 | 行跨距不是 256 字节整数倍的缓冲纹理（文本行、A8 字形掩码、WindowServer 在其上绘制的纹理）现在改为由 GPU 从缓冲区填充的图像（ef414d6、1d93f66、ee2a718）。机主已于 2026-10-25 确认。 |
| B-23 | S2 | 启动 | OpenCore 默认启动 Windows，而不是 macOS。 | `bless --setBoot` 将 macOS 启动项设为第一项。机主已于 2026-10-25 确认：无需选择即可启动 macOS。 |
| B-16 | S1 | 驱动 | 断电后，笔记本启动时没有图形加速。 | phxboot 立即保存 NVRAM（766b21a），2026-10-22。 |
| B-17 | S1 | 驱动 | 连续运行数小时后，显示器唤醒后屏幕不再刷新。 | PhxFB 持久显存池（c4e8df0），2026-10-21。 |
| B-18 | S2 | 电池 | 电池不显示，电源适配器被视为已断开。 | SMCBatteryManager 和 SSDT-ADP1，2026-10-21。 |
| B-19 | S2 | Bluetooth | Bluetooth AX210 无法工作。 | IntelBluetoothFirmware 2.5.2，2026-10-21。 |
| B-20 | S2 | 驱动 | CoreImage 模糊输出为黑色。 | 描述符集中的绑定顺序（4574303），2026-10-21。 |
| B-21 | S3 | 驱动 | Liquid Glass 以 4 像素块绘制。 | WindowServer 中的线性采样器，2026-10-20。 |
