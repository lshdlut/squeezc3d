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
- Diagnostics path: `sqzc3d_last_error`, `sqzc3d_last_error_detail`, status/error enums.
- Capability introspection: `sqzc3d_get_features`.
- CI matrix + dependency/documentation scaffolding (`.github/workflows/ci.yml`, `DEPENDENCIES.md`, `NOTICE`).
- Local benchmark/perf noise cleanup hooks.

## ✅ Notable non-blocking follow-ups

- Deep semantic regression for exotic C3D dialects (mask/residual variants, analog interpolation corners).
- Broader fixture matrix across heterogeneous motion datasets.

## 📦 Delivery readiness

- `README.md`: public usage and option documentation.
- `docs/API.md`: interface map for integration.
- CMake compatibility mapping for legacy `sqzc3d_WITH_EZC3D`.
- Samples and smoke tooling remain available for integration checks.
