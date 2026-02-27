# 数据布局 — Materialize

本页描述 `sqzc3d` 在 materialize（内存化）时使用的默认数组布局。

目标是让 shape 与内存顺序对下游代码“可预测”。

30 秒版本：

- points：`(T, P, 3)`，并且永远搭配同索引的 `points_valid (T, P)`。
- analogs：默认 `(C, N)`，并且永远搭配同布局的 `analogs_valid (C, N)`。
- `layout="tcs"` 更像“看起来方便”的视图；底层连续存储还是 `layout="CN"`。

形象化：把 C3D 当成两张表（points 表 + analogs 表），你关心的首先是“表的形状”和“如何索引”。

## Points — 点轨迹

Materialized points 的约定：

- Layout：frame-major（按帧优先）
- Pack：AoS XYZ，并搭配独立的 `valid` mask

在 Python 中，默认 `Chunk.points(...)` 返回：

- `points`：shape `(T, P, 3)`，dtype `float64`
- `points_valid`：shape `(T, P)`，dtype `uint8`

```python
import sqzc3d as sq

v = sq.read("trial.c3d")
points = v.points
valid = v.points_valid

# 例：对单个 marker 过滤 invalid samples
p = v.point["LANK"]                 # (T, 3)
pv = v.point_valid["LANK"]          # (T,)
p_clean = p[pv != 0]
```

在 C API 中，`sqzc3d_points_view_frames(...)` 以 view 的形式暴露同一块内存：

- `view.points_xyz`：`(T, P, 3)`，frame-major，连续（contiguous）
- `view.points_valid`：`(T, P)`，连续（contiguous）

## Analogs — 模拟通道

Materialized analogs 默认是 channel-major（按通道优先）：

- Layout：`(C, N)`，其中 `N = T * S`
- `S = n_analog_by_frame`（每帧的 subframes 数）

在 Python 中：

```python
import sqzc3d

dec = sqzc3d.Decoder("trial.c3d")
chunk = dec.read()
dec.close()

values_cn, valid_cn = chunk.analogs(layout="CN")   # (C, N)
values_tcs, valid_tcs = chunk.analogs(layout="tcs")  # (T, C, S) (strided view helper)
```

备注：

- `layout="CN"` 是连续（contiguous）的。
- `layout="tcs"` 是一个 **strided** view：它基于底层 `(C, N)` 存储提供更方便的 frame 视角。

## 拷贝 vs 视图

这里的 view 指“共享同一块底层内存的视角”，不发生额外拷贝。

`sqzc3d` 会避免隐式的 gathers/copies：

- 对连续选择可以返回 view（`copy=False`）。
- 对非连续选择必须 `copy=True`。

这样可以让性能开销显式化，并保持内存行为可预测。
