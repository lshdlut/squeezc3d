# Materialize 与 Streaming

`sqzc3d` 支持两种访问模型，它们都能得到一致的语义（layout/validity/units），差异在于资源与交付形态：

30 秒版本：

- Materialize：一次性读入并产出数组。大多数用户默认就用它。
- Streaming：保持文件打开，按需读到你提供的 buffer。只有在内存很紧或 I/O 很特殊时才需要。

形象化：Materialize 像把整张表读进来；Streaming 像需要哪几行就读哪几行。

## Materialize：一次性内存化为 `Chunk`

Materialize 的特点：

- 解析 C3D payload。
- 分配紧凑的连续数组。
- 返回一个 `Chunk`，包含：
  - `points_xyz`: `(T, P, 3)` `float64`（frame-major）
  - `points_valid`: `(T, P)` `uint8`
  - 可选的 `analogs`: `(C, N)` `float64`（channel-major）

这适合大多数分析/仿真场景：加载一次，后续反复切片/计算都很快。

Python easy 层（`read -> View`）与 C API（`sqzc3d_build_chunks`）走的都是这条路径。

## Streaming：只保留头信息，按需读入用户缓冲区

Streaming 的特点：

- 解析 header + parameters + labels。
- 保持文件打开。
- 按需从文件读取 frames/windows/trajectories。

该模式通过 `include/sqzc3d_c3d_stream.h` 中的 C++ API 暴露。

典型用例（更偏“工程约束”而不是“更好用”）：

- 极端内存受限环境。
- 虚拟文件系统（例如浏览器 WASM file APIs）。
- 只需要读取小窗口/少量帧，不想把全量数据放进内存。

## 如何选择

- 优先用 Materialize：语义最直观、数组最友好、也更符合 Python 用户心智。
- 需要极低内存或特殊 I/O 才用 Streaming：你需要自己管理输出 buffer，并显式选择读取的窗口。

Units：

- 默认情况下，Streaming 返回 raw point units（见 Units）；如果下游需要某个单位，可显式请求缩放。
