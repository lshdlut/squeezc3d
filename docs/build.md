# Build and installation

## Python (pip)

```bash
pip install sqzc3d
```

## C/C++ (from source)

```bash
cmake -S . -B build
cmake --build build --config Release --parallel
```

If you want the CLI samples / benchmarks, enable:

- `-DSQZC3D_BUILD_EXAMPLES=ON`

## Common CMake options

- `SQZC3D_WITH_EZC3D` (`ON|OFF`, default `ON`)  
  Enable/disable the C3D parsing feature (`open_file` / `open_memory` / `build_chunks`).
  If `OFF`, the runtime still supports bundle load/export APIs.
- `SQZC3D_FETCH_EZC3D` (`ON|OFF`, default `ON`)  
  Auto-fetch `ezc3d` when not found in the current toolchain.
- `SQZC3D_APPLY_EZC3D_PATCHES` (`ON|OFF`)  
  Apply local compatibility patches to fetched `ezc3d` (recommended for WASM builds).
- `SQZC3D_BUILD_EXAMPLES` (`ON|OFF`, default `OFF`)  
  Build CLI samples and benchmarks.

## CMake usage (dependency)

```cmake
add_subdirectory(path/to/squeezc3d)
target_link_libraries(your_target PRIVATE sqzc3d)
```

## Runtime capabilities (optional)

You can query runtime capability bits via:

```c
int features = sqzc3d_get_features();
```

This is useful if you build bundle-only binaries (`SQZC3D_WITH_EZC3D=OFF`) and want to adapt behavior at runtime.

