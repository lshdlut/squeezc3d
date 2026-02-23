# Squeezed C3D (`sqzc3d`)

[![CI](https://github.com/lshdlut/squeezc3d/actions/workflows/ci.yml/badge.svg)](https://github.com/lshdlut/squeezc3d/actions/workflows/ci.yml)

`Squeezed C3D` (`sqzc3d`) is a small C/C++ library for:
- parsing C3D point/analog data,
- building compact chunk structures,
- querying selected markers/channels by index/label,
- exporting/importing persisted bundle files.

It is designed as a pure dependency for higher-level projects (for example `sikc`) and keeps a minimal runtime API surface.

---

## Why Squeezed C3D

### Why Squeezed C3D (benchmark intent)

Only performance-relevant signals are shown:

- **Chunk materialize**: build a compact, contiguous `double` buffer `[frame][point][3]` (+ valid mask).
- **Access patterns**: copy/extract frame & window outputs, marker-trajectory access, reorder (frame-major -> point-major).
- **Peak memory**: avoid a full object graph; keep only needed arrays.

Bench method: fully load a C3D file, then measure access patterns on each library's **native loaded representation**.
For `sqzc3d`, the native representation is the chunk's contiguous frame-major array; for `ezc3d`, it is
`ezc3d::c3d`'s in-memory frame/point containers. Numbers below are from the C++ sample benches (repeat=1):

- `bench_sqzc3d <file.c3d> 1`
- `bench_ezc3d <file.c3d> 1`

### Load & memory (C++)

| Dataset | Frames | Points | sqzc3d `load_ms` | ezc3d `load_ms` | `load_speedup_x` | sqzc3d `peak_rss_mb` | ezc3d `peak_rss_mb` | `rss_ratio_x` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| PFERD (117.96 MB) | 55,844 | 132 | 171.385 | 3,717.442 | 21.7 | 182.090 | 1,019.414 | 5.6 |

`load_speedup_x = ezc3d / sqzc3d`, `rss_ratio_x = ezc3d / sqzc3d` (higher is better for `sqzc3d`).

### Access patterns (native, after load; PFERD, `T=256`)

All numbers are **per-operation milliseconds** unless noted.

| Metric | sqzc3d | ezc3d | `speedup_x` |
| --- | ---: | ---: | ---: |
| `frame_view_ns_kall` (ns/op) | 4.196 | 76.054 | 18.1x |
| `frame_copy_ms_kall` | 0.000096 | 0.002589 | 27.0x |
| `window_read_ms_T256_kall` | 0.009656 | 0.143652 | 14.9x |
| `traj_strided_ms_T256_kall` | 0.075748 | 0.228896 | 3.0x |
| `traj_strided_ms_Tfull_k1` | 0.132440 | 3.362906 | 25.4x |
| `reorder_ms_T256_kall` | 0.132432 | 0.167740 | 1.27x |
| `sel_apply_ms_T256_k32` | 0.005532 | 0.046642 | 8.4x |

`speedup_x = ezc3d / sqzc3d` (higher is better for `sqzc3d`).

### Streaming mode (sqzc3d-only, low memory)

Optional extreme low-memory mode that reads from the file on demand (e.g. WASM VFS):

- `bench_sqzc3d_stream <file.c3d> 1`

Example (PFERD, repeat=1):

| Metric | sqzc3d stream |
| --- | ---: |
| `open_ms` | 1.289 |
| `peak_rss_delta_mb` | 2.762 |
| `read_window_ms_T256_kall` | 0.532 |
| `read_window_ms_T256_k32` | 0.889 |

### Feature profile

- **Core + easy split**: stable C API (`sqzc3d.h`) plus ergonomic C++ helper layer (`sqzc3d_easy.h`).
- **Preset-first workflow**: shared presets for common read patterns.
- **Chunk-first runtime contracts**: frame-major point arrays + explicit valid mask.
- **Type-group aware filtering**: `type_group_*` metadata for marker set control.
- **Build split**: `SQZC3D_WITH_EZC3D=OFF` still supports bundle-only runtime.

### New in 0.2

- Preset builders for common workflows (`stream_frame_all`, `stream_frame_sel`, `window_analysis`, `interpolation_ready`).
- Dual-layer API:
  - **C layer** for stable runtime ABI (`sqzc3d.h`).
  - **C++ easy layer** for ergonomic one-shot window reads (`sqzc3d_easy.h`).
- Type-group metadata in chunk for marker-group-aware workflows.

---

## Build

```bash
cmake -S . -B build
cmake --build build --config Release --parallel
```

### Common options

- `SQZC3D_WITH_EZC3D` (`ON|OFF`, default `ON`)  
  enable/disable the C3D parser feature.
- `SQZC3D_FETCH_EZC3D` (`ON|OFF`, default `ON`)  
  auto-fetch ezc3d when not found in the current toolchain.
- `SQZC3D_APPLY_EZC3D_PATCHES` (`ON|OFF`, default `ON` on Emscripten/WASM, `OFF` otherwise)  
  apply local compatibility patches to fetched ezc3d (see `cmake/patches/README.md`).
- `SQZC3D_BUILD_EXAMPLES` (`ON|OFF`, default `OFF`)  
  build CLI samples.
- `SQZC3D_EZC3D_GIT_REPOSITORY` / `SQZC3D_EZC3D_GIT_TAG`  
  control fetch source when `SQZC3D_FETCH_EZC3D=ON`.

> Compatibility note: legacy `sqzc3d_WITH_EZC3D` is tolerated for CMake compatibility and mapped to the canonical `SQZC3D_WITH_EZC3D`.

### Indexing model (chunk-local)

- All point indices exposed from a chunk (including `type_group_indices`) are in the **chunk-local** point index space `[0..n_points)`.
- Optional: `sqzc3d_chunk_point_indices_total()` (and Python `Chunk.point_indices_total`) provides a mapping from **chunk-local -> source-total** point indices when available.

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

### Runtime capability matrix

| Feature | ON | OFF |
| --- | --- | --- |
| C3D parsing (`open_file`/`open_memory`) | ✅ | ❌ |
| Chunk build (`build_chunks`) | ✅ | ❌ |
| Bundle export/load | ✅ | ✅ |
| Analog support | ✅ | ✅ |

Runtime availability is always queryable via `sqzc3d_get_features()`.

### CMake usage (dependency)

```cmake
add_subdirectory(path/to/sqzc3d)
target_link_libraries(your_target PRIVATE sqzc3d)
```

---

## Quick start

### 1) Parse C3D and build chunks

```c
sqzc3d_default_open_opt(&open_opt);
sqzc3d_open_file(&dec, path, &open_opt);
sqzc3d_default_build_opt(&build_opt);
sqzc3d_build_chunks(dec, &build_opt, &chunk);

// use chunk metadata/queries/views
sqzc3d_free_chunk(chunk);
sqzc3d_close_dec(dec);
```

### 2) Load from bundle

```c
sqzc3d_load_bundle(bundle_path, &chunk);
sqzc3d_free_chunk(chunk);
```

All API contracts use plain integers and pointers, so this is usable from both C and C++ projects.

Quickly check runtime identity:

```c
printf("sqzc3d version=%s abi=%d\n", sqzc3d_version(), sqzc3d_abi_version());
```

### 3) C++ easy entry (`sqzc3d_easy.h`)

```c++
#include "sqzc3d_easy.h"

sqzc3d::ReadPointsWindow(dec, 0, 32, nullptr, 0, nullptr, &chunk);
const auto view = sqzc3d::FrameMajorPointsView(chunk);
```

`sqzc3d_easy.h` is a lightweight C++ helper that builds common window reads with defaults and exposes
`PointWindow` / `AnalogWindow` lightweight views plus frame-major -> point-major reorder.

Analog values are stored as channel-major `(C, N)` where `N = n_frames * n_analog_by_frame`.
For consumers who prefer frame-major indexing, `sqzc3d_easy.h` provides a non-contiguous (strided) `(T, C, S)` view helper.

### 4) Fast onboarding (selection + shape assumptions)

- For one-off integration, start from C++ easy helpers (`ReadPointsWindow`, `FrameMajorPointsView`) to get a deterministic
  `n_frames x n_points x 3` layout and explicit `[frame][point]` validity.
- For production bindings, use the C API directly and keep `sqzc3d_points_view_*` / `sqzc3d_analogs_view_*` explicit.

---

## Data model at a glance

- **Open**  
  `sqzc3d_open_file` / `sqzc3d_open_memory` (memory-open currently writes a temporary file before parsing)
- **Build**  
  `sqzc3d_build_chunks`
- **Query**  
  metadata APIs + label/index helpers + frame/point/channel views, optional `type_group_*` metadata.
- **Type groups** (if present)
  - `n_type_groups`
  - `type_group_names` (group labels)
  - `type_group_starts` (`n_type_groups + 1` prefix offsets)
  - `type_group_indices` (flat point-index list in `point_labels` order)
- Easy-layer shape contract: points are returned as `FrameMajor PointWindow`
  (`n_frames x n_points x 3`) with `frame_stride = n_points * 3`, `point_stride = 3`; valid mask is `[n_frames x n_points]`.
- **Persist/load**  
  `sqzc3d_export_bundle` / `sqzc3d_load_bundle`

Public structs:
- `sqzc3d_open_opt_t`
- `sqzc3d_build_opt_t`
- `sqzc3d_chunk_t`
- `sqzc3d_points_view_t`
- `sqzc3d_analogs_view_t`

Return value conventions are C-style integers. See `include/sqzc3d_types.h` for status constants.

---

## Feature flags and capabilities

```c
sqzc3d_get_features();
```

Capability bits are available in `include/sqzc3d.h`:
- `SQZC3D_FEATURE_OPEN_FILE`
- `SQZC3D_FEATURE_OPEN_MEMORY`
- `SQZC3D_FEATURE_BUILD_CHUNKS`
- `SQZC3D_FEATURE_BUNDLE`
- `SQZC3D_FEATURE_ANALOG`

Use this to adapt behavior for `ON/OFF` builds at runtime.

---

## Validation

- `sqzc3d_load_bundle_with_options` supports strict mode.
- `samples/*` provide smoke tests:
  - `bench_sqzc3d`
  - `bench_ezc3d`
  - `bench_sqzc3d_stream`
  - `c3dinfo_sqzc3d`
  - `export_sqzc3d_bundle`
  - `verify_correctness_matrix_sqzc3d`
  - `easy_window_sqzc3d`

---

## Documentation

- Public API details: `docs/API.md`
- Build and usage notes: this file
- Development milestones: `PLAN.md`
- Dependencies and notices: `DEPENDENCIES.md` / `NOTICE`

---

## Python

`sqzc3d` provides a lightweight Python API built on top of `pybind11`.
The package ships a `Decoder` + `Chunk` API intended for terminal/analysis workflows.

### Install

`pip install sqzc3d`

### Minimal usage

```python
import sqzc3d

dec = sqzc3d.Decoder("trial.c3d", preserve_raw_params=False, label_norm=sqzc3d.SQZC3D_LABEL_NORM_TRIM)
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

### Build notes

- The package is built with `scikit-build-core` + `pybind11` and publishes `cp39`..`cp313` wheels.
- This is configured in `pyproject.toml` and `.github/workflows/pypi.yml`.

---

## License

`Squeezed C3D (sqzc3d)` is released under **MIT**.
`ezc3d` upstream license is **MIT**.

## License & third-party notices

See `LICENSE` and `NOTICE` for dependency/license notes, `DEPENDENCIES.md` for build requirements.
