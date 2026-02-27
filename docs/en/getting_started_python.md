# Python

## Install

```bash
pip install sqzc3d
```

## Recommended: Easy — `read -> View`

TL;DR:

- Use `sqzc3d.read(...)` when you want arrays fast.
- Select by labels, and treat `*_valid` as the source of truth.
- If you need full control, jump to Core (`Decoder -> Chunk`) below.

```python
import sqzc3d as sq

v = sq.read("trial.c3d")  # default: points=ALL, analogs=ALL
print(v.meta["n_frames"], v.meta["n_points"], v.meta["n_analogs"])

pts = v.points              # (T, P, 3) float64
pts_valid = v.points_valid  # (T, P) uint8

# One marker by label (points)
ank = v.point["LANK"]              # (T, 3)
ank_valid = v.point_valid["LANK"]  # (T,)

# Select a subset by labels (Easy surface is labels-only)
v.point_labels = ["LASI", "RASI"]
pts2 = v.points  # (T, 2, 3)
assert pts2.shape[1] == 2  # order matches point_labels

# One analog channel by label (default layout: channel-major (C, N))
emg1 = v.analog["EMG1"]              # (N,)
emg1_valid = v.analog_valid["EMG1"]  # (N,)

# Disable analog reads (empty selection)
v_no_analog = sq.read("trial.c3d", analogs=[])
print(v_no_analog.meta["n_analogs"])  # 0
```

Bundles are also supported:

```python
import sqzc3d as sq

v = sq.read("trial.sqzc3d")  # single-file bundle
v = sq.read("bundle_dir/")   # directory bundle (meta.json + data.bin)
```

Selector semantics in Python:

- `None` means default (ALL).
- `[]` means empty selection.
- `str` or `sequence[str]` means label selection.

Advanced escape hatch:

- `View._chunk` exposes the underlying `Chunk` (indices/masks/etc are considered advanced).

## Advanced: Core — `Decoder -> Chunk`

```python
import sqzc3d

dec = sqzc3d.Decoder("trial.c3d")
chunk = dec.read(start_frame=0, frame_count=-1, points=None, analogs=None)
dec.close()

pts, pts_valid = chunk.points(copy=True)            # (T, P, 3), (T, P)
ana, ana_valid = chunk.analogs(layout="CN")         # (C, N), (C, N)
ana_tcs, _ = chunk.analogs(layout="tcs", copy=True) # (T, C, S), (T, C, S)
```

Notes:

- `[]` is a valid empty selector and returns empty arrays (not an error).
- Non-contiguous selection requires `copy=True` (the API avoids implicit gathers/copies).
