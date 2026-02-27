# squeezc3d 文档

`squeezc3d`（简称 `sqzc3d`）是一个面向 C3D 的高性能加载库。
目标很直接：把 C3D 读成**可预测的数组**（`points/analogs`），并把关键语义讲清楚。

## 给普通用户的 30 秒版本

- 你只想要数组来做分析/仿真 → 直接用 Python easy：`sqzc3d.read(...)`。
- 你会拿到数组 + 显式的 `*_valid` mask，从此“丢点/缺失样本”不再靠猜。
- 你可以按 label 选点、读取窗口、并在 native 与 WASM 下保持一致行为。
- 你担心“换库后数据会不会悄悄变” → 直接看 `user_guide_correctness_compatibility`，并跑 `verify.py stress`。

它从我们在工程中使用 `ezc3d` 的出发点来改造：

- `ezc3d` 的解析与参数树很成熟，但在批量/大文件/WASM 场景下，对象图构建、拷贝与布局整理的成本偏高
- 下游往往还需要自己再做 selection、validity、units、marker-set / type-groups 等语义对齐

## 术语小抄

| 术语 | 通俗解释 |
| --- | --- |
| `read -> View` | easy 层，“直接给我数组”的接口。 |
| `Decoder -> Chunk` | core 层，想更精细控制选择与布局时用。 |
| materialize | 一次性读入并分配数组，后续切片/计算很快。 |
| `*_valid` | 每个样本的“可用/不可用”贴纸。 |
| selector | 按 labels 或 indices “点名”你要的点/通道。 |

## 从哪里开始

- 你用 Python：先看 `getting_started_python`，再看 `user_guide_python_easy`。
- 你用 C/C++：先看 `getting_started_c`，再看 `build`。
- 你想在替换 `ezc3d` 前先放心：看 `user_guide_correctness_compatibility`，并跑 `verify.py stress`。

`squeezc3d` 的主要成果：

- **Chunk-first**：一次性 materialize 为紧凑 chunk，直接得到连续数组，NumPy 友好
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
:maxdepth: 2
:caption: 正确性与兼容性

user_guide_correctness_compatibility
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
