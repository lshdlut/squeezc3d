# squeezc3d documentation

`squeezc3d` (abbrev `sqzc3d`) is a high-performance C3D loading library. The goal is not to build a complex object
graph, but to deliver `points/analogs` as predictable arrays with clear semantics.

It is designed around practical `ezc3d`-based workflows:

- `ezc3d` has mature parsing and parameter tree support, but in batch / large-file / WASM settings, object graph
  construction, copying, and layout normalization can be expensive.
- Downstream code often still needs consistent selection, validity, units, and marker-set (type-groups) semantics.

Key results in `squeezc3d`:

- **Chunk-first**: materialize into a compact chunk and get contiguous arrays (NumPy-friendly).
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
:caption: API reference

API
```
