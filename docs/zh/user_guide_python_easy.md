# Python Easy 接口

30 秒版本：

- 你只想“读出来就是数组” → 用 Easy 层。
- 把 `View` 当成“数组 + 当前选择状态（labels/type-groups）”。
- 需要更精细控制时，再下探到 `View._chunk`（Core `Chunk`）。

形象化：`View` 像一个小控制台，你先改选择器，再读取对应的两张表。

这层是给“直接用数据做分析/仿真”的用户准备的。你不需要先理解 chunk/query/recipe 的内部结构，只要记住：

- `sqzc3d.read(...)` 负责把数据读成一个 `View`
- `View` 保存当前选择状态（labels/type-groups），并以属性方式暴露数据与 validity
- 需要高级能力时再下探到 `View._chunk`（Core `Chunk`）

如果你已经有 indices、或者需要更精细的 layout/view/copy 控制，请直接使用 Core API（`Decoder` / `Chunk`）。

## 读入：`read`

```python
import sqzc3d as sq

v = sq.read("trial.c3d")        # 从 C3D materialize
v = sq.read("trial.sqzc3d")     # 加载 single-file bundle
v = sq.read("bundle_dir/")      # 加载 directory bundle
```

常用参数：

- `start_frame`, `frame_count`：按帧的 window 选择
- `points`, `analogs`：label 选择器
- `analog_range`：按 sample-range 的选择（高级）
- `label_norm`：label 归一化模式
- `recipe`：可复用的过滤 recipe（目前主要是 type-groups）

选择器语义：

- `None` 表示默认（ALL）
- `[]` 表示空选择
- `str` 或 `sequence[str]` 表示按 label 选择

## 选择状态：直接改 `View` 字段

`View` 把选择状态保存在字段里，你可以直接查看和修改：

```python
import sqzc3d as sq

v = sq.read("trial.c3d")

# 查看
print(v.point_labels)   # None 表示 ALL
print(v.analog_labels)  # None 表示 ALL

# 修改选择（labels-only）
v.point_labels = ["LASI", "RASI"]
v.analog_labels = ["EMG1"]
```

关于顺序（非常重要）：

- 当 `view.point_labels` 被设置时，`view.points` 会按 **`point_labels` 的顺序** 返回 points。
- 当 `view.analog_labels` 被设置时，`view.analogs` 会按 **`analog_labels` 的顺序** 返回 channels。

## 获取数据：属性 + validity

- `view.points` / `view.points_valid`
- `view.analogs` / `view.analogs_valid`

```python
pts = v.points
valid = v.points_valid
```

## 按 label 取单个 marker / channel

提供了按 label 的便捷访问（避免你手动去找 index）：

```python
ank = v.point["LANK"]              # (T, 3)
ank_valid = v.point_valid["LANK"]  # (T,)

emg = v.analog["EMG1"]             # (N,) 或 (T, S)（取决于 analog_layout）
emg_valid = v.analog_valid["EMG1"]
```

## Type-groups

可选。

如果 chunk 上有 type-groups，可将其作为额外的 AND-filter：

```python
import sqzc3d as sq

v = sq.read("trial.c3d")
v.type_groups = ["MARKER"]  # AND with point_labels if set
pts = v.points
```

缺失元数据时的行为：

- `type_groups_missing_meta="all"`（默认）：缺失 TYPE_GROUPS 元数据时视作 no-op
- `type_groups_missing_meta="empty"`：缺失元数据时直接得到空集

严格模式：

- `type_groups_strict=True` 会在缺失/无效时直接抛错

## Recipe

可选。

`Recipe` 是可复用的配置对象，可作为参数传入 `read(...)`（适合把“选择规则”抽出来复用）。

```python
import sqzc3d as sq

rcp = sq.Recipe(
    type_groups=("MARKER",),
    type_groups_strict=False,
    type_groups_missing_meta="all",
)

v = sq.read("trial.c3d", recipe=rcp)
print(v.describe())
```

## 进阶：进入 Core 层

不主推，但随时可用。

Easy 层会把高级能力保留为“可用但不默认”的形式。

`View._chunk` 暴露底层 core `Chunk`：

```python
chunk = v._chunk
pts, valid = chunk.points(None, copy=False)
```
