# sqzc3d stress plan (vs ezc3d)

目的：把“是否可替换 ezc3d（在 **sqzc3d 覆盖的主流 read/materialize 场景内**）”拆成一组可复用、可报告的 stress 场景。

本文件只描述 **预设场景 → 需求（契约）→ stress 方式**。实现可以落在：

- Python: `samples/verify/_impl/smoke_vs_ezc3d.py`（native 对照）
- C++: `samples/verify/cpp/verify_correctness_matrix_sqzc3d.cpp`（C API + bundle 矩阵）
- WASM: `samples/verify/_impl/playwright_wasm_sqzc3d_open_modes.py`（open_file/open_memory 烟测）

> 约定：除非场景明确写“metamorphic/self-oracle”，否则以 `ezc3d` 为对照 oracle（同一份 `.c3d`，按统一归一化规则比对）。

---

## 0. 范围（sqzc3d 视角）

**覆盖主流需求：**

- C3D → materialize：points/analogs + labels + meta/meta_tree（参数树）
- 选择器：`None`/`[]`/labels/indices、窗口读（frame_range / analog_range）
- validity：`*_valid` mask 的语义稳定
- bundles：`export_bundle` + `load_bundle`（strict / non-strict）
- streaming（如果纳入对比）：按 window/frame 读取到用户 buffer
- open_memory：从 bytes/buffer 打开（即使内部走临时文件也要行为正确）

**显式不覆盖（除非以后决定支持）：**

- 写回/编辑 C3D（ezc3d 支持但 sqzc3d 非目标）
- 高阶 biomech 语义计算（例如自动计算 force plate wrench），除非只是要求参数树保真

---

## 1. 对照归一化（让“变态精准”可落地）

`ezc3d` 的 Python surface 有一些“怪形状/怪约定”，为了稳定对照，建议固定以下归一化：

### 1.1 points（ez → sq 统一为 `(T, P, 3)`）

- `ezc3d["data"]["points"]` 常见形状：`(4, P, T)`（第 4 行是 residual，不是坐标）
- 统一转换：
  - `xyz_ez = transpose(ez_points[0:3], (2, 1, 0))` → `(T, P, 3)`
  - `residual_ez = transpose(ez_points[3], (1, 0))` → `(T, P)`（如果存在）

### 1.2 analogs（ez → sq 统一为 `(C, N)`）

- ezc3d analog 常见形状：
  - `(C, N)` 或 `(1, C, N)` 或更复杂的 3D 变体
- 统一转换后：`analog_ez` 为 `(C, N)`，其中 `N = T * S`。

### 1.3 units（points 的长度单位）

这块不建议默认做“硬 fail”的障碍（用户通常一眼能辨别），推荐拆成两层：

- 默认对照（G0/S01/S08…）：保持 **raw units**（跟 `ezc3d["data"]["points"]` 一致），用于做最直接的数值一致性回归。
- S07 单独报告：`sqzc3d` 的默认输出是否与 `ezc3d` 的 raw 输出一致，并同时给出“按 `POINT:UNITS` 归一化到米”的对照 diff（便于定位“有没有做单位缩放/缩放了多少”）。

若未来要把单位策略上升为明确 contract，可通过 `--unit-contract raw/meters` + `--strict-unit` 把 mismatch 提升为 FAIL。

### 1.4 validity（points/analogs）

对照必须先声明 validity 的 oracle，否则会出现“数值相等但 valid 不同”的争论。

建议分两层：

- Level-0（最稳定）：`valid = isfinite(values)`（所有 xyz finite 才 valid；analog 同理）
- Level-1（可选）：若要对齐 residual gate，则把 `residual` + `residual_gate_mm` 纳入 valid 规则

### 1.5 参数树（meta_tree）

`ezc3d` 参数值经常是多维数组 + 行列方向与直觉不同（例如 corners/origin 这类矩阵参数）。

对照策略建议：

- 把 `ezc3d["parameters"]` 先规范成一个“纯 Python dict”（扁平化 list/ndarray/bytes）
- 多维数组：按固定规则 `T + reshape`（或 flatten）变成一维列表再比较
- 允许 string/bytes 的容错解码（utf-8 ignore）

---

## 2. 报告输出（建议）

每个场景输出一份可合并报告（最小字段）：

- `scenario_id`, `scenario_name`
- `inputs`: file list / roots / sample seed
- `versions`: sqzc3d version/ABI、ezc3d version、Python、OS
- `result`: pass/fail + failure list（按文件、按断言类别聚类）
- `metrics`（可选）：`max_abs_diff_points`, `max_abs_diff_analogs`, `load_ms`, `peak_rss_mb`

---

## 3. 场景索引（建议优先级）

| ID | 场景名 | Oracle | 优先级 | 现有覆盖线索 |
| --- | --- | --- | --- | --- |
| G0 | 通用：全库抽样回归 | ezc3d + self | P0 | `verify.py all/native/wasm/cpp` |
| S01 | 基础：points-only materialize | ezc3d | P0 | `smoke_vs_ezc3d.py` |
| S02 | 基础：含 analog materialize | ezc3d | P0 | `smoke_vs_ezc3d.py` |
| S03 | 选择器语义：`None` / `[]` / labels 顺序 | spec + ezc3d | P0 | `smoke_vs_ezc3d.py`（部分） |
| S04 | 窗口读：`start_frame/frame_count` 等价性 | metamorphic + ezc3d | P0 | 需补（Python） |
| S05 | indices vs labels（index-space 契约） | spec + ezc3d | P0 | `cpp matrix`（C）；Python 需补 |
| S06 | label normalization（TRIM/CASEFOLD_WS） | spec | P1 | 需补 |
| S07 | units & scaling（POINT:UNITS / SCALE / header scale） | ezc3d + spec | P0 | `smoke_vs_ezc3d.py`（隐含） |
| S08 | validity：缺失点/NaN/Inf/残差 gate | spec + ezc3d | P1 | 需补（尤其 residual） |
| S09 | analog scaling（GEN_SCALE/SCALE/OFFSET） | ezc3d | P1 | 需补（依赖含标定文件） |
| S10 | meta_tree：多维参数（FORCE_PLATFORM/EVENT 等） | ezc3d | P1 | `smoke_vs_ezc3d.py`（基础） |
| S11 | bundles：roundtrip 与 strictness | metamorphic | P0 | `cpp matrix`（已做） |
| S12 | open_memory（Python/C/WASM） | metamorphic | P1 | WASM 已有；Python/C 需补 |
| S13 | streaming vs materialize（窗口一致性） | metamorphic | P2 | 需补（C++） |
| S14 | feature gating（WITH_EZC3D=OFF） | spec | P2 | 需补（构建矩阵） |
| S15 | 病理输入：header/params 不一致与错误诊断 | spec | P1 | 需补（数据集/合成） |

> 优先级建议：先把 P0 打到“可回归、可报告”，再扩展 P1/P2。

---

## G0. 通用场景：全库抽样回归（P0）

**预设场景**

- 输入：一个包含大量 `.c3d` 的目录（真实杂乱越好）
- 抽样：固定 seed（保证可复现）
- 分三条线跑：Python native 对照、C++ matrix、WASM open smoke

**需求（契约）**

- 对任意抽样文件：
  - points/analogs/labels/meta（至少 counts）与 oracle 一致
  - 异常必须可诊断：明确报错、返回码一致，不 silent fail
- 结果可用于回归：同版本同输入应稳定

**stress 方式**

- Python native（ezc3d oracle）：
  - 采样 N 个文件，跑 points/analogs/labels/meta/meta_tree 对照
  - 输出：按文件列出 mismatch 类型（shape/value/labels/meta/tree）
- C++ matrix（self-oracle + bundle roundtrip）：
  - 对每个文件跑选择矩阵 + export/load(strict) + index-contract
- WASM：
  - 对每个文件 smoke `open_file`/`open_memory` + build 1 frame

实现入口（现有）：`python samples/verify/verify.py all --c3d <dir> --limit <N>`

---

## S01. 基础：points-only materialize（P0）

**预设场景**

- 输入文件特征：无 analog 或 `ANALOG:USED=0`
- points 有缺失/无缺失均可（最好两类都有）

**需求（契约）**

- `points` shape 必为 `(T, P, 3)`，dtype 为 float（建议 float64）
- `points_valid` shape 必为 `(T, P)`，dtype 为 uint8
- `analogs` / `analogs_valid` 返回空但结构正确（`(0, N)`）
- labels 与 `POINT:LABELS*` 一致（包含 `LABELS2/3...` 拼接）

**stress 方式**

- ezc3d 对照：
  - 归一化 ez points → `(T,P,3)` 后逐元素比较（只在 `valid_mask` 上比）
  - `points_valid` 对照（至少 Level-0：finite）
- 自一致：
  - `read(frame_count=1)` 与 full read slice 的第一帧一致

---

## S02. 基础：含 analog materialize（P0）

**预设场景**

- 输入文件特征：`ANALOG:USED>0` 且存在标定（SCALE/OFFSET/GEN_SCALE）
- 至少覆盖：
  - `S = n_analog_by_frame = 1`
  - `S > 1`（常见：100Hz points + 1000Hz analog）

**需求（契约）**

- `analogs(layout="CN")` shape 为 `(C, T*S)`，dtype float64
- `analogs_valid` 同形状 uint8
- `meta["n_analog_by_frame"]` 与 `ANALOG:RATE/POINT:RATE` 一致（或按数据长度推导的一致规则）

**stress 方式**

- ezc3d 对照：
  - analog 归一化为 `(C, N)` 后逐元素 compare（allclose）
  - 对齐 `n_analog_by_frame` 的推导逻辑并在报告中打印（便于定位“rate 不整除”的边角）

---

## S03. 选择器语义：`None` / `[]` / labels 顺序（P0）

**预设场景**

- 任意含 points/analogs 的文件（最好 labels 数量 > 2）

**需求（契约）**

- `None == ALL`：返回完整点/通道
- `[] == empty`：返回空数组但 shape 语义正确
- labels 选择必须 **保序**：输出顺序与输入 labels 顺序一致
- `View.point["LABEL"]` / `View.analog["LABEL"]` 行为稳定：单 label 输出降维后的 shape 符合文档

**stress 方式**

- spec 驱动断言：
  - `points=None`/`points=[]`/`points=[a,b]` 三类断言 shape + 计数
  - `points=["B","A"]` 时输出第 0 列必须对应 `B`
- ezc3d 对照（可选）：
  - labels → indices 后做相同 gather，再比较值

现有线索：`smoke_vs_ezc3d.py` 已覆盖 `None/[]` 的一部分。

---

## S04. 窗口读：`start_frame/frame_count` 等价性（P0）

**预设场景**

- 任意中等长度 trial（T >= 10）

**需求（契约）**

- `read(start_frame=s, frame_count=k)` 等价于 `full.points[s:s+k]`（同 analog）
- 边界条件：
  - `frame_count=-1` 表示到结尾
  - `s=0,k=0` 返回空 window（不是报错）
  - `s<0` 或 `k< -1` 必须报错（或定义清晰的裁剪策略，但要固定）

**stress 方式**

- metamorphic（自一致）：
  - full read 后 slice vs window read 逐元素 compare
  - 对 analog：注意 `N=T*S` 的映射（window 对齐到样本索引）
- ezc3d（可选）：
  - 用 ez full read slice 作为 oracle（避免两边窗口实现不同导致“巧合相等”）

---

## S05. indices vs labels（index-space 契约）（P0）

**预设场景**

- 任意含 points 的文件
- 至少一个文件满足 `n_points_total > n_points`（通过选择器构造也行）

**需求（契约）**

- C API：`build_opt.point_sel` 的 indices 必须是 **source-total** index-space
- Chunk 暴露的 indices（type-groups / views / label lookup）必须是 **chunk-local**
- 若提供 `sqzc3d_chunk_point_indices_total`：
  - 映射长度 == `n_points`
  - 值域在 `[0, n_points_total)`

**stress 方式**

- C++ matrix（已有）：
  - stride2 indices selection + label selection + bundle roundtrip + strict load
- Python core（建议补）：
  - `chunk.points([0,2,4], copy=True)` 与 `chunk.points(["label0","label2","label4"], copy=True)` 一致（在同一个 chunk-local 空间内）
  - 报告必须明确“source-total vs chunk-local”是哪一层的错误

---

## S06. label normalization（TRIM/CASEFOLD_WS）（P1）

**预设场景**

- 输入文件特征：labels 含尾随空格/大小写混用/多空白（最好包含 `\t`）

**需求（契约）**

- `LABEL_NORM_EXACT`：完全精确匹配（空白/大小写都敏感）
- `LABEL_NORM_TRIM`：去首尾空白后匹配
- `LABEL_NORM_CASEFOLD_WS`：大小写折叠 + 统一空白（契约需写清“空白统一规则”）

**stress 方式**

- spec 驱动：
  - 用同一份文件，对不同 `label_norm` 运行同一组 label query
  - 断言：匹配集合/优先级（当多 label 归一化后冲突时要定义：报错/取第一个/稳定选择）
- 兼容性建议：
  - 把“归一化后冲突”当成一个必须单测的 failure path

---

## S07. units & scaling（P0）

**预设场景**

- 至少准备 4 类文件：
  - `POINT:UNITS = mm/cm/m`（各一）
  - `POINT:UNITS` 缺失或未知 token（触发 fallback）
- 覆盖点数据存储为 int16（header scale）和 float（header scale < 0）两类

**需求（契约）**

- 默认行为可解释：报告 `sqzc3d` 默认 points 是否与 `ezc3d` 的 raw 输出一致；同时给出按 `POINT:UNITS` 归一化后的对照 diff
- 若要把单位策略上升为硬 contract：用 `--unit-contract raw/meters` + `--strict-unit` 将 mismatch 提升为 FAIL
- `point_scale` / `header_scale`（如果暴露）在 meta/Chunk 内一致且可解释
- fallback 规则固定：缺失/未知单位 → 采用 `mm`（或其它，但要固定）

**stress 方式**

- oracle = ezc3d + spec：
  - 用参数树读 `POINT:UNITS`，对照 points 的数量级是否符合契约
  - 对同一份数据：改变 `POINT:UNITS`（如果有可合成文件）应导致确定比例缩放

---

## S08. validity：缺失点/NaN/Inf/残差 gate（P1）

**预设场景**

- 输入文件特征：存在缺失 marker（常见：gap）
- 如果要对齐 residual：需要 residual 有意义的文件（ez points 第 4 行）

**需求（契约）**

- Level-0：只要 xyz 中出现 NaN/Inf → `valid=0`
- 若启用 residual gate：
  - residual 超过阈值（或 residual==0/==-1 等编码）→ `valid=0` 的规则必须写死
- 对无效点的 xyz：允许保留原值/NaN，但必须与 valid 协同（下游只看 valid）

**stress 方式**

- ezc3d 对照：
  - 统计：`valid mismatch count`、`invalid-but-finite`、`valid-but-nonfinite`
  - 若 residual gate 纳入：把 residual 分布与 gate 阈值打印到报告

---

## S09. analog scaling（GEN_SCALE/SCALE/OFFSET）（P1）

**预设场景**

- 输入文件特征：存在 `ANALOG:GEN_SCALE` / `ANALOG:SCALE` / `ANALOG:OFFSET`
- 最好包含 force plate 或 EMG（更贴近真实）

**需求（契约）**

- sqzc3d 输出 analog 的数值应等价于 ezc3d（同一套标定规则）
- `analogs_valid` 的规则必须与 points 一样明确（至少 finite）

**stress 方式**

- ezc3d 对照：
  - 逐元素 allclose
  - 对每个 channel 输出：scale/offset（从参数树读）与数值范围摘要（max/min）

---

## S10. meta_tree：多维参数（FORCE_PLATFORM/EVENT 等）（P1）

**预设场景**

- 输入文件特征：包含丰富参数组（至少一个多维矩阵参数）
  - 例：`FORCE_PLATFORM:CORNERS`（通常是 3x4xN）
  - 例：`EVENT:TIMES`

**需求（契约）**

- `View.meta_tree` 的结构稳定（纯 dict/list/number/string）
- 多维参数值的 flatten/转置规则固定（否则永远 diff）
- 对缺失参数：要么不存在，要么是明确默认值（不能时有时无）

**stress 方式**

- ezc3d 对照：
  - 用统一的 normalize 规则把两边的 meta_tree 变成可比较结构
  - diff 输出要可读：限定最大 diff 条数，并带 path（`GROUP/PARAM/...`）

现有线索：`smoke_vs_ezc3d.py --strict` 已做基础对照，但建议用含 FORCE_PLATFORM/EVENT 的文件强化。

---

## S11. bundles：roundtrip 与 strictness（P0）

**预设场景**

- 任意文件（含/不含 analog 都要覆盖）

**需求（契约）**

- `export_bundle` → `load_bundle`：数据完全一致
- `meta_tree`（参数树）应随 bundle 一起保存并在 load 后可用（尤其 FORCE_PLATFORM 这类参数）
- strict/non-strict 行为固定：
  - strict 必须在不匹配时报错且给出 reason/detail
  - non-strict 允许 best-effort，但不得 silent corruption

**stress 方式**

- metamorphic（自一致）：
  - built vs loaded vs strict_loaded：逐字段/逐元素 compare
  - bundle 文件（`*.sqzc3d`）在成功后应可删除（验证无句柄泄漏）

现有覆盖：C++ matrix 已覆盖（并且做了删除）。

---

## S12. open_memory（Python/C/WASM）（P1）

**预设场景**

- 任意 `.c3d` 文件，读取为 bytes（Python `Path.read_bytes()`）

**需求（契约）**

- `open_file` 与 `open_memory` 的输出一致（同 selection/window；包括 `meta_tree`）
- 即使内部实现走临时文件，也必须：
  - 关闭后可删除临时文件（Windows 尤其关键）
  - 错误信息可诊断（`last_error(NULL)` 在 open 失败时可用）

**stress 方式**

- WASM（已有）：对同一份 bytes 跑两种 open mode
- Python/C 建议补：
  - bytes 路径下跑与 file 路径完全相同的对照

---

## S13. streaming vs materialize（窗口一致性）（P2）

**预设场景**

- 大文件优先（体现 streaming 价值），但也要有小文件便于定位

**需求（契约）**

- streaming 读窗口输出 == materialize 后 slice（同 units/valid 规则）
- streaming 的单位转换（`set_target_unit`）必须是确定比例缩放

**stress 方式**

- metamorphic（自一致）：
  - 对若干随机 window，逐元素 compare
  - 重复读同一 window 100 次（检测状态污染）

---

## S14. feature gating（WITH_EZC3D=OFF）（P2）

**预设场景**

- 构建一个 bundle-only 版本（`SQZC3D_WITH_EZC3D=OFF`）

**需求（契约）**

- `features()`/`sqzc3d_get_features()` 必须准确反映 capability
- `open_file/open_memory/build_chunks` 必须返回 `NOT_IMPLEMENTED`
- bundle API 仍可用（load/export）

**stress 方式**

- spec 驱动：
  - 对每个 gated API 断言返回码 + 错误信息
  - bundle roundtrip 仍然 PASS

---

## S15. 病理输入：header/params 不一致与错误诊断（P1）

**预设场景**

- 输入文件特征（至少覆盖其中几类）：
  - `POINT:USED` 与 labels 数量不一致
  - `ANALOG:RATE` 与 `POINT:RATE` 比例不是整数
  - 缺失关键参数（例如 labels/rate）
  - 截断/损坏文件

**需求（契约）**

- 不允许 silent success：要么正确读出并给警告（可选），要么 fail fast
- 错误必须可定位（API 名/section/index/message）

**stress 方式**

- spec 驱动：
  - 对每类病理文件断言错误码/异常类型 + message 包含关键字段
  - 回归要求：同类错误不应随机变化
