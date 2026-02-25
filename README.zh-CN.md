# squeezc3d（缩写：`sqzc3d`）

[English](README.md) | [简体中文](README.zh-CN.md)

[![CI](https://github.com/lshdlut/squeezc3d/actions/workflows/ci.yml/badge.svg)](https://github.com/lshdlut/squeezc3d/actions/workflows/ci.yml)

`squeezc3d`（缩写 `sqzc3d`）是一个高性能 C3D 加载库，专注于快速 chunk materialization 与可预测的数组语义。

- Chunk-first：连续的 `(T, P, 3)` 点数组 + 显式 `valid` mask。
- 清晰的 selector/index-space 契约（labels、type-groups）。
- 可选能力：streaming 读取与 `.sqzc3d` bundle，用于缓存/低内存等场景。

链接：
- 文档（English）：`docs/index.md`
- 文档（简体中文）：`docs/zh/index.md`
- 基准测试：`docs/zh/benchmarks.md`
- 构建：`docs/zh/build.md`
- API 参考（markdown）：`docs/zh/API.md`

## 安装（Python）

```bash
pip install sqzc3d
```

## 快速上手（Python，Easy）

```python
import sqzc3d as sq

v = sq.read("trial.c3d")  # 默认：points=ALL, analogs=ALL
print(v.meta["n_frames"], v.meta["n_points"], v.meta["n_analogs"])

pts = v.points              # (T, P, 3) float64
pts_valid = v.points_valid  # (T, P) uint8

# 按 label 获取单个 marker/channel
ank = v.point["LANK"]              # (T, 3)
ank_valid = v.point_valid["LANK"]  # (T,)

emg1 = v.analog["EMG1"]              # (N,)
emg1_valid = v.analog_valid["EMG1"]  # (N,)
```

## 快速上手（C）

```c
#include "sqzc3d.h"

#include <stdio.h>

int main(int argc, char** argv) {
  const char* path = argv[1];  // 假设 argv[1] 是有效路径

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

## 基准测试摘要（materialize）

`speedup_x = ezc3d / sqzc3d`（值越大表示 `sqzc3d` 越好）。

| Case | Language | `load_ms` `speedup_x` | `peak_rss_mb` `speedup_x` |
| --- | --- | ---: | ---: |
| PFERD (117.96 MB) | C++ | 11.4x | 5.5x |
| PFERD (117.96 MB) | Python | 14.8x | 6.6x |
| DOG (4.23 MB) | C++ | 9.0x | 3.5x |
| DOG (4.23 MB) | Python | 13.0x | 2.9x |

完整方法与结果：`docs/zh/benchmarks.md`。

## 致谢

`squeezc3d` 使用并改造了 [`ezc3d`](https://github.com/pyomeca/ezc3d) 的能力（C3D 解析与参数树）。
感谢 `ezc3d` 的作者与 Pyomeca 社区。

## 生成式 AI 声明

本仓库的部分内容（代码/测试/文档）由生成式 AI 辅助撰写。
所有变更均由维护者审阅；如发现不一致之处请提交 issue。

## 许可证

`squeezc3d`（缩写 `sqzc3d`）使用 **MIT** 许可证发布。
`ezc3d` 上游许可证为 **MIT**。

## 许可证与第三方声明

依赖/许可证说明见 `LICENSE` 与 `NOTICE`，构建依赖见 `DEPENDENCIES.md`。
