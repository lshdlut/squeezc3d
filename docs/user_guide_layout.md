# Data layout (Materialize)

This page documents the default array layouts used by `sqzc3d` materialization.

The goal is predictable shape and memory order for downstream code.

## Points (trajectories)

Materialized points contract:

- Layout: frame-major
- Pack: AoS XYZ with a separate `valid` mask

In Python, the default `Chunk.points(...)` returns:

- `points`: shape `(T, P, 3)`, dtype `float64`
- `points_valid`: shape `(T, P)`, dtype `uint8`

```python
import sqzc3d as sq

v = sq.read("trial.c3d")
points = v.points
valid = v.points_valid

# Example: filter out invalid samples for one point.
p = v.point["LANK"]                 # (T, 3)
pv = v.point_valid["LANK"]          # (T,)
p_clean = p[pv != 0]
```

In the C API, `sqzc3d_points_view_frames(...)` exposes the same memory as a view:

- `view.points_xyz`: `(T, P, 3)` frame-major, contiguous
- `view.points_valid`: `(T, P)` contiguous

## Analogs (channels)

Materialized analogs are channel-major by default:

- Layout: `(C, N)` where `N = T * S`
- `S = n_analog_by_frame` (subframes per frame)

In Python:

```python
import sqzc3d

dec = sqzc3d.Decoder("trial.c3d")
chunk = dec.read()
dec.close()

values_cn, valid_cn = chunk.analogs(layout="CN")     # (C, N)
values_tcs, valid_tcs = chunk.analogs(layout="tcs")  # (T, C, S) (strided view helper)
```

Notes:

- `layout="CN"` is contiguous.
- `layout="tcs"` is a **strided** view over the underlying `(C, N)` storage. It is convenient for frame-based logic.

## Copies vs views

`sqzc3d` avoids implicit gathers/copies:

- Contiguous selections can return a view (`copy=False`).
- Non-contiguous selections require `copy=True`.

This makes performance costs explicit and keeps memory behavior predictable.

