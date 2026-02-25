# Bundles（`.sqzc3d`）

`sqzc3d` 支持把 materialize 后的 chunk 持久化为一个紧凑的 “bundle” 格式，用于快速重新加载。

支持两种 bundle 布局：

- 目录 bundle（legacy）：目录内包含 `meta.json` + `data.bin`
- 单文件 bundle（推荐）：`*.sqzc3d`

## Python

Easy 入口 `sqzc3d.read(...)` 会自动识别 bundles：

- 若 `source` 是目录，则加载目录 bundle。
- 若 `source` 以 `.sqzc3d` 结尾（大小写不敏感），则加载单文件 bundle。
- 否则将其视作 C3D path/buffer 并 materialize 出 chunk。

```python
import sqzc3d as sq

v = sq.read("trial.sqzc3d")     # 加载单文件 bundle
v = sq.read("bundle_dir/")      # 加载目录 bundle
v = sq.read("trial.c3d")        # 从 C3D materialize

print(v.meta["n_frames"], v.meta["n_points"])
```

严格性：

```python
v = sq.read("trial.sqzc3d", bundle_strict=True)   # 默认
v = sq.read("trial.sqzc3d", bundle_strict=False)  # best-effort（尽量加载）
```

## C API

Export：

```c
// Writes <out_dir>/meta.json and <out_dir>/data.bin.
int st = sqzc3d_export_bundle(out_dir, chunk);
```

Load：

```c
sqzc3d_chunk_t* chunk = NULL;
int st = sqzc3d_load_bundle(bundle_path_or_dir, &chunk);
```

备注：

- bundle 会加载成 `sqzc3d_chunk_t*`，因此可复用相同的 view/query/free APIs。
- bundle 可能包含额外元数据；若出现不匹配，可使用 `chunk->reason` / `sqzc3d_last_error_detail(...)` 进行诊断。
