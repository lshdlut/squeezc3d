# 基准测试

本页包含基准测试的方法说明与代表性结果。

定义：`speedup_x = ezc3d / sqzc3d`（值越大表示 `sqzc3d` 越好，包含内存比例）。

## 测什么

- **Chunk materialize**：构建紧凑且连续的 `double` 缓冲区 `[frame][point][3]`（+ valid mask）。
- **访问模式**：复制/提取 frame 与 window 输出、marker 轨迹访问、重排（frame-major -> point-major）。
- **峰值内存**：避免完整对象图，仅保留必要数组。

基准方法：完整加载一个 C3D 文件，然后基于每个库各自的**原生加载表示**测量访问模式。
对 `sqzc3d` 而言，原生表示是 chunk 内连续的 frame-major 数组；对 `ezc3d` 而言，原生表示是
`ezc3d::c3d` 的内存 frame/point 容器。

说明：

- `bench_sqzc3d` 与 `bench_ezc3d` 都会完整解析一个 C3D（点数据 + 如存在则包含 analogs）。
  访问模式 microbench 只针对点数组；analog 数组会被加载，但不会被访问。
  本页展示的示例文件不含 analog 通道，因此 analog materialization 不影响这里的数值。

## 复现

```bash
# C++（构建时启用 -DSQZC3D_BUILD_EXAMPLES=ON）
<build_dir>/bench_sqzc3d <file.c3d> <repeat>
<build_dir>/bench_ezc3d  <file.c3d> <repeat>

# Streaming（仅 sqzc3d）
<build_dir>/bench_sqzc3d_stream <file.c3d> 1

# Python
python samples/bench/bench_python.py <file.c3d> --lib sqzc3d --repeat <repeat>
python samples/bench/bench_python.py <file.c3d> --lib ezc3d  --repeat <repeat>
```

对于多配置生成器（Visual Studio），可执行文件可能位于 `<build_dir>/Release/`。

## Materialize 模式

### C++ — 原生

PFERD（117.96 MB, frames=55,844, points=132, repeat=1）：

| 指标 | sqzc3d | ezc3d | `speedup_x` |
| --- | ---: | ---: | ---: |
| `load_ms` | 218.023 | 2481.406 | 11.4x |
| `frame_copy_us_kall` | 0.100 | 2.204 | 22.0x |
| `window_copy_us_T256_kall` | 10.375 | 154.118 | 14.9x |
| `peak_rss_mb` | 182.398 | 1011.125 | 5.5x |

小文件（DOG, 4.23 MB, frames=4,634, points=57, repeat=10）：

| 指标 | sqzc3d | ezc3d | `speedup_x` |
| --- | ---: | ---: | ---: |
| `load_ms` | 10.979 | 98.711 | 9.0x |
| `frame_copy_us_kall` | 0.013 | 0.743 | 57.2x |
| `window_copy_us_T256_kall` | 2.544 | 54.254 | 21.3x |
| `peak_rss_mb` | 12.031 | 42.070 | 3.5x |

### Python

PFERD（117.96 MB, frames=55,844, points=132, repeat=1, `sqzc3d` v0.3.2 (ABI 3), `ezc3d` v1.6.0）：

| 指标 | sqzc3d | ezc3d | `speedup_x` |
| --- | ---: | ---: | ---: |
| `load_ms` | 197.913 | 2924.975 | 14.8x |
| `frame_copy_us_kall` | 1.234 | 4.594 | 3.7x |
| `window_copy_us_T256_kall` | 13.070 | 159.610 | 12.2x |
| `peak_rss_mb` | 209.617 | 1373.492 | 6.6x |

小文件（DOG, 4.23 MB, frames=4,634, points=57, repeat=5, `sqzc3d` v0.3.2 (ABI 3), `ezc3d` v1.6.0）：

| 指标 | sqzc3d | ezc3d | `speedup_x` |
| --- | ---: | ---: | ---: |
| `load_ms` | 8.975 | 116.915 | 13.0x |
| `frame_copy_us_kall` | 0.809 | 1.504 | 1.9x |
| `window_copy_us_T256_kall` | 3.983 | 42.041 | 10.6x |
| `peak_rss_mb` | 44.789 | 130.855 | 2.9x |

## Streaming 模式 — 仅 sqzc3d，低内存

示例（PFERD, repeat=1）：

| 指标 | sqzc3d stream |
| --- | ---: |
| `open_ms` | 1.289 |
| `peak_rss_delta_mb` | 2.762 |
| `read_window_ms_T256_kall` | 0.532 |
| `read_window_ms_T256_k32` | 0.889 |
