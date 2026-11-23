# Contributing / 贡献指南

[English](#english) · [简体中文](#简体中文)

## English

### Before you contribute

This is a hardware-specific project. Changes that work on one BIOS revision or macOS build may break another. Please include evidence, exact versions and a rollback path.

1. Search existing issues and [`docs/known-issues.md`](docs/known-issues.md) first.
2. For a bug, open the Bug report form with hardware, BIOS, macOS build, boot arguments, installed kexts and reproduction steps.
3. For a feature, describe the user problem and the acceptance test, not only the implementation.
4. Never post SMBIOS serial numbers, MLB, UUIDs, Apple account data, personal paths or unredacted logs.
5. Do not upload Apple software or third-party files whose license does not allow redistribution.

### Pull requests

- Keep changes focused and explain the hardware/software environment in which they were tested.
- Update both `README.md` and `README.zh-CN.md` when public project facts or installation steps change.
- Update `ROADMAP.md` when a milestone task is completed.
- Add a test, probe or reproducible command for behavioral changes where possible.
- Preserve third-party notices and upstream license headers.
- Do not mix generated binaries, backups or private SMBIOS data with source changes.

### Validation

Run the repository checks before opening a pull request:

```sh
make check
```

For hardware changes, include the output of:

```sh
sudo bash macos/check.sh
```

For VCN changes, also run the checks in [`macos/nullarc/vcn/README.md`](macos/nullarc/vcn/README.md) and record the exact PhxBringup build.

### Commit and review style

- Use imperative commit subjects, for example `Document clean-install prerequisites`.
- Keep unrelated refactors out of a fix.
- Explain risk, rollback and verification in the pull request.
- By submitting a contribution, you agree that original parts of it may be distributed under the repository's MIT license; third-party material remains under its own license.

## 简体中文

### 贡献前

本项目具有强硬件依赖。一个 BIOS 版本或 macOS build 上可用的改动，可能在另一个版本上导致无法启动。请提供证据、准确版本和回退方式。

1. 先搜索已有 Issue 和 [`docs/known-issues.md`](docs/known-issues.md)。
2. 报告 Bug 时使用 Bug 模板，填写硬件、BIOS、macOS build、boot-args、已安装 kext 和复现步骤。
3. 提交功能时先描述用户问题和验收方式，而不只是实现方案。
4. 不要公开 SMBIOS 序列号、MLB、UUID、Apple 账号信息、个人路径或未脱敏日志。
5. 不要上传 Apple 软件或许可证不允许再分发的第三方文件。

### Pull Request

- 保持改动聚焦，并说明测试所基于的硬件/软件环境。
- 公共事实或安装步骤改变时，同时更新 `README.md` 和 `README.zh-CN.md`。
- 完成路线图任务时同步更新 `ROADMAP.md`。
- 行为变更尽量附带测试、探针或可复现命令。
- 保留第三方声明和上游许可证头。
- 不要把生成二进制、备份或私有 SMBIOS 数据混入源码改动。

### 验证

提交 PR 前运行仓库检查：

```sh
make check
```

涉及硬件的改动请附上：

```sh
sudo bash macos/check.sh
```

涉及 VCN 的改动还应执行 [`macos/nullarc/vcn/README.md`](macos/nullarc/vcn/README.md) 中的检查，并记录准确的 PhxBringup build。

### 提交与评审

- 提交标题使用祈使句，例如 `Document clean-install prerequisites`。
- 不要在一个修复中混入无关重构。
- 在 PR 中说明风险、回退方式和验证结果。
- 提交贡献即表示你同意原创部分可按仓库 MIT 许可证分发；第三方材料仍遵循其自身许可证。
