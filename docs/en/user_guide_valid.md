# Validity

`sqzc3d` always returns data together with a validity mask.

Quick summary:

- Treat `*_valid` as the source of truth.
- If a sample is invalid, consider it “missing” and do not use the value.
- Empty selections still return empty arrays together with empty valid masks.

Mental model: every sample comes with a “usable / not usable” sticker.

## Points

Materialize and streaming both define:

- `points_xyz`: point coordinates.
- `points_valid`: `uint8` mask (same indexing as points).

If a point is invalid, `points_valid` is `0`.

In Python:

```python
import sqzc3d as sq

v = sq.read("trial.c3d")
pts = v.points
valid = v.points_valid
```

## Analogs

When analogs are enabled and present:

- `analogs`: channel-major `(C, N)` by default.
- `analogs_valid`: same layout as `analogs` with dtype `uint8`.

Empty selections are valid and return empty arrays together with empty valid masks.

## Why validity exists

Real-world C3D files frequently encode missing samples.

Downstream code should:

- Prefer `*_valid` masks for filtering.
- Avoid assuming all values are finite.
