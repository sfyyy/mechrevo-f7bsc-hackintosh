# Roadmap / 路线图

[English](#english) · [简体中文](#简体中文)

## English

The roadmap uses evidence-based milestones. A milestone is complete only when its acceptance criteria are reproducible on documented hardware.

### M1 — Repository and documentation baseline

**Status: in progress, nearly complete**

- [x] Add an explicit root license and third-party license boundary.
- [x] Publish an English README and a Simplified Chinese translation.
- [x] State supported hardware, features, non-goals and maturity level.
- [x] Add contribution, security and code-of-conduct documents.
- [x] Add issue and pull-request templates.
- [x] Add a bilingual installation guide and clean-install acceptance gate.
- [x] Add GitHub description and search topics.
- [x] Add documentation link/private-path checks and CI.
- [ ] Record screenshots and a short status report from a known-good build.

### M2 — Reproducible clean installation

**Status: next; required before calling the documentation mature**

- [ ] Document the exact supported BIOS version and every required BIOS setting.
- [ ] Document firmware mode, Secure Boot, IOMMU, UMA frame-buffer and boot-order settings.
- [ ] Document macOS installer download, USB creation and OpenCore installer boot.
- [ ] Document disk layout, APFS/ESP creation and preservation of an existing Linux boot path when dual-booting.
- [ ] Validate EFI deployment on at least one non-Arch Linux distribution or document a distribution-independent manual path.
- [ ] Pin every external artifact by version, SHA-256 and license/source URL.
- [ ] Provide a preflight script that checks files, hashes, ESP mount, SMBIOS uniqueness and recovery media.
- [ ] Complete two clean same-model installations without maintainer intervention.
- [ ] Test recovery after a failed EFI and failed GPU path.
- [ ] Publish the validation results with dates, BIOS versions and macOS builds.

### M3 — Stable same-model release

- [ ] Validate Wi-Fi, Bluetooth, Ethernet, audio, USB, sleep/wake and reboot cycles independently.
- [ ] Eliminate known WindowServer crashes and persistent display artifacts under routine workloads.
- [ ] Document long-run stability, thermal behavior and rollback acceptance.
- [ ] Provide a beginner-capable checklist with expected output for every step.
- [ ] Tag the first release only after M2 and M3 acceptance records exist.

### M4 — Media and performance

- [ ] Reuse VCN decode sessions across keyframes when stream parameters are unchanged.
- [ ] Replace the fixed `IOSleep(1)` fence wait with a bounded adaptive wait.
- [ ] Validate at least three concurrent 1080p30 streams without timeouts or frame errors.
- [ ] Investigate HEVC hardware support and document whether it is feasible.
- [ ] Publish repeatable CPU, latency, throughput and power comparisons.

### M5 — Wider hardware support

**Status: not active.** Generalizing to other machines is allowed only after the same-model path is reproducible and stable.

### Labels and contribution entry points

| Label | Use |
|---|---|
| `area/docs` | Documentation, diagrams, translations and link hygiene |
| `area/installation` | Installer, EFI deployment, BIOS prerequisites and recovery |
| `area/display` | WindowServer, framebuffer, tearing and ghosting |
| `area/video` | VideoToolbox, VCN, encode/decode and benchmarks |
| `area/performance` | CPU, GPU, latency, thermal and power regressions |
| `status/needs-testing` | Implemented but awaiting real-hardware confirmation |
| `status/blocked-hardware` | Requires reboot, hardware access or unavailable equipment |
| `priority/high` | Blocks M2/M3 reproducibility or causes data-loss/boot failures |

Good first contributions are documentation corrections, link checks, reproducible validation reports, and tests that run without hardware access.

## 简体中文

路线图以可验证的里程碑划分。只有验收标准能够在明确记录的硬件上重复通过，里程碑才算完成。

### M1 — 仓库与文档基线

**状态：正在进行，接近完成**

- [x] 添加仓库根许可证并明确第三方许可证边界。
- [x] 提供英文 README 与简体中文翻译。
- [x] 明确支持硬件、功能、非目标和成熟度等级。
- [x] 添加贡献、安全和行为准则文档。
- [x] 添加 Issue 与 Pull Request 模板。
- [x] 添加双语安装指南和全新安装验收门槛。
- [x] 设置 GitHub 仓库描述和搜索标签。
- [x] 添加文档链接、隐私路径检查与 CI。
- [ ] 记录已知良好构建的截图和简短状态报告。

### M2 — 可复现的全新安装

**状态：下一步；完成后文档才能称为成熟**

- [ ] 记录受支持的 BIOS 版本和所有必要设置。
- [ ] 记录固件模式、Secure Boot、IOMMU、UMA 帧缓冲和启动顺序。
- [ ] 记录 macOS 安装器下载、U 盘制作和 OpenCore 安装启动流程。
- [ ] 记录磁盘布局、APFS/ESP 创建，以及双系统时保留现有 Linux 启动路径的方法。
- [ ] 在至少一个非 Arch Linux 发行版上验证 EFI 部署，或记录不依赖发行版的手动部署流程。
- [ ] 固定所有外部文件的版本、SHA-256、许可证和源码地址。
- [ ] 提供预检脚本，检查文件、哈希、ESP 挂载、SMBIOS 唯一性和恢复介质。
- [ ] 从同型号裸机连续两次无需维护者介入完成安装。
- [ ] 测试 EFI 启动失败和 GPU 路径失败后的恢复。
- [ ] 发布带日期、BIOS 版本和 macOS build 的验收结果。

### M3 — 稳定的同型号发布

- [ ] 分别验证 Wi-Fi、蓝牙、有线网卡、声卡、USB、睡眠/唤醒和重启循环。
- [ ] 消除日常负载下已知的 WindowServer 崩溃和持续显示异常。
- [ ] 记录长时间稳定性、温控行为和回退验收。
- [ ] 提供每一步都有预期输出的新手可执行清单。
- [ ] 只有在 M2 和 M3 验收记录齐全后才创建首个 release tag。

### M4 — 媒体与性能

- [ ] 在码流参数不变时跨关键帧复用 VCN 解码会话。
- [ ] 将固定 `IOSleep(1)` fence 等待替换为有界自适应等待。
- [ ] 验证至少 3 路 1080p30 并发，不出现超时或帧错误。
- [ ] 调研 HEVC 硬件能力并记录是否可行。
- [ ] 发布可重复的 CPU、延迟、吞吐和功耗对比。

### M5 — 扩展硬件

**状态：未启动。** 只有在同型号流程可复现且稳定后，才允许适配其他机器。

### 标签与贡献入口

| 标签 | 用途 |
|---|---|
| `area/docs` | 文档、图表、翻译和链接检查 |
| `area/installation` | 安装器、EFI 部署、BIOS 前置条件与恢复 |
| `area/display` | WindowServer、帧缓冲、撕裂和残影 |
| `area/video` | VideoToolbox、VCN、编解码和性能基准 |
| `area/performance` | CPU、GPU、延迟、温控和功耗回归 |
| `status/needs-testing` | 已实现但等待真机确认 |
| `status/blocked-hardware` | 需要重启、硬件访问或当前缺少设备 |
| `priority/high` | 阻塞 M2/M3 可复现性或可能导致掉盘/无法启动 |

适合首次贡献的任务包括文档纠错、链接检查、可复现的验收报告，以及无需硬件的测试。
