# Benchmarks

This page contains benchmark methodology and representative results.

Definition: `speedup_x = ezc3d / sqzc3d` (higher is better for `sqzc3d`, including the memory ratio).

## What is measured

- **Chunk materialize**: build a compact, contiguous `double` buffer `[frame][point][3]` (+ valid mask).
- **Access patterns**: copy/extract frame & window outputs, marker-trajectory access, reorder (frame-major -> point-major).
- **Peak memory**: avoid a full object graph; keep only needed arrays.

Bench method: fully load a C3D file, then measure access patterns on each library's **native loaded representation**.
For `sqzc3d`, the native representation is the chunk's contiguous frame-major array; for `ezc3d`, it is
`ezc3d::c3d`'s in-memory frame/point containers.

Notes:

- `bench_sqzc3d` and the Python `sqzc3d` bench disable analog reads to focus on point materialize + access patterns.
- `ezc3d` materializes full in-memory structures (including analogs if present).

## Reproduce

```bash
# C++ (build with -DSQZC3D_BUILD_EXAMPLES=ON)
<build_dir>/bench_sqzc3d <file.c3d> <repeat>
<build_dir>/bench_ezc3d  <file.c3d> <repeat>

# Streaming (sqzc3d-only)
<build_dir>/bench_sqzc3d_stream <file.c3d> 1

# Python
python samples/bench/bench_python.py <file.c3d> --lib sqzc3d --repeat <repeat>
python samples/bench/bench_python.py <file.c3d> --lib ezc3d  --repeat <repeat>
```

On multi-config generators (Visual Studio), binaries may be under `<build_dir>/Release/`.

## Materialize mode

### C++ (native)

PFERD (117.96 MB, frames=55,844, points=132, repeat=1):

| Metric | sqzc3d | ezc3d | `speedup_x` |
| --- | ---: | ---: | ---: |
| `load_ms` | 218.023 | 2481.406 | 11.4x |
| `frame_copy_us_kall` | 0.100 | 2.204 | 22.0x |
| `window_copy_us_T256_kall` | 10.375 | 154.118 | 14.9x |
| `peak_rss_mb` | 182.398 | 1011.125 | 5.5x |

Small (DOG, 4.23 MB, frames=4,634, points=57, repeat=10):

| Metric | sqzc3d | ezc3d | `speedup_x` |
| --- | ---: | ---: | ---: |
| `load_ms` | 10.979 | 98.711 | 9.0x |
| `frame_copy_us_kall` | 0.013 | 0.743 | 57.2x |
| `window_copy_us_T256_kall` | 2.544 | 54.254 | 21.3x |
| `peak_rss_mb` | 12.031 | 42.070 | 3.5x |

### Python

PFERD (117.96 MB, frames=55,844, points=132, repeat=1, `sqzc3d` v0.3.2 (ABI 3), `ezc3d` v1.6.0):

| Metric | sqzc3d | ezc3d | `speedup_x` |
| --- | ---: | ---: | ---: |
| `load_ms` | 197.913 | 2924.975 | 14.8x |
| `frame_copy_us_kall` | 1.234 | 4.594 | 3.7x |
| `window_copy_us_T256_kall` | 13.070 | 159.610 | 12.2x |
| `peak_rss_mb` | 209.617 | 1373.492 | 6.6x |

Small (DOG, 4.23 MB, frames=4,634, points=57, repeat=5, `sqzc3d` v0.3.2 (ABI 3), `ezc3d` v1.6.0):

| Metric | sqzc3d | ezc3d | `speedup_x` |
| --- | ---: | ---: | ---: |
| `load_ms` | 8.975 | 116.915 | 13.0x |
| `frame_copy_us_kall` | 0.809 | 1.504 | 1.9x |
| `window_copy_us_T256_kall` | 3.983 | 42.041 | 10.6x |
| `peak_rss_mb` | 44.789 | 130.855 | 2.9x |

## Streaming mode (sqzc3d-only, low memory)

Example (PFERD, repeat=1):

| Metric | sqzc3d stream |
| --- | ---: |
| `open_ms` | 1.289 |
| `peak_rss_delta_mb` | 2.762 |
| `read_window_ms_T256_kall` | 0.532 |
| `read_window_ms_T256_k32` | 0.889 |

