# Security policy / 安全策略

[English](#english) · [简体中文](#简体中文)

## English

This project changes boot loaders, kernel extensions, NVRAM variables, system extensions and LaunchDaemons. A mistake can make the machine unbootable or expose local data.

### Reporting a vulnerability

- Use GitHub's private vulnerability reporting for this repository when available.
- If private reporting is unavailable, contact the maintainer through the private contact method listed on their GitHub profile. Do not publish exploit details in a public issue.
- Include affected versions, hardware, macOS build, impact, reproduction steps and a suggested fix if known.
- Never include personal serial numbers, MLB, UUIDs, account tokens or unredacted system logs.

### Scope

Examples of in-scope reports include unsafe shell execution, backup/restore failures, privilege escalation introduced by project scripts, secret leakage through logs, or a driver change that causes data loss.

This is a research project, not a supported security product. Unsupported hardware and experimental driver paths may receive best-effort fixes only.

## 简体中文

本项目会修改引导器、内核扩展、NVRAM、系统扩展和 LaunchDaemon。错误操作可能导致机器无法启动或泄露本地数据。

### 报告安全问题

- 优先使用本仓库的 GitHub 私密漏洞报告功能。
- 如果无法私密报告，请通过维护者 GitHub 主页列出的私密联系方式联系。不要在公开 Issue 中发布利用细节。
- 请提供受影响版本、硬件、macOS build、影响、复现步骤和已知修复建议。
- 不要包含个人序列号、MLB、UUID、账号 token 或未脱敏系统日志。

### 范围

例如脚本不安全执行、备份/恢复失败、项目脚本引入提权、日志泄露密钥，或驱动改动导致数据损坏。

这是研究项目，不是有 SLA 的安全产品。不受支持的硬件和实验性驱动路径只能尽力修复。
