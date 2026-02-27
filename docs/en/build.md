# Build and installation

Quick summary:

- If you only use Python, `pip install sqzc3d` is enough.
- If you need C/C++ or want to build samples/benchmarks, build from source via CMake.
- Optional features are controlled by CMake options like `SQZC3D_WITH_EZC3D`.

## Python — pip

```bash
pip install sqzc3d
```

## C/C++ — from source

```bash
cmake -S . -B build
cmake --build build --config Release --parallel
```

If you want the CLI samples / benchmarks, enable:

- `-DSQZC3D_BUILD_EXAMPLES=ON`

## Common CMake options

- `SQZC3D_WITH_EZC3D`  
  Values: `ON|OFF`. Default: `ON`.  
  Enable/disable the C3D parsing feature (`open_file` / `open_memory` / `build_chunks`).
  If `OFF`, the runtime still supports bundle load/export APIs.
- `SQZC3D_FETCH_EZC3D`  
  Values: `ON|OFF`. Default: `ON`.  
  Auto-fetch `ezc3d` when not found in the current toolchain.
- `SQZC3D_APPLY_EZC3D_PATCHES`  
  Values: `ON|OFF`.  
  Apply local compatibility patches to fetched `ezc3d` (recommended for WASM builds).
- `SQZC3D_BUILD_EXAMPLES`  
  Values: `ON|OFF`. Default: `OFF`.  
  Build CLI samples and benchmarks.

## CMake usage — dependency

```cmake
add_subdirectory(path/to/squeezc3d)
target_link_libraries(your_target PRIVATE sqzc3d)
```

## Runtime capabilities — optional

You can query runtime capability bits via:

```c
int features = sqzc3d_get_features();
```

This is useful if you build bundle-only binaries (`SQZC3D_WITH_EZC3D=OFF`) and want to adapt behavior at runtime.
