# `sqzc3d` API 参考

适用于 `sqzc3d` v0.3.x（ABI `SQZC3D_ABI_VERSION=3`）。

## 头文件

- `include/sqzc3d.h`: public C API
- `include/sqzc3d_types.h`: shared scalar and status types
- `include/sqzc3d_easy.h`: lightweight C++ convenience helpers

## 初始化 helpers

| API | 用途 |
| --- | --- |
| `sqzc3d_default_open_opt(sqzc3d_open_opt_t*)` | 以安全默认值填充 `sqzc3d_open_opt_t`。 |
| `sqzc3d_default_build_opt(sqzc3d_build_opt_t*)` | 以安全默认值填充 `sqzc3d_build_opt_t`。 |
| `sqzc3d_apply_preset_stream_frame_all(sqzc3d_build_opt_t*)` | streaming 全帧 points 读取的 preset。 |
| `sqzc3d_apply_preset_stream_frame_sel(sqzc3d_build_opt_t*)` | streaming 且显式点选择的 preset。 |
| `sqzc3d_apply_preset_window_analysis(sqzc3d_build_opt_t*)` | window analysis preset（带 residual gate 默认值）。 |
| `sqzc3d_apply_preset_interpolation_ready(sqzc3d_build_opt_t*)` | interpolation 友好的 window 读取 preset。 |
| `sqzc3d_default_bundle_load_opt(sqzc3d_bundle_load_opt_t*)` | 以安全默认值填充 bundle-load options。 |
| `sqzc3d_default_error_detail(sqzc3d_error_detail_t*)` | 以零/默认填充 error-detail struct。 |
| `sqzc3d_version()` | 返回语义版本字符串。 |
| `sqzc3d_abi_version()` | 返回 ABI 版本整数。 |

默认行为备注：

- Option structs 必须设置 `struct_size == sizeof(struct)`；请用 `sqzc3d_default_*_opt(...)` 初始化。
- `sqzc3d_default_build_opt(...)` 默认会 materialize analogs（`analog_enable = sqzc3d_ANALOG_EN_ON`）。
- 当 `analog_enable = sqzc3d_ANALOG_EN_AUTO` 时，`sqzc3d_build_chunks` 会强制 `analog_size_soft_limit_bytes`
  （默认 `500 MiB`），若预计 analog payload 超过该限制则以 `sqzc3d_STATUS_INVALID_ARGUMENT` 失败。

## 生命周期

| API | 用途 |
| --- | --- |
| `sqzc3d_open_file` | 从文件路径打开 C3D。 |
| `sqzc3d_open_memory` | 从内存 buffer 打开 C3D。 |
| `sqzc3d_close_dec` | 释放 decoder handle。 |
| `sqzc3d_last_error` / `sqzc3d_last_error_detail` | 获取最近一次 API 错误文本或结构化详情。 |

备注：

- 当前 `sqzc3d_open_memory` 的实现会先 materialize 到临时文件，再复用 `open_file`。
  上游集成应将其视作 API 兼容性（目前并非真正 zero-copy）。
- `sqzc3d_last_error(NULL)` 与 `sqzc3d_last_error_detail(NULL, ...)` 会返回当前线程的最近一次错误。
  这对 `sqzc3d_open_file` / `sqzc3d_open_memory` 在返回 decoder handle 之前就失败的情况非常有用。

> 在 `SQZC3D_WITH_EZC3D=OFF` 时，`sqzc3d_open_file`、`sqzc3d_open_memory`、`sqzc3d_build_chunks` 会返回
> `sqzc3d_STATUS_NOT_IMPLEMENTED`；以 `sqzc3d_get_features()` 为准。

## Chunk 构建与查询

| API | 用途 |
| --- | --- |
| `sqzc3d_build_chunks` | 将指定 frame/point/analog 范围解析并 materialize 为一个 chunk 对象。 |
| `sqzc3d_free_chunk` | 释放 chunk 资源。 |
| `sqzc3d_chunk_num_frames` / `sqzc3d_chunk_num_points` / `sqzc3d_chunk_num_scalar` | 获取 chunk shape。 |
| `sqzc3d_chunk_point_indices_total` | 可选的 chunk-local -> source-total point index 映射（若可用）。 |
| `sqzc3d_chunk_meta_tree_json` | 可选的 `meta_tree` 快照（UTF-8 JSON；若可用）。 |
| `sqzc3d_chunk_time_axis` | 可选的时间轴元数据（源 first/last frame、采样率、窗口起点）。 |
| `sqzc3d_point_indices_for_labels` / `sqzc3d_analog_indices_for_labels` | labels -> indices 映射。 |
| `sqzc3d_points_view_frames` / `sqzc3d_points_view_points` | 按 frame 或 index list 构建 points views。 |
| `sqzc3d_analogs_view_samples` / `sqzc3d_analogs_view_channels` | 按 sample range 或 channel list 构建 analog views。 |

## Bundle 持久化

| API | 用途 |
| --- | --- |
| `sqzc3d_export_bundle` | 写出 `meta.json` + `data.bin` 或 single-file bundle。 |
| `sqzc3d_load_bundle` | 从持久化 bundle 加载为 chunk。 |
| `sqzc3d_load_bundle_with_options` | 带 strict flag 的严格加载变体。 |

### Type-group metadata and default layout

- `chunk->type_group_names`: group names（长度 `chunk->n_type_groups`）。
- `chunk->type_group_starts`: prefix-sum offsets，长度 `n_type_groups + 1`。
- `chunk->type_group_indices`: flattened group indices，位于 **chunk-local** point index space `[0..n_points)`；
  `indices[type_group_starts[i]..type_group_starts[i+1])` 是 `type_group_names[i]` 对应的 indices（索引到 `chunk->point_labels`）。
- Optional：`sqzc3d_chunk_point_indices_total()` 在可用时提供 chunk-local -> source-total 映射。

在 v0.x 中，默认 points layout 固定为：

- `points_layout = sqzc3d_POINTS_LAYOUT_FRAME_MAJOR`
- `points_pack = sqzc3d_POINTS_PACK_AOS_XYZ_VALID`
- 默认 easy 层契约：`PointWindow` 为 frame-major，AoS XYZ；`valid` 为 frame-major [T][K]。
  - `points_xyz_shape = [n_frames][n_points][3]`（contiguous）
  - `points_xyz_stride = [n_points*3, 3, 1]`
  - `points_valid_shape = [n_frames][n_points]`（contiguous）
  - `points_valid_stride = [n_points, 1]`
- v0.x 默认 analog layout 为 channel-major `(C, N)`：
  - `analog_shape = [n_analogs][n_frames*n_analog_by_frame]`（contiguous）
  - `analog_stride = [n_frames*n_analog_by_frame, 1]`

## 特性与能力

| API | 用途 |
| --- | --- |
| `sqzc3d_get_features` | 读取运行时可用性 bits。 |

在实践中，这通常是第一步调用：在进入 feature-gated 路径之前，用它来判断当前 build 是否启用了 C3D 解析与 analog APIs。

## Easy API

`include/sqzc3d_easy.h` 中的 header-only helpers：

- `sqzc3d::MakeFrameWindowBuildOpt` / `sqzc3d::ReadPointsWindow`
  - 以选定的 preset 构建一个带 window 的 frame-range chunk。
- `sqzc3d::ReadPointsWindowByLabels`
  - 基于 labels 的 windowing。
- `sqzc3d::FrameMajorPointsView`
  - 将 chunk 指针转换为 frame-major AoS view，shape 为 `[frame][point][xyz]`。
- `sqzc3d::AnalogSamplesView`
  - 将 chunk 指针转换为 analog view，采用 channel-major `(C, N)` 布局。
- `sqzc3d::FrameMajorAnalogViewTCS`
  - 在底层 `(C, N)` 存储之上提供非连续（strided）的 frame-major view `(T, C, S)`。
- `sqzc3d::PointIndicesFromTypeGroups`
  - 将 type-group names 转换为一个扁平的 **chunk-local** point index list。
  - 默认过滤语义：缺失 TYPE_GROUPS 元数据 => no-op（all points）；group name 缺失 => 空集。
- `sqzc3d::PointIndicesFromTypeGroupsStrict`
  - 严格变体，显式报错（返回 `sqzc3d_STATUS_*`）。
- `sqzc3d::ChunkQuery` / `sqzc3d::ChunkRecipe`
  - 最小 AND-only 的 chunk-local filtering helpers（Query 绑定 chunk；Recipe 可复用）。
- `sqzc3d::ReorderFrameMajorToPointMajor`
  - 供需要 point-major layout 的消费者使用的 reorder helper。

## 状态码与枚举

- 返回码为来自 `sqzc3d_types.h` 的 C-style ints：
  - `sqzc3d_STATUS_SUCCESS`
  - `sqzc3d_STATUS_INVALID_ARGUMENT`
  - `sqzc3d_STATUS_DIMENSION_MISMATCH`
  - `sqzc3d_STATUS_NOT_IMPLEMENTED`
  - `sqzc3d_STATUS_INTERNAL_ERROR`
- 重要的 enum families：
  - input type：`sqzc3d_FILE`、`sqzc3d_MEMORY`
  - selection mode：indices/labels/all
  - read policy：`AUTO`、`DENSE`、`SPARSE`
  - valid policy：`sqzc3d_VALID_POLICY_FINITE_XYZ`

## 备注

- API 与 C89 兼容（C++ 可通过 `extern "C"` 使用）。
- 所有非 `const` out-parameters 都要求 caller 提供可写内存。
- 由本库分配的资源必须使用对应的 `free` APIs 释放。
- v0.x 暂不提供 residual/camera-mask 的 public payload；若下游需要，可通过 request/extension 添加。

## Python API

Python bindings 基于 pybind11。

Python 高层导出：

- `sqzc3d.version()`
- `sqzc3d.abi_version()`
- `sqzc3d.features()`
- `sqzc3d.read(...) -> sqzc3d.View`（easy 层）
- `sqzc3d.View`（easy 层）
- `sqzc3d.Recipe`（`sqzc3d.ChunkRecipe` 的 alias）
- `sqzc3d.Decoder`
- `sqzc3d.Chunk`
- `sqzc3d.ChunkQuery` / `sqzc3d.ChunkRecipe`（AND-only chunk-local filtering）
- `sqzc3d.type_group_indices(chunk, group_names, strict=False)`
- `sqzc3d.load_bundle(path: str, strict: bool = True)`
- `sqzc3d.export_bundle(out_dir: str, chunk: sqzc3d.Chunk)`

### Easy 层

推荐大多数用户从这里开始。

`read`：

- `sqzc3d.read(source, *, start_frame=0, frame_count=-1, points=None, analogs=None, analog_range=None, label_norm=..., recipe=None) -> View`

选择器语义（Python）：

- `None` = default（ALL）
- `[]` = empty selection

`View`：

- Selection state（labels-first）：
  - `view.point_labels`（`None | list[str]`）
  - `view.analog_labels`（`None | list[str]`）
  - `view.type_groups`（`list[str]`）
- Data（properties）：
  - `view.points` / `view.points_valid`
  - `view.analogs` / `view.analogs_valid`
- Label accessors：
  - `view.point["LANK"]` / `view.point_valid["LANK"]`
  - `view.analog["EMG1"]` / `view.analog_valid["EMG1"]`
- Metadata：
  - `view.meta`（flat dict）
  - `view.meta_tree`（EZ parameter tree；若可用；bundle 会保留；从 C3D 提取需要 `SQZC3D_WITH_EZC3D=ON`）
- Advanced escape hatch：
  - `view._chunk`（pybind `Chunk`；indices/masks 等被视作 advanced）

Notes：

- easy 层按设计为 **labels-only**。若你已有 indices，请使用 core API 并直接 slice 数组。

`Decoder`：

- `Decoder(path, label_norm=sqzc3d.SQZC3D_LABEL_NORM_EXACT)`
- `Decoder.read(start_frame=0, frame_count=-1, points=None, analogs=None, analog_range=None)`
- `Decoder.close()`
- `Decoder.source_path`（只读）
- `Decoder.closed`（只读 bool）

`Chunk`：

- `chunk.points(selector=None, copy=True) -> (values, valid)`
- `chunk.analogs(selector=None, layout="CN", copy=True) -> (values, valid)`
  - `layout="tcs"` 会返回一个非连续的 frame-major view `(T, C, S)`（底层仍为 channel-major 存储）。
- `chunk.meta`（dict）
- `chunk.meta_tree`（dict：若可用；bundle 会保留；`open_memory` 也应可用）
- `chunk.source_path`（只读）

Python payload 语义：

- `points` values：`float64`，默认 frame-major shape `(T, P, 3)`，valid mask `(T, P)` dtype `uint8`。
- `analogs` values：
  - `layout="CN"` 默认：`(C, N)`，其中 `N = n_frames * n_analog_by_frame`
  - `layout="tcs"`：`(T, C, S)` non-contiguous view helper
- selector：
  - `None` 表示 all
  - `[]` 表示 empty
  - `int` 或 `list/tuple[int]` 为 index selection
  - `str` 或 `list/tuple[str]` 为 label selection
- 非连续选择需要 `copy=True`；`copy=False` 当前会抛 `RuntimeError`，以避免隐藏的转换。
