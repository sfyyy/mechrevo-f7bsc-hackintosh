# Radeon 780M VCN 4.0.2 真机结果

## build 64 · 2026-11-22

重启后 IORegistry 报告 `phx-build = 64`。已加载的 PhxBringup 可执行文件与仓库 EFI 二进制的 Mach-O UUID 均为 `3F78DC1A-721E-35EB-AFBE-62C9DC7D7D1F`。测试开始时 VCN 已下电，随后使用 `vcn-test.sh --keep` 初始化并保持上电，应用测试以普通用户运行。

| 真机用例 | 结果 |
| --- | --- |
| 应用注册 Phoenix H.264 编码器 | `--phx-avc` 通过 8/8 帧。会话使用 `org.nullarc.phoenix.avc.encoder` 并报告硬件编码。独立软件解码逐帧校验了时间戳和八组不同的 160×128 NV12 亮度、Cb、Cr 图案。 |
| 底层队列与会话 | 固件启动、四次 END、六次 fence（含回绕）和两种会话均通过。 |
| 固定 H.264 帧 | 解码 fence 14 完成，NV12 输出 737280 字节，字节和为 `0x020345d8`。编码 fence 17 完成，正常导出 136 字节，软件解码后的四象限校验通过。 |
| 应用未经注册直接要求硬件 | H.264、HEVC 的系统编码会话返回 `-12908`，解码会话返回 `-12906`。 |

测试结束后 VCN 正常下电。复现命令见 [应用接入和验收说明](README.md)。其他分辨率、应用硬解码、HEVC 和系统级发现仍待实现。

## build 62 · 2026-11-22

重启后 IORegistry 确认加载 PhxBringup build 62。固件初始化成功，四次 END、六次 fence（包含队列回绕）、解码会话和编码会话均通过。测试退出时已将 VCN 下电。

| 诊断 | 真机结果 |
| --- | --- |
| H.264 单帧硬解 | 864×480 NV12，行跨度 1024。生成 737280 字节，字节和为预期的 `0x020345d8`，fence 14 完成。 |
| H.264 单帧硬编 | fence 16 完成，硬件生成 136 字节 Annex B 码流。独立软件解码得到预期的 160×128 亮度四象限和中性色度。 |
| VideoToolbox | H.264、HEVC 硬件编码会话创建返回 -12908，硬件解码会话创建返回 -12906。软件对照两种格式各通过 8 帧往返。底层 HEVC 帧命令仍待实现。 |

build 62 的 `encode-frame` 因固件反馈 DWORD 8 为 5 而拒绝导出，导致脚本退出 1。按 DWORD 6 中的长度 136 从显存的码流缓冲区起点取回数据，SPS、PPS 和 IDR 分别从偏移 0、17、25 开始。运行日志和原始输入保存在工作目录的 `vcntest/results/build62-NdR6Ba96/`，其中 `feedback.hex`、`stream.hex`、`recovered.h264` 和 `recovered-check.log` 可用于复核。

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
cd /Users/sfyyy/Documents/apple/work/vcntest
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
