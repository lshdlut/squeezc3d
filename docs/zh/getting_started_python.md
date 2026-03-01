# Python

## 安装

```bash
pip install sqzc3d
```

## 推荐：Easy

`read -> View`：直接把 C3D 读成数组的那层。

30 秒版本：

- 想快速拿到数组 → 直接用 `sqzc3d.read(...)`。
- 按 labels 选择，`*_valid` 当成唯一可信来源。
- 需要更精细控制时再下探到 Core（`Decoder -> Chunk`）。

```python
import sqzc3d as sq

v = sq.read("trial.c3d")  # 默认：points=ALL, analogs=ALL
print(v.meta["n_frames"], v.meta["n_points"], v.meta["n_analogs"])

pts = v.points             # (T, P, 3) float64
pts_valid = v.points_valid # (T, P) uint8

# 按 label 取一个 marker（points）
ank = v.point["LANK"]              # (T, 3)
ank_valid = v.point_valid["LANK"]  # (T,)

# 按 labels 选择子集（Easy surface 是 labels-only）
v.point_labels = ["LASI", "RASI"]
pts2 = v.points  # (T, 2, 3)
assert pts2.shape[1] == 2  # 返回顺序与 point_labels 一致

# 按 label 取一个 analog channel（默认 layout: channel-major (C, N)）
emg1 = v.analog["EMG1"]              # (N,)
emg1_valid = v.analog_valid["EMG1"]  # (N,)

# 禁用 analog 读取（空选择）
v_no_analog = sq.read("trial.c3d", analogs=[])
print(v_no_analog.meta["n_analogs"])  # 0
```

也支持 bundles：

```python
import sqzc3d as sq

v = sq.read("trial.sqzc3d")  # single-file bundle
v = sq.read("bundle_dir/")   # directory bundle (meta.json + data.bin)
```

Python 选择器语义：

- `None` 表示默认（ALL）。
- `[]` 表示空选择。
- `str` 或 `sequence[str]` 表示按 label 选择。

高级出口：

- `View._chunk` 暴露底层 `Chunk`（indices/masks 等属于高级功能，不主推但可用）。

## 进阶：Core

`Decoder -> Chunk`：更精细的窗口/选择器/布局控制。

```python
import sqzc3d

dec = sqzc3d.Decoder("trial.c3d")
chunk = dec.read(start_frame=0, frame_count=-1, points=None, analogs=None)
dec.close()

pts, pts_valid = chunk.points(copy=True)            # (T, P, 3), (T, P)
ana, ana_valid = chunk.analogs(layout="CN")         # (C, N), (C, N)
ana_tcs, _ = chunk.analogs(layout="tcs", copy=True) # (T, C, S), (T, C, S)
```

备注：

- `[]` 是一个合法的空选择器，会返回空数组（不是错误）。
- 非连续选择需要 `copy=True`（API 会避免隐式 gather/copy）。
