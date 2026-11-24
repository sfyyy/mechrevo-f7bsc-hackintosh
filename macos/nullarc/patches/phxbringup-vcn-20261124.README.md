# phxbringup 解码性能补丁（2026-11-24）

`phxbringup-vcn-20261124.patch` = `diff -ruN`（原始树 → 当前树，`src/phxbringup`，排除 `build/`），应用方式：

    cd work/nullarc-0.4.0-alpha && patch -p1 < patches/phxbringup-vcn-20261124.patch

已验证：对原始树应用后与当前工作树逐文件一致（只差 .DS_Store 和测试夹具）。

相对旧补丁 `phxbringup-vcn.patch`（build 75）新增：

| 内容 | 文件 |
|---|---|
| fence 等待状态机（自旋 + 亚毫秒睡眠 + 兜底，EWMA 封顶 8 ms） | `PhxVcnWait.h`（新）、`PhxBringup.cpp` |
| IDR 会话复用判定与 errorFree 语义 | `PhxVcnAvcStream.h`、`PhxBringup.cpp` |
| 回读刷新范围（0/1/2）与 flush/copy 计时 | `PhxBringup.cpp` |
| 主机测试：等待状态机 7 组、复用判定 7 条 | `tests/vcn_host_test.cpp` |

R-8 的 FFmpeg 补丁不在这里，见 `vcntest/results/ffmpeg-patched/`。

2026-11-24 与 build 79 的性能数据一并提交。
