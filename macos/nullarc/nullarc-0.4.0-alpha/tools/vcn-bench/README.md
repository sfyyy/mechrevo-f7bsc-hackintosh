# VCN 解码基准与回归脚本

这些脚本在本机做性能对比和逐帧像素比对，输出写到本目录的 `results/`（已被忽略）。它们需要先准备测试素材：

- `vt-bench`：由 `../vcn/vt-bench.c` 编译，放在本目录（`clang -std=c11 -O2 ../vcn/vt-bench.c ... -o vt-bench`，链接参数见文件头）。
- 码流素材：`make-regression-streams.py` 生成合成回归流；实录素材（直播抓包等）不随仓库提供，脚本里的 `results/…` 路径需要自行放置。

`postboot.sh` 是安装新 kext 并重启后的验收入口，其他脚本的用法见各自文件头注释。这些脚本没有在迁入仓库后重新跑过。
