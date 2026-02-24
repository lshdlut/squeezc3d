# Squeezed C3D (`sqzc3d`)

[English](README.md) | [简体中文](README.zh-CN.md)

[![CI](https://github.com/lshdlut/squeezc3d/actions/workflows/ci.yml/badge.svg)](https://github.com/lshdlut/squeezc3d/actions/workflows/ci.yml)

`Squeezed C3D` (`sqzc3d`) 是一个小型 C/C++ 库，用于：
- 解析 C3D 的点/模拟（analog）数据，
- 构建紧凑的 chunk 结构，
- 通过索引/标签查询所选 marker/channel，
- 导出/导入持久化的 bundle 文件。

它被设计为上层项目（例如 `smocap`）的纯依赖，并尽量保持最小的运行时 API 面。

---

## 为什么是 Squeezed C3D

### 为什么是 Squeezed C3D（基准测试意图）

这里只展示与性能直接相关的指标：

- **Chunk materialize**：构建紧凑且连续的 `double` 缓冲区 `[frame][point][3]`（+ valid mask）。
- **访问模式**：复制/提取 frame 与 window 输出、marker 轨迹访问、重排（frame-major -> point-major）。
- **峰值内存**：避免完整对象图，仅保留必要数组。

基准方法：完整加载一个 C3D 文件，然后基于每个库各自的**原生加载表示**测量访问模式。
对 `sqzc3d` 而言，原生表示是 chunk 内连续的 frame-major 数组；对 `ezc3d` 而言，原生表示是
`ezc3d::c3d` 的内存 frame/point 容器。

复现命令：

```bash
# C++
local_tools/build/Release/bench_sqzc3d.exe <file.c3d> <repeat>
local_tools/build/Release/bench_ezc3d.exe  <file.c3d> <repeat>

# Python
python samples/bench/bench_python.py <file.c3d> --lib sqzc3d --repeat <repeat>
python samples/bench/bench_python.py <file.c3d> --lib ezc3d  --repeat <repeat>
```

说明：
- `bench_sqzc3d` 以及 Python 的 `sqzc3d` bench 会关闭 analog 读取，以聚焦于点数据 materialize + 访问模式。
- `ezc3d` 会 materialize 完整的内存结构（如存在，也包括 analogs）。

### 基准测试（materialize 模式）

定义：`speedup_x = ezc3d / sqzc3d`（值越大表示 `sqzc3d` 越好，包含内存比例）。

#### C++（原生）

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

#### Python

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

### Streaming 模式（仅 sqzc3d，低内存）

可选的极限低内存模式：按需从文件读取（例如 WASM VFS）：

- `bench_sqzc3d_stream <file.c3d> 1`

示例（PFERD, repeat=1）：

| 指标 | sqzc3d stream |
| --- | ---: |
| `open_ms` | 1.289 |
| `peak_rss_delta_mb` | 2.762 |
| `read_window_ms_T256_kall` | 0.532 |
| `read_window_ms_T256_k32` | 0.889 |

### 特性概览

- **Core + easy 分层**：稳定的 C API（`sqzc3d.h`）+ 易用的 C++ helper 层（`sqzc3d_easy.h`）。
- **Preset 优先工作流**：为常见读取模式提供共享 preset。
- **Chunk 优先的运行时契约**：frame-major 点数组 + 显式 valid mask。
- **Type group 感知过滤**：`type_group_*` 元数据用于 marker 集控制。
- **构建拆分**：`SQZC3D_WITH_EZC3D=OFF` 时仍支持仅 bundle 的运行时。

### 0.2 新增

- 常见工作流的 preset builder（`stream_frame_all`, `stream_frame_sel`, `window_analysis`, `interpolation_ready`）。
- 双层 API：
  - 用于稳定运行时 ABI 的 **C 层**（`sqzc3d.h`）。
  - 用于易用的一次性 window 读取的 **C++ easy 层**（`sqzc3d_easy.h`）。
- chunk 中的 type-group 元数据，用于 marker-group-aware 的工作流。

---

## 构建

```bash
cmake -S . -B local_tools/build
cmake --build local_tools/build --config Release --parallel
```

### 常用选项

- `SQZC3D_WITH_EZC3D`（`ON|OFF`, 默认 `ON`）  
  启用/禁用 C3D 解析功能。
- `SQZC3D_FETCH_EZC3D`（`ON|OFF`, 默认 `ON`）  
  当当前 toolchain 中找不到 ezc3d 时自动拉取。
- `SQZC3D_APPLY_EZC3D_PATCHES`（`ON|OFF`, Emscripten/WASM 默认 `ON`，其他默认 `OFF`）  
  对拉取到的 ezc3d 应用本地兼容性 patch（见 `cmake/patches/README.md`）。
- `SQZC3D_BUILD_EXAMPLES`（`ON|OFF`, 默认 `OFF`）  
  构建 CLI 示例程序。
- `SQZC3D_EZC3D_GIT_REPOSITORY` / `SQZC3D_EZC3D_GIT_TAG`  
  当 `SQZC3D_FETCH_EZC3D=ON` 时控制拉取源。

> 兼容性说明：出于 CMake 兼容考虑，旧的 `sqzc3d_WITH_EZC3D` 仍会被容忍，并映射到规范的 `SQZC3D_WITH_EZC3D`。

### 索引模型（chunk-local）

- chunk 对外暴露的所有点索引（包括 `type_group_indices`）都位于 **chunk-local** 的点索引空间 `[0..n_points)`。
- 可选：`sqzc3d_chunk_point_indices_total()`（以及 Python `Chunk.point_indices_total`）在可用时提供从 **chunk-local -> source-total** 的点索引映射。

```python
import sqzc3d

dec = sqzc3d.Decoder("path/to/file.c3d")
chunk = dec.read(frame_count=1, points=["LHEE", "RHEE"])
meta = chunk.meta

# type-group indices are chunk-local indices into meta["point_labels"].
# Default: missing TYPE_GROUPS metadata is treated as a no-op (all points).
marker_idx = sqzc3d.type_group_indices(chunk, "MARKER", strict=False)
marker_labels = [meta["point_labels"][i] for i in marker_idx]
print(marker_labels)
print(chunk.point_indices_total)  # optional local->total mapping (may be None)
```

### 运行时能力矩阵

| 特性 | ON | OFF |
| --- | --- | --- |
| C3D 解析（`open_file`/`open_memory`） | ✅ | ❌ |
| Chunk 构建（`build_chunks`） | ✅ | ❌ |
| Bundle 导出/加载 | ✅ | ✅ |
| Analog 支持 | ✅ | ✅ |

运行时可用性始终可以通过 `sqzc3d_get_features()` 查询。

### CMake 用法（作为依赖）

```cmake
add_subdirectory(path/to/sqzc3d)
target_link_libraries(your_target PRIVATE sqzc3d)
```

---

## 快速上手

### 1）解析 C3D 并构建 chunks

```c
sqzc3d_default_open_opt(&open_opt);
sqzc3d_open_file(&dec, path, &open_opt);
sqzc3d_default_build_opt(&build_opt);
sqzc3d_build_chunks(dec, &build_opt, &chunk);

// use chunk metadata/queries/views
sqzc3d_free_chunk(chunk);
sqzc3d_close_dec(dec);
```

注意：
- 始终使用 `sqzc3d_default_*_opt(...)` 初始化 option struct（要求：`struct_size == sizeof(struct)`）。
- 默认 build options 会 materialize analog；如需跳过 analog，可设置 `build_opt.analog_enable = sqzc3d_ANALOG_EN_OFF`。

### 2）从 bundle 加载

```c
sqzc3d_load_bundle(bundle_path, &chunk);
sqzc3d_free_chunk(chunk);
```

所有 API 契约都只使用整数与指针，因此同时适用于 C 与 C++ 项目。

快速检查运行时身份：

```c
printf("sqzc3d version=%s abi=%d\n", sqzc3d_version(), sqzc3d_abi_version());
```

### 3）C++ easy 入口（`sqzc3d_easy.h`）

```c++
#include "sqzc3d_easy.h"

sqzc3d::ReadPointsWindow(dec, 0, 32, nullptr, 0, nullptr, &chunk);
const auto view = sqzc3d::FrameMajorPointsView(chunk);
```

`sqzc3d_easy.h` 是一个轻量的 C++ helper：用默认参数构建常见的 window 读取，并暴露
`PointWindow` / `AnalogWindow` 轻量 view，以及 frame-major -> point-major 重排。

Analog 值以 channel-major 的 `(C, N)` 存储，其中 `N = n_frames * n_analog_by_frame`。
对于更偏好 frame-major 索引的消费者，`sqzc3d_easy.h` 提供一个非连续（strided）的 `(T, C, S)` view helper。

### 4）快速接入（selection + shape 假设）

- 对于一次性集成，建议从 C++ easy helper（`ReadPointsWindow`, `FrameMajorPointsView`）开始，以获得确定性的
  `n_frames x n_points x 3` 布局与显式的 `[frame][point]` 有效性。
- 对于生产级 binding，直接使用 C API，并保持 `sqzc3d_points_view_*` / `sqzc3d_analogs_view_*` 的显式性。

---

## 数据模型一览

- **Open**  
  `sqzc3d_open_file` / `sqzc3d_open_memory`（memory-open 当前会先写临时文件再解析）
- **Build**  
  `sqzc3d_build_chunks`
- **Query**  
  元数据 API + label/index helper + frame/point/channel view，以及可选的 `type_group_*` 元数据。
- **Type groups**（如存在）
  - `n_type_groups`
  - `type_group_names`（group 标签）
  - `type_group_starts`（`n_type_groups + 1` 的 prefix offset）
  - `type_group_indices`（按 `point_labels` 顺序的扁平点索引列表）
- Easy 层 shape 契约：点数据以 `FrameMajor PointWindow` 返回
  （`n_frames x n_points x 3`），其中 `frame_stride = n_points * 3`，`point_stride = 3`；valid mask 为 `[n_frames x n_points]`。
- **Persist/load**  
  `sqzc3d_export_bundle` / `sqzc3d_load_bundle`

公开结构体：
- `sqzc3d_open_opt_t`
- `sqzc3d_build_opt_t`
- `sqzc3d_chunk_t`
- `sqzc3d_points_view_t`
- `sqzc3d_analogs_view_t`

返回值约定采用 C 风格整数。状态常量见 `include/sqzc3d_types.h`。

---

## 特性开关与能力

```c
sqzc3d_get_features();
```

能力位在 `include/sqzc3d.h` 中定义：
- `SQZC3D_FEATURE_OPEN_FILE`
- `SQZC3D_FEATURE_OPEN_MEMORY`
- `SQZC3D_FEATURE_BUILD_CHUNKS`
- `SQZC3D_FEATURE_BUNDLE`
- `SQZC3D_FEATURE_ANALOG`

可据此在运行时对 `ON/OFF` 构建做自适应处理。

---

## 验证

- `sqzc3d_load_bundle_with_options` 支持 strict mode。
- `samples/*` 提供 smoke 测试：
  - `bench_sqzc3d`
  - `bench_ezc3d`
  - `bench_sqzc3d_stream`
  - `c3dinfo_sqzc3d`
  - `export_sqzc3d_bundle`
  - `verify_correctness_matrix_sqzc3d`
  - `easy_window_sqzc3d`

---

## 文档

- 公共 API 详情：`docs/API.md`
- 构建与用法说明：本文件
- 开发里程碑：`PLAN.md`
- 依赖与声明：`DEPENDENCIES.md` / `NOTICE`

---

## Python

`sqzc3d` 提供一个基于 `pybind11` 的轻量 Python API。
该包提供 `Decoder` + `Chunk` API，面向终端/分析类工作流。

### 安装

`pip install sqzc3d`

### 最小用法

```python
import sqzc3d

dec = sqzc3d.Decoder("trial.c3d", label_norm=sqzc3d.SQZC3D_LABEL_NORM_TRIM)
chunk = dec.read(
    start_frame=0,
    frame_count=-1,
    points=[0, 2],      # marker index mode
    analogs=["EMG1", "EMG2"]  # label mode is supported too
)

pts, pts_valid = chunk.points()  # default: copy=True, returns (values, valid)
ana, ana_valid = chunk.analogs(layout="CN")  # default analog layout is channel-major (C, N)
ana_tm, ana_tm_valid = chunk.analogs(selector=[0, 1], layout="tcs")  # frame-major view helper

print(chunk.meta["n_frames"], len(chunk.meta["point_labels"]))
```

### 构建说明

- 本包使用 `scikit-build-core` + `pybind11` 构建，并发布 `cp39`..`cp313` wheels。
- 相关配置位于 `pyproject.toml` 与 `.github/workflows/pypi.yml`。

---

## 许可证

`Squeezed C3D (sqzc3d)` 使用 **MIT** 许可证发布。
`ezc3d` 上游许可证为 **MIT**。

## 许可证与第三方声明

依赖/许可证说明见 `LICENSE` 与 `NOTICE`，构建依赖见 `DEPENDENCIES.md`。

