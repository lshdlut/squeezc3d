# sqzc3d

[![CI](https://github.com/lshdlut/squeezc3d/actions/workflows/ci.yml/badge.svg)](https://github.com/lshdlut/squeezc3d/actions/workflows/ci.yml)

`sqzc3d` is a small C/C++ library for:
- parsing C3D point/analog data,
- building compact chunk structures,
- querying selected markers/channels by index/label,
- exporting/importing persisted bundle files.

It is designed as a pure dependency for higher-level projects (for example `sikc`) and keeps a minimal runtime API surface.

---

## Why sqzc3d

- **Small surface**: C API with a small set of stable entry points and explicit struct-size based options.
- **Feature split**:
  - `SQZC3D_WITH_EZC3D=ON` (default): full C3D parsing path.
  - `SQZC3D_WITH_EZC3D=OFF`: bundle-only path, no ezc3d dependency.
- **Runtime-friendly**: chunk/view access avoids full object retention.
- **Practical robustness**: selection by index or label, optional strict bundle checks, diagnostics.

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
- `SQZC3D_BUILD_EXAMPLES` (`ON|OFF`, default `ON`)  
  build CLI samples.
- `SQZC3D_EZC3D_GIT_REPOSITORY` / `SQZC3D_EZC3D_GIT_TAG`  
  control fetch source when `SQZC3D_FETCH_EZC3D=ON`.

> Compatibility note: legacy `sqzc3d_WITH_EZC3D` is tolerated for CMake compatibility and mapped to the canonical `SQZC3D_WITH_EZC3D`.

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

---

## Data model at a glance

- **Open**  
  `sqzc3d_open_file` / `sqzc3d_open_memory`
- **Build**  
  `sqzc3d_build_chunks`
- **Query**  
  metadata APIs + label/index helpers + frame/point/channel views
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
  - `c3dinfo_sqzc3d`
  - `export_sqzc3d_bundle`
  - `load_sqzc3d_bundle`
  - `bundle_roundtrip_sqzc3d` (requires `SQZC3D_WITH_EZC3D=ON`)
  - `verify_correctness_matrix_sqzc3d`

---

## Documentation

- Public API details: `docs/API.md`
- Build and usage notes: this file
- Development milestones: `PLAN.md`
- Dependencies and notices: `DEPENDENCIES.md` / `NOTICE`

---

## License & third-party notices

See `NOTICE` for ezc3d dependency notes and `DEPENDENCIES.md` for build requirements.
