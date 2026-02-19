# Squeezed C3D (`sqzc3d`) Development Plan

This file tracks completion state for public delivery.

## ✅ Completed

- `open_* -> build_chunks -> points/analogs/views` two-stage workflow.
- Selector support for points/analogs by index and label, including label normalization.
- AUTO/DENSE/SPARSE read policy (`sel_ratio`, `span`, buffer-threshold driven).
- Build-time feature split:
  - `SQZC3D_WITH_EZC3D=ON` for C3D parse path,
  - `SQZC3D_WITH_EZC3D=OFF` for bundle-only path.
- Bundle v1/v2 compatibility for both directory and single-file layouts.
- Strict bundle validation: schema, version, endianness, byte-size, checksum.
- `raw_params` persistence and replay.
- Public API versioning surface (`sqzc3d_version`, `sqzc3d_abi_version`) and version macros.
- Diagnostics path: `sqzc3d_last_error`, `sqzc3d_last_error_detail`, status/error enums.
- Capability introspection: `sqzc3d_get_features`.
- Python binding skeleton:
  - `pybind11` extension source in `python/bindings/sqzc3d_pybind.cpp`.
  - Runtime API exposed as `Decoder` / `Chunk`:
    - `Decoder.read`, `close`, `source_path`, `closed`
    - `Chunk.points`, `analogs`, `meta`, `meta_tree`
    - `load_bundle(path, strict=True)` and constants export.
  - Python package entry (`python/sqzc3d/__init__.py`) now exports API and constants instead of WIP placeholders.
- CI matrix + dependency/documentation scaffolding (`.github/workflows/ci.yml`, `DEPENDENCIES.md`, `NOTICE`).
- Local benchmark/perf noise cleanup hooks.
- C API presets for stream/frame-window usage.
- Easy-layer helpers are now available via `include/sqzc3d_easy.h` and `easy_window_sqzc3d`.
- Type-group metadata exposed to callers:
  - `n_type_groups`
  - `type_group_names`, `type_group_starts`, `type_group_indices`
- `README` includes direct `sqzc3d vs ezc3d` benchmark row.
- Public docs were reviewed end-to-end (`README.md`, `docs/API.md`) and aligned to current API boundaries.

## ✅ Notable non-blocking follow-ups

- Deep semantic regression for exotic C3D dialects (mask/residual variants, analog interpolation corners).
- Broader fixture matrix across heterogeneous motion datasets.

## 📦 Delivery readiness

- `README.md`: public usage and option documentation.
- `docs/API.md`: interface map for integration.
- `include/sqzc3d_easy.h` + `samples/tools/easy_window_sqzc3d.cpp` for low-friction easy workflows.
- CMake compatibility mapping for legacy `sqzc3d_WITH_EZC3D`.
- Samples and smoke tooling remain available for integration checks.
- `sqzc3d_open_memory` behavior is now explicitly documented as temp-file based compatibility path.
- CI/run matrix evidence and release notes should be captured before public handoff.
