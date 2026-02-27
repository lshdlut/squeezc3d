# squeezc3d documentation

`squeezc3d`, aka `sqzc3d`, is a high-performance C3D loading library.
The goal is simple: read C3D into **predictable arrays** (`points/analogs`) with clear semantics.

## 30-second summary

- If you just want arrays for analysis or simulation, start with Python Easy: `sqzc3d.read(...)`.
- You get arrays plus explicit `*_valid` masks, so missing samples are not a guessing game.
- You can select by labels, read windows, and keep behavior stable across native and WASM.
- If you worry about “will my data silently change?”, read `user_guide_correctness_compatibility` and run `verify.py stress`.

It is designed around practical `ezc3d`-based workflows:

- `ezc3d` has mature parsing and parameter tree support, but in batch / large-file / WASM settings, object graph
  construction, copying, and layout normalization can be expensive.
- Downstream code often still needs consistent selection, validity, units, and marker-set / type-groups semantics.

## Mini glossary

| Term | Plain meaning |
| --- | --- |
| `read -> View` | the easy “just give me arrays” API. |
| `Decoder -> Chunk` | the core API when you want full control over selection and layouts. |
| materialize | read once, allocate arrays, then slice fast. |
| `*_valid` | per-sample “usable / not usable” masks. |
| selector | picking markers/channels by labels or indices. |

## Where to start

- If you use Python: start with `getting_started_python`, then `user_guide_python_easy`.
- If you use C/C++: start with `getting_started_c`, then `build`.
- If you want confidence before switching from `ezc3d`: read `user_guide_correctness_compatibility` and run `verify.py stress`.

Key results in `squeezc3d`:

- **Chunk-first**: materialize into a compact chunk and get contiguous arrays, NumPy-friendly.
- **Clear contracts**: fixed layout, explicit `valid` masks, unambiguous selector/index-space rules, and well-defined
  defaults for type-groups and units.
- **Application-facing API**: Python `read -> View` stays small; Core `Decoder/Chunk` and C/C++ APIs remain available
  for precise control.
- **Optional modes**: streaming reads and `.sqzc3d` bundles for low-memory, browser, and cached-load workflows.

```{toctree}
:maxdepth: 2
:caption: Getting started

getting_started_python
getting_started_c
build
```

```{toctree}
:maxdepth: 2
:caption: User guide

user_guide_python_easy
user_guide_materialize_vs_streaming
user_guide_layout
user_guide_selectors
user_guide_valid
user_guide_type_groups
user_guide_units
user_guide_bundles
user_guide_errors
```

```{toctree}
:maxdepth: 2
:caption: Correctness & Compatibility

user_guide_correctness_compatibility
```

```{toctree}
:maxdepth: 1
:caption: Benchmarks

benchmarks
```

```{toctree}
:maxdepth: 2
:caption: API reference

API
```
