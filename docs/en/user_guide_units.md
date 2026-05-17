# Units and scaling

Quick summary:

- `POINT:UNITS` is metadata describing the length unit of point coordinates.
- By default, `sqzc3d` does not implicitly normalize point coordinates to meters.
- In streaming mode and chunk building, you can request an explicit target unit when you want conversion.

Mental model: units are ruler marks (mm/cm/m). Default is “don’t silently swap rulers”.

## POINT:UNITS

`sqzc3d` treats `POINT:UNITS` as metadata describing the length unit of point coordinates.

In streaming mode:

- If `POINT:UNITS` is missing, `sqzc3d` assumes `mm` (and sets `meta.point_units_source = 1`).
- If `POINT:UNITS` is present but unknown, `sqzc3d` assumes `mm` (and sets `meta.point_units_source = 2`).

## No implicit normalization

By default, `sqzc3d` does not implicitly normalize point coordinates to meters.

Streaming mode and chunk building return source units by default (typically `mm`).

## Explicit scaling

If downstream code wants a specific unit, it can request it explicitly:

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

Supported unit tokens:

- `mm`, `cm`, `m`, `km` (case-insensitive; whitespace ignored)

Advanced:

- `sqzc3d::sqzc3d_c3d_stream_set_target_units_per_meter(&r, 1.0)` means meters.
- `sqzc3d::sqzc3d_c3d_stream_set_target_units_per_meter(&r, 1000.0)` means millimeters.

Chunk building uses the same unit parser:

```cpp
sqzc3d_build_opt_t opt = {};
sqzc3d_default_build_opt(&opt);
opt.target_unit = "m";
sqzc3d_build_chunks(dec, &opt, &chunk);
```

`target_unit` only scales `chunk->points_xyz`. `chunk->points_residual` is raw decoded residual in source
point units and is not scaled. Metadata fields record the unit context:

- `chunk->point_units_per_meter`
- `chunk->target_units_per_meter`
- `chunk->residual_units_per_meter`
- `chunk->point_units_source`
