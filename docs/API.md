# Squeezed C3D (`sqzc3d`) API Reference

Applies to `sqzc3d` v0.3.x (ABI `SQZC3D_ABI_VERSION=3`).

## Headers

- `include/sqzc3d.h`: public C API
- `include/sqzc3d_types.h`: shared scalar and status types
- `include/sqzc3d_easy.h`: lightweight C++ convenience helpers

## Initialization helpers

| API | Purpose |
| --- | --- |
| `sqzc3d_default_open_opt(sqzc3d_open_opt_t*)` | Fill `sqzc3d_open_opt_t` with safe defaults. |
| `sqzc3d_default_build_opt(sqzc3d_build_opt_t*)` | Fill `sqzc3d_build_opt_t` with safe defaults. |
| `sqzc3d_apply_preset_stream_frame_all(sqzc3d_build_opt_t*)` | Preset for streaming full-frame point reads. |
| `sqzc3d_apply_preset_stream_frame_sel(sqzc3d_build_opt_t*)` | Preset for streaming with explicit user point selection. |
| `sqzc3d_apply_preset_window_analysis(sqzc3d_build_opt_t*)` | Preset for window analysis with residual gate default. |
| `sqzc3d_apply_preset_interpolation_ready(sqzc3d_build_opt_t*)` | Preset for interpolation-friendly window reads. |
| `sqzc3d_default_bundle_load_opt(sqzc3d_bundle_load_opt_t*)` | Fill bundle-load options with safe defaults. |
| `sqzc3d_default_error_detail(sqzc3d_error_detail_t*)` | Fill an error-detail struct with zeros/defaults. |
| `sqzc3d_version()` | Return the semantic version string. |
| `sqzc3d_abi_version()` | Return the ABI version integer. |

Default behavior notes:
- Option structs must set `struct_size == sizeof(struct)`; use `sqzc3d_default_*_opt(...)` to initialize.
- `sqzc3d_default_build_opt(...)` materializes analogs by default (`analog_enable = sqzc3d_ANALOG_EN_ON`).
- When `analog_enable = sqzc3d_ANALOG_EN_AUTO`, `sqzc3d_build_chunks` enforces `analog_size_soft_limit_bytes`
  (default `500 MiB`) and fails with `sqzc3d_STATUS_INVALID_ARGUMENT` if the projected analog payload exceeds the limit.

## Life-cycle

| API | Purpose |
| --- | --- |
| `sqzc3d_open_file` | Open C3D from file path. |
| `sqzc3d_open_memory` | Open C3D from memory buffer. |
| `sqzc3d_close_dec` | Release decoder handle. |
| `sqzc3d_last_error` / `sqzc3d_last_error_detail` | Retrieve last API error text or structured detail. |

Notes:
- In `sqzc3d_open_memory`, current implementation materializes to a temporary file first, then reuses `open_file`.
  Upstream integrations should treat this as API compatibility, not zero-copy yet.
- `sqzc3d_last_error(NULL)` and `sqzc3d_last_error_detail(NULL, ...)` report the last error on the current thread.
  This is useful when `sqzc3d_open_file` / `sqzc3d_open_memory` fails before returning a decoder handle.

> In `SQZC3D_WITH_EZC3D=OFF`, `sqzc3d_open_file`, `sqzc3d_open_memory`, `sqzc3d_build_chunks` return
> `sqzc3d_STATUS_NOT_IMPLEMENTED` and keep `sqzc3d_get_features()` authoritative.

## Chunk building and query

| API | Purpose |
| --- | --- |
| `sqzc3d_build_chunks` | Parse selected frame/point/analog ranges into a chunk object. |
| `sqzc3d_free_chunk` | Release chunk resources. |
| `sqzc3d_chunk_num_frames` / `sqzc3d_chunk_num_points` / `sqzc3d_chunk_num_scalar` | Access chunk shape. |
| `sqzc3d_chunk_point_indices_total` | Optional chunk-local -> source-total point index mapping (when available). |
| `sqzc3d_point_indices_for_labels` / `sqzc3d_analog_indices_for_labels` | Map labels to indices. |
| `sqzc3d_points_view_frames` / `sqzc3d_points_view_points` | Build point views by frame or index list. |
| `sqzc3d_analogs_view_samples` / `sqzc3d_analogs_view_channels` | Build analog views by sample range or channel list. |

## Bundle persistence

| API | Purpose |
| --- | --- |
| `sqzc3d_export_bundle` | Write `meta.json` + `data.bin` or single-file bundle. |
| `sqzc3d_load_bundle` | Load persisted bundle into chunk. |
| `sqzc3d_load_bundle_with_options` | Strict-load variant with strict flag. |

### Type-group metadata and default layout

- `chunk->type_group_names`: group names (length `chunk->n_type_groups`).
- `chunk->type_group_starts`: prefix-sum offsets length `n_type_groups + 1`.
- `chunk->type_group_indices`: flattened group indices in the **chunk-local** point index space `[0..n_points)`;
  `indices[type_group_starts[i]..type_group_starts[i+1])` are indices into `chunk->point_labels` for `type_group_names[i]`.
- Optional: `sqzc3d_chunk_point_indices_total()` provides a chunk-local -> source-total mapping when available.

In v0.x the default points layout is fixed:

- `points_layout = sqzc3d_POINTS_LAYOUT_FRAME_MAJOR`
- `points_pack = sqzc3d_POINTS_PACK_AOS_XYZ_VALID`
- Default easy layer contracts: `PointWindow` is frame-major, AoS XYZ and `valid` is frame-major [T][K].
  - `points_xyz_shape = [n_frames][n_points][3]` (contiguous)
  - `points_xyz_stride = [n_points*3, 3, 1]`
  - `points_valid_shape = [n_frames][n_points]` (contiguous)
  - `points_valid_stride = [n_points, 1]`
- Default analog layout in v0.x is channel-major `(C, N)`:
  - `analog_shape = [n_analogs][n_frames*n_analog_by_frame]` (contiguous)
  - `analog_stride = [n_frames*n_analog_by_frame, 1]`

## Feature and capability

| API | Purpose |
| --- | --- |
| `sqzc3d_get_features` | Read runtime availability bits. |

In practice this is the first call to decide whether C3D parsing and analog APIs are enabled before invoking feature-gated paths.

## Easy API

Header-only helpers in `include/sqzc3d_easy.h`:

- `sqzc3d::MakeFrameWindowBuildOpt` / `sqzc3d::ReadPointsWindow`
  - Build a windowed frame-range chunk with a chosen preset.
- `sqzc3d::ReadPointsWindowByLabels`
  - Label-driven windowing.
- `sqzc3d::FrameMajorPointsView`
  - Convert chunk pointer to frame-major AoS view with `[frame][point][xyz]` shape.
- `sqzc3d::AnalogSamplesView`
  - Convert chunk pointer to analog view with channel-major `(C, N)` layout.
- `sqzc3d::FrameMajorAnalogViewTCS`
  - Provide a non-contiguous (strided) frame-major view `(T, C, S)` over the underlying `(C, N)` storage.
- `sqzc3d::PointIndicesFromTypeGroups`
  - Convert type-group names to a flat **chunk-local** point index list.
  - Default filter semantics: missing TYPE_GROUPS metadata => no-op (all points); missing group name => empty set.
- `sqzc3d::PointIndicesFromTypeGroupsStrict`
  - Strict variant with explicit error signaling (returns a `sqzc3d_STATUS_*` code).
- `sqzc3d::ChunkQuery` / `sqzc3d::ChunkRecipe`
  - Minimal AND-only chunk-local filtering helpers (bound to a chunk; recipes are reusable).
- `sqzc3d::ReorderFrameMajorToPointMajor`
  - Reorder helper for consumers requiring point-major layout.

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
- In v0.x, there is no residual/camera-mask public payload; if needed downstream, add via request/extension.

## Python API (pybind11)

High-level Python exports:

- `sqzc3d.version()`
- `sqzc3d.abi_version()`
- `sqzc3d.features()`
- `sqzc3d.read(...) -> sqzc3d.View` (easy layer)
- `sqzc3d.View` (easy layer)
- `sqzc3d.Recipe` (alias of `sqzc3d.ChunkRecipe`)
- `sqzc3d.Decoder`
- `sqzc3d.Chunk`
- `sqzc3d.ChunkQuery` / `sqzc3d.ChunkRecipe` (AND-only chunk-local filtering)
- `sqzc3d.type_group_indices(chunk, group_names, strict=False)`
- `sqzc3d.load_bundle(path: str, strict: bool = True)`

### Easy layer (recommended)

`read`:

- `sqzc3d.read(source, *, start_frame=0, frame_count=-1, points=None, analogs=None, analog_range=None, label_norm=..., recipe=None) -> View`

Selector semantics (Python):

- `None` = default (ALL)
- `[]` = empty selection

`View`:

- Selection state (labels-first):
  - `view.point_labels` (`None | list[str]`)
  - `view.analog_labels` (`None | list[str]`)
  - `view.type_groups` (`list[str]`)
- Data (properties):
  - `view.points` / `view.points_valid`
  - `view.analogs` / `view.analogs_valid`
- Label accessors:
  - `view.point["LANK"]` / `view.point_valid["LANK"]`
  - `view.analog["EMG1"]` / `view.analog_valid["EMG1"]`
- Metadata:
  - `view.meta` (flat dict)
  - `view.meta_tree` (EZ parameter tree; only when source is a `.c3d` file path and `SQZC3D_WITH_EZC3D=ON`)
- Advanced escape hatch:
  - `view._chunk` (pybind `Chunk`; indices/masks/etc are considered advanced)

Notes:

- The easy layer is **labels-only** by design. If you already have indices, use the core API and slice arrays directly.

`Decoder`:

- `Decoder(path, label_norm=sqzc3d.SQZC3D_LABEL_NORM_EXACT)`
- `Decoder.read(start_frame=0, frame_count=-1, points=None, analogs=None, analog_range=None)`
- `Decoder.close()`
- `Decoder.source_path` (read-only)
- `Decoder.closed` (read-only bool)

`Chunk`:

- `chunk.points(selector=None, copy=True) -> (values, valid)`
- `chunk.analogs(selector=None, layout="CN", copy=True) -> (values, valid)`
  - `layout="tcs"` returns a non-contiguous frame-major view `(T, C, S)` over channel-major storage.
- `chunk.meta` (dict)
- `chunk.meta_tree` (dict, full parsed EZ metadata tree when source file path is available)
- `chunk.source_path` (read-only)

Python payload semantics:

- `points` values: `float64`, default frame-major shape `(T, P, 3)`, valid mask `(T, P)` with dtype `uint8`.
- `analogs` values:
  - `layout="CN"` default: `(C, N)` where `N = n_frames * n_analog_by_frame`
  - `layout="tcs"`: `(T, C, S)` non-contiguous view helper
- selector:
  - `None` means all
  - `[]` means empty
  - `int` or list/tuple of ints for index selection
  - `str` or list/tuple of str for label selection
- non-contiguous selection requires `copy=True`; `copy=False` currently raises `RuntimeError` and avoids hidden conversions.
