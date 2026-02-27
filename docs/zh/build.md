# 构建与安装

30 秒版本：

- 只用 Python：`pip install sqzc3d` 就够了。
- 需要 C/C++ 或想跑 samples/benchmarks：用 CMake 从源码构建。
- 可选能力由 CMake 选项控制，例如 `SQZC3D_WITH_EZC3D`。

## Python — pip

```bash
pip install sqzc3d
```

## C/C++ — 从源码构建

```bash
cmake -S . -B build
cmake --build build --config Release --parallel
```

如需构建 CLI samples / benchmarks，请启用：

- `-DSQZC3D_BUILD_EXAMPLES=ON`

## 常用 CMake 选项

- `SQZC3D_WITH_EZC3D`  
  取值：`ON|OFF`。默认：`ON`。  
  启用/禁用 C3D 解析能力（`open_file` / `open_memory` / `build_chunks`）。
  若为 `OFF`，运行时仍支持 bundle 的 load/export 接口。
- `SQZC3D_FETCH_EZC3D`  
  取值：`ON|OFF`。默认：`ON`。  
  当当前 toolchain 中找不到 `ezc3d` 时自动拉取。
- `SQZC3D_APPLY_EZC3D_PATCHES`  
  取值：`ON|OFF`。  
  对拉取到的 `ezc3d` 应用本地兼容性 patch（WASM 构建建议开启）。
- `SQZC3D_BUILD_EXAMPLES`  
  取值：`ON|OFF`。默认：`OFF`。  
  构建 CLI samples 与 benchmarks。

## CMake 用法 — 作为依赖

```cmake
add_subdirectory(path/to/squeezc3d)
target_link_libraries(your_target PRIVATE sqzc3d)
```

## 运行时能力 — 可选

你可以通过 `sqzc3d_get_features()` 查询运行时能力位：

```c
int features = sqzc3d_get_features();
```

这对 bundle-only 构建（`SQZC3D_WITH_EZC3D=OFF`）的运行时自适应很有用。
