# squeezc3d 文档

`squeezc3d`（缩写 `sqzc3d`）是一个面向 C3D 的高性能加载库：目标不是“把整个 C3D 变成复杂对象图”，而是把下游真正要用的 `points/analogs` 以稳定、可预测的数组语义交付出来。

它从我们在工程中使用 `ezc3d` 的出发点来改造：

- `ezc3d` 的解析与参数树很成熟，但在批量/大文件/WASM 场景下，对象图构建、拷贝与布局整理的成本偏高
- 下游往往还需要自己再做 selection、validity、units、marker-set（type-groups）等语义对齐

`squeezc3d` 的主要成果：

- **Chunk-first**：一次性 materialize 为紧凑 chunk，直接得到连续数组（NumPy 友好）
- **语义契约明确**：固定布局、显式 `valid` mask、清晰的 selector/index-space 规则、type-groups 与 units 的默认行为
- **面向应用的接口**：Python `read -> View` 简洁；同时保留 Core `Decoder/Chunk` 与 C/C++ API 以便精确控制
- **可选能力**：streaming 读取与 `.sqzc3d` bundles，用于极低内存/浏览器/缓存加载等场景

```{toctree}
:maxdepth: 2
:caption: 快速开始

getting_started_python
getting_started_c
build
```

```{toctree}
:maxdepth: 2
:caption: 用户指南

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
:maxdepth: 1
:caption: 基准测试

benchmarks
```

```{toctree}
:maxdepth: 2
:caption: API 参考

API
```
