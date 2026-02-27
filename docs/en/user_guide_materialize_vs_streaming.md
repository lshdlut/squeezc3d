# Materialize and streaming

`sqzc3d` supports two access models. Both aim for consistent semantics (layout/validity/units).
The difference is resource usage and how data is delivered.

TL;DR:

- Materialize: read once and get arrays. This is the default you probably want.
- Streaming: keep the file open and read into your buffers on demand. Use this for tight memory or special I/O.

Mental model: materialize is “load the spreadsheet”, streaming is “read rows on demand”.

## Materialize: one-shot `Chunk` in memory

Materialize means:

- Parse the C3D payload.
- Allocate compact contiguous arrays.
- Return a `Chunk` containing:
  - `points_xyz`: `(T, P, 3)` `float64` (frame-major)
  - `points_valid`: `(T, P)` `uint8`
  - optional `analogs`: `(C, N)` `float64` (channel-major)

This is ideal for most analysis/simulation workflows: load once, then slice/compute fast.

Python Easy (`read -> View`) and the C API (`sqzc3d_build_chunks`) use this path.

## Streaming: keep header/meta and read into user buffers

Streaming means:

- Parse header + parameters + labels.
- Keep the file open.
- Read frames/windows/trajectories on demand into user-provided buffers.

This mode is exposed via the C++ API in `include/sqzc3d_c3d_stream.h`.

Typical use cases (more about constraints than convenience):

- Extremely memory constrained environments.
- Virtual file systems (e.g. browser WASM file APIs).
- You only need a small window, and you do not want to materialize everything.

## How to choose

- Prefer Materialize: simplest semantics, most NumPy-friendly.
- Use Streaming only when you have tight memory / special I/O, and you are OK managing output buffers.

Units:

- By default, streaming returns raw point units (see Units). Downstream code can request explicit scaling.
