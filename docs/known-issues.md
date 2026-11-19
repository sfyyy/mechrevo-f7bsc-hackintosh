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

## 调用 Vision 文字识别的程序启动即崩溃（nullarc 驱动 bug，已绕过 Shottr，待从源码修复）

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
2. **每次 draw 新建描述符集**：未解决，见下一节。
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

## WindowServer 每次 draw 新建描述符集（待优化）

perf2 之后，这是 WindowServer 用户态里最大的一块插件开销（忙碌样本 1948 个中）：

- `-[NVMTLRenderCommandEncoder nvmtlBindSamplerPipeline]` 298（15%），其中 `nvmtl_snapshot_set_md` 203（10%），
  `radv_UpdateDescriptorSets` 130（7%）；`setFragmentTexture:` 139。
- 原因：CoreAnimation 几乎每个 draw 都换一张纹理（每个图层一个 surface），`nvmtl_snapshot_set_c` 判断上一个集合已脏，
  于是从池里取新集合，未变的绑定复制（dirty-only），变了的重写。每帧几十到上百个 draw，每个都要
  `vkUpdateDescriptorSets`。
- 方向：
  1. **跨命令缓冲缓存写好的集合**：按（管线绑定掩码，各槽句柄）哈希。CA 每帧画的基本是同一批窗口 surface，
     命中后直接绑定、不再写。集合写好后不再修改，复用是安全的；要处理的是生命周期：句柄被销毁时让对应条目失效，
     并且只能在 GPU 用完后回收。
  2. 只有纹理槽变化时（CA 的典型情况），只写那一个 binding，其余用 copy；先量一下 `nvmtl_dirty_only_on` 实际复制了多少。
  3. 评估 WindowServer 用 bindless 堆（日志里部分进程是 `bindless-all`）：纹理变成表里的下标，draw 之间不再换集合。
- 验收：`wsprof.sh` 里 `nvmtlBindSamplerPipeline` 占比明显下降；`surftest` 和 bench2 不变差；看 WindowServer 日志
  `commit high-water: N descriptor sets`。

## 测试页最慢一帧总是 67 ms（待定位）

每 3 秒窗口的最慢帧几乎固定是 66–68 ms，也就是连丢 3 帧；一轮 25 秒里有 17–36 次 >25 ms 的帧。原插件和 perf2 都有
（perf2 次数约减半，最慢帧不变），所以不是 perf2 引入的。今早一次测量（09:10）曾出现连续窗口 0 卡顿，同一插件之后重测
没有复现，环境影响很大（发热、后台负载、开机后多久，这台机器的 `pmset -g therm` 读不到温度）。

待查方向：

1. 先区分是 Chrome 侧还是 WindowServer 侧：同时记录 Chrome 的 rAF 时间戳和 WindowServer 每帧提交/完成时间
   （插件的 `NVMTL_PERF_LOG=1` 会在每次提交打 `PERFSUB` 行：hook / submit / wait / retire 耗时），对齐看慢帧时谁在等。
2. `NVMTL_WS_WAIT_COMPLETION=1`（`macos/display` 为绕过动画残影打开）：WindowServer 每帧等 GPU 完成，渲染线程约 12%
   的时间在 `waitUntilCompleted`。一帧 GPU 时间稍超 16.7 ms 就会连锁错过 vsync；量化每帧 GPU 时间分布。
3. 67 ms ≈ 4 个刷新周期：检查是否和软件 VBL / 双缓冲翻页（`debug.phxfb_dbuf`）的超时或相位有关；查 PhxAMDFB 日志里的翻页等待。
4. 换不滚动 / 不画 canvas 的页面，看是哪种负载触发（滚动的分块上传、canvas 的 surface-out 回读）。
- 验收：bench2 最慢帧 < 34 ms，卡顿次数明显下降。

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

## 没有 HiDPI

PhxAMDFB 只报告固件 GOP 点亮的一个模式（2560×1440），没有缩放器，也没有 `kDisplayModeValidForHiResFlag`，
所以 one-key-hidpi、BetterDisplay 的覆盖文件不会产生 HiDPI 选项（nullarc#11）。

- dandingdjianjiao/nullarc 的 `thinkbook-hidpi-fb-20261110` 分支给该模式加上 HiRes 标志，只能得到严格 2 倍：
  2K 屏上为「看起来像 1280×720」。需要在 macOS 上用 MacKernelSDK 重新编译 PhxAccel。
- 「看起来像 1920×1080」等非整数倍需要显示管线支持缩放，目前没有实现。

## 只能用 HDMI 单屏

nullarc 0.4.0 不驱动外接显示接口的 PHY，本机靠固件 GOP 在 HDMI 上点亮的画面工作。分辨率与刷新率由固件决定，
启动后不能更改；热插拔未经验证。
