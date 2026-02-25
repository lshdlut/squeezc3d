# C

This page shows a minimal end-to-end C flow:

1. Open a C3D file (`sqzc3d_open_file`).
2. Materialize a chunk (`sqzc3d_build_chunks`).
3. Obtain a view over the points payload (`sqzc3d_points_view_frames`).

```c
#include "sqzc3d.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: demo <path-to-c3d>\\n");
    return 1;
  }

  const char* path = argv[1];

  sqzc3d_open_opt_t open_opt;
  sqzc3d_default_open_opt(&open_opt);

  sqzc3d_dec_t* dec = NULL;
  int st = sqzc3d_open_file(&dec, path, &open_opt);
  if (st != sqzc3d_STATUS_SUCCESS) {
    fprintf(stderr, "open failed: %s\\n", sqzc3d_last_error(NULL));
    return 2;
  }

  sqzc3d_build_opt_t build_opt;
  sqzc3d_default_build_opt(&build_opt);
  build_opt.frame_range.start = 0;
  build_opt.frame_range.count = -1;  // all frames
  build_opt.point_sel_mode = sqzc3d_POINT_SEL_ALL;
  build_opt.analog_enable = sqzc3d_ANALOG_EN_ON;  // or OFF

  sqzc3d_chunk_t* chunk = NULL;
  st = sqzc3d_build_chunks(dec, &build_opt, &chunk);
  if (st != sqzc3d_STATUS_SUCCESS) {
    fprintf(stderr, "build failed: %s\\n", sqzc3d_last_error(dec));
    (void)sqzc3d_close_dec(dec);
    return 3;
  }

  const int n_frames = sqzc3d_chunk_num_frames(chunk);
  const int n_points = sqzc3d_chunk_num_points(chunk);
  printf("frames=%d points=%d\\n", n_frames, n_points);

  sqzc3d_points_view_t view;
  st = sqzc3d_points_view_frames(chunk, 0, n_frames, &view);
  if (st != sqzc3d_STATUS_SUCCESS) {
    fprintf(stderr, "view failed: %s\\n", sqzc3d_last_error(dec));
    (void)sqzc3d_free_chunk(chunk);
    (void)sqzc3d_close_dec(dec);
    return 4;
  }

  // view.points_xyz layout: (T, P, 3) frame-major, contiguous.
  // view.points_valid layout: (T, P) uint8.
  if (view.n_frames > 0 && view.n_points > 0) {
    const sqzc3d_num_t x0 = view.points_xyz[0];
    const unsigned char v0 = view.points_valid[0];
    printf("first x=%f valid=%d\\n", (double)x0, (int)v0);
  }

  (void)sqzc3d_free_chunk(chunk);
  (void)sqzc3d_close_dec(dec);
  return 0;
}
```

Notes:

- A selection with count `0` is a valid empty selection (it returns an empty chunk payload).
- Point indices in `sqzc3d_build_opt_t` are in the source-total index space (original C3D label table order).
- Point indices exposed by a materialized chunk (type-groups, views) are chunk-local.

