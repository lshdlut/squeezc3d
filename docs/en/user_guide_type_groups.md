# Type-groups (marker sets)

Some C3D files include metadata describing point "type groups" (for example, a marker group).

`sqzc3d` exposes this metadata from a chunk as:

- `type_group_names`: group names
- `type_group_starts`: prefix-sum offsets
- `type_group_indices`: flattened point index list

All indices are in the chunk-local point index space.

## Python Easy (recommended)

The easy layer supports type-groups via `View.type_groups`.

The effective selection is an AND:

- `point_labels` selection (if set)
- type-groups selection (if non-empty)

```python
import sqzc3d as sq

v = sq.read("trial.c3d")
v.type_groups = ["MARKER"]
pts = v.points
```

## Python Core: map type-groups to indices

```python
import sqzc3d

dec = sqzc3d.Decoder("trial.c3d")
chunk = dec.read(frame_count=1)
dec.close()

idx = sqzc3d.type_group_indices(chunk, "MARKER", strict=False)
pts, valid = chunk.points(idx, copy=True)
```

Default semantics:

- Missing TYPE_GROUPS metadata means no-op (ALL points).
- Metadata exists but the group name is missing means empty set.
- `strict=True` makes missing/invalid cases error out.

The Python helper also exposes the missing-meta policy:

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

## C/C++: consume type-group metadata from a chunk

When a chunk contains type-group metadata, it is exposed as:

- `chunk->type_group_names` (length `chunk->n_type_groups`)
- `chunk->type_group_starts` (length `chunk->n_type_groups + 1`)
- `chunk->type_group_indices` (flattened chunk-local point indices)

Example (C-style):

```c
for (int gi = 0; gi < chunk->n_type_groups; ++gi) {
  const char* name = chunk->type_group_names[gi];
  const int s = chunk->type_group_starts[gi];
  const int e = chunk->type_group_starts[gi + 1];
  // indices are chunk-local point indices: chunk->type_group_indices[s..e)
}
```

