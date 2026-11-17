# 已知问题

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
