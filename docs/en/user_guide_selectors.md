# Selectors and index spaces

This page defines selection semantics across Python and C.

Quick summary:

- `None` means “ALL”, `[]` means “empty”.
- Label selection is order-preserving.
- Indices can mean different index spaces; know which one you are using.

Mental model: labels are “names”, indices are “numbers”, and you must not mix numbering schemes.

## Python Easy

The easy surface is labels-only.

```python
import sqzc3d as sq

v = sq.read("trial.c3d", points=None)        # ALL points
v = sq.read("trial.c3d", points=["LANK"])    # label selection
v = sq.read("trial.c3d", points=[])          # empty selection
```

Selector semantics:

- `None` means default (ALL).
- `[]` means empty selection.
- `str` or `sequence[str]` means label selection.

## Python Core

The core surface supports indices or labels:

```python
chunk.points(None)         # ALL
chunk.points([])           # empty
chunk.points([0, 1, 2])    # indices (chunk-local point indices)
chunk.points(["LANK"])     # labels (chunk-local label table)
```

Notes:

- For non-contiguous selections, `copy=True` is required.

## C API — `sqzc3d_build_opt_t`

The C materialize API selects points/analogs during chunk construction.

Point selection:

- `point_sel_mode = sqzc3d_POINT_SEL_ALL`
- `point_sel_mode = sqzc3d_POINT_SEL_INDICES` and `point_sel` are source-total point indices
- `point_sel_mode = sqzc3d_POINT_SEL_LABELS` and `point_labels` are matched against source labels

Empty selection:

- `*_sel_count == 0` is a valid empty selection (not an error).

## Index spaces

There are two point index spaces:

- Source-total: indices into the original C3D `POINT:LABELS` table.
- Chunk-local: indices into the materialized chunk, range `[0..chunk->n_points)`.

In the C API:

- `sqzc3d_build_opt_t.point_sel` uses the source-total space.
- `sqzc3d_chunk_t.type_group_indices` are chunk-local.
- All point indices exposed from a chunk (views/type-groups) are chunk-local.
