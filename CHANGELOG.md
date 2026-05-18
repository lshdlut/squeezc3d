# Changelog

## 0.4.0 - 2026-05-18

- Added first-class chunk residual payloads: `points_residual` and `residual_nscalar`.
- Added chunk build `target_unit` support for materialized `points_xyz`; residuals remain raw source-unit values.
- Made residual-gated validity an explicit policy while keeping the default finite-XYZ validity behavior.
- Added residual/unit metadata and schema version 4 bundle output while preserving schema version 3 legacy loading.
- Reworked `sqzc3d_open_memory` to use decoder-owned memory instead of temporary files.
- Documented borrowed view lifetimes for chunk-backed views and caller-provided gather indices.
- Expanded native, C++, bundle, parser, open-memory, and WASM verification coverage.
