# C

本页展示一个最小端到端的 C 流程：

1. 打开 C3D 文件（`sqzc3d_open_file`）。
2. materialize（内存化）出一个 chunk（`sqzc3d_build_chunks`）。
3. 直接使用 chunk 数组（固定布局契约）。

```c
#include "sqzc3d.h"

#include <stdio.h>

int main(int argc, char** argv) {
  const char* path = argv[1];  // 假设 argv[1] 是有效路径

  sqzc3d_dec_t* dec = NULL;
  if (sqzc3d_open_file(&dec, path, NULL) != sqzc3d_STATUS_SUCCESS) return 1;

  sqzc3d_build_opt_t opt;
  sqzc3d_default_build_opt(&opt);  // 默认：全帧、全点、analog 开启
  // opt.analog_enable = sqzc3d_ANALOG_EN_OFF;  // 可选：禁用 analog materialization

  sqzc3d_chunk_t* chunk = NULL;
  if (sqzc3d_build_chunks(dec, &opt, &chunk) != sqzc3d_STATUS_SUCCESS) {
    (void)sqzc3d_close_dec(dec);
    return 2;
  }

  const int n_frames = sqzc3d_chunk_num_frames(chunk);
  const int n_points = sqzc3d_chunk_num_points(chunk);
  printf("frames=%d points=%d\\n", n_frames, n_points);

  // points_xyz 布局：(T, P, 3) float64，按 frame-major 连续存放。
  // points_valid 布局：(T, P) uint8，按 frame-major 连续存放。
  // 元素索引：((t * P + p) * 3 + k)，k∈{0,1,2} 分别对应 x,y,z。
  if (chunk->points_xyz && n_frames > 0 && n_points > 0) {
    printf("first x=%f\\n", (double)chunk->points_xyz[0]);
  }

  // 按 label 取单个 marker（不拷贝；label->index + 跨帧步进访问）。
  // 注意：label 查询返回的是 chunk-local 索引。
  const char* want = "LANK";
  int p = -1;
  (void)sqzc3d_point_indices_for_labels(chunk, &want, 1, &p, -1);
  if (p >= 0 && n_frames > 0) {
    const size_t off = (size_t)p * 3;  // frame 0
    printf("LANK x0=%f\\n", (double)chunk->points_xyz[off + 0]);
  }

  // 按 label 取单个 analog channel（不拷贝；analog 是 channel-major 连续存放）。
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

备注：

- count 为 `0` 的选择器是合法的空选择（返回空的 chunk payload）。
- `sqzc3d_build_opt_t` 里的 point indices 使用 source-total index space（原始 C3D label table 的顺序）。
- materialized chunk 暴露出来的 indices（type-groups、views 等）是 chunk-local。
