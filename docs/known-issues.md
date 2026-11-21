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

原因：默认 `debug.nvaccel_surface_gpu_on=1` 时，WindowServer 不等 GPU 合成完成就交出画面，改由内核按标记等待。
2026-11-21 已定位为 surface mark 的编号/transaction 边界缺失（见下节）；此前 `BUGS.md` B-58 的“等待有遗漏”只是现象。
本机与撕裂无关：双缓冲打开、`surface_gpu_on=1` 时残影照样出现，并记录到一次 `debug.nvaccel_surface_gpu_timeouts`。

- 绕过：`debug.nvaccel_surface_gpu_on=0`，并在 `/Library/GPUBundles/nvmtl/nvrm610.conf` 写入
  `NVMTL_WS_WAIT_COMPLETION=1`，让 WindowServer 自己等每次合成完成。`macos/display/install.sh` 两项都做。
- 代价：WindowServer 每帧多等一次 GPU，帧率可能略低。

### 2026-11-21：B-58 根因修复候选

根因不是 GPU 慢，而是 surface completion mark 只有“每个 surface 一个 bit”的语义：WindowServer 的重叠 command buffer
会共享同一个 pending 状态，已排队的 flip 因此会等待 transaction 之后提交的新 command buffer；超过 100 ms 后等待
超时，scanout 继续读到未完成的旧 surface（鼠标残影、旧帧）。这也解释了为什么 `surface_gpu_on=0` 的同步等待可以
让残影消失，但代价是主线程每帧多等约 4–6 ms。

修复改为编号 mark：每个 `(surface, command buffer)` 分配递增编号，DisplayPipe transaction 入队时记录当前编号快照，
flip 只等待 `<= 快照` 的 mark；后续 command buffer 不会延长当前 flip 的等待，超时也只丢弃本 transaction 范围内的
mark。旧的无编号 ready 仍保留兼容语义。代码位于：

- `nullarc-0.4.0-alpha/src/phxaccel/accel/nvrm-accel.cpp`
- `nullarc-0.4.0-alpha/src/phxaccel/nvrm_surfshare_abi.h`
- `nullarc-0.4.0-alpha/src/phxbringup/PhxBringup.cpp`
- `capfix/nmplugin/NVMTLEventSync.m`、`capfix/nmplugin/nvmtl_vk.c`
- 可复用补丁：`macos/nullarc/patches/phxaccel-surface-marks.patch`、`macos/nullarc/patches/nvmtl-surface-marks.patch`
- 单元测试：`macos/nullarc/tests/test-surface-mark-seq.py`

**尚未安装/重启验证**。一键安装器：`/Users/sfyyy/Documents/apple/work/install-surface-marks.command`（会备份、
校验并安装三者，不自动重启）；回滚器：`rollback-surface-marks.command`。候选 kext 和插件均已原生编译；
`test-surface-mark-seq.py` 验证了“后续 mark 不延长等待”、
超时只清理旧 mark、512 槽和序号回绕。`wsnowait`、`surftest`、`desctest`、`carender` 在未安装插件路径下通过。
验证通过后，`phxdisplay.sh` 才能把 `debug.nvaccel_surface_gpu_on` 改回 1，并删除 `NVMTL_WS_WAIT_COMPLETION=1`
兜底。

## 锁屏动画、菜单、系统设置局部花屏 / 撕裂（已修复，待验证：`macos/display`）

锁屏 / 解锁的过渡动画、打开系统菜单、在「关于本机」「系统设置」里快速移动时，偶尔出现横向断层或局部花块，
画面停下后恢复。`debug.phxfb_dbuf` 开、关都会出现，`debug.nvaccel_surface_gpu_on=0` 也不能消除，所以与双缓冲和
B-58 的绕过无关。

原因：这些画面由各自的进程（loginwindow、系统设置、菜单所属的程序）用 Metal 画进 IOSurface。CoreAnimation 在命令
缓冲 *scheduled* 后就把 surface 交给 WindowServer（苹果 GPU 上各进程的工作按提交顺序执行，scheduled 即可保证顺序）。
nullarc 上每个进程是独立的 Vulkan 队列，彼此没有顺序；插件默认在提交后立即报告 scheduled，WindowServer 合成时
GPU 可能还没画完，或者行宽不是 256 字节倍数的 surface 还没从私有图像拷回，于是采到画了一半的内容。

修复：nullarc 插件自带开关 `NVMTL_SCHEDULE_AFTER_SURFACE_WRITES=1`（默认关）：写了共享 IOSurface 的命令缓冲要等 GPU
完成、内容回写之后才报告 scheduled（CAMetalLayer 的 drawable 不受影响）。`macos/display/install.sh` 把它写进
`/Library/GPUBundles/nvmtl/nvrm610.conf`，所有进程都会读取；每个进程下次启动时生效，所以装完要注销或重启。

- 验证生效：`log show --last 10m --predicate 'process == "loginwindow" AND senderImagePath CONTAINS "NVMTL"' | grep conf`
  显示读到的开关数（含本项为 3 个）。
- 代价：这些程序每次提交多等一次自己的 GPU 工作，动画帧率可能略低。
- 撤销：从 `nvrm610.conf` 删掉这一行再注销，或 `sudo bash macos/display/install.sh --remove`。

## 核显空闲时仍"忙碌"（nullarc#14）

`GRBM_STATUS = 0xa800302c`（`macos/check.sh` 会显示）。nullarc 0.4.0 的 mesboot 没有设置 `CP_MEC_DOORBELL_RANGE`，
gfx 队列建立后命令处理器一直报告忙碌，PMFW 把 GFXCLK 钉在 2600 MHz，空闲时风扇高速转。

- 现状：`macos/thermal` 把上限压到 800 MHz 并打开时钟门控，缓解但未根治（GPU 在 800 MHz 下空转）。
- 根治：用 nullarc#14 的 mesboot（建队列前写入 Linux 的门铃范围 `0 / 0x450`）重新编译。上游尚未合并、没有发布版本。
  队列建立后再写这些寄存器无效。

## 截屏 / 录屏时 WindowServer 崩溃（已修复：PhxBringup build 35）

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

之后 2026-11-19 20:24、11-20 09:06、10:00 又各一次，触发点是截屏、录屏、显示器休眠前截桌面，或新建显示表面。
插件日志在崩溃前都是 `vk: colour image: VRAM full and no system-memory type takes it -> -2`，而当时图像只占几十 MB。

**根因不是性能，是 PhxBringup 每个进程的 GPU 页表上限。** 每个进程的页表是 64 块 × 64 KiB = 1024 页，
每页映射 2 MiB，合计约 2 GiB。显存池、GTT 池和导入的 IOSurface 都计入这一上限。
`tests/vkprobe/vkfill` 一直申请到失败：不论先申请哪种内存，总量都停在 2032 MB（先 VRAM 1792 MB，GTT 只剩 240 MB）。
WindowServer 长时间运行后加上截屏缓冲就会撞墙，映射失败 → 纹理为空 → SkyLight `abort`。
另外，RADV 在 APU 上按「可见 256 MB + GTT 512 MB」的 2/3 报堆大小，插件因此以为显卡只有 512 MB，
会过早把纹理驱逐或挪到系统内存，这也是卡顿来源之一。这些池在 APU 上都是锁定的系统内存，额度是人为设的。

修复 `macos/nullarc/patches/phxbringup-ptfix.patch`（build 35，`EFI/OC/Kexts/PhxBringup.kext` 已更新，只换二进制，
Info.plist 保持 F7BSC 版）：

| 项 | build 34 | build 35 |
|---|---|---|
| 每进程页表（可映射总量） | 64 块 ≈ 2 GiB | 256 块 ≈ 8 GiB |
| VRAM hi 池 | 1536 MiB | 3840 MiB |
| GTT 上限 / 单块 | 512 / 256 MiB | 2048 / 1024 MiB |
| RADV 堆（VRAM / GTT） | 512 / 256 MB | 1536 / 768 MB |
| `vkfill` 可申请总量 | 2032 MB | 6128 MB |

页表满时内核日志会出现 `PhxBringup: GPU page tables of context N full`。同一份原版源码用本机 CLT 编译，与 nullarc
0.4.0 发布的二进制逐字节一致，说明补丁之外没有引入编译差异。验证：`tests/vkprobe/vkheap`（看堆大小）、`vkfill 0 2`。

## 调用 Vision 识别（文字、二维码）的程序崩溃（nullarc 驱动 bug，已修复：`patches/nvmtl-nn-hide.patch`，已安装）

2026-11-18 11:27–11:28，Shottr 1.10.0 连续 3 次启动即退出（`SIGABRT`，Abort trap: 6）。Shottr 启动时会调用 Vision 的
`VNRecognizeTextRequest`（OCR）。崩溃线程：

```
TextRecognition → espresso_plan_add_network → Espresso::load_network
→ Espresso::cpu_context_transfer_algo_t::is_kernel_registered_on_engine
→ Espresso::MPSEngine::pool_kernel::recreate_kernel → MPSNeuralNetwork
→ MPSDevice::GetLibrary → MTLReportFailure → __assert_rtn → abort
```

断言原文：

```
MPSCore/Utility/MPSDeviceComputePipelineState.mm:60: failed assertion `Failed to load library at
".../MPSNeuralNetwork.framework/Resources/default.metallib" error: "(null)
newLibraryWithFile: neural-network kernels are not run on this GPU"'
```

原因：错误信息来自 nullarc 插件本身。`src/nmplugin/NVMTLDevice.m` 的 `nvmtl_nn_library()` 拒绝加载 Espresso 与
MPSNeuralNetwork 的 metallib（上游 B-15：这些神经网络内核会让 780M 挂起，曾拖垮 WindowServer）。代码注释认为
「框架会改在 CPU 上运行」，实际上 MPSCore 拿到 nil 库直接断言退出，不会回落到 CPU。

- 影响面不只 Shottr：任何在自身进程里调用 Vision 文字 / 人脸 / 条码识别（以及走 Espresso 的 Core ML 模型）的程序
  都会同样崩溃。上游对 mediaanalysisd、photoanalysisd、Spotlight 的做法是整进程拒绝 Metal，所以没有暴露这条路径。
- **`usesCPUOnly = YES` 也崩溃**：Espresso 在决定在哪里运行之前就先在 MPS 引擎上注册内核，所以程序自己无法绕开。
- 复现：`macos/nullarc/tests/vision-ocr.m`（对一张空白图做一次 `VNRecognizeTextRequest`，编译方法见文件头）。
  默认与 `cpu` 参数两种运行方式均以 134 退出并打印上面的断言。没有 Metal 设备的进程（在拒绝名单里）则正常返回。

当前绕过（只针对 Shottr）：把 `Shottr` 加进 `phxboot.sh` 的 `DENY`，开机生成的 `/Library/GPUBundles/nvmtl-allow.txt`
含 `-Shottr`，Shottr 拿不到 Metal 设备，Vision 走 CPU。

- 改动位置：已安装的 `/Users/Shared/gpu/phxboot.sh`（原文件备份为 `phxboot.sh.bak-shottr`）与源码树
  `nullarc-0.4.0-alpha/tools/phxctl/phxboot.sh`。本仓库脚本不负责安装 phxboot.sh，**重新安装 nullarc 会覆盖此改动**。
- 代价：Shottr 整个进程不用 GPU（OCR 变慢，其它 Metal 用途也落到 CPU）。其它调用 Vision 的程序需逐个加入名单。
- 注意：**不要在开机后手动运行 `phxboot.sh`**。它只应开机运行一次；系统已启动时重跑会触发 M2 保护
  （「WindowServer opened the panel before the arming」）：删除 `phxboot-m2`、清掉 NVRAM `phxfb-boot` 并重启，
  之后 WindowServer 留在固件帧缓冲上（整机卡顿、没有玻璃效果）。恢复：
  `sudo touch /Users/Shared/gpu/phxboot-m2 && sudo nvram phxfb-boot=1`，然后正常重启。

待修复（基于 v0.4.0-alpha 源码，在插件里改，而不是逐个拒绝程序）：

1. **只对 Espresso / MPS 神经网络路径隐藏 GPU**（优先尝试）：识别来自 Espresso 的设备查询并回答「没有 GPU」，
   效果等同拒绝名单，但只限神经网络；程序的渲染和其它 Metal 用途仍走 780M。需要先反汇编 Espresso、MPSCore，
   确认它们如何获取设备、如何决定是否使用 GPU，再选挂钩点。
2. 不在加载库时拒绝，改在更晚的阶段拒绝：取决于 MPS 有没有容错路径，不确定，可能只是把断言挪到别处。
3. 根治 GPU 挂起，让神经网络内核真正在 780M 上运行：涉及闭源翻译器（参见上游 B-15、B-29），工作量大。

验证标准：`vision-ocr` 与 `vision-ocr cpu` 均打印 `ok=1` 并以 0 退出；把 `Shottr` 移出 `DENY` 后 Shottr 能启动、OCR 正常，且仍在使用 GPU。

2026-11-19 QQ 7.0.3 同样崩溃 5 次：QQ 主进程的 `QBAR_THREAD`（`scanQBar`，识别图片里的二维码）调用
`VNDetectBarcodesRequest`，堆栈相同，只是入口是 Vision 的条码检测器。2026-11-20 临时把 `QQ` 也加进 `DENY`。

**修复（方向 1，2026-11-20）**：`patches/nvmtl-nn-hide.patch`（新文件 `src/nmplugin/nvmtl_nn_hide.h`，在
`initWithAcceleratorPort:` 设备建好后安装）。只让推理栈看不到 GPU，程序自己的 Metal 照常用 780M。逆向结果：

- Vision 从 Core ML 的 `MLAllComputeDevices()` 选计算设备，它读 `-[MLGPUComputeDeviceRegistry registeredComputeDevices]`；
  这个注册表由 Core ML 调 `MTLCopyAllDevicesWithObserver` 建立。Vision 选了 GPU，就让 Espresso 建 MPS 上下文（引擎 5）。
- Espresso 自己通过 `MTLCopyAllDevices`、`MTLCreateSystemDefaultDevice`、`MPSGetPreferredDevice` 取设备。
  CPU 计划在 `cpu_context_transfer_algo_t::is_kernel_registered_on_engine` 中用这个设备注册 MPS 内核，这就是
  `usesCPUOnly` 也会崩的原因。
- 共享缓存（x86_64）里各 dylib 调用这些函数都经过自己的 `__stubs`（`jmp *slot(%rip)`），但槽位在缓存统一的 GOT 中，
  每个符号只有一个槽，所有 dylib 共用。所以不能只改 Espresso 的槽。

做法：

1. Core ML：替换 `-[MLGPUComputeDeviceRegistry registeredComputeDevices]`，返回空数组。插件加载前已建好的注册表也会
   失效，因为 `MLAllComputeDevices()` 每次都重新读它。同时，从 Core ML 代码发出的 `MTLCopyAllDevicesWithObserver`
   调用也返回空列表（观察者照常注册）。这一条覆盖 Core ML 在插件之后才加载的情况：dyld 报告镜像时它的 ObjC 类还没
   注册，所以由这次调用来完成上面的方法替换。
2. Espresso：上面三个查询函数的共享 GOT 槽换成包装函数。包装函数只在返回地址落在 Espresso 的 `__TEXT` 内时回答
   「没有设备」，其它调用方原样转发给真函数。

于是 Vision 和 Core ML 在 CPU 上做推理，不再加载 MPSNeuralNetwork 库；`nvmtl_nn_library()` 的拒绝仍然保留作为兜底。
WindowServer 不安装这些钩子；`NVMTL_ALLOW_NN=1` 时整个功能关闭。

验证（未安装，`tests/runp.sh` 加载）：`tests/vision-nn.m` 的 5 种模式全部以 0 退出，并且确实识别出内容（二维码
`https://www.qq.com`，OCR 读出 `Hello Vision 12345`）：Vision 是进程里第一个用 Metal 的、程序先建设备、Vision 用
dlopen 后加载、OCR 默认、OCR `usesCPUOnly`。程序自己的设备仍是 780M（`MTLCopyAllDevices=1`，命令缓冲状态 4）。
已安装的旧插件下，这些模式全部以 134 退出。`carender` 600 帧 × 40 层 PASS，0 错像素，耗时与旧插件相同。
编译基线：去掉 MPS 能力候选改动后的源码，编译结果与已安装插件逐字节一致（`7cecd00b`），因此本补丁是唯一差别。

安装：`sudo bash work/install-nnhide.sh`。它替换插件，并把 `QQ`、`Shottr` 移出拒绝名单（`nvmtl-allow.txt` 与
`phxboot.sh` 的 `DENY`，`phxboot.sh` 恢复到加 Shottr 之前的原样）。新启动的程序立即生效，WindowServer 重启后才换新插件。
撤销：`sudo bash work/rollback-nnhide.sh`。

已安装（2026-11-20 11:31，11:43 重启）：安装后的插件除代码签名外与 `NVMTLDriver.nnhide` 逐字节一致。重启后，用已安装
插件直接运行（不经 runp）`vision-nn` 的 5 种模式与 Swift 二维码复现，全部识别成功并以 0 退出。系统日志中，QQ 与 Shottr
均为 `allow-listed`（有 GPU），并出现三行 `nn-hide` 安装记录。Shottr 启动时做 OCR，日志显示推理栈请求设备被答复「无」，
随后正常运行，没有崩溃报告。WindowServer 自重启后没有新的崩溃。
用户验收（11:48）：在 QQ 里识别图片二维码成功，没有闪退。日志中 QQ 主进程有一行「推理栈请求设备 → 答复无，改在 CPU 上进行」。

仍未覆盖（只有兜底的断言）：程序直接用 MPSNeuralNetwork / MPSCNN（不经 Espresso）；程序手动把 780M 设备交给 Core ML
（`MLModelConfiguration.preferredMetalDevice`）；以及插件加载之前 Espresso 就已经取到了设备（插件加载发生在进程第一次
枚举设备时，Vision 和 Core ML 的流程里这一步总是 Core ML 的注册表先做，所以实测不会出现）。

## Chrome 等程序卡顿，WindowServer 占满一个核（已修复：`patches/nvmtl-surface-key.patch` + `nvmtl-perf2-surface-in.patch`，尚无安装脚本）

CPU、内存都不高，但 Chrome 滚动、拖动窗口明显卡顿。`chrome://gpu` 里的 `non-existent mailbox` 报错和 Skia Graphite
都不是原因：用隔离配置测试，换成 Ganesh、软件合成，甚至 `--disable-gpu`，WindowServer 都在约 70%，而且窗口越小
负载反而越高（640×400 时 78%，2560×1400 时 55%）。说明这是每帧固定的开销，与像素多少无关。

原因（`sudo sample WindowServer` 采样 8 秒）：WindowServer 57% 的 CPU 在 nullarc 插件的 `nvmtl_surface_keep` 里。
`src/nmplugin/NVMTLDevice.m` 为 IOSurface 纹理做的缓存以 `NSArray`（7 个 `NSNumber`）为键，而 `-[NSArray hash]`
只返回元素个数，所有键都落进同一个桶，每次查找都要逐个比较数组；每次插入还会遍历整个字典、再逐个查一遍，
开销随存活的 surface 数呈平方增长。Chrome 同时持有大量 IOSurface（分块、视频帧、多标签），所以最先暴露。
`nvrm610.conf` 里的 `NVMTL_WS_WAIT_*` 等待项只占约 0.2%，与此无关。

修复（基于 v0.4.0-alpha）：键改为 7 个 `uint64` 打包成的 `NSData`（按内容哈希），清理改为遍历一次、批量删除。

- 本机实测（同一测试页 60 fps）：WindowServer 从约 70% 降到 38%。
- 编译：`sh src/nmplugin/build.sh <Vulkan-Headers v1.4.362>/include` 生成 `NVMTLDriver.new`。安装时替换
  `/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver`
  并重新 ad-hoc 签名，重启后 WindowServer 才加载。**不要直接用上游的 `tools/phxctl/plugdeploy.sh`**：
  它会把 `nvrm610.conf` 覆盖成一行，丢掉 `macos/display` 写入的开关。
- 待办：仿照 `4-dbuf.sh` 写安装脚本（下载 nullarc 与 Vulkan-Headers、打补丁、编译、备份、安装、保留 conf）。

后续优化方向（WindowServer 合成每帧是单线程串行的，插件的翻译开销全部落在它的渲染线程上，只能靠降低每帧开销）：

1. ~~每帧重复创建 / 释放 GPU 内存~~：已由 `nvmtl-perf2-surface-in.patch` 解决，见下文。
2. **每次 draw 新建描述符集**：已修复（`nvmtl-fetch-mask.patch`），见下文。
3. **把非关键路径的工作移出渲染线程**：surface-in 的拷贝已移出（见下文）；资源回收、BO 释放尚未处理。

### 第二轮：surface-in 拷贝延迟提交 + surface 缓存短暂保留（`patches/nvmtl-perf2-surface-in.patch`，2026-11-19）

在上一个补丁之上打。修复后重新采样（测试页 60 fps，WindowServer 用户态忙碌样本计 100%）：

- CoreAnimation 每帧为同一批 IOSurface 新建 MTLTexture。Chrome 每个图层轮流使用 3–4 个 IOSurface，CA 一释放旧的，
  缓存就把它的 backing 丢掉，下一帧同一个 surface 回来又要导入页面、建私有图像、映射 VA，私有图像还要整张上传：
  每秒约 190 次，约占 10%（`nvmtlNewSurfaceTexture` 578 个样本，`-[NVMTLTexture dealloc]` 206 个）。
- 行宽不是 256 字节倍数的 surface（RADV 不给建线性图像）走私有图像。它的内容在 `setFragmentTexture` 里同步拷贝：
  单独提交一次，原地等 GPU 完成（`nvmtlSurfaceIn` 373 个样本，约 5%）。这类拷贝占 WindowServer 全部提交的一半。

改法：

- surface-in 不再单独提交：编码时只把纹理记进待办列表，在下一次提交时录进已有的 prologue 命令缓冲
  （`nvmtl_vk_pre_image_from_host`，和 managed 上传、zero-fill 同一套机制）。原来的同步拷贝在编码时执行、排在之后
  所有提交之前；prologue 跟着编码后的第一次提交走，GPU 执行顺序不变。待办超过 64 个或 prologue 打不开时，退回同步拷贝。
  开关：`NVMTL_DEFER_SURFACE_IN=0` 恢复旧行为。
- surface 缓存：只剩缓存持有的 backing 再保留 300 ms（够一组 surface 轮换一圈），空闲最多 32 个、64 MB，
  清理最多每 50 ms 一次。开关：`NVMTL_SURFACE_KEEP_MS`（0 = 旧行为）、`NVMTL_SURFACE_KEEP_IDLE`、`NVMTL_SURFACE_KEEP_MB`。
  WindowServer 日志每 10 秒一行 `surface keep: N hits, N new, ...`。

结果（同一时段、刚开机负载降下来后，各跑 3 次 `perf/bench2.sh`，用户的 Chrome 关闭；补丁组时 Chrome 实际是开着的）：

| 插件 | 测试页平均 FPS | 每 25 秒卡顿帧（>25 ms） | WindowServer CPU |
|---|---|---|---|
| 只打 surface-key 补丁 | 57.0–57.2 | 33–36 | 36–41% |
| 再打 perf2 补丁 | 58.1–58.8 | 17–23 | 24–38% |

同一个二进制用开关关掉新行为再测（`NVMTL_DEFER_SURFACE_IN=0`、`NVMTL_SURFACE_KEEP_MS=0`）：54.7–56.3 fps，卡顿 47–55。

**第一版的教训（已回滚）**：第一版让 backing 保留 1 秒、空闲上限 384 MB，并且每次拷贝单独异步提交（各占一个命令池、
命令缓冲、fence、IB），要等同一张图像下次被拷贝才释放。PhxBringup 给每个进程最多 4095 个 BO 句柄、512 MB GTT，
WindowServer 平时就有一千多张图像。资源耗尽后 RADV 建不出命令缓冲（`radv_create_cmd_buffer` 在出错路径里自己崩溃），
WindowServer 连续崩溃 3 次，phxboot 关掉加速，桌面退回软件合成（135% CPU，30 fps）。重启时还有一次内核 panic
（`mach_vm_page_range_query` → `phys_attribute_test`，发生在崩溃诊断扫描内存时），没有确认原因。
恢复：换回旧插件，`touch /Users/Shared/gpu/phxboot-m2`，`nvram phxfb-boot=1`，重启。

测试方法（**新插件先在应用进程里测，再让 WindowServer 加载**：插件装好后新启动的应用立即使用，WindowServer 要重启才换）：

- `macos/nullarc/tests/surftest.m`：按 CA 的方式每帧重新包装 IOSurface 纹理，用 blit 读回逐像素校验；分别测 4 个 surface
  轮换和每帧新建再释放两种情况。设 `NVMTL_LOG_PATH` 后，可在插件日志里看 image census 是否封顶。
- `macos/nullarc/perf/bench2.sh`：临时 Chrome 配置打开测试页 25 秒，输出全部 3 秒窗口的平均 FPS、卡顿总数、
  WindowServer 平均 CPU。（旧的 `run.sh` 只显示最后两个窗口，曾让人误以为“稳定 59 帧”。）
- `macos/nullarc/perf/wsprof.sh`（sudo）：跑 bench2 的同时对 WindowServer 和 Chrome GPU 进程各 `sample` 8 秒；
  `sampstat.py` 按调用链统计忙碌样本、按调用方归类等待，`callers.py` 查某个函数被谁调用。
- `macos/nullarc/perf/gpuinfo.py`：通过 DevTools 协议导出 `chrome://gpu` 全文。本机：Graphite → Dawn → Metal →
  NVMTL → RADV，Zero-copy 分块，无驱动报错，无回退，所以 Chrome 侧没有可调的配置。

## WindowServer 每次 draw 新建描述符集（已修复：`patches/nvmtl-fetch-mask.patch`，2026-11-19 已装入 WindowServer）

perf2 之后，这是 WindowServer 用户态里最大的一块插件开销（忙碌样本 1948 个中，`nvmtlBindSamplerPipeline` 298 个，约 15%；
其中 `nvmtl_snapshot_set_md` 203 个，`radv_UpdateDescriptorSets` 130 个）。

**真正原因**：不是“CA 每个 draw 都换纹理”，而是每个新集合都写满了全部 193 个描述符（32 个缓冲、128 张纹理、
32 个采样器、地址表），每次 draw 两个集合（顶点、片元）。实测：每个集合写 193.00 个，复制 0 个。

- 插件靠 `nvmtl_spirv_bmask` 从 SPIR-V 算出每个阶段实际用到哪些 binding，只写这些。
- CoreAnimation 的片元着色器都声明了 framebuffer fetch 输入（`[[color(n)]]`，set 3、binding 192）。这个函数只排除了
  set 2（bindless 堆），把 set 3 的 192 也当成本阶段的 binding。192 不在合法范围内，于是判定失败，退回“全部 binding”。
- set 3 本来就是单独绑定的（`nvmtl_fetch_set`；WindowServer 默认关 fetch，绑的是全零集合），不该算进来。

修复：`nvmtl_spirv_bmask` 把 set 3 的变量和 set 2 一样排除，一行条件。开关：`NVMTL_FETCH_MASK=0` 恢复旧行为。
不需要跨命令缓冲缓存（上午那一版缓存在 WindowServer 里约 6 分钟后让 GPU 挂住，看门狗杀掉 WindowServer，
phxboot 关闭加速，已回滚；源码留在 `work/desccache-attempt-1253`）。

测试（全部在应用进程里跑，插件不安装、WindowServer 不动，见下面的 `runp.sh`）：

- `tests/carender.m`：在进程内用 CoreAnimation 自己的 Metal 渲染器（`CARenderer`）渲染一棵和 WindowServer 类似的图层树：
  IOSurface 内容、每帧轮换 surface、圆角、移动，并逐帧回读检查每个图层中心的颜色。它的采样分布和 WindowServer 一致
  （`nvmtlBindSamplerPipeline` 18%、`radv_UpdateDescriptorSets` 8%），可以替代 WindowServer 做测试。
- 结果：每个集合写的描述符从 193 个降到 6.8 个；`nvmtlBindSamplerPipeline` 的样本数降 66%（466 → 158），
  `radv_UpdateDescriptorSets` 基本消失。按 WindowServer 的配置（fetch 关）跑，CARenderer 每帧 CPU 时间降 10–20%（每轮 3 次）。
- 正确性：carender 最多 10000 帧 × 120 层 × 8 个 surface，0 个错误像素；半透明图层整帧回读和旧插件逐字节一致
  （fetch 开、关各两帧）；`surftest`、`desctest`、`descsurfdraw`、`descdrawstress` 全部通过。

安装（`work/` 下，需要 sudo，WindowServer 重启后才加载）：`sudo bash install-fetchmask.sh`（装 `NVMTLDriver.fetchmask`，
即 perf2 加本补丁），然后重启。回滚：`sudo bash rollback-fetchmask.sh` 后重启；WindowServer 崩溃时用
`sudo bash recover-fetchmask.sh`（回滚、清除 phxboot 的关闭标记、重新启用 PhxAMDFB、重启）。

WindowServer 验收（2026-11-19，装好后重启，开机负载降下来后测）：

| | perf2（`s2`） | perf2 + fetch-mask（`fm-ws`） |
|---|---|---|
| WindowServer 忙碌样本（8 秒） | 1948 | 1075 |
| `nvmtlBindSamplerPipeline` | 298（15.3%） | 52（4.8%） |
| `nvmtl_snapshot_set_md` / `radv_UpdateDescriptorSets` | 203 / 130 | 低于统计阈值 |
| bench2（3 轮）FPS / 每 25 秒卡顿 / 最慢帧 | 58.1–58.8 / 17–23 / 67 ms | 57.8–58.8 / 14–26 / 67–68 ms |

WindowServer 日志里有 `framebuffer-fetch inputs (set 3) left out of the per-stage descriptor mask`。用户态忙碌样本少了约
45%，FPS 和卡顿持平：卡顿和 67 ms 来自下面一节的 CVDisplayLink，不是 WindowServer 的 CPU。还要再观察一段时间，
确认长时间运行没有问题。

`tests/runp.sh DRIVER 命令...`：让测试程序加载一个**未安装**的插件。第一次运行时把已安装的 bundle 复制到 `tests/drv`
（翻译器和 air-runtime 要在同一目录），编译 `tests/shim/redirect.c`。这个 DYLD 垫片把 Metal 对 NVMTLDriver 的
dlopen 改到指定文件，并在 `class_getImageName` / `objc_copyClassNamesForImage` 上返回 bundle 路径（Metal 会核对这两项）。
不用 sudo，也不用重启，只影响这一个进程。

## 测试页最慢一帧总是 67 ms（已修复：`patches/phxaccel-vbl-timing.patch`，2026-11-19 已安装）

每 3 秒窗口的最慢帧几乎固定是 66–68 ms，也就是连丢 3 帧；一轮 25 秒里有 17–36 次 >25 ms 的帧。原插件和 perf2 都有
（perf2 次数约减半，最慢帧不变）。

2026-11-19 的测量（Chrome 关闭、锁屏，WindowServer 约 10% CPU，`caffeinate -u` 保持屏幕点亮）：

1. **不是 Chrome**：一个和 Chrome 无关的原生程序 `perf/vblprobe.m`（只开一个 CVDisplayLink，记录回调间隔）在没有 Chrome 时
   同样出现 33 / 50 / 67 ms 的间隔：每 2 分钟 2–3 次，5 分钟最多 28 次。Chrome 的 rAF 用的就是这个时钟，所以 Chrome
   的 67 ms 帧应当来自这里，和页面内容无关。
2. **不是内核的软件 VBL**：`perf/vbltick.c` 每 0.25 ms 读一次 PhxAMDFB 的 `debug.phxfb_vbl_n` / `_late_us_sum`。
   30 秒 1800 个 tick，最晚只晚 0.6 ms，用户态看到的间隔全部是 1 个周期。同期 vblprobe 有 3 次 4 个周期（66.7 ms）的间隔。
   翻页等待也正常（`debug.phxfb_dbuf_wait_max_us` 2.9 ms，0 次超时）。
3. **不是全系统停顿**：`hiccup`（两个只读时钟的忙循环线程）观察到两个线程同时停 20–50 ms 的情况，5 分钟约 25 次，
   来源未查。但它们和 CVDisplayLink 的间隔**没有一次重叠**（0/19），是另一件事，留作记录。
4. **WindowServer 发布的 VBL 时间**（`SLSDisplayGetPriorVBLTime`，CVDisplayLink 每帧用它重新对时；`perf/vblstale.m`）
   2 分钟里有 8 次停了两个周期没更新，但时间上也和 CVDisplayLink 的间隔对不上。
5. CVDisplayLink 自己的输出时间戳在间隔前后跳了好几秒（`hostTime` 前后不一致），`rateScalar` 0.991–1.001，
   名义周期每次启动读到的都不一样（16.40–16.88 ms）。说明 CoreVideo 对这块屏幕的时间模型不稳，它按错误的预测
   多睡了 2–3 个周期。CoreVideo 用的是 WindowServer 的 VBL 时间和周期（`SLSDisplayGetCurrentVBLDelta`），而它们来自
   PhxAMDFB 的软件 tick（每个 tick 有 0–0.6 ms 抖动）。最可疑的是这个抖动，或者 IOFramebuffer 报给系统的时序信息
   和 tick 不一致，尚未证实。

### 根因（逆向 CoreVideo 和 IOGraphicsFamily）

- CoreVideo 的 `CVCGDisplayLink::updateDisplayTiming` 每次回调前都重新读 `SLSDisplayGetCurrentVBLDelta`，直接当作刷新周期；
  视频时间 = 开机以来的帧数 × 周期，所以周期 0.5% 的抖动会变成好几秒的时间误差（上面第 5 条）。
- `CVDisplayLink::calculateNextWakeUpTime` 对 ≤ 4 个周期的等待直接相信，超过才重新同步，于是误差落在 1–4 个周期之间时
  就表现为 33 / 50 / 67 ms 的间隔（从不出现 5 个周期以上）。
- WindowServer 的 VBL delta 来自 IOFramebuffer 共享内存。`IOFramebuffer::handleVBL` 有一个“时序模式”：显示模式带有效的
  detailed timing（像素时钟 = 最小 = 最大）时，vblDelta 固定为 `htotal × vtotal / pixclk`，不随 tick 抖动。PhxAMDFB 的
  `getTimingInfoForDisplayMode` 把时序信息清零返回，所以从来没进过这个模式，vblDelta 每帧是两个 tick 触发时刻之差（16.51–16.82 ms）。
- 用户态验证（`perf/cvfix.m`，把 CoreVideo 私有的 VBL delta 函数指针换成常数 16666666 ns）：66 ms 间隔从 12 秒 1 次降到 0，
  `hostTime` 误差从秒级降到 < 9 ms。

### 失败的尝试：`patches/phxaccel-vbl-spin.patch`

让软件 tick 忙等到精确时刻再触发（`debug.phxfb_vbl_spin_us`）。同一次开机 A/B：spin=0 每 15 秒 890/892 次回调；spin=200
只有 687–777 次，间隔约 100 个。时间戳变准后 CoreVideo 反而锁到错误的整周期上。默认已改为 0，保留补丁只为记录；
vbl-timing 补丁在它之上。

### 修复：`patches/phxaccel-vbl-timing.patch`

PhxAMDFB 返回 `kIODetailedTimingValid`，总尺寸 2720×1481，像素时钟 = floor(像素数 × 1e9 / 周期 ns) = 241699209
（对应间隔 16666666 ns，正好是软件 tick 的周期）；`measureRefresh` 移到 `IOFramebuffer::start` 之前。开关
`debug.phxfb_timing`（1 开，0 旧行为，设置显示模式时读取）。需要重新编译 PhxAccel、sudo 安装、重启。

验收（2026-11-19 安装后）：

- `ioreg` 的 `IOFBCurrentPixelClock` = 241699209；WindowServer 读到的 VBL delta 恒为 16.667 ms。
- `perf/cvjitter`：900/900 次回调无间隔，`hostTime` 误差 ≤ 8.4 ms；`vblprobe` 0 个间隔。
- bench2 × 3：FPS 60.0、卡顿 0、最慢帧 18 ms（同一天修复前 56.7–57.7 FPS、卡顿 17–31、最慢 50–67 ms）。bench3 × 3 相同。
- WindowServer CPU 31–49%（原 24–41%），因为它现在真的合成满 60 帧。

排查工具：`defaults write com.apple.corevideo cv_note -int 5` 打开 CoreVideo 日志（`log stream` 子系统 `com.apple.corevideo`）。

## 每隔几秒一次 100–130 ms 的卡顿（已修复：`patches/nvmtl-defer-buftex.patch`，2026-11-19 已装入 WindowServer）

67 ms 修好以后，Chrome 自己报告 60 fps、0 卡顿，但肉眼仍能看到每隔 2–5 秒顿一下。`perf/stall.c`（每 0.25 ms 读
PhxAMDFB 的翻页计数 `debug.phxfb_dbuf_flips`）显示 WindowServer 每隔 2–5 秒有 6–8 帧（100–130 ms）不翻页，70 秒 14 次。

- 不是 GPU 卡住：`perf/gpuping.m` 同时每 4 ms 提交一个小任务，最慢 8 ms。
- 不是防残影的 `NVMTL_WS_WAIT_COMPLETION`（它在停顿里只占很小一部分，这次没有动）。
- `perf/wsblock.d`（`sudo dtrace -p WindowServer`：阻塞 > 30 ms 的调用栈 + 199 Hz 采样）：停顿时渲染线程在
  CoreAnimation 上传图层图片（`bind_image → copy_image_to_texture → newTiledTextureWithBytesNoCopy`）。CA 给的偏移是
  32 字节，不能直接映射，插件给每张图建一个私有替身图像（`_bufCopy`），并在 `setFragmentTexture:` 里**提交复制并等 GPU
  做完**。一帧里几十张新图（菜单、窗口内容刷新）就是几十次“提交 + 等待”。

修复：替身图像的填充和 surface-in 一样，记进下一次提交的开头（prologue），不再单独提交、单独等待。
开关 `NVMTL_DEFER_BUFTEX=0` 恢复旧行为。

测试（应用进程，`tests/runp.sh`）：`CARENDER_IMAGES=1 carender 300 40 4`（每帧 40 张新图）CARenderer 每帧
10.1 → 3.7 ms，最慢 14–17 → 7 ms；3000 帧 × 120 层 0 错误像素；半透明画面与旧插件逐字节一致；surftest、desctest、
descsurfdraw、descdrawstress 通过。

WindowServer 验收（装好重启后，`perf/stall.c` + bench2 页面）：

| | 修复前（70 s） | 修复后（70 s） |
|---|---|---|
| ≥ 6 帧（≥ 100 ms）不翻页 | 14 次，周期性 | 稳定段 0 次（只在 Chrome 启动的头 2 秒） |
| 2 帧间隔 | 430 | 24 |

剩余开销：每张新图仍要新建 GPU 内存（导入页面、建图像、分配显存地址），测试里约占渲染时间一半；下一步可做替身图像复用。
另有一种整机停顿（所有用户线程同时停 30–60 ms，内核 VBL 定时器照常），原因未查，AirportItlwm 扫描是嫌疑之一。

安装：`sudo bash work/install-buftex.sh` 后重启；回滚 `rollback-buftex.sh`；WindowServer 崩溃时 `recover-buftex.sh`。

## kernel_task 空闲时占 30–40% CPU（已缓解：`xhcinudge=1` → `4`）

屏幕静止、没有任何操作时，`kernel_task` 持续占 28–43%。

原因：`EFI/OC/Kexts/XhciNudge.kext`（取自 bbbbrt 的 EFI，只有二进制）。它映射 local APIC，按定时器向每个 xHCI
控制器的 MSI 向量发送中断，用来绕过 AMD Zen 4 xHCI 在 macOS 下丢中断的问题。反汇编确认：

- `PE_parse_boot_argn("xhcinudge")` 的值存入 `gIntervalMs`，用作 `clock_interval_to_deadline(gIntervalMs, 1000000)`，
  **单位是毫秒**；`=0` 关闭；不加该参数时默认 4 ms。
- 定时回调对每个开启了 MSI 的 xHCI 无条件注入（不检查有没有未处理的事件，没有设备的 XHC1 也一样）；
  `gTicksPerCheck` 只是重新扫描 PCI 设备的周期。
- 上游 README 把 `xhcinudge=1` 当开关写，实际含义是把间隔从 4 ms 缩短到 1 ms。本仓库原先照抄了 `=1`。
- 没有 `stop()`，定时器不会取消，**不要 kextunload**（会 panic）。

A/B 测量（2026-11-18，空闲，`kprof.sh`）：

| xhcinudge | kernel_task | 每个 XHC 中断/s | 键鼠 |
|---|---|---|---|
| 1（原配置） | 10–43%（波动大） | 882–906 | 正常 |
| **4（现配置）** | 6.7% | 230 | 正常 |
| 0（关闭） | — | — | **不可用**：按键连发、鼠标移动极慢 |

XHC 中断数与间隔严格成比例，确认全部来自 XhciNudge；IPI 数与它无关（随开机后的后台活动波动）。显示驱动的
`debug.phxfb_dbuf` / `nvaccel_iop_vsync` / `phxfb_vbl_staged` 置 0 均不影响 kernel_task。

处理：`EFI/OC/config.plist` boot-args 改为 `xhcinudge=4`。XhciNudge 不能去掉。

注意：config.plist 的 `NVRAM.Delete` 含 `boot-args` 且 `WriteFlash=true`，OpenCore 每次启动都用 config.plist 覆盖，
**`nvram boot-args=...` 重启后即失效**，必须改启动用 EFI 分区上的 config.plist。

- 待办：写一个开源替代 kext（替换 XhciNudge），**只在 xHCI 有未处理事件时才注入中断**，空闲时开销接近零。设计要点：
  - 沿用原版的注入方式：从 MSI capability 读目标 APIC ID（地址位 12–19）和向量（数据低 8 位），写 LAPIC ICR
    （x2APIC 用 MSR 0x830，按 `IA32_APIC_BASE` 第 10 位判断）。跳过 MSI 未启用、D3、`USBSTS.HCH`=1 的控制器。
  - 每个 tick 读 BAR0 运行时寄存器（`RTSOFF`，中断器 0 在 +0x20）判断是否「有事件没被处理」：
    `ERDP.EHB`=1（控制器发过中断但驱动没处理），或事件环出队位置的 TRB 已经被控制器写入
    （其 cycle 位与前一个 TRB 相同；位于段首时按 ERST 处理换段、回到段 0 时 cycle 翻转）。
    事件环地址是 DMA 地址，本机没有 IOMMU 驱动，等于物理地址；映射按段缓存，ERST 变了再重建。
  - 有了这个判断，轮询间隔可以缩到 1–2 ms（只读几个寄存器），键鼠延迟比现在的无条件 4 ms 更低。
  - 必须实现 `stop()`：取消并释放定时器、解除映射，才能安全 kextunload。保留 `xhcinudge=<ms>` 启动参数（`0` 关闭）。
  - 验收：空闲时 XHC 中断接近 0，键鼠与 `xhcinudge=4` 无差别（`=0` 时按键连发、鼠标极慢，可当回归对照）。
  - 编译环境：本机只有 Command Line Tools（无 Xcode），需用 SDK 自带的 Kernel.framework 头文件或 MacKernelSDK 直接用 clang 编译。

工具（不在仓库中）：`~/Documents/apple/work/chrometest/setnudge.sh <ms>`（需 sudo；自动找内置盘上的 OpenCore EFI，
重启后 diskN 编号会变）、`kprof.sh`（需 sudo，空闲约 40 秒，结果在 `kprof/<值>/`，汇总追加到 `kprof/results.txt`）。

## 共享屏幕 / 录屏时采集只有 17–19 帧，本机画面也掉到 40 帧（已缓解：`patches/nvmtl-ws-capture-nowait.patch` + 去掉 `NVMTL_WS_WAIT_INTERNAL`）

2026-11-20 腾讯会议共享屏幕：功能正常，但对方看到的画面很卡。腾讯会议日志（`xcast_*.log`）显示，它用
ScreenCaptureKit 按 2560×1440 BGRA、25 帧采集，实际采集和编码在 11–21 帧之间波动。

复现（`~/Documents/apple/work/scktest`，不在仓库中）：`anim` 开一个 60 Hz 变色窗口，`sck <宽> <高> <帧率> <格式> <秒>`
统计 ScreenCaptureKit 实际帧率和帧间隔：

| 请求 | 实际 | 帧间隔 |
|---|---|---|
| 60 帧，任意分辨率 / 格式 | 28–42 帧 | 集中在 32 ms（每隔一个 vsync） |
| 25 帧 | 17–19 帧 | 40–64 ms |

采集期间屏幕翻页从 60 次/秒降到 40–42 次/秒（`debug.phxfb_dbuf_flips`），所以**本机画面也会卡**。
帧率与分辨率、格式无关；客户端 CPU 读取一帧只要 0.7 ms，普通进程里全屏 IOSurface 拷贝的 GPU 往返约 1 ms。

根因（`wssample.sh`：sudo 对 WindowServer 各采样 8 秒，一次不采集、一次采集）：

- ScreenCaptureKit 让 WindowServer 在主线程上把所有窗口**再合成一遍**，写进「Selective Sharing」采集表面
  （`WSSelectiveSharingUpdateDisplayStreamSurface` → `CompositorMetal::CompositeLayersToDestination`），
  再用 `DisplayStreamStateMetal::CopyFrame` 拷到客户端的缓冲。
- 为绕过 B-58 残影，本机设置了 `debug.nvaccel_surface_gpu_on=0` 和 `NVMTL_WS_WAIT_COMPLETION=1`（见上文「动画残影」）。
  于是插件在 `commitAndWaitUntilSubmitted` 里对每个写 IOSurface 的命令缓冲调用 `waitUntilCompleted`，
  在主线程上同步等 GPU 完成。
- 主线程忙碌比例从 27% 升到 52%。显示合成的等待从 271 个采样增加到 969 个；采集的二次合成又等了 627 个采样
  （共 1196 个）。一帧里要同步等两次全屏合成，超出 16.7 ms，于是显示和采集都隔一个 vsync 才出一帧。

修复 `macos/nullarc/patches/nvmtl-ws-capture-nowait.patch`（基于已安装的 nn-hide 版本）：采集二次合成的目标表面只会被
随后的 `CopyFrame` 在 GPU 上读取。进程内所有 Metal 队列共用一个 Vulkan 队列，GPU 按提交顺序执行；`CopyFrame`
自己会等待，结果也由它交给客户端。所以只在 WindowServer 里，对只写「Selective Sharing」表面（SkyLight
`SurfacePool` 给这个池起的 IOSurface 名字）的命令缓冲，不再等 GPU 完成，也不再推迟 scheduled
（`NVMTL_SCHEDULE_AFTER_SURFACE_WRITES`）。显示合成的等待保持不变（B-58）。

- 验证 `tests/wsnowait.m`（编译成名叫 `WindowServer` 的程序，经 `runp.sh` 加载未安装的插件）：写采集表面的命令缓冲
  0.15 ms 返回（旧版 23.7 ms，即等完 40 次全屏拷贝），随后另一条队列上的拷贝读到的像素正确；其他表面照旧等待。
  `carender` 通过，耗时与旧版相同（5.15 / 5.19 ms 每帧）。
- 安装：`sudo bash ~/Documents/apple/work/install-capfix.sh`，然后重启（WindowServer 重启后才加载）。
  撤销：`rollback-capfix.sh`；不重装也可以关掉：在 `nvrm610.conf` 加 `NVMTL_WS_CAPTURE_WAIT=1` 后重启。
- 装后检查：`/tmp/nvmtl.log` 应出现 `capture-nowait: WindowServer writes surface … named 'Selective Sharing' -> capture composite`；
  `anim` + `sck 2560 1440 25 BGRA 8` 的帧率，以及采集期间的 `debug.phxfb_dbuf_flips` 每秒增量。

2026-11-21 装上补丁后复测：采样确认采集二次合成下已没有 GPU 等待（627 → 0 个采样），但屏幕合成本身在采集期间
每帧要等 GPU 约 5 ms，采集仍是 20 帧，屏幕约 30 帧。再在同一次开机里做 A/B（`tests/ab.sh`：60 Hz 动画 +
采集，统计采集帧率与 `phxfb_dbuf_flips`）：

| 设置 | 采集时屏幕（请求 25 帧） | 采集（请求 60 帧） | 采集时屏幕（请求 60 帧） |
|---|---|---|---|
| 原设置（WindowServer 自己等合成 + `NVMTL_WS_WAIT_INTERNAL=1`） | 27–29 帧 | 22–24 帧 | 24–25 帧 |
| `surface_gpu_on=1`，保留 `WAIT_INTERNAL` | 35–40 帧 | 40–45 帧 | 42–48 帧 |
| **去掉 `WAIT_INTERNAL`，`surface_gpu_on=0`（现用）** | **45–56 帧** | **39–49 帧** | **42–54 帧** |
| 去掉 `WAIT_INTERNAL`，`surface_gpu_on=1` | 30–45 帧 | 29–39 帧 | 31–40 帧 |

不采集时一直是 59–60 帧。主要开销是 `NVMTL_WS_WAIT_INTERNAL=1`（nullarc 安装时写入 `nvrm610.conf`）：WindowServer
连 CoreAnimation 每次离屏绘制都要等 GPU 完成。去掉它以后，内核等待（`surface_gpu_on=1`）没有额外收益，所以保留
B-58 的绕过。`surface_gpu_on=1` 期间使用未见残影。`macos/display/install.sh` 现在会从 conf 删掉这一行。

- 撤销：`echo NVMTL_WS_WAIT_INTERNAL=1 | sudo tee -a /Library/GPUBundles/nvmtl/nvrm610.conf`，重启。
- 请求 25 帧时采集仍只有约 21 帧：ScreenCaptureKit 只在 WindowServer 出帧时出帧、且两帧至少相隔 40 ms，屏幕约
  50 帧时间隔落在 40–60 ms。屏幕在采集期间稳定 60 帧才能拿到满 25 帧，剩下的是 WindowServer 主线程上 CoreAnimation
  组装命令的 CPU 开销（插件的绘制命令路径）。

实际腾讯会议投屏（2026-11-21，采集 2048×1152 `420f` 25 帧，排除自身窗口）：屏幕仍只有 25–34 帧（叠加 60 Hz 动画也是
33 帧），主线程忙约 64%。WindowServer 采样（`tests/wslive.sh`，8 秒，约 264 帧）中主线程每帧约 15.6 ms：

| 环节 | 采样 | 每帧 |
|---|---|---|
| 屏幕合成：CoreAnimation 组装命令（CPU） | 1134 | 4.3 ms |
| 屏幕合成：等 GPU 完成（B-58 绕过） | 1188 | 4.5 ms |
| 采集二次合成（已不等 GPU，日志确认 `capture composite, no completion wait`） | 507 | — |
| `CopyFrame` 缩放 + 转 420（等 GPU 339，`initialize_scale_program` 98） | 458 | — |
| `prepare_coreanimation` | 475 | 1.8 ms |

剩余开销在插件的绘制命令路径（CPU）和屏幕合成的同步等待上，需要继续做驱动性能优化：
`initialize_scale_program` 是否每帧重建；`CopyFrame` 的等待能否像采集合成一样省掉（它写的是交给客户端的表面，
需要跨进程的完成标记）；以及 B-58 的根因。

同一次投屏里的 A/B（叠加 60 Hz 动画，看 `phxfb_dbuf_flips`）：`surface_gpu_on=1` 时 31–32 帧（还记录到 1 次
`gpu_timeouts`），改回 0 时 33 帧；GPU 频率上限从 800 MHz 临时调到 1600 MHz（`phxctl smu 0x1b 1600`）后是 31–36 帧，
屏幕合成的 GPU 等待只从 1188 降到 1104 个采样。`phxfb_dbuf_waits` 为 0，VBL 每秒 60 次。所以等待的大头不是 GPU
算力。怀疑是排队：所有进程共用一个 GFX 队列、没有优先级（插件 `setGPUPriority` 不支持），WindowServer 的合成要排在
腾讯会议等程序提交的工作后面。下一步：让插件在 WindowServer 里统计每个命令缓冲从提交到 GPU 开始、从开始到结束
各用了多少时间（GPU 时间戳），区分排队和执行。

插件统计（`NVMTL_WS_GPUTIME=1`，`patches/nvmtl-ws-capture-nowait.patch` 所在构建；每 2 秒一行写进 `/tmp/nvmtl.log`）把
WindowServer 每次阻塞等待的屏幕合成拆开（60 Hz 动画，单位 ms，平均）：

| 情况 | 屏幕 | 排队 | GPU 执行 | 完成→发现 | 合计 |
|---|---|---|---|---|---|
| 不投屏 | 59 帧 | 0.10 | 0.13 | 0.07 | 0.35 |
| 投屏（活动监视器、Chrome 全屏开着） | 32 帧 | 0.26 | 4.4 | 1.0 | 5.8 |
| 同上，GPU 频率上限和下限都设为 2600 MHz | 32 帧 | 0.25 | 3.2 | 0.8 | 4.5 |
| 投屏，关掉活动监视器、最小化 Chrome，800 MHz | 33 帧 | 0.12 | 2.8 | 0.85 | 3.9 |

- 不是排队（其他进程的 GPU 工作）造成的；是投屏时屏幕合成本身变重：不投屏时只重画变化的小块，投屏时每帧重画整屏。
- 逐 pass GPU 计时（`touch /tmp/nvmtl-passtime-request` 让 WindowServer 记录 400 个 pass，输出在 WindowServer 的
  `$TMPDIR/nvmtl-passtime-<pid>.log`，需 sudo 取出）：一次合成 59 个 pass，其中一个 2560×1320 的离屏 pass（最大化的
  活动监视器 / Chrome 窗口被 CoreAnimation 压平成纹理，`FlattenNode::retain_surface`）要 1.0–2.6 ms；
  另有若干全屏 2560×1440 pass，每个约 0.2–0.25 ms（腾讯会议在 layer 1000 的全屏共享边框、程序坞层等）。
- 频率提高 3 倍，GPU 执行只少 25%，说明主要受内存带宽或驱动生成的着色器效率限制，不是算力。
- 关掉大窗口后单次等待降到 3.9 ms，但屏幕仍是 33 帧：主线程每帧还有 CPU 组装命令约 4 ms、采集二次合成约 3–4 ms、
  `CopyFrame` 约 1.5 ms、准备工作约 1.8 ms，加上等待仍超过 16.7 ms。
- 「完成→发现」在投屏时从 0.07 ms 涨到约 0.85 ms，插件的完成检测在负载下变慢，可以单独优化。

结论：要让投屏时屏幕稳定 60 帧，需要同时降低插件在 WindowServer 里的 CPU 开销和 CoreAnimation 离屏 pass 的 GPU 开销，
属于驱动整体性能工作，暂不继续；先做 VCN。临时缓解：投屏时少开最大化、带半透明侧栏的窗口。
关掉统计：从 `nvrm610.conf` 删掉 `NVMTL_WS_GPUTIME=1`（关掉后零开销，重启生效）。

## Radeon 780M 的 H.264 / HEVC 硬件编解码仍未接入 VideoToolbox

- 腾讯会议启动时记录 `no hw h264 encoder ablity` 和 `no hw hevc encoder ablity`，软件编码 2560×1440 HEVC 时 CPU 温度曾达 88–91 °C。向日葵远控日志报告 `support hevc 0`。
- 当前加载 PhxBringup build 59（VCN 诊断版），安装的 RADV 只报告图形、计算和传输相关队列。VideoToolbox 硬件验收中，H.264 和 HEVC 的编码器均返回 `-12908`，解码器均返回 `-12906`。

### PhxBringup build 59：VCN 固件仍未能启动（2026-11-22 暂停）

两条启动路径都已试透，`UVD_SCRATCH1` 始终为 0，固件一次都没有真正运行：

- **PSP 间接路径**（Linux 实际使用的路径）：`LOAD_IP_FW(type=49)` 被 PSP 拒绝，返回 `0xFFFF0006`。
  大小（216–4096）、显存地址（业务块内 8 页 + 固件缓冲区 3 处）、`fw_type`（13/49/50/51）、
  列表长度（1–27 对）、`POWER_STATUS` 的 PG_MODE 位均逐项排除；我们发出的命令缓冲区从显存导出后，
  与 Linux 侧 kprobe 抓到的字段完全一致（`bs=0 bv=0 id=6 lo/hi sz=216 ty=49`）。
  唯一尚未比对的，是那 27 对（偏移,值）的具体顺序与取值。
- **静态非 DPG 路径**（Linux 在这颗芯片上从不使用）：`--stage 1..7` 全部通过，第 8 步 VCPU 不报告就绪。

途中修掉的真实缺陷：`--fw-type` 原先根本没有传进 kext（应为 `>>16` 而非 `>>24`）；
`vcnctl toc` 送的是整个 2560 字节文件而非 2304 字节载荷——修正后 PSP **接受 LOAD_TOC（status 0）**。

完整断点、已排除项清单、复现脚本与后续待办见 [VCN NEXT](../macos/nullarc/vcn/NEXT.md)。
里程碑：固件运行（`SCRATCH1 = 0x09118010`）→ `vcnctl fence` 通过，之后才谈编解码指令流与 VideoToolbox 注册。

## 没有 HiDPI

PhxAMDFB 只报告固件 GOP 点亮的一个模式（2560×1440），没有缩放器，也没有 `kDisplayModeValidForHiResFlag`，
所以 one-key-hidpi、BetterDisplay 的覆盖文件不会产生 HiDPI 选项（nullarc#11）。

- dandingdjianjiao/nullarc 的 `thinkbook-hidpi-fb-20261110` 分支给该模式加上 HiRes 标志，只能得到严格 2 倍：
  2K 屏上为「看起来像 1280×720」。需要在 macOS 上用 MacKernelSDK 重新编译 PhxAccel。
- 「看起来像 1920×1080」等非整数倍需要显示管线支持缩放，目前没有实现。

## 只能用 HDMI 单屏

nullarc 0.4.0 不驱动外接显示接口的 PHY，本机靠固件 GOP 在 HDMI 上点亮的画面工作。分辨率与刷新率由固件决定，
启动后不能更改；热插拔未经验证。
