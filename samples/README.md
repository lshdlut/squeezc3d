# Samples

This directory contains small CLI programs for:

- benchmarks (`samples/bench/`)
- basic tools (`samples/tools/`)
- validation / developer checks (`samples/verify/`)

Build them with:

```bash
cmake -S . -B build -DSQZC3D_BUILD_EXAMPLES=ON
cmake --build build --config Release --parallel
```

Programs:

- Benchmarks:
  - `bench_sqzc3d`
  - `bench_ezc3d`
  - `bench_sqzc3d_stream`
- Tools:
  - `c3dinfo_sqzc3d`
    - reads both `.c3d` and bundle (`.sqzc3d` or bundle directory)
  - `export_sqzc3d_bundle`
  - `easy_window_sqzc3d`
- Validation:
  - `verify_correctness_matrix_sqzc3d`
