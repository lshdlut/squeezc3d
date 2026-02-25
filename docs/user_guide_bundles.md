# Bundles (`.sqzc3d`)

`sqzc3d` can persist a materialized chunk into a compact "bundle" format for fast reloads.

Two bundle layouts are supported:

- Directory bundle (legacy): a directory containing `meta.json` + `data.bin`
- Single-file bundle (recommended): `*.sqzc3d`

## Python

The easy entry point `sqzc3d.read(...)` auto-detects bundles:

- If `source` is a directory, it loads a directory bundle.
- If `source` ends with `.sqzc3d` (case-insensitive), it loads a single-file bundle.
- Otherwise it treats `source` as a C3D path/buffer and materializes a chunk.

```python
import sqzc3d as sq

v = sq.read("trial.sqzc3d")     # load a single-file bundle
v = sq.read("bundle_dir/")      # load a directory bundle
v = sq.read("trial.c3d")        # materialize from C3D

print(v.meta["n_frames"], v.meta["n_points"])
```

Strictness:

```python
v = sq.read("trial.sqzc3d", bundle_strict=True)   # default
v = sq.read("trial.sqzc3d", bundle_strict=False)  # best-effort load
```

## C API

Export:

```c
// Writes <out_dir>/meta.json and <out_dir>/data.bin.
int st = sqzc3d_export_bundle(out_dir, chunk);
```

Load:

```c
sqzc3d_chunk_t* chunk = NULL;
int st = sqzc3d_load_bundle(bundle_path_or_dir, &chunk);
```

Notes:

- Bundles load into a `sqzc3d_chunk_t*`, so you can reuse the same view/query APIs.
- Bundles may optionally contain extra metadata; consumers should use `chunk->reason` / `sqzc3d_last_error_detail(...)` for diagnostics on mismatch.

