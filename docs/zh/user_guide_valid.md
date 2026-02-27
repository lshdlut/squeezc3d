# 有效性 — Validity

`sqzc3d` 会始终返回数据以及对应的 validity mask。

30 秒版本：

- 把 `*_valid` 当成唯一可信来源。
- `valid=0` 就当作“缺失样本”，别把数值当真。
- 空选择也是合法的，会返回空数组 + 空的 valid masks。

形象化：每个样本旁边都有一张“可用/不可用”的贴纸，先看贴纸再看数值。

## Points

Materialize 与 streaming 都定义：

- `points_xyz`：点坐标。
- `points_valid`：`uint8` mask（与 points 的索引一致）。

若 point 无效，则 `points_valid` 为 `0`。

在 Python 中：

```python
import sqzc3d as sq

v = sq.read("trial.c3d")
pts = v.points
valid = v.points_valid
```

## Analogs

当 analogs 启用且存在时：

- `analogs`：默认 channel-major `(C, N)`。
- `analogs_valid`：与 `analogs` 同布局，dtype 为 `uint8`。

空选择是合法的，会返回空数组以及空的 valid masks。

## 为什么需要 validity

真实世界的 C3D 文件经常编码缺失样本。

下游代码应当：

- 优先使用 `*_valid` masks 做过滤。
- 避免假设所有值都有限（finite）。
