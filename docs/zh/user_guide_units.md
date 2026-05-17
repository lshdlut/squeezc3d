# 单位与缩放

30 秒版本：

- `POINT:UNITS` 是单位标签（元数据），默认按原始单位返回点坐标。
- 默认不会偷偷把点坐标归一化到 `m`。
- Streaming 模式和 chunk build 中，如果你确实想要换算单位，可以显式指定目标单位。

形象化：单位就是尺子的刻度（mm/cm/m）；默认不偷偷换尺子；如果遇到 `POINT:UNITS` 缺失/未知，会按约定 fallback，但会在 meta 里留下痕迹（`meta.point_units_source`）。

## POINT:UNITS

`sqzc3d` 将 `POINT:UNITS` 视作描述点坐标长度单位的元数据。

在 streaming 模式下：

- 如果 `POINT:UNITS` 缺失，`sqzc3d` 会假设 `mm`（并设置 `meta.point_units_source = 1`）。
- 如果 `POINT:UNITS` 存在但 token 未知，`sqzc3d` 会假设 `mm`（并设置 `meta.point_units_source = 2`）。

## 不做隐式归一化

默认情况下，`sqzc3d` 不会将点坐标隐式归一化到 `m`。

streaming 模式与 chunk build 默认返回 source units（通常为 `mm`）。

## 显式缩放

如果下游需要特定单位，可以显式请求：

```cpp
#include "sqzc3d_c3d_stream.h"

sqzc3d::C3dStreamReader r = {};
sqzc3d::sqzc3d_c3d_stream_open_file(&r, "trial.c3d");

// Convert subsequent xyz reads to meters.
sqzc3d::sqzc3d_c3d_stream_set_target_unit(&r, "m");

double xyz[3 * 132];
sqzc3d::sqzc3d_c3d_stream_read_frame_all_xyz(&r, 0, xyz, (int)(sizeof(xyz) / sizeof(xyz[0])));

sqzc3d::sqzc3d_c3d_stream_close(&r);
```

支持的单位 tokens：

- `mm`、`cm`、`m`、`km`（大小写不敏感；忽略空白）

高级：

- `sqzc3d::sqzc3d_c3d_stream_set_target_units_per_meter(&r, 1.0)` 表示 meters。
- `sqzc3d::sqzc3d_c3d_stream_set_target_units_per_meter(&r, 1000.0)` 表示 millimeters。

Chunk build 复用同一套单位 parser：

```cpp
sqzc3d_build_opt_t opt = {};
sqzc3d_default_build_opt(&opt);
opt.target_unit = "m";
sqzc3d_build_chunks(dec, &opt, &chunk);
```

`target_unit` 只缩放 `chunk->points_xyz`。`chunk->points_residual` 是 source point units 下的 raw decoded
residual，不会被缩放。单位上下文记录在：

- `chunk->point_units_per_meter`
- `chunk->target_units_per_meter`
- `chunk->residual_units_per_meter`
- `chunk->point_units_source`
