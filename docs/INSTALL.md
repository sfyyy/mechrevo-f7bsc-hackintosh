# Installation guide / 安装指南

[English](#english) · [简体中文](#简体中文)

> [!WARNING]
> **Draft support level: M1, advanced same-model users only.** This guide starts from an already installed macOS plus an environment that can safely mount and write the ESP. Arch Linux is not required; it was only the maintainer's reference environment. The guide does not yet cover bare-metal BIOS configuration, macOS installer creation, disk partitioning, or a fully tested recovery from a blank disk.
>
> Do not run any command until you have a complete backup and a bootable recovery USB.

## English

### Supported boundary

This guide currently targets only:

- MECHREVO Wujie S mini **F7BSC**.
- AMD Ryzen 7 7840H with Radeon 780M (`1002:15bf`).
- macOS 26 Tahoe, currently validated on macOS 26.7.1 (25G241).
- OpenCore 1.0.8 with the repository's `EFI/OC` configuration.
- Access to the macOS ESP from Linux or another suitable environment. The supplied deployment script is currently tested only on Arch Linux with the ESP mounted at `/boot`; Arch itself is not a requirement.
- A macOS installation that already boots and has working network access.
- A Linux staging environment with `sudo`, `python3`, `rsync`, `tar` and `mountpoint` for `linux/deploy-efi.sh`. Command Line Tools are required later for the macOS rebuild step.

It does **not** support a clean install from bare hardware yet, and it must not be reused on another model or hardware revision.

### Prerequisites

Before starting:

1. Read [known-issues.md](known-issues.md) and the VCN limitations in [macos/nullarc/nullarc-0.4.0-alpha/docs/vcn/README.md](../macos/nullarc/nullarc-0.4.0-alpha/docs/vcn/README.md).
2. Back up the existing EFI partition and any data you cannot lose.
3. Prepare a bootable Linux or macOS recovery USB and a second computer. The maintainer used Arch Linux, but any environment that can safely access the ESP is acceptable if you know how to use it.
4. Confirm that Secure Boot, firmware passwords, and disk encryption settings will not block recovery. The complete BIOS checklist is still a roadmap task.
5. Do not publish or commit `smbios.private.json`.

### Required external files

The repository intentionally does not redistribute the following files:

| File | Version | Source | Expected executable SHA-256 |
|---|---|---|---|
| `nullarc-0.4.0-alpha.zip` | v0.4.0-alpha | [nullarc release](https://github.com/luuumi-dev/nullarc/releases/tag/v0.4.0-alpha) | ZIP: `dcfeaa04917719683487b24259933b6ea84513a840cc862a68cd91205af47bbb` |
| `AMFIPass.kext` | 1.4.1 | [OpenCore Legacy Patcher payload](https://github.com/dortania/OpenCore-Legacy-Patcher/tree/main/payloads/Kexts/Acidanthera) | `4c35bc196d35c69b5f9dca83fe733801211c7828716f51585c7f5450039ca884` |
| `SimpleHDA.kext` | 1.0.0 | [laobamac/SimpleHDA](https://github.com/laobamac/SimpleHDA/releases/tag/v1.0.0) | `f74c9dbddf43266c97c5f72cd76498bce1d6b5bd3731a947be62f1378b4c7954` |

The `4-dbuf.sh` script downloads its build dependencies and verifies their SHA-256 values itself.

### 1. Create a private SMBIOS identity

Use [GenSMBIOS](https://github.com/corpnewt/GenSMBIOS) to generate a unique `MacBookPro16,1` identity. Replace the MAC placeholder with your machine's wired network MAC, without separators.

Create `smbios.private.json` in the repository root:

```json
{
  "SystemProductName": "MacBookPro16,1",
  "SystemSerialNumber": "REPLACE_ME",
  "MLB": "REPLACE_ME",
  "SystemUUID": "REPLACE_ME",
  "ROM": "112233445566"
}
```

This file is git-ignored. Do not reuse another person's identity and do not upload the completed file.

### 2. Deploy the OpenCore EFI

Install the two external kexts under `EFI/OC/Kexts/`, then run from a Linux environment:

```sh
sudo linux/deploy-efi.sh
```

For a separate USB ESP:

```sh
sudo linux/deploy-efi.sh /mnt/usbefi --with-boot
```

The script refuses to continue if enabled ACPI, driver or kext entries are missing. It archives the current `EFI/OC` under `backups/` before replacing it. The backup directory is git-ignored. This script is only validated on Arch Linux + systemd-boot; other distributions may need equivalent tools or a documented manual copy.

### 3. Install the nullarc userspace stack

Boot macOS and download the release ZIP inside macOS. Verify the ZIP checksum before extraction:

```sh
shasum -a 256 ~/Downloads/nullarc-0.4.0-alpha.zip
sudo bash macos/nullarc/1-install.sh ~/Downloads/nullarc-0.4.0-alpha.zip
```

Reboot after the script confirms success. Then apply the device-ID fix:

```sh
sudo bash macos/nullarc/2-fix-15bf.sh
```

Reboot again. Enable GPU-backed WindowServer compositing:

```sh
sudo bash macos/nullarc/3-enable-m2.sh
```

Automatic login is recommended before the next reboot because the login window may be black on the GPU path. The script prints a warning if automatic login is disabled. Reboot twice as instructed by its output.

### 4. Install the double-buffer display patch

Install Command Line Tools first, then run:

```sh
xcode-select --install
sudo bash macos/nullarc/4-dbuf.sh
```

The script downloads pinned nullarc and MacKernelSDK archives, verifies their checksums, applies `patches/phxaccel-dbuf.patch`, backs up the current Aux KC and kext, and refuses to publish an Aux KC that would lose existing third-party kexts. Reboot after success.

### 5. Install display and thermal controls

```sh
sudo bash macos/display/install.sh
sudo bash macos/thermal/install.sh
```

Both installers create LaunchDaemons. Review their logs under `/Users/Shared/f7bsc-logs/` and the documented uninstall commands before proceeding.

### 6. Validate the installation

Run the read-only status check:

```sh
sudo bash macos/check.sh
```

Record the following results in an issue or local test note:

- `sw_vers` version and build.
- Loaded `PhxAccel`, `PhxBringup`, CPU-power and thermal components.
- Metal/WindowServer GPU compositing is active.
- HDMI mode is 2560×1440@60 and remains stable while dragging windows.
- Wi-Fi works; Bluetooth, Ethernet and audio are tested independently.
- Display and thermal LaunchDaemons are running.
- No recent WindowServer crash reports.
- H.264 encoding/decoding claims match [BENCHMARKS.md](../macos/nullarc/nullarc-0.4.0-alpha/docs/vcn/BENCHMARKS.md).

### 7. Rollback

- EFI: restore the printed `backups/OC-boot-<timestamp>.tar.gz` after removing the failed `EFI/OC` directory.
- Double-buffer kext: `sudo bash macos/nullarc/4-dbuf.sh --restore <backup-directory>`.
- Display controls: `sudo bash macos/display/install.sh --remove`.
- Thermal controls: `sudo bash macos/thermal/uninstall.sh`.
- GPU compositing: remove `/Users/Shared/gpu/phxboot-m2`, remove the `phxfb-boot` NVRAM variable, then reboot.
- If the system no longer boots, use the recovery USB instead of attempting to edit the ESP from the failed macOS installation.

### Clean-install acceptance gate

This guide becomes M2 only after all of the following are documented and independently tested:

- [ ] Exact supported BIOS version and every required BIOS setting.
- [ ] macOS installer download, USB creation and OpenCore installer boot.
- [ ] Disk layout, APFS/ESP creation and dual-boot recovery.
- [ ] Every external artifact pinned with a checksum and license reference.
- [ ] A clean-install attempt succeeds twice from bare metal without maintainer intervention.
- [ ] Full validation checklist and rollback tested after a failed GPU boot.

Until these tasks are complete, the project remains an experimental same-model reference build.

## 简体中文

### 支持边界

当前指南只面向：

- 机械革命无界 S mini **F7BSC**。
- AMD Ryzen 7 7840H，Radeon 780M（`1002:15bf`）。
- macOS 26 Tahoe，目前验证版本为 macOS 26.7.1（25G241）。
- OpenCore 1.0.8 与仓库中的 `EFI/OC` 配置。
- 能从 Linux 或其他合适环境访问 macOS ESP。随仓库提供的部署脚本目前只在 Arch Linux（ESP 挂载于 `/boot`）上验证；Arch 本身不是必需项。
- 已安装且能启动的 macOS，并有可用网络。
- 用于运行 `linux/deploy-efi.sh` 的 Linux 环境需要 `sudo`、`python3`、`rsync`、`tar` 和 `mountpoint`；后续 macOS 重编译还需要 Command Line Tools。

它**尚不支持从裸机全新安装**，也不得用于其他型号或硬件批次。

### 前置条件

开始前：

1. 阅读 [known-issues.md](known-issues.md) 和 [VCN 限制说明](../macos/nullarc/nullarc-0.4.0-alpha/docs/vcn/README.md)。
2. 备份现有 EFI 分区和所有不能丢失的数据。
3. 准备可启动的 Linux 或 macOS 恢复 U 盘，以及第二台电脑。维护者使用 Arch Linux，但只要你能安全访问 ESP，其他环境同样可以提供所需能力。
4. 确认 Secure Boot、固件密码和磁盘加密不会阻碍恢复。完整 BIOS 清单仍是路线图任务。
5. 不要公开或提交 `smbios.private.json`。

### 必须自行准备的外部文件

仓库有意不再分发以下文件：

| 文件 | 版本 | 来源 | 预期可执行文件 SHA-256 |
|---|---|---|---|
| `nullarc-0.4.0-alpha.zip` | v0.4.0-alpha | [nullarc release](https://github.com/luuumi-dev/nullarc/releases/tag/v0.4.0-alpha) | ZIP：`dcfeaa04917719683487b24259933b6ea84513a840cc862a68cd91205af47bbb` |
| `AMFIPass.kext` | 1.4.1 | [OpenCore Legacy Patcher payload](https://github.com/dortania/OpenCore-Legacy-Patcher/tree/main/payloads/Kexts/Acidanthera) | `4c35bc196d35c69b5f9dca83fe733801211c7828716f51585c7f5450039ca884` |
| `SimpleHDA.kext` | 1.0.0 | [laobamac/SimpleHDA](https://github.com/laobamac/SimpleHDA/releases/tag/v1.0.0) | `f74c9dbddf43266c97c5f72cd76498bce1d6b5bd3731a947be62f1378b4c7954` |

`4-dbuf.sh` 会自行下载构建依赖并校验 SHA-256。

### 1. 创建私有 SMBIOS 身份

使用 [GenSMBIOS](https://github.com/corpnewt/GenSMBIOS) 生成唯一的 `MacBookPro16,1` 身份。`ROM` 填本机有线网卡 MAC，去掉分隔符。

在仓库根目录创建 `smbios.private.json`：

```json
{
  "SystemProductName": "MacBookPro16,1",
  "SystemSerialNumber": "REPLACE_ME",
  "MLB": "REPLACE_ME",
  "SystemUUID": "REPLACE_ME",
  "ROM": "112233445566"
}
```

该文件已被 git 忽略。不要复用他人的身份，也不要上传填写后的文件。

### 2. 部署 OpenCore EFI

先把两个外部 kext 放入 `EFI/OC/Kexts/`，然后在 Linux 环境运行：

```sh
sudo linux/deploy-efi.sh
```

独立 U 盘 ESP：

```sh
sudo linux/deploy-efi.sh /mnt/usbefi --with-boot
```

如果 config 中启用的 ACPI、驱动或 kext 文件缺失，脚本会停止。替换前会把现有 `EFI/OC` 打包到 `backups/`，该目录不入库。该脚本只在 Arch Linux + systemd-boot 上验证过；其他发行版可能需要等价工具或经过记录的手动复制流程。

### 3. 安装 nullarc 用户态组件

启动 macOS 后，在 macOS 内下载 release ZIP。解压前校验：

```sh
shasum -a 256 ~/Downloads/nullarc-0.4.0-alpha.zip
sudo bash macos/nullarc/1-install.sh ~/Downloads/nullarc-0.4.0-alpha.zip
```

脚本确认成功后重启，然后应用设备 ID 修复：

```sh
sudo bash macos/nullarc/2-fix-15bf.sh
```

再次重启，启用 WindowServer GPU 合成：

```sh
sudo bash macos/nullarc/3-enable-m2.sh
```

下一次重启前建议打开自动登录，因为 GPU 路径下登录窗口可能黑屏。脚本会在自动登录关闭时打印警告。按其输出要求重启两次。

### 4. 安装双缓冲显示补丁

先安装 Command Line Tools：

```sh
xcode-select --install
sudo bash macos/nullarc/4-dbuf.sh
```

脚本会下载固定版本的 nullarc 和 MacKernelSDK，校验哈希，应用 `patches/phxaccel-dbuf.patch`，备份当前 Aux KC 与 kext，并拒绝生成会丢失其他第三方 kext 的 Aux KC。成功后重启。

### 5. 安装显示与温控

```sh
sudo bash macos/display/install.sh
sudo bash macos/thermal/install.sh
```

两个安装器都会创建 LaunchDaemon。继续前检查 `/Users/Shared/f7bsc-logs/` 中的日志，并阅读对应的卸载命令。

### 6. 验收

运行只读状态检查：

```sh
sudo bash macos/check.sh
```

记录以下结果：

- `sw_vers` 版本和 build。
- `PhxAccel`、`PhxBringup`、CPU 电源管理和温控组件是否加载。
- Metal/WindowServer GPU 合成是否生效。
- HDMI 是否为 2560×1440@60，拖动窗口时是否稳定。
- Wi-Fi 是否可用；蓝牙、有线网卡和声卡是否分别验证。
- 显示与温控 LaunchDaemon 是否运行。
- 最近是否有 WindowServer 崩溃报告。
- H.264 编解码结果是否符合 [BENCHMARKS.md](../macos/nullarc/nullarc-0.4.0-alpha/docs/vcn/BENCHMARKS.md)。

### 7. 回退

- EFI：删除失败的 `EFI/OC` 后恢复脚本打印的 `backups/OC-boot-<时间>.tar.gz`。
- 双缓冲 kext：`sudo bash macos/nullarc/4-dbuf.sh --restore <备份目录>`。
- 显示控制：`sudo bash macos/display/install.sh --remove`。
- 温控：`sudo bash macos/thermal/uninstall.sh`。
- GPU 合成：删除 `/Users/Shared/gpu/phxboot-m2`，删除 NVRAM 中的 `phxfb-boot`，然后重启。
- 如果系统已经无法启动，使用恢复 U 盘，不要尝试从失败的 macOS 中修改 ESP。

### 全新安装验收门槛

只有以下事项全部完成并经过独立测试，本指南才能升级为 M2：

- [ ] 记录受支持的 BIOS 版本及全部必要设置。
- [ ] 记录 macOS 安装器下载、U 盘制作和 OpenCore 安装启动流程。
- [ ] 记录磁盘分区、APFS/ESP 创建与双系统恢复流程。
- [ ] 所有外部文件固定版本、校验值及许可证出处。
- [ ] 从裸机开始连续两次无需维护者介入安装成功。
- [ ] 完整验收清单，以及 GPU 启动失败后的回退流程经过测试。

在这些任务完成前，项目仍是同型号实验性参考构建。
