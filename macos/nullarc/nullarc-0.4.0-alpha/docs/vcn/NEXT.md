# Radeon 780M VCN 4.0.2 · build 74 解码直写 surface + 6 会话（待上机验收）

## build 82 · 2026-11-24（动态分配 AVC 显存；构建完成，未安装）

**背景**：build 81 用固定 3 槽×16 MiB 换取不碰 128 MiB 以上显存，代价是最多 3 路同时硬解；而 context 池里真正用到的很少（720p 4 参考帧 context 仅 ~3.5 MiB，其余是内联 DPB）。

**做法**：会话句柄仍是 6 个，显存不再按槽号绑定，而是每路流按实际大小从两个池里首次适配分配（`avcPoolAlloc`，64 KiB 对齐，已被其他会话占用的区间——含关闭失败后仍保留的——不会再发）：
- context 池：80..128 MiB（48 MiB，与 build 79 同一范围，不扩）。
- DPB 池：1 GiB 起 192 MiB（GPU 专用，与 build 79 同一范围）。
- DPB 优先进 DPB 池（`separateDpb`，原先只有大分辨率才走），context 池只放固件会话 + codec context。
- 池满时 IDR 返回 Busy 并打一行 `AVC pool full slot=… need ctx=… dpb=…`，会话保持关闭，下一个 IDR 重试。

**容量（主机探针）**：1080p L5.0 refs8 → 3 路；1080p refs4 / 720p / 540p → 6 路（build 81 固定为 3 路）。

**改动**：PhxVcnAvcStream.h（池常量、`avcPoolAlloc`、DPB 优先分离）；PhxBringup.cpp（会话 contextOff/dpbOff、`allocAvcMemory`、范围检查、PHX_KEXT_BUILD 82）；tests/vcn_host_test.cpp（分配器用例、容量用例、旧布局断言）。
**验证（主机）**：vcn_selftest.sh 全绿；产物 work/PhxBringup-build82.kext，kmutil 依赖 OK。
**风险**：小流的 DPB 也改走 DPB 池（此前只有大流走），上机需回归 540p/720p/1080p gop60；DPB 池区间与 build 79 相同。
**上机后要验**：Metal 3 仍在、`phxboot.log` 有 `done`；13563 的 1080p 高帧率无绿屏；同时开 4~6 个页签 `PhxAvcActiveSessions` 随之增加；斗鱼 540p、gop60 1080p 回归通过。回退：`sudo bash work/rollback-vcn.sh --rearm`。

## build 81 · 2026-11-24（基于 build 79 重做；构建完成，未安装；直播 1080p 高帧率 AVC 绿屏）

**背景**：build 80 把 context bank 从 80..128 MiB 扩到 80..176 MiB，装入后首次启动 M2 失败（Metal 3 丢失）。
用 `rollback-vcn.sh --rearm` 回到 build 79 后恢复正常，所以 build 80 的内存布局改动是嫌疑，但具体是 128..176 MiB 被谁占用尚未查明。

**build 81 的取舍**：保留 build 80 对根因的两处修复（level 5.0/5.1/5.2 表、单槽 16 MiB 以容纳 1080p refs=8 的 13.4 MiB context），
但把槽数 6→3，context bank 仍是 48 MiB、仍在 80..128 MiB，**不触碰 128 MiB 以上显存**，布局上界与 build 79 一致。
代价：同时硬解的 AVC 会话从 6 个降到 3 个（超出时 findAvcSlot 返回 Busy，插件应回落软解）。

**改动**：PhxVcnAvcStream.h（SlotCount=3、静态断言上界 128 MiB）；PhxBringup.cpp（PHX_KEXT_BUILD 81）；tests/vcn_host_test.cpp 第 322 行上界 192→128 MiB。
**验证（主机）**：vcn_selftest.sh 全绿。产物：work/PhxBringup-build81.kext（sha bd72ccb2ebe2），kmutil 依赖 OK。

**安装**：`mv work/PhxBringup-build81.kext work/PhxBringup-vcn.kext`（先把现有同名目录挪走），`sudo bash work/install-vcn.sh`，重启。
失败回退：`sudo bash work/rollback-vcn.sh --rearm` 后重启。
**上机后要验**：Metal 3 仍在、`phxboot.log` 有 `done`；房间 13563 切 1080p 高帧率无绿屏；ioreg `PhxAvcLastTiming` 为 1920x1080；同时开 3 个直播页签，第 4 个应回落软解而非绿屏。

## build 80 · 2026-11-24（已于 14:26 装入 ESP；14:30 首次启动 M2 失败，Metal 3 丢失，待回滚到 build 79 判定；直播 1080p 高帧率 AVC 绿屏）

> **状态更正**：首次启动 `claim -> kr 0xe00002d8`（NotReady，MES 队列未 adopt）→ `M2 failed (NMXAccel did not start)` → 后续 phxboot 触发 kill switch。是否由 build 80 引起尚未证实：回滚到 build 79（`sudo bash work/rollback-vcn.sh --rearm`）后若恢复，则定位 128..176 MiB 新占用；若仍失败则与本次改动无关。

**现象**：B 站房间 13563 切到「1080p 高帧率」后整片绿；720p 正常。日志里每帧
`Phoenix AVC rejected phase=submit status=-12910 ... profile=100 level=50 max_refs=8 poc_type=0 mb_minus1=119x67`。

**根因（两处，缺一不可）**：
1. Annex A level 表只到 4.2，遇到 level_idc=50（5.0）查表失败 → maxFrameMbs=0 → 几何判定直接 false；
2. 即使放宽 level：1080p + max_num_ref_frames=8 需要 9 个 DPB 槽，codec context = 13.4 MiB，
   而 kAvcStreamSlotBytes 只有 8 MiB → persistentBytes > slot，仍然 false。

（顺带确认：level=42、refs=8 的 1080p 仍应被拒 —— Annex A 的 DPB 预算 32768 < 8160×8，属于正确拒绝。）

**改动**（src/phxbringup/PhxVcnAvcStream.h）：
1. level 表补 {50,22080,110400}、{51,36864,184320}、{52,36864,184320}。
2. kAvcStreamSlotBytes 8 → 16 MiB，context bank 随之 80..176 MiB（仍低于 200 MiB 的 BO 窗口；
   PhxBringup.cpp 里 avcContextSlotOffset(6) <= kBoOffset 的断言仍成立，头文件内的上界断言改为 192 MiB）。
3. 主机测试：vcn_host_test.cpp 按 16 MiB 槽更新（1080p 4 参考帧才走 DPB bank，1 参考帧留在槽内），
   新增 1080p L5.0 refs=8 用例（应通过）与同尺寸 L4.2 refs=8 用例（应拒绝）。

**验证（主机）**：tests/vcn_selftest.sh 全绿；几何探针 1080p L50 refs8 → dpb 26.9 MB ≤ 32 MB、ctx 13.4 MB ≤ 16 MB。

| 产物 | 位置 | 备份 |
| --- | --- | --- |
| PhxBringup build 80 | work/PhxBringup-build80.kext（sha d8afd9a7dbb3），已装入 ESP | 备份（build 79）：/Users/Shared/gpu/backup/vcn-20261124-142633 |

**安装**：把目录改名成 work/PhxBringup-vcn.kext 后 sudo bash work/install-vcn.sh，重启生效；回退 sudo bash work/rollback-vcn.sh。

**上机后要验**：房间 13563 选 1080p 高帧率 10 分钟无绿屏；ioreg 的 PhxAvcLastTiming 出现 1920x1080；
开 3-4 个直播页签确认 6 个会话够用（每会话 context 上限 16 MiB，bank 总数不变）。

## 插件 v78p · 2026-11-24 部署完成（直播 AVC 绿屏之二：全范围 420f surface）

v77p 之后房间 `13563`（PGL Major，1280x720）在 Chrome 里仍然整片绿，但同一条流用 ffmpeg 硬解正常 —— 说明只有浏览器的路径失败。

**判据**：给 `submitFrame` 的格式检查加一行日志后，Chrome 播放时每帧都是
`Phoenix decode surface rejected format=420f 1280x720 planes=2 visible=1280x720`。

**根因**：这条流的 SPS `video_full_range_flag=1`（全范围 / JPEG range，推流端按 pc 范围输出）。VT 因此按
`kCVPixelFormatType_420YpCbCr8BiPlanarFullRange`（`420f`）分配输出 surface，而插件只接受
`420v`（video range），于是每帧都被判 Unsupported（-12910）→ 空 surface → 绿。
ffmpeg 的 hwaccel 默认要 `420v`，所以命令行测试从来不触发这个问题 —— 这是「只有部分直播间绿」的另一半。

**改动**（`work/vcntest/va-load-probe/phoenix_decode.c`）：
1. `submitFrame` 同时接受 `420v` 与 `420f`（两者都是 8-bit 双平面 420，布局一致，只有 range 标记不同）。
2. 非直写回退路径的填充色按 range 取 `0/128`（全范围）或 `16/128`（视频范围）。
3. 格式被拒时打一行 `Phoenix decode surface rejected format=… WxH planes=… visible=…`。

| 产物 | 位置 | 备份 |
| --- | --- | --- |
| PhoenixVALoadProbe.bundle（v78p） | `/Library/GPUBundles/`，sha 前缀 `89bd8b345de9` | `/Users/Shared/gpu/backup/phoenix-fullrange-20261124-141602`（v77p，`0a2c8edf4696`） |

**验证**：
- Chrome 房间 13563：`surface rejected` 归零，画面正常（截图像素绿色占比 0），`PhxAvcActiveSessions`=2、`PhxAvcLastTiming` 1280x720 `readback=uma`
- 全范围合成流（x264 `range=full`）：硬解输出 0..255 满幅；软解参考被 ffmpeg 压到 16..235，所以**不能**拿 ffmpeg 软解做全范围流的像素基准
- 回归（不回退）：gop60 1080p 560/560、斗鱼 540p 91/91、房间 25999675（HRD）61/61

**回退**：`sudo ditto <备份>/PhoenixVALoadProbe.bundle /Library/GPUBundles/PhoenixVALoadProbe.bundle && sudo killall VTDecoderXPCService`。

## 插件 v77p · 2026-11-24 部署完成（直播 AVC 绿屏：VUI 里的 HRD）

B 站直播间走 AVC 硬解整片纯绿的问题定位到插件侧，已修。内核不受影响，无需重启。

**现象与判据**：绿屏像素是 NV12 全 0，即设备交回的是没写过的 surface。同时 `log show` 里每个 AU 一行
`Phoenix AVC rejected phase=parameters status=1 ...`、每帧一行 `AVD submit failed op=1 function=8 command=4 status=1`。

**根因**：直播间 SPS 的 VUI 带 `nal_hrd_parameters_present_flag=1`（CBR 推流，编码器 `BMT-SRT LiveHime/8.15.0 Win`）。
AppleGVA 把解析结果放进固定布局的 VUI 块，NAL/VCL HRD 表落在 `0x54..0x3A0`；`avc_parameters.c:encodeVUI`
要求这段必须全 0（`!zeroWords(a, 0x54, 0x3a4)`），于是 `PhoenixAVDExportAVCParameters` 直接返回 false，
每帧都拒绝，VT 照常回调空 surface → 绿。**只有部分直播间**绿，是因为带不带 HRD 由推流端的限码方式决定
（对照：同样能播的 `21622811` 该标志为 0）。

**改动**（`work/vcntest/va-load-probe/avc_parameters.c`）：
1. `encodeVUI` 不再要求 HRD 区为空。合成 SPS 本来就不发 HRD（`putBits(bits, 0, 3)`），宏块级解码不读 HRD，丢掉即可。
2. 导出失败时新增 `Phoenix AVC export failed ...` 一行 + 各区间非零字摘要（sps-header/scaling/sequence/vui-head/vui-rest/pps-*），
   下次换别的语法被拒时能直接看出是哪个字段，不用再靠猜。

| 产物 | 位置 | 备份 |
| --- | --- | --- |
| PhoenixVALoadProbe.bundle（v77p） | `/Library/GPUBundles/`，sha 前缀 `0a2c8edf4696` | `/Users/Shared/gpu/backup/phoenix-exportdiag-20261124-135157`（v76p，`962138cabdf1`） |

**验证**（`results/avc-hrd-green.ROj2QZ37/`，硬解 vs 软解逐帧哈希）：
- B 站 25999675（HRD 1080p）：旧插件 61 帧 / 1 个不同哈希 / 0 帧一致 → 新插件 61 / 61 / 61
- x264 1080p CBR+HRD：0/60 → 60/60；1080p60 CBR+HRD+B 帧 3+refs4：120/120
- 回归：gop60 1080p 560/560、斗鱼 540p 91/91、B 站 21622811（无 HRD）181/181

**回退**：`sudo ditto <备份>/PhoenixVALoadProbe.bundle /Library/GPUBundles/PhoenixVALoadProbe.bundle && sudo killall VTDecoderXPCService`。

**遗留**：插件对“不支持”的流仍然是每帧拒绝 + 交空 surface（=绿），而不是让 VT 回退软解；
VT 侧的 session 在 AVD 建上下文时只看得到宽高，看不到 SPS，所以目前只能在导出这一层尽量放行。
如果以后还出现整片绿，先看 `log show --predicate 'eventMessage CONTAINS "Phoenix AVC export failed"'`。

## build 79 · 2026-11-24（已上机，验收通过；修复 build 78 的 fence 等待自锁）

验收结果（详见 `results/decode-perf-acceptance.K5tR9w/report.md`）：原生 B **275.7 fps** / A **269.5 fps**（G1/G2 达标）、stock FFmpeg **110.7 / 110.7**（G4 达标、G3 差 8%）、自编 FFmpeg（R-8）**183.9 / 146.1**；P 帧 ioctl 3.63 → **2.313 ms**、IOSleep 归零、30 fps CPU −2.8%；五条流 1886 帧像素全一致；IDR 复用与 flush 三档均验证。遗留：AC-6 浏览器场景、AC-7.1 两小时长稳、G3 与 AC-2.2 判据修订待确认。

build 78 上机验收时发现一个真 bug：一次 9 ms 的帧尖峰把 `hw_ewma` 推到上限后，预睡时长本身成了下一帧的样本，平均值被自己证实、永久停在上限。实测 `hw_ewma_us=7970`、`fence_us=8241`、`spin_us=0`、`wait=2`，原生帧率从 223 掉到 86 fps。

修复：
1. 预睡命中（fence 在预睡结束时已就绪）时**减半**平均值，不再把这段 sleep 当样本；其它命中按 1/8 平均。
2. 上限 8 ms → 4 ms，下限 200 µs。
3. 新增 `debug.phx_vcn_hw_ewma_ns`（RW）：读当前平均值，写入即重置，现场调参不用重启。
4. IDR 重建时打一行 `IOLog`（`reason=new-client|unclean|disabled|geometry`），AC-3 的判定可以直接在 `log show` 里核对；稳态复用不打日志。

| 产物 | 位置 | 备份 |
| --- | --- | --- |
| PhxBringup build 79 | `work/PhxBringup-vcn.kext`（sha `184b5386cc1e`） | ESP 上现为 build 78（`d03b2121`），install 脚本会先备份 |

验证：主机测试新增 3 组「预睡命中减半 / 下限 200 µs」用例，全绿。上机待重启。

## build 78 · 2026-11-24（已上机，EWMA 自锁问题见 build 79）

| 产物 | 位置 | 备份 |
| --- | --- | --- |
| PhxBringup build 78 | `work/PhxBringup-vcn.kext`（sha `d03b2121d6f8`） | build 75：`/Users/Shared/gpu/backup/vcn-20261124-001932/PhxBringup.kext`；仅 S2 的 build 76：`work/PhxBringup-build76.kext`（sha `0b7ade0a`） |

为什么合成一个 build：三项改动各有 sysctl，同一次开机内就能单独开关做 A/B，省两次重启。

开关（均默认开启新行为）：
- `debug.phx_vcn_wait` 0/1/2（默认 2）、`debug.phx_vcn_spin_max_us`（200）、`debug.phx_vcn_slice_us`（50）
- `debug.phx_avc_idr_reuse`（1）
- `debug.phx_avc_flush_mode`（1）

改动：
1. fence 等待：短自旋 → 亚毫秒睡眠（2 倍退避、1 ms 封顶）→ IOSleep(1) 兜底；EWMA 只由 P 帧更新（一次性置位）。
2. IDR 会话复用：几何/profile/参考帧数未变且上一帧无错时不再 close+create，只清 feedback。
3. 回读刷新范围：只刷可见行（模式 1）/逐行交错（模式 2），并计时 flush_us/copy_us。
4. 可观测：`PhxAvcLastTiming` 增 `wait/sleeps/spin_us/hw_ewma_us/idr_reuse/flush_us/copy_us`；`PhxAvcCounters` 单行计数。

A/B（同一次开机，ABBA 顺序）：

    for w in 0 1 2; do sudo sysctl debug.phx_vcn_wait=$w; <bench>; done
    sudo sysctl debug.phx_avc_idr_reuse=0 / =1        # 看 idr_reuse= 与 idr_reused
    sudo sysctl debug.phx_avc_flush_mode=0 / 1 / 2    # 看 flush_us / copy_us

**回滚**：`sudo bash work/rollback-vcn.sh`；或换回 `work/PhxBringup-build76.kext` 重装。

## build 76 · 2026-11-24（仅 S2，已并入 build 78）

fence 等待（S2 / R-1 / R-6 / R-7）。插件为 v76p，已部署。

| 产物 | 位置 | 备份 |
| --- | --- | --- |
| PhxBringup build 76 | `work/PhxBringup-vcn.kext`（sha `0b7ade0a4eab`），尚未安装 | `/Users/Shared/gpu/backup/vcn-20261124-001932/PhxBringup.kext`（build 75） |

改动：
1. 新增 `PhxVcnWait.h`：解码路径的 fence 等待改为「短自旋 → 亚毫秒睡眠 → IOSleep(1) 兜底」，自旋预算按 P 帧硬件耗时 EWMA 自适应（上限 200 µs），短睡段 2 倍退避、1 ms 封顶（IDR 不会打几百次唤醒）。
2. 3 个开关：`debug.phx_vcn_wait`（0/1/2，默认 2）、`debug.phx_vcn_spin_max_us`（200）、`debug.phx_vcn_slice_us`（50）；同名 boot-arg 作启动兜底。
3. `PhxAvcLastTiming` 追加 `wait= sleeps= spin_us= hw_ewma_us= idr_reuse=`；新增 `PhxAvcCounters` 单行计数。
4. `vcntest/tools/fence-wait.d`、`perf-capture.py` 同步。

验证：主机测试 `phxbringup/tests/vcn_selftest.sh` 全绿（含新增等待状态机用例）。**上机 A/B 待重启**：
`sudo sysctl debug.phx_vcn_wait=0` → 1 → 2，跑 `fence-sleep.d`/`fence-wait.d` + `vt-metal` + `bench.py` + `vt-bench --realtime 30`。

**回滚**：`sudo bash work/rollback-vcn.sh`。

## 插件 v76p · 2026-11-24 部署完成（无需重启）

1080p 直写修复（S1 / R-3）。内核仍是 build 75。

| 产物 | 位置 | 备份 |
| --- | --- | --- |
| PhoenixVALoadProbe.bundle | `/Library/GPUBundles/`，sha 前缀 `962138cabdf1` | `/Users/Shared/gpu/backup/phoenix-decode-20261123-234746`（sha `5d89296d`，即 build 75 插件） |

改动：
1. 新增 `phoenix_layout.h`：直写判定改为按实际平面地址取 chromaOffset（1080p surface 的色度平面在第 1088 行，旧代码按 1080 行判断，永远退回中转缓冲）。
2. 新增 `phx_preferences.h`：插件开关统一读 `/Library/Preferences/com.phoenix.va.plist`（环境变量到不了系统 XPC 服务）。
3. 首帧打印 `Phoenix AVC layout direct=… pitch=… chroma=…`。

验证：1080p/720p/540p 全部 `direct=1`；流 A/B、斗鱼 540/1080 与软解逐帧 MD5 一致（0 帧不同）；原生吞吐 205.3→215.8 fps（流 B）、158.2→165.6 fps（流 A），服务端 CPU 3.00→2.73 ms/帧。

**回退**：`sudo rm -rf /Library/GPUBundles/PhoenixVALoadProbe.bundle && sudo ditto <备份> … && sudo killall VTDecoderXPCService`；或运行时 `sudo defaults write /Library/Preferences/com.phoenix.va DirectAnyOffset -bool NO`。

## build 75 · 2026-11-23（待重启）

build 74 上机发现两件事：
1. B 站直播 AVC（720p High、max_num_ref_frames=8、B 帧、加权预测）被内核以 kIOReturnUnsupported 拒绝 → 绿屏。原因 `kAvcMaxReferences=4`。已提到 16（消息布局本就有 16 项），由 Annex A 的 MaxDpbMbs 约束实际可用数（1080p@4.x 仍是 4，720p@4.0 可到 8）。
2. build 74 把 DPB bank 放在 512 MiB，与 PhxAMDFB 双缓冲第二块扫描面（phxfb.cpp gDbufOffMb=512）重叠。build 75 移到 1 GiB，32 MiB/槽。
3. build 74 插件回归（参考 selector 收到 2 个标量 → status=1）已修，自检 622080 字节 0 不匹配。

主机测试 `src/phxbringup/tests/vcn_host_test.cpp` 已更新为新布局并全部通过。ESP 备份：`/Users/Shared/gpu/backup/vcn-20261123-150208`（即 build 74）。

## build 74 · 2026-11-23 部署完成，等待重启

| 产物 | 位置 | 备份 |
| --- | --- | --- |
| PhxBringup build 74 | OpenCore ESP `EFI/OC/Kexts/PhxBringup.kext`（sha 前缀 b6c32f9ee1ed0576） | `/Users/Shared/gpu/backup/vcn-20261123-134935` |
| PhxAccel（2 标量布局 ABI） | `/Library/Extensions/PhxAccel.kext` + Aux KC | `/Users/Shared/gpu/backup/phxaccel-20261123-134953`、`aux.kc.20261123-135013` |
| PhoenixVALoadProbe.bundle | `/Library/GPUBundles/`（sha 前缀 0ec2a883b5c59b2e） | `/Users/Shared/gpu/backup/phoenix-decode-20261123-135035` |

改动：
1. 内核回读不再 IOMalloc 中转，`map()` 输出描述符后按目标行距直写；去掉每帧 3 MB 清零和整帧 clflush 之后的重分配。
2. 视频上下文方法新增 2 个标量：surface 行距 `pitch` 与 UV 偏移 `chromaOffset`。插件在 surface 两平面连续时直接把 plane0 交给内核，用户态零拷贝；否则退回紧凑缓冲 + 逐行拷贝。
3. 槽位 2 → 6：codec context 放 80–128 MiB（aperture 内，IDR 时仍可从 CPU 清零），DPB bank 移到 512 MiB 以上（GPU-only，不再从 CPU 清零）。
4. `avd_accel.c` 的 pool id 0/22/5 拆成 3 个独立池。

**回滚**：`sudo bash install/vcn/rollback-vcn.sh`；Aux KC `sudo cp -p /Users/Shared/gpu/backup/aux.kc.20261123-135013 /Library/KernelCollections/AuxiliaryKernelExtensions.kc`；插件用上面的备份目录 ditto 回去。

验收：`ioreg -r -c PhxBringup -d 1 | grep -E 'phx-build|PhxAvc'` 应为 build 74、`PhxAvcLastTiming` 含 `pitch=`、`PhxAvcReadbackMode=uma`；开 2–4 个视频页签看 `PhxAvcActiveSessions` 与绿屏是否消失。

# Radeon 780M VCN 4.0.2 · build 62 真机结果（2026-11-22）

重启后 IORegistry 确认加载 PhxBringup build 62。固件初始化成功，四次 END、六次 fence（包含队列回绕）、解码会话和编码会话均通过。测试退出时已将 VCN 下电。

| 诊断 | 真机结果 |
| --- | --- |
| H.264 单帧硬解 | 864×480 NV12，行跨度 1024。生成 737280 字节，字节和为预期的 `0x020345d8`，fence 14 完成。 |
| H.264 单帧硬编 | fence 16 完成，硬件生成 136 字节 Annex B 码流。独立软件解码得到预期的 160×128 亮度四象限和中性色度。 |
| VideoToolbox | H.264、HEVC 硬件编码会话创建返回 -12908，硬件解码会话创建返回 -12906。软件对照两种格式各通过 8 帧往返。底层 HEVC 帧命令仍待实现。 |

build 62 的 `encode-frame` 因固件反馈 DWORD 8 为 5 而拒绝导出，导致脚本退出 1。按 DWORD 6 中的长度 136 从显存的码流缓冲区起点取回数据，SPS、PPS 和 IDR 分别从偏移 0、17、25 开始。运行日志和原始输入保存在工作目录的 `vcntest/results/build62-NdR6Ba96/`，其中 `feedback.hex`、`stream.hex`、`recovered.h264` 和 `recovered-check.log` 可用于复核。

## 下次真机验收

build 64 保留 build 63 的固定帧导出修复，并接收应用提供的 NV12 图像。驱动已从干净源码打补丁编译。x86_64 macOS 26.7.1 的 VideoToolbox 已在模拟 IOKit 后端上通过应用注册、按 ID 创建会话、8 帧回调和错误传递。真实驱动仍为 build 62，应用注册返回 -12908。部署 build 64 并重启后按 [应用接入 H.264 编码](README.md#应用接入-h264-编码) 编译并运行验收。

    ioreg -r -c PhxBringup -d 1 | grep phx-build
    sudo bash vcn-test.sh --keep
    ./vcntest/phx-vt-roundtrip --phx-avc
    sudo /Users/Shared/gpu/vcnctl down

每次底层诊断保存在 `/Users/Shared/gpu/vcn-run.XXXXXXXX/`。验收时记录应用的八帧硬编及独立解码结果。后续工作包括其他分辨率、应用硬解码、HEVC 和系统级注册。

build 62 所在 OpenCore ESP 分区 UUID 为 72CA6637-9804-4FDE-A316-6905B904ECFA，kext 可执行文件 SHA-256 为 91ee5a18a75005f0c743a810b316da33e615acdf2eb9b72dd3a5acf5e50402ed。安装前备份位于 `/Users/Shared/gpu/backup/vcn-20261122-033140/PhxBringup.kext`，回滚入口为工作目录的 `rollback-vcn.sh`。

## 旧启动路径排查记录

以下内容保留自较早 build 59 的 PSP 启动诊断，作为历史记录。build 61 补上了其间发现的启动列表字段和队列路径缺陷。早期的“固件始终没跑起来”等描述不能代替 build 61 真机结果。

# VCN 4.0.2 bring-up — 断点记录（2026-11-22 收尾，暂停）

## 状态一句话
固件始终没跑起来（`SCRATCH1` 恒为 0）。两条启动路径都试透：
PSP indirect（Linux 实际用的）被 PSP 拒；静态非 DPG（Linux 从不用的）在 VCPU 启动这步死。

## 环境
- kext 源码：`work/nullarc-0.4.0-alpha/src/phxbringup/`（build 59）
- 部署：`work/vcntest/PhxBringup.kext` + `vcnctl`；`install.sh` 装、`rollback.sh` 回滚
- OpenCore 的 ESP **盘号每次启动会变**（见过 disk0s1 / disk1s1），`findoc.sh` 按卷名自动找，别写死
- 改装后**必须重启**才生效（kext 在 OpenCore 里）
- 备份：`/Users/Shared/gpu/backup/PhxBringup.kext.*`（最早的原始版 `...20261121-144851`）
- 固件：`/Users/Shared/gpu/fw/{vcn_4_0_0.bin,psp_13_0_4_toc.bin}`（TOC 的 md5 与 Linux 一致）

## 已验证的结论（不要再推翻）
- PGFSM：**0 = 平面开，2 = 关**；PowerUpVcn 后全关 `0x2aaaaaaa` 是正常的
- DPG 门控下 `UVD_STATUS` 恒 `0xdeadbeef`；就绪判据是 **`UVD_SCRATCH1`（Linux = `0x09118010`）**
- 读回全 1 / `0xdeadbeef` 的寄存器（`VCPU_CNTL`、cache win 0/1 BAR、`LMI_CTRL`）是**不可读**，不代表写失败
- Linux 实机 dump 证明它**能用** PSP 间接加载（kprobe `amdgpu_vcn_psp_update_sram` 命中，之后 UVD_STATUS=0x26、SCRATCH1=0x09118010、RB_WPTR 在动）

## 关键结论：两条路径的现状

### A. PSP indirect（`--dpg-sram`）—— Linux 走的路，被 PSP 拒
`LOAD_IP_FW(type=49)` → **`0xFFFF0006` = TEE_ERROR_BAD_PARAMETERS**。
已逐项排除（全部无效）：
- 大小：216 / 256 / 512 / 768 / 1024 / 2048 / 4096（`--sram-pad`）→ 只有 <768 与 ≥768 两种错误码差异
- 地址：BO 内 8 个页（`--sram-page`）+ `fw_buf` 内 3 个位置（`--sram-abs`）
- fw_type：13 / 49 / 50 / 51（`--fw-type`）→ 13/51 给 `0xFFFF0010`(BUSY)，49/50 给 BAD_PARAMETERS
- 列表长度：1..27 对（`--sram-trim`）→ 全部 BAD_PARAMETERS
- `POWER_STATUS` 补 PG_MODE（0x105，与 Linux 运行时一致）→ 无效
- `--psp-ignore` 跳过错误继续跑 → SCRATCH1 仍 0，说明状态码是真错误
- 我们发出的命令缓冲（VRAM `0x1104000`）经实测：`id=6 lo=0x0c8b1000 hi=0x80 sz=216 ty=49 bs=0 bv=0`
  与 Linux 探针抓到的 `bs=0 bv=0 id=6 rlo..rsz=0 lo=0x7fca1000 hi=0x80 sz=216 ty=49` **逐字段一致**

**唯一没比对的**：那 27 对 (offset,value) 的**具体顺序与取值**。

### B. 静态非 DPG（默认 `vcnctl init`）—— Linux 从不使用
`--stage 1..7` 全部通过；**stage 8（VCPU 启动）失败**：`UVD_STATUS` 停在 `0x4`，SCRATCH1=0。
- `PGFSM` 我们 `0x2aaa8aa0` vs Linux `0x2a2a8aa0`，差值 bits[23:22]=**UVDJ（JPEG）平面**，
  Linux 的 `disable_static_power_gating` 同样不碰它 → 非根因
- `VCPU_CNTL` 常量与 Linux 一致（CLK_EN=bit9、BLK_RST=bit28、PRB_TIMEOUT=16:20）
- 曾一度以为 `CACHE_SIZE0`/`CACHE_BAR` 读全 1 是写失败 —— 那是不可读寄存器，误判

### C. DPG 直写（`--dpg`）—— 试过，SCRATCH1 仍 0
DPG 端口在门控下读回全 `0xffffffff`（正常）。这条路要真正可用可能得先开平面，未再深入。

## 顺手修掉的真实 bug（已装）
1. `--fw-type` 根本进不到 kext：kext 解码 `stopStage>>24`，vcnctl 放在 op bit24 → 正确是 `>>16`
2. `vcnctl toc` 送 TOC 时送整个 2560 字节文件 → 应送 payload（`ucode_array_offset_bytes`=0x100 起、
   `ucode_size_bytes`=2304）。修正后 **PSP 接受 TOC（status 0x0）**
3. `vcnctl toc` 的 `ftell` 后没 `fseek(0)` → 必然 short read

## 后续待办（按优先级）

### 1. 拿到 Linux 的 27 对列表（唯一还缺的证据）
- 脚本：`~/Downloads/vcn-btf.sh`（也拷到 ESP 根目录）
- **顺序关键**：冷启动 Linux → **立刻** `sudo bash /boot/efi/vcn-btf.sh 60` → 提示出现后再播硬件解码视频
- 原因：`amdgpu_vcn_psp_update_sram` 每次开机只调用一次，晚了抓不到
- 自校验：应打印 `GPU_OK` 且 gpu 地址形如 `0x807f…`；若 `GPU_BAD` 则 BTF 偏移不对，
  用 `bpftool btf dump file /sys/kernel/btf/amdgpu format c` 重新算
  （涉及 `adev->vcn`=202264、`amdgpu_vcn_inst`=3776、`dpg_sram_cpu_addr`=3432）
- 拿到后与我们 VRAM `0xC8B1000` 处 dump 的 27 对逐对比

### 2. 若列表一致仍失败 → 放弃 PSP 路径，改攻 B（静态路径的 VCPU）
需要更多 instrumentation：VCPU 取指地址、`GPCOM_DATA` 输出、`UVD_STATUS` 全字段含义

### 3. 里程碑（两条路都一样）
固件起来（SCRATCH1=0x09118010）→ `vcnctl fence` 通过（固件把 fence 值写回 ring WB 槽）
→ 才算 "VCN 可用"。之后的解码/编码指令流、Vulkan Video、VT 注册都在这之后（数百小时量级）

### 4. 并行考虑：不改 VCN，优化现有 CPU 编码 / WS 采集链路
腾讯会议投屏的原始瓶颈主要是 WS 主线程等待（capfix 已修，27→45-56fps），编码器是 CPU v265。
多线程/多实例可能就够用，成本远低于 VCN

## 常用命令
```
cd macos/nullarc/nullarc-0.4.0-alpha/tools/vcn-bench
sudo ./install.sh                      # 装进 OpenCore，重启生效
sudo ./vcnctl status                   # VCN 状态
sudo ./vcnctl init <fw>                # 静态非 DPG 路径
sudo ./vcnctl init --dpg-sram <fw>     # PSP indirect 路径
sudo ./vcnctl toc <toc.bin>            # 发 LOAD_TOC（必须先有 PSP ring）
sudo ./vcnctl down                     # 跑过 --psp-ignore 后必须先 down
sudo ./vcnctl crumbs | tail -20        # 130=SCRATCH1 131-134=DPG 读回 135=PSP 状态 136=dword 数 137=fw_type
sudo ./vcnctl init --stage N <fw>      # N=1..8 分段 bisect
sudo /tmp/phxctl vram 0xC8B1000 64     # 直接看 SRAM 列表（phxctl 需现编）
```
