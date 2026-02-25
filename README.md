# squeezc3d (abbrev: `sqzc3d`)

[English](README.md) | [简体中文](README.zh-CN.md)

[![CI](https://github.com/lshdlut/squeezc3d/actions/workflows/ci.yml/badge.svg)](https://github.com/lshdlut/squeezc3d/actions/workflows/ci.yml)

`squeezc3d` (abbrev `sqzc3d`) is a high-performance C3D loading library focused on fast chunk materialization and
predictable array semantics.

- Chunk-first: contiguous `(T, P, 3)` points + explicit `valid` masks.
- Clear selector/index-space contracts (labels, type-groups).
- Optional modes: streaming reads and `.sqzc3d` bundles for cached/low-memory workflows.

Links:
- Documentation (English): `docs/en/index.md`
- Documentation (简体中文): `docs/zh/index.md`
- Benchmarks: `docs/en/benchmarks.md`
- Build: `docs/en/build.md`
- API reference (markdown): `docs/en/API.md`

## Install (Python)

```bash
pip install sqzc3d
```

## Quickstart (Python, Easy)

```python
import sqzc3d as sq

v = sq.read("trial.c3d")  # default: points=ALL, analogs=ALL
print(v.meta["n_frames"], v.meta["n_points"], v.meta["n_analogs"])

pts = v.points              # (T, P, 3) float64
pts_valid = v.points_valid  # (T, P) uint8

# By label (single marker / channel)
ank = v.point["LANK"]              # (T, 3)
ank_valid = v.point_valid["LANK"]  # (T,)

emg1 = v.analog["EMG1"]              # (N,)
emg1_valid = v.analog_valid["EMG1"]  # (N,)
```

## Quickstart (C)

```c
#include "sqzc3d.h"

#include <stdio.h>

int main(int argc, char** argv) {
  const char* path = argv[1];  // assumes argv[1] is a valid path

  sqzc3d_dec_t* dec = NULL;
  if (sqzc3d_open_file(&dec, path, NULL) != sqzc3d_STATUS_SUCCESS) return 1;

  sqzc3d_build_opt_t opt;
  sqzc3d_default_build_opt(&opt);

  sqzc3d_chunk_t* chunk = NULL;
  if (sqzc3d_build_chunks(dec, &opt, &chunk) != sqzc3d_STATUS_SUCCESS) {
    (void)sqzc3d_close_dec(dec);
    return 2;
  }

  printf("frames=%d points=%d\\n", chunk->n_frames, chunk->n_points);
  printf("first x=%f\\n", (double)chunk->points_xyz[0]);

  (void)sqzc3d_free_chunk(chunk);
  (void)sqzc3d_close_dec(dec);
  return 0;
}
```

## Benchmark summary (materialize)

`speedup_x = ezc3d / sqzc3d` (higher is better for `sqzc3d`).

| Case | Language | `load_ms` `speedup_x` | `peak_rss_mb` `speedup_x` |
| --- | --- | ---: | ---: |
| PFERD (117.96 MB) | C++ | 11.4x | 5.5x |
| PFERD (117.96 MB) | Python | 14.8x | 6.6x |
| DOG (4.23 MB) | C++ | 9.0x | 3.5x |
| DOG (4.23 MB) | Python | 13.0x | 2.9x |

Full methodology and results: `docs/en/benchmarks.md`.

## Acknowledgements

`squeezc3d` builds upon [`ezc3d`](https://github.com/pyomeca/ezc3d) for C3D parsing and parameter tree handling.
Thanks to the `ezc3d` authors and the Pyomeca community.

## Generative AI usage

Parts of this repository (code/tests/docs) were developed with the assistance of generative AI tools.
All changes are reviewed by maintainers; please report issues if you spot inconsistencies.

## License

`squeezc3d` (abbrev `sqzc3d`) is released under **MIT**.
`ezc3d` upstream license is **MIT**.

## License & third-party notices

See `LICENSE` and `NOTICE` for dependency/license notes, `DEPENDENCIES.md` for build requirements.
