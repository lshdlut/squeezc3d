# 错误与诊断

本页描述 Python 与 C API 的错误报告方式。

30 秒版本：

- Python 失败会抛 `RuntimeError`。
- C API 通常返回 `sqzc3d_STATUS_*`；用 `sqzc3d_last_error(...)` 拿到可读错误信息。
- 如果某个 build 缺失能力，可能会遇到 `sqzc3d_STATUS_NOT_IMPLEMENTED`。

形象化：把错误当成“可行动的提示”，不要把失败当作“默默降级成功”。

## Python

Python bindings 在失败时会抛出 `RuntimeError`。

示例：

```python
import sqzc3d

try:
    v = sqzc3d.read("missing.c3d")
except RuntimeError as e:
    print("failed:", e)
```

功能探测（feature discovery）：

```python
import sqzc3d

features = sqzc3d.features()
print("features=", features)
```

如果某个 build 缺失能力，API 可能以 `sqzc3d_STATUS_NOT_IMPLEMENTED` 失败（Python 中会体现为异常）。

## C API

大多数 API 返回一个 `sqzc3d_STATUS_*` code（见 `sqzc3d_types.h`）。

当调用失败时，使用：

- `sqzc3d_last_error(dec)` 获取可读的错误信息
- `sqzc3d_last_error(NULL)` 用于在 decoder handle 尚不存在时的错误（例如 open 失败）

示例：

```c
sqzc3d_dec_t* dec = NULL;
int st = sqzc3d_open_file(&dec, path, &open_opt);
if (st != sqzc3d_STATUS_SUCCESS) {
  fprintf(stderr, "open failed: %s\n", sqzc3d_last_error(NULL));
  return 1;
}
```

结构化错误详情（可选）：

- `sqzc3d_last_error_detail(dec, &detail)`
