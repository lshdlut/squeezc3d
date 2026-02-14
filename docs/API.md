# API Reference

## Headers

- `include/sqzc3d.h`: public C API
- `include/sqzc3d_types.h`: shared scalar and status types

## Initialization helpers

| API | Purpose |
| --- | --- |
| `sqzc3d_default_open_opt(sqzc3d_open_opt_t*)` | Fill `sqzc3d_open_opt_t` with safe defaults. |
| `sqzc3d_default_build_opt(sqzc3d_build_opt_t*)` | Fill `sqzc3d_build_opt_t` with safe defaults. |
| `sqzc3d_default_bundle_load_opt(sqzc3d_bundle_load_opt_t*)` | Fill bundle-load options with safe defaults. |
| `sqzc3d_default_error_detail(sqzc3d_error_detail_t*)` | Fill an error-detail struct with zeros/defaults. |

## Life-cycle

| API | Purpose |
| --- | --- |
| `sqzc3d_open_file` | Open C3D from file path. |
| `sqzc3d_open_memory` | Open C3D from memory buffer. |
| `sqzc3d_close_dec` | Release decoder handle. |
| `sqzc3d_last_error` / `sqzc3d_last_error_detail` | Retrieve last API error text or structured detail. |

## Chunk building and query

| API | Purpose |
| --- | --- |
| `sqzc3d_build_chunks` | Parse selected frame/point/analog ranges into a chunk object. |
| `sqzc3d_free_chunk` | Release chunk resources. |
| `sqzc3d_chunk_num_frames` / `sqzc3d_chunk_num_points` / `sqzc3d_chunk_num_scalar` | Access chunk shape. |
| `sqzc3d_point_indices_for_labels` / `sqzc3d_analog_indices_for_labels` | Map labels to indices. |
| `sqzc3d_points_view_frames` / `sqzc3d_points_view_points` | Build point views by frame or index list. |
| `sqzc3d_analogs_view_samples` / `sqzc3d_analogs_view_channels` | Build analog views by sample range or channel list. |

## Bundle persistence

| API | Purpose |
| --- | --- |
| `sqzc3d_export_bundle` | Write `meta.json` + `data.bin` or single-file bundle. |
| `sqzc3d_load_bundle` | Load persisted bundle into chunk. |
| `sqzc3d_load_bundle_with_options` | Strict-load variant with strict flag. |

## Feature and capability

| API | Purpose |
| --- | --- |
| `sqzc3d_get_features` | Read runtime availability bits. |

## Status and enums

- Return codes are C-style ints from `sqzc3d_types.h`:
  - `sqzc3d_STATUS_SUCCESS`
  - `sqzc3d_STATUS_INVALID_ARGUMENT`
  - `sqzc3d_STATUS_DIMENSION_MISMATCH`
  - `sqzc3d_STATUS_NOT_IMPLEMENTED`
  - `sqzc3d_STATUS_INTERNAL_ERROR`
- Important enum families:
  - input type: `sqzc3d_FILE`, `sqzc3d_MEMORY`
  - selection mode: indices/labels/all
  - read policy: `AUTO`, `DENSE`, `SPARSE`
  - valid policy: `sqzc3d_VALID_POLICY_FINITE_XYZ`

## Notes

- API is C89-compatible (`extern "C"` available for C++).
- All non-`const` out-parameters are expected writable by caller.
- Resources allocated by this library must be released with corresponding `free` APIs above.
