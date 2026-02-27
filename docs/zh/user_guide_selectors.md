# 选择器与索引空间

本页定义 Python 与 C 中的选择器语义。

30 秒版本：

- `None` 表示“全选”，`[]` 表示“空选”。
- 按 labels 选择会**保序**（你传入什么顺序，就按什么顺序返回）。
- indices 有“编号体系”的问题：先确认你用的是哪一种 index space。

形象化：labels 像“姓名”，indices 像“工号”；更重要的是别把不同部门的工号体系混着用。

## Python easy 层

Easy surface 是 labels-only（不暴露 indices）。

```python
import sqzc3d as sq

v = sq.read("trial.c3d", points=None)        # ALL points
v = sq.read("trial.c3d", points=["LANK"])    # label selection
v = sq.read("trial.c3d", points=[])          # empty selection
```

选择器语义：

- `None` 表示默认（ALL）。
- `[]` 表示空选择。
- `str` 或 `sequence[str]` 表示按 label 选择。

## Python core 层

Core surface 支持 indices 或 labels：

```python
chunk.points(None)         # ALL
chunk.points([])           # empty
chunk.points([0, 1, 2])    # indices (chunk-local point indices)
chunk.points(["LANK"])     # labels (chunk-local label table)
```

备注：

- 对非连续选择，必须 `copy=True`。

## C API — `sqzc3d_build_opt_t`

C materialize API 会在构建 chunk 时对 points/analogs 做选择（也就是“在 materialize 阶段做过滤”）。

Point selection：

- `point_sel_mode = sqzc3d_POINT_SEL_ALL`
- `point_sel_mode = sqzc3d_POINT_SEL_INDICES` 且 `point_sel` 是 source-total point indices
- `point_sel_mode = sqzc3d_POINT_SEL_LABELS` 且 `point_labels` 按 source labels 匹配

空选择：

- `*_sel_count == 0` 是合法的空选择（不是错误）。

## 索引空间 — Index spaces

points 存在两个 index space：

- Source-total：原始 C3D `POINT:LABELS` 表的索引。
- Chunk-local：materialized chunk 的索引，范围 `[0..chunk->n_points)`。

在 C API 中：

- `sqzc3d_build_opt_t.point_sel` 使用 source-total space。
- `sqzc3d_chunk_t.type_group_indices` 是 chunk-local。
- chunk 暴露出来的所有 point indices（views/type-groups）都是 chunk-local。
