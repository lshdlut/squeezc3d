# Python Easy API

Quick summary:

- Use this layer when you want “just give me arrays”.
- Think of `View` as “arrays + current selection state”.
- When you need more control, drop to `View._chunk` (Core `Chunk`).

Mental model: `View` is a small dashboard; you change selectors, then read tables.

This layer is for "give me arrays for analysis/simulation".
You do not need to learn chunk/query/recipe internals. Remember:

- `sqzc3d.read(...)` loads data into a `View`.
- `View` holds the current selection state (labels/type-groups) and exposes data + validity as properties.
- When you need advanced features, drop to `View._chunk` (Core `Chunk`).

If you already have indices, or you need precise layout/view/copy control, use the Core API (`Decoder` / `Chunk`).

## Load: `read`

```python
import sqzc3d as sq

v = sq.read("trial.c3d")        # materialize from C3D
v = sq.read("trial.sqzc3d")     # load a single-file bundle
v = sq.read("bundle_dir/")      # load a directory bundle
```

Common parameters:

- `start_frame`, `frame_count`: frame window selection
- `points`, `analogs`: label selectors
- `analog_range`: analog window selection (advanced, in the same frame index space as `start_frame`)
- `label_norm`: label normalization mode (ambiguous matches raise)
- `recipe`: reusable filtering recipe (currently mainly type-groups)

Selector semantics:

- `None` means default (ALL)
- `[]` means empty selection
- `str` or `sequence[str]` means label selection

## Windows and time axis

`View.meta` includes time-axis metadata (when available):

- `source_first_frame`, `source_last_frame`: absolute frame indices from the original C3D
- `point_rate_hz`, `analog_rate_hz`: sampling rates
- `frame_start`, `frame_start_abs`: point window origin (relative / absolute)
- `analog_frame_start`, `analog_frame_start_abs`: analog window origin (relative / absolute)

If you want analogs aligned with the point window, pass `analog_range=(start_frame, frame_count)` explicitly:

```python
import sqzc3d as sq

v = sq.read("trial.c3d", start_frame=100, frame_count=200, analog_range=(100, 200))
print(v.meta["frame_start_abs"], v.meta["analog_frame_start_abs"])
```

## Selection state: mutate `View` fields

`View` keeps selection state in fields you can inspect and update:

```python
import sqzc3d as sq

v = sq.read("trial.c3d")

# Inspect
print(v.point_labels)   # None means ALL
print(v.analog_labels)  # None means ALL

# Update (labels-only)
v.point_labels = ["LASI", "RASI"]
v.analog_labels = ["EMG1"]
```

Order (important):

- If `view.point_labels` is set, `view.points` returns points in the **same order as `point_labels`**.
- If `view.analog_labels` is set, `view.analogs` returns channels in the **same order as `analog_labels`**.

## Get data: properties + validity

- `view.points` / `view.points_valid`
- `view.analogs` / `view.analogs_valid`

```python
pts = v.points
valid = v.points_valid
```

## By-label access: one marker / one channel

Convenient label accessors (avoids manual index lookup):

```python
ank = v.point["LANK"]              # (T, 3)
ank_valid = v.point_valid["LANK"]  # (T,)

emg = v.analog["EMG1"]             # (N,) or (T, S) depending on analog_layout
emg_valid = v.analog_valid["EMG1"]
```

## Type-groups

Optional.

If type-groups exist on the chunk, they can be applied as an extra AND-filter:

```python
import sqzc3d as sq

v = sq.read("trial.c3d")
v.type_groups = ["MARKER"]  # AND with point_labels if set
pts = v.points
```

Missing-metadata behavior:

- `type_groups_missing_meta="all"` (default): missing TYPE_GROUPS metadata is treated as no-op
- `type_groups_missing_meta="empty"`: missing metadata yields an empty set

Strict mode:

- `type_groups_strict=True` makes missing/invalid cases raise.

## Recipe

Optional.

A `Recipe` is a reusable config object that can be passed into `read(...)`:

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

## Advanced: drop to Core

Always available.

`View._chunk` exposes the underlying Core `Chunk`:

```python
chunk = v._chunk
pts, valid = chunk.points(None, copy=False)
```
