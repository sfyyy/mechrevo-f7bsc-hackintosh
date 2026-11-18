# 第三方组件声明

本仓库再分发以下二进制和脚本。源码在各自的上游仓库。

| 组件 | 许可证 | 版权 | 源码 |
|---|---|---|---|
| OpenCore（OpenCore.efi、BOOTx64.efi、OpenRuntime、ResetNvramEntry）、OpenHfsPlus | BSD-3-Clause（OpenHfsPlus 见上游） | acidanthera | https://github.com/acidanthera/OpenCorePkg |
| Lilu、VirtualSMC、AppleALC、NVMeFix、RestrictEvents | BSD-3-Clause | vit9696、acidanthera | https://github.com/acidanthera |
| VoodooPS2Controller、BlueToolFixup | APSL-2.0 / GPL-2.0 | acidanthera | https://github.com/acidanthera/VoodooPS2 、https://github.com/acidanthera/BrcmPatchRAM |
| AirportItlwm | GPL-2.0（同 itlwm） | zxystd、laobamac | https://github.com/OpenIntelWireless/itlwm |
| IntelBluetoothFirmware、IntelBTPatcher | GPL-3.0；内含 Intel 蓝牙固件 | zxystd（钟先耀） | https://github.com/OpenIntelWireless/IntelBluetoothFirmware |
| LucyRTL8125Ethernet | GPL-2.0-or-later | Laura Müller | https://github.com/Mieze/LucyRTL8125Ethernet |
| AMDRyzenCPUPowerManagement、SMCAMDProcessor | BSD-3-Clause（`ryzenidle` 补丁 MIT） | trulyspinach；WUJIE15XA-Hackintosh contributors | https://github.com/trulyspinach/SMCAMDProcessor ，补丁 https://github.com/QiRanChenk/MECHREVO-WUJIE15XA-8745HS-Hackintosh/tree/main/patches |
| PhxBringup | MIT 或 GPL-2.0-only | luuumi-dev 与 nullarc 贡献者 | https://github.com/luuumi-dev/nullarc （`src/phxbringup`） |
| XhciNudge、AppleMCEReporterDisabler | MIT | bbbbrt | https://github.com/bbbbrt/mechrevo-wujie14pro-7840hs-780m-hackintosh-efi |
| `macos/thermal/`（gpuclk.sh、gpucg.sh、local.gpuclk.plist） | MIT | WUJIE15XA-Hackintosh contributors | https://github.com/QiRanChenk/MECHREVO-WUJIE15XA-8745HS-Hackintosh/tree/main/tools |

不随仓库分发（需自行下载）：AMFIPass、SimpleHDA，见 README。

## PhxAccel 补丁（macos/nullarc/patches/phxaccel-dbuf.patch）

补丁修改的是 nullarc `src/phxaccel`（PhxAccel.kext），该部分以 PolyForm Noncommercial License 1.0.0 授权，
仅限非商业用途：https://polyformproject.org/licenses/noncommercial/1.0.0 。仓库只分发补丁，不分发编译出的 kext。

Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)

## nullarc（PhxBringup）

```
MIT License

Copyright (c) 2026 luuumi-dev and the nullarc contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## WUJIE15XA-Hackintosh（macos/thermal、ryzenidle 补丁）

```
MIT License

Copyright (c) 2026 WUJIE15XA-Hackintosh contributors

Applies to the original parts of this repository only (ACPI-src, tools, patches).
Third-party binaries under EFI/ keep their own upstream licenses.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## bbbbrt/mechrevo-wujie14pro-7840hs-780m-hackintosh-efi（XhciNudge、AppleMCEReporterDisabler）

```
MIT License

Copyright (c) 2026 bbbbrt

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

This license covers only original documentation, helper scripts, and
configuration arrangement authored for this repository. Third-party components
are governed by their own upstream licenses; see THIRD_PARTY_NOTICES.md.
```
