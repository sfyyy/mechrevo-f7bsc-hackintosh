# 已知问题

## 拖动窗口时画面撕裂（已修复：`macos/nullarc/4-dbuf.sh` + `macos/display`）

快速拖动窗口时，窗口内容被横向切成两截，停下后立刻恢复。

原因：nullarc 0.4.0 在 780M 上不做硬件翻页，每一帧由 CPU 直接拷进正在被扫描的那一块缓冲（`nvAccelIopFlip` 只对
NVIDIA 生效）。拷贝时机由软件 VBL 定时器决定，和面板真实的 vblank 没有相位关系，扫描线落在哪里就从哪里撕开。

修复（`patches/phxaccel-dbuf.patch`，基于 v0.4.0-alpha）：在 carve-out 偏移 512 MiB 处再放一块扫描缓冲，帧拷进
不在屏幕上的那一块，再把 HUBP0 的 `PRIMARY_SURFACE_ADDRESS` 指过去，由 DCN 在 vblank 锁存；写之前等上一次
`FLIP_PENDING` 清零。启用前逐项对照硬件当前状态（扫描地址、pitch、线性布局、FB 窗口、system aperture、
update lock），任何一项不符就保持原来的单缓冲。

- 开关 `debug.phxfb_dbuf`：0 关（驱动默认）、1 开、N>1 试用 N 秒后自动退回。`macos/display` 开机设为 1。
- 状态：`sysctl debug.phxfb_dbuf_state`（1 生效；负数为拒绝原因，含义见 `sysctl -d debug.phxfb_dbuf_state`），
  `debug.phxfb_dbuf_flips` / `_wait_timeouts` / `_wait_max_us`。
- 本机实测：约 6000 次翻页、0 次超时，最长等待约 15 ms（不到一帧）。

## 动画残影，一截图就消失（nullarc B-58，已绕过：`macos/display`）

打开带弹出动画的软件时，动画中途的画面（或鼠标指针）残留在屏幕上；截图时 WindowServer 重新合成，残影消失，
所以截不到。

原因：默认 `debug.nvaccel_surface_gpu_on=1` 时，WindowServer 不等 GPU 合成完成就交出画面，改由内核按标记等待；
这套等待有遗漏，会显示没画完的画面。上游尚未定位根因（BUGS.md B-58，`docs/AUDIT-2026-11-14.md`）。本机与撕裂无关：
双缓冲打开、`surface_gpu_on=1` 时残影照样出现，并记录到一次 `debug.nvaccel_surface_gpu_timeouts`。

- 绕过：`debug.nvaccel_surface_gpu_on=0`，并在 `/Library/GPUBundles/nvmtl/nvrm610.conf` 写入
  `NVMTL_WS_WAIT_COMPLETION=1`，让 WindowServer 自己等每次合成完成。`macos/display/install.sh` 两项都做。
- 代价：WindowServer 每帧多等一次 GPU，帧率可能略低。

## 核显空闲时仍"忙碌"（nullarc#14）

`GRBM_STATUS = 0xa800302c`（`macos/check.sh` 会显示）。nullarc 0.4.0 的 mesboot 没有设置 `CP_MEC_DOORBELL_RANGE`，
gfx 队列建立后命令处理器一直报告忙碌，PMFW 把 GFXCLK 钉在 2600 MHz，空闲时风扇高速转。

- 现状：`macos/thermal` 把上限压到 800 MHz 并打开时钟门控，缓解但未根治（GPU 在 800 MHz 下空转）。
- 根治：用 nullarc#14 的 mesboot（建队列前写入 Linux 的门铃范围 `0 / 0x450`）重新编译。上游尚未合并、没有发布版本。
  队列建立后再写这些寄存器无效。

## 截屏 / 录屏时 WindowServer 崩溃

2026-11-17 20:29 记录到一次 WindowServer `SIGABRT`：

```
MetalBacking::AbortWithTextureInfo
MetalIOSurfaceBacking::PrepareForUse
MetalBacking::CreateCompositeDestination
CaptureSurfaceMetal::Populate
WS::Capture::create_iosurface_for_window_list
_XHWCaptureWindowListToIOSurface
```

有程序请求窗口截图（截屏、录屏、共享屏幕、微信截图等），nullarc 的 Metal 驱动（`com.nullmoth.NVMTLDriver`）
没能为截图目标创建 IOSurface 纹理，WindowServer 退出，表现为黑屏后回到登录界面。上游 BUGS.md 与 issue 中尚无记录。
目前只能避开这类功能。

## 没有 HiDPI

PhxAMDFB 只报告固件 GOP 点亮的一个模式（2560×1440），没有缩放器，也没有 `kDisplayModeValidForHiResFlag`，
所以 one-key-hidpi、BetterDisplay 的覆盖文件不会产生 HiDPI 选项（nullarc#11）。

- dandingdjianjiao/nullarc 的 `thinkbook-hidpi-fb-20261110` 分支给该模式加上 HiRes 标志，只能得到严格 2 倍：
  2K 屏上为「看起来像 1280×720」。需要在 macOS 上用 MacKernelSDK 重新编译 PhxAccel。
- 「看起来像 1920×1080」等非整数倍需要显示管线支持缩放，目前没有实现。

## 只能用 HDMI 单屏

nullarc 0.4.0 不驱动外接显示接口的 PHY，本机靠固件 GOP 在 HDMI 上点亮的画面工作。分辨率与刷新率由固件决定，
启动后不能更改；热插拔未经验证。
