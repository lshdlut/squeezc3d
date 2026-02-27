# C

This page shows a minimal end-to-end C flow:

1. Open a C3D file (`sqzc3d_open_file`).
2. Materialize a chunk (`sqzc3d_build_chunks`).
3. Use the chunk arrays directly (fixed layout contract).

Quick summary:

- `sqzc3d_build_chunks(...)` gives you contiguous arrays you can index directly.
- Always pair data arrays with their `*_valid` masks when you care about missing samples.
- Remember there are two index spaces: source-total vs chunk-local.

Mental model: decode once, then treat `chunk` as “a set of C arrays + metadata”.

```c
#include "sqzc3d.h"

#include <stdio.h>

int main(int argc, char** argv) {
  const char* path = argv[1];  // assumes argv[1] is a valid path

  sqzc3d_dec_t* dec = NULL;
  if (sqzc3d_open_file(&dec, path, NULL) != sqzc3d_STATUS_SUCCESS) return 1;

  sqzc3d_build_opt_t opt;
  sqzc3d_default_build_opt(&opt);  // defaults: all frames, all points, analog on
  // opt.analog_enable = sqzc3d_ANALOG_EN_OFF;  // optional: disable analog materialization

  sqzc3d_chunk_t* chunk = NULL;
  if (sqzc3d_build_chunks(dec, &opt, &chunk) != sqzc3d_STATUS_SUCCESS) {
    (void)sqzc3d_close_dec(dec);
    return 2;
  }

  const int n_frames = sqzc3d_chunk_num_frames(chunk);
  const int n_points = sqzc3d_chunk_num_points(chunk);
  printf("frames=%d points=%d\\n", n_frames, n_points);

  // points_xyz layout: (T, P, 3) float64, frame-major, contiguous.
  // points_valid layout: (T, P) uint8, frame-major, contiguous.
  // element index: ((t * P + p) * 3 + k) where k in {0,1,2} for x,y,z.
  if (chunk->points_xyz && n_frames > 0 && n_points > 0) {
    printf("first x=%f\\n", (double)chunk->points_xyz[0]);
  }

  // One marker by label (no copies; just label->index lookup + strided access).
  // Note: point indices from label lookup are chunk-local indices.
  const char* want = "LANK";
  int p = -1;
  (void)sqzc3d_point_indices_for_labels(chunk, &want, 1, &p, -1);
  if (p >= 0 && n_frames > 0) {
    const size_t off = (size_t)p * 3;  // frame 0
    printf("LANK x0=%f\\n", (double)chunk->points_xyz[off + 0]);
  }

  // One analog channel by label (no copies; analog is channel-major contiguous).
  const char* awant = "EMG1";
  int a = -1;
  (void)sqzc3d_analog_indices_for_labels(chunk, &awant, 1, &a, -1);
  if (a >= 0 && chunk->analog) {
    const int N = chunk->n_frames * chunk->n_analog_by_frame;
    const sqzc3d_num_t* emg1 = chunk->analog + (size_t)a * (size_t)N;
    printf("EMG1 first=%f\\n", (double)emg1[0]);
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
