# MECHREVO 无界 S mini（F7BSC）黑苹果

本机专用的 OpenCore EFI 与安装后脚本：Ryzen 7 7840H / Radeon 780M 迷你主机，macOS 26 Tahoe，与 Arch Linux 双系统。

> 仅供学习研究。GPU 加速依赖实验性的 [nullarc](https://github.com/luuumi-dev/nullarc)，使用风险自负。

## 硬件与状态

| 部件 | 型号 | 状态 |
|---|---|---|
| CPU | Ryzen 7 7840H（Zen 4, Phoenix） | ✅ AMD Vanilla 补丁 + AMDRyzenCPUPowerManagement（`ryzenidle=1`） |
| 核显 | Radeon 780M `1002:15bf` | ✅ nullarc 0.4.0：Metal 3，WindowServer 由 GPU 合成（M2）；双缓冲补丁修复拖动撕裂；见已知问题 |
| 显示 | HDMI 外接 2560×1440@60 | ✅ 固件 GOP 点亮的单一模式；❌ 无 HiDPI |
| Wi-Fi | Intel AX210 | ✅ AirportItlwm 2.4.0 |
| 蓝牙 | Intel AX210 `8087:0032` | ⚠️ IntelBluetoothFirmware + IntelBTPatcher 2.5.3、NVRAM 失败标记重置；部署后未记录实测结论 |
| 有线网卡 | Realtek RTL8125 | 已启用 LucyRTL8125Ethernet 1.2.4，未单独验证 |
| 声卡 | Realtek ALC269VB | 已启用 AppleALC `alcid=11` + SimpleHDA，未单独验证 |
| 磁盘 | Kingston SNV2S 1TB（macOS）、WD Blue SN5100 1TB（Arch） | ✅ NVMeFix |

机型：`MacBookPro16,1`。系统：macOS 26.7.1（25G241）。OpenCore 1.0.8（DEBUG 版，`Target=67` 会在 ESP 根目录写 `opencore-*.txt` 日志）。

## 目录

```
EFI/                OpenCore（config.plist 中的机器身份为占位值）
  BOOT/BOOTx64.efi  OpenCore 引导程序，仅用于 U 盘等独立 ESP
  OC/               ACPI（3 张 SSDT）、Drivers、Kexts、config.plist
ACPI/               SSDT 源码（.dsl）
linux/              部署 EFI 到 ESP 的脚本、systemd-boot 启动项
macos/
  nullarc/          780M 驱动安装脚本（1–3 安装，4 双缓冲补丁）与 patches/
                    tests/（复现与回归测试）、perf/（WindowServer 采样与 Chrome 基准）
  display/          开机设置：双缓冲、动画残影绕过（LaunchDaemon）
  thermal/          核显限频 800 MHz + 时钟门控（LaunchDaemon）
  check.sh          只读状态检查
docs/known-issues.md
```

## 启动链

Arch ESP（WD 盘，挂载在 `/boot`）：systemd-boot → 「macOS」启动项 → `/EFI/OC/OpenCore.efi` → 不显示选单，直接启动 Kingston 上的 macOS
（`ShowPicker=false`；`ScanPolicy=17760515` 只扫描 APFS，避免绕回 systemd-boot）。
需要 OpenCore 选单时，选中「macOS」后按住 Alt（Option）。

## 部署 EFI（在 Arch 中）

1. 需要自行下载的 kext（许可证未声明允许再分发，仓库不收录，config 中已保留条目）。解压后放进 `EFI/OC/Kexts/`：

   | 文件 | 来源 | 可执行文件 sha256（本机使用的版本） |
   |---|---|---|
   | `AMFIPass.kext` 1.4.1 | 原仓库 dhinakg/AMFIPass 已下线；[OpenCore Legacy Patcher 附带的 AMFIPass-v1.4.1-RELEASE.zip](https://github.com/dortania/OpenCore-Legacy-Patcher/tree/main/payloads/Kexts/Acidanthera) | `4c35bc196d35c69b5f9dca83fe733801211c7828716f51585c7f5450039ca884` |
   | `SimpleHDA.kext` 1.0.0 | [laobamac/SimpleHDA v1.0.0](https://github.com/laobamac/SimpleHDA/releases/tag/v1.0.0) | `f74c9dbddf43266c97c5f72cd76498bce1d6b5bd3731a947be62f1378b4c7954` |

   缺少任何一个时 `deploy-efi.sh` 会列出并停止。

2. 机器身份：仓库根目录放 `smbios.private.json`（不入库）：

   ```json
   {"SystemProductName": "MacBookPro16,1", "SystemSerialNumber": "…", "MLB": "…", "SystemUUID": "…", "ROM": "112233445566"}
   ```

   `config.plist` 里是占位值。每台机器都要有自己唯一的一套：用 [GenSMBIOS](https://github.com/corpnewt/GenSMBIOS) 为 `MacBookPro16,1`
   生成序列号、MLB、UUID，ROM 填本机网卡 MAC。不要复用别人的，也不要在已登录 Apple ID 的机器上随意更换。

3. 部署：

   ```sh
   sudo linux/deploy-efi.sh                          # Arch ESP /boot，只替换 EFI/OC，必要时安装 systemd-boot 启动项
   sudo linux/deploy-efi.sh /mnt/usbefi --with-boot  # 其他 ESP（如 U 盘），同时写入 OpenCore 的 EFI/BOOT
   ```

   部署前会把目标的 `EFI/OC` 打包到 `backups/`（不入库）。目标 `EFI/OC` 会变成与仓库完全一致，多余文件会被删除。

回退：`sudo tar xzf backups/OC-boot-<时间>.tar.gz -C /boot/EFI`（先删除 `/boot/EFI/OC`）。

## 安装后（在 macOS 中）

### 780M 加速（nullarc 0.4.0）

先下载 [nullarc v0.4.0-alpha](https://github.com/luuumi-dev/nullarc/releases/tag/v0.4.0-alpha) 的 `nullarc-0.4.0-alpha.zip`
（sha256 `dcfeaa04917719683487b24259933b6ea84513a840cc862a68cd91205af47bbb`），在 macOS 里下载，不要从 Linux 拷贝解压后的文件。

```sh
sudo bash macos/nullarc/1-install.sh ~/Downloads/nullarc-0.4.0-alpha.zip   # 仅应用侧 Metal，不接管桌面
sudo bash macos/nullarc/2-fix-15bf.sh    # 发布包的 PhxAccel 只匹配 0x1900，改成上游的设备列表；然后重启
sudo bash macos/nullarc/3-enable-m2.sh   # PhxFB 也匹配 15bf 并启用 M2；重启两次后桌面由 GPU 合成
sudo bash macos/nullarc/4-dbuf.sh        # 重新编译带双缓冲补丁的 PhxAccel（修复拖动撕裂）；需要 Command Line Tools；然后重启
sudo bash macos/display/install.sh       # 开机打开双缓冲，绕过动画残影（nullarc B-58）和跨进程花屏
```

- 4-dbuf 从 GitHub 下载 nullarc v0.4.0-alpha 源码和 MacKernelSDK（固定版本，校验 sha256），打上
  `macos/nullarc/patches/phxaccel-dbuf.patch` 后编译，保留已安装的 Info.plist，备份旧 kext 与 Aux KC。
  撤销：`sudo bash macos/nullarc/4-dbuf.sh --restore <脚本打印的备份目录>` 后重启。原理见 `docs/known-issues.md`。
- display 的撤销：`sudo bash macos/display/install.sh --remove` 后重启。日志 `/Users/Shared/f7bsc-logs/phxdisplay.log`。

- 每步日志在 `/Users/Shared/f7bsc-logs/`。重新运行 nullarc 的 `install.sh` 会覆盖 Info.plist，之后需要再跑第 2、3 步。
- 3-enable-m2 前建议打开自动登录：GPU 路径下登录窗口可能黑屏。
- 撤销 M2：`sudo rm /Users/Shared/gpu/phxboot-m2; sudo nvram -d phxfb-boot` 后重启。
- 关闭 GPU：`sudo touch /Users/Shared/gpu/phxboot-off` 后重启。

### 降温

```sh
sudo bash macos/thermal/install.sh     # 需要 boot-arg phxdebug=1（config 已含）
sudo bash macos/check.sh               # 查看状态
```

不想开时钟门控：`sudo touch /Users/Shared/gpu/gpucg-off`。卸载：`sudo bash macos/thermal/uninstall.sh` 后重启。

## config 要点

- ACPI：`SSDT-PLUG-ALT`（CPU 对象）、`SSDT-USBX`、`SSDT-EC-F7BSC`（在 `PCI0.SBRG` 下提供 Darwin 用的 EC，保留固件 EC0）。
- Kernel：AMD Vanilla 补丁（core count 按本机设置）、`ProvideCurrentCpuInfo`、`DummyPowerManagement`。
- boot-args：`-v keepsyms=1 debug=0x100 msgbuf=1048576` 为调试用，稳定后可去掉；
  `amfi=0x80 amfi_get_out_of_my_way=0x1 -amfipassbeta` 为 nullarc 所需；`phxdebug=1` 供 phxctl 使用（降温脚本依赖）；
  `ryzenidle=1` 让 CPU 空闲时使用 `hlt`（`2` 为 CC6，会导致画面不刷新）；`alcid=11` 声卡布局；`-btlfxallowanyaddr` 蓝牙。
- NVRAM：每次启动重置 `bluetoothExternalDongleFailed` / `bluetoothInternalControllerInfo`。

## Kext 来源

| Kext | 版本 | 来源 |
|---|---|---|
| Lilu、VirtualSMC、AppleALC、NVMeFix、RestrictEvents | 1.7.2 / 1.3.8 / 1.9.8 / 1.1.3 / 1.1.6 | acidanthera |
| AMDRyzenCPUPowerManagement、SMCAMDProcessor | 0.7.2 补丁版 | trulyspinach/SMCAMDProcessor，`ryzenidle` 补丁来自 QiRanChenk/MECHREVO-WUJIE15XA-8745HS-Hackintosh |
| PhxBringup | 1.0.0（build 35 = build 34 + `macos/nullarc/patches/phxbringup-ptfix.patch`，加 `1002:15bf`） | luuumi-dev/nullarc 0.4.0（`src/phxbringup`，MIT 或 GPL-2.0） |
| AMFIPass | 1.4.1 | 自行下载，见上文 |
| SimpleHDA | 1.0.0 | 自行下载，见上文 |
| AirportItlwm | 2.4.0（Tahoe 版） | laobamac 基于 OpenIntelWireless/itlwm 的分支 |
| IntelBluetoothFirmware、IntelBTPatcher | 2.5.3 / 2.5.3 | OpenIntelWireless |
| BlueToolFixup | 2.7.2 | acidanthera/BrcmPatchRAM |
| LucyRTL8125Ethernet | 1.2.4 | Mieze |
| VoodooPS2Controller | 2.3.7 | acidanthera |
| AppleMCEReporterDisabler、XhciNudge | 1.2 / 1.0.0 | 取自 bbbbrt/mechrevo-wujie14pro-7840hs-780m-hackintosh-efi |

各组件遵循其各自许可证，源码见各自上游仓库；许可声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 参考

- https://github.com/luuumi-dev/nullarc （issue #11 HiDPI、#14 空闲功耗、#27 外接显示）
- https://github.com/QiRanChenk/MECHREVO-WUJIE15XA-8745HS-Hackintosh
- https://github.com/bbbbrt/mechrevo-wujie14pro-7840hs-780m-hackintosh-efi
