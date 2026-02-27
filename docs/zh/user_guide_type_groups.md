# 点类型组

30 秒版本：

- Type-groups 可以理解为 points 的“标签/分组”（例如 MARKER 组）。
- 在 Python easy 层，设置 `View.type_groups` 就能作为额外的 AND-filter 来筛选 points。
- 缺失元数据时的行为是可配置的；strict 模式可以直接抛错。

形象化：像给一张表加了“分类”列，然后按分类筛选。

一些 C3D 文件会包含描述 point “type groups”（例如 marker group）的元数据。

`sqzc3d` 会将这类元数据从 chunk 中暴露为：

- `type_group_names`：组名列表
- `type_group_starts`：prefix-sum offsets
- `type_group_indices`：扁平化的 point index 列表

所有 indices 都在 chunk-local point index space 中。

## Python Easy

推荐：大多数用户直接用这层即可。

Easy 层通过 `View.type_groups` 支持 type-groups。

effective selection 是 AND：

- `point_labels` 的选择（如果被设置）
- type-groups 的选择（如果非空）

```python
import sqzc3d as sq

v = sq.read("trial.c3d")
v.type_groups = ["MARKER"]
pts = v.points
```

## Python Core：从 type-groups 映射到 indices

```python
import sqzc3d

dec = sqzc3d.Decoder("trial.c3d")
chunk = dec.read(frame_count=1)
dec.close()

idx = sqzc3d.type_group_indices(chunk, "MARKER", strict=False)
pts, valid = chunk.points(idx, copy=True)
```

默认语义：

- 缺失 TYPE_GROUPS 元数据：视作 no-op（ALL points）。
- 元数据存在但 group name 缺失：返回空集。
- `strict=True` 会在缺失/无效时抛错。

Python helper 也暴露了 missing-meta policy：

```python
import sqzc3d

dec = sqzc3d.Decoder("trial.c3d")
chunk = dec.read(frame_count=1)
dec.close()

# If TYPE_GROUPS metadata is missing:
# - missing_meta="all" yields ALL points (default)
# - missing_meta="empty" yields empty
idx = sqzc3d.type_group_indices(chunk, ["MARKER"], strict=False, missing_meta="all")
```

## C/C++：从 chunk 消费 type-group 元数据

当 chunk 含有 type-group 元数据时，会暴露为：

- `chunk->type_group_names`（长度 `chunk->n_type_groups`）
- `chunk->type_group_starts`（长度 `chunk->n_type_groups + 1`）
- `chunk->type_group_indices`（flattened chunk-local point indices）

示例（C 风格）：

```c
for (int gi = 0; gi < chunk->n_type_groups; ++gi) {
  const char* name = chunk->type_group_names[gi];
  const int s = chunk->type_group_starts[gi];
  const int e = chunk->type_group_starts[gi + 1];
  // indices are chunk-local point indices: chunk->type_group_indices[s..e)
}
```
