# 正确性与兼容性 — Correctness & Compatibility

本页回答一个很现实的问题：

> 如果我把 `ezc3d` 替换成 `sqzc3d`，只用于“把 C3D 读成数组（points/analogs）”，会不会在不知情的情况下读出不一样的数据？

`sqzc3d` 并不打算做 `ezc3d` 的“对象图克隆”。它更像是一个 **数组交付器**：

- 交付可预测的数组（`points/analogs` + 显式 `*_valid` mask）
- 交付清晰的 selector / index-space 语义契约
- 并且在 native + WASM 环境下保持一致行为

因此，这里的“正确性”也按这个范围来定义。

## 给初级使用者的 30 秒版本

TL;DR：

如果你只是想快速判断“能不能放心换”，可以按下面顺序看：

- **我最关心：换库后读出来的数组有没有变** → 先跑 `verify.py native` 或 stress 的 `G0`（它就是“sqzc3d vs ezc3d 的对照表”）。
- **我会按 label 选点 / 只想要部分 markers** → 看 `S03/S05/S06`（选择器与编号体系）。
- **我只读一段 frames（滑窗/抽帧）** → 看 `S04`（窗口读取等价于切片）。
- **我担心单位/缩放** → 看 `S07`（默认会把差异讲清楚，需要时再 `--strict-unit`）。
- **我担心丢点/无效样本** → 看 `S08`（`*_valid` 的语义是否可靠）。
- **我想缓存加速** → 看 `S11`（bundle 往返）。
- **我从 bytes 读取（网络/压缩包/浏览器）** → 看 `S12`（`open_memory`）。
- **我担心坏文件会“读出来但其实错”** → 看 `S15`（病理输入要失败得足够响）。

## 术语小抄

下面这些术语会反复出现，但你可以把它们理解得很“生活化”：

| 术语 | 通俗解释 |
| --- | --- |
| materialize | 一次性把 C3D 这“本文件”倒进内存，变成数组（像把 Excel 读成 NumPy）。 |
| selector | “点名”你要哪些点/通道（labels / indices）。 |
| index-space | “编号体系”：同一个点可能同时有“原文件编号”和“读出来后的编号”。 |
| window read | 只读一段 frames（像只解码视频的一段，不应改变内容）。 |
| `*_valid`（validity mask） | 每个样本旁边的“是否可信”贴纸/开关：无效样本不要拿来当真。 |
| units / scaling | 单位标签（mm/cm/m）以及是否做数值换算（默认倾向不偷偷换算）。 |
| `meta_tree` | C3D 的参数树（像“设置菜单/元数据字典”）。 |
| bundle | `sqzc3d` 的缓存包（把结果打包，下一次直接加载）。 |
| stress | 按场景跑的一套“契约体检”（输出 PASS/WARN/FAIL，可选 JSON 报告）。 |
| `--strict-*` | 把某些“默认只报告差异”的项目，升级为硬门槛（不满足就 FAIL）。 |
| oracle | 对照/参考答案（这里用 `ezc3d` 当 Oracle）。 |

## 预期结果

- 在“相同解释策略”下（单位、validity 等），`sqzc3d` 输出的 points/analogs 与 `ezc3d`（作为对照/Oracle）应一致，避免“悄悄的数值漂移”。
- 对于用户肉眼容易辨析但可能有心智差异的点（例如单位、validity gate（无效样本如何处理/是否参与对比）），stress（场景化压力测试）默认倾向于**清晰报告**而不是让你“莫名 FAIL”；需要更严格时再通过 flags 把它们升级为硬失败（例如 `--strict-unit` / `--strict-validity`）。
- 可选能力（例如 `meta_tree`、bundles）在缺依赖或未启用构建选项时，应显式降级（WARN/跳过/提示），而不是静默成功。

## 验证范围

覆盖（对外文档承诺的契约）：

- C3D → materialize 为数组（Python/C/C++；也就是“读出来就是 NumPy/连续数组”）。
- 从文件或从内存打开（`open_memory`；也就是“从 bytes 读，不一定要落盘”）。
- Selectors（`None`/`[]`/labels/indices；也就是“点名要哪些点/通道”）与窗口读取（`start_frame`/`frame_count`；也就是“只读一段 frames”）。
- Validity masks（`*_valid`；也就是“哪些样本可信/不可信”的贴纸）与缺失样本的处理。
- units 元数据（`POINT:UNITS`；单位标签）与缩放策略（默认不偷偷换算）。
- type-groups（marker-set 语义；也就是“这一组 markers 属于同一套标记系统/人体模型约定”）。
- `meta_tree`（EZ 的参数树；像“设置菜单/元数据字典”；仅在 `SQZC3D_WITH_EZC3D=ON` 时）。
- bundles（`export_bundle` / `load_bundle`；像“缓存包”；strict vs best-effort）。

不覆盖（设计目标之外）：

- 写入/编辑 C3D。
- 更高层的生物力学语义（例如力台后处理、步态事件等）。

## 自证入口

所有公开的自证工具都在源码仓库的 `samples/verify/`：

- `python samples/verify/verify.py native` — 最像日常用法：读成数组，并用 `ezc3d` 做对照（points/analogs/labels/meta/meta_tree）。
- `python samples/verify/verify.py wasm` — 验证浏览器/WASM 也能稳定读（Playwright；`open_file` + `open_memory`）。
- `python samples/verify/verify.py stress` — 按“场景契约”给你一张体检单（PASS/WARN/FAIL + 可选 JSON 报告）。
- `python samples/verify/verify.py cpp` — 更硬核的 C++ 正确性矩阵（尤其适合 bundle + index-space 的硬校验）。

快速复现：

```bash
python samples/verify/verify.py all --c3d path/to/c3d_dir --wasm-dir path/to/wasm_out --limit 5
python samples/verify/verify.py stress --c3d path/to/c3d_dir --sample 0 --report stress_report.json
```

## 解释：如何理解结果

你可以把 stress 输出当成一张“体检单”：每个 Scenario 就是一项体检。

- **PASS**：该场景的契约在本次输入上成立（或该文件因“不具备该特征”而被跳过）。
- **PASS + "skipped: ..."**：正常，表示“这个文件没有这个特性”（例如没有 analog 通道）。
- **WARN**：无法对比（缺依赖）或观察到潜在易踩坑行为，但未判定为硬失败。
- **FAIL**：契约被破坏，或对照（Oracle）不一致。

提示：如果你的数据集不包含 analog / 参数矩阵等特性，对应场景会显示 skipped；想尽量覆盖全部场景，建议准备“points-only + analog-heavy”混合数据。

如果你看到 **FAIL**，通常按这个顺序定位最快：

1. 先加 `--report stress_report.json`，打开报告看“是哪一个文件 + 哪一项契约”触发。
2. 用 `--scenarios <ID>` 单独重跑那一个场景（不要一次跑全套，先把问题缩小）。
3. 如果 FAIL 来自“策略差异”（单位/validity），优先看报告里有没有提示，并决定是否启用 `--strict-unit` / `--strict-validity` 作为硬门槛。
4. 如果 FAIL 来自 `meta_tree`，确认构建是否启用了 `SQZC3D_WITH_EZC3D=ON`（否则可能只能 WARN/跳过对照）。

## Stress 场景

读法：预设 → 契约 → stress 方式 → 结果（尽量大白话）。

完整 spec 在 `samples/verify/STRESS_PLAN.md`，入口是 `python samples/verify/verify.py stress`。

### G0 — 混合数据集回归

- 预设：一个包含不同来源 C3D 的文件夹（points-only 与 analog-heavy 混合）。
- 契约：核心 payload 与 `ezc3d` 一致（points/analogs/labels + core meta + meta_tree）。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios G0`
- 结果：PASS 表示“在同一解释规则下，没有静默数值漂移”。
- 形象化：同一份试验数据交给两位“翻译”（`sqzc3d`/`ezc3d`），逐项对照；PASS = 翻译一致。

### S01 — 仅 points 的 materialize

- 预设：一个不含 analog 通道的 trial。
- 契约：points 数组 shape 稳定；analogs 数组为空但“形状合法”。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S01`
- 结果：PASS 是预期；含 analog 的文件会被跳过。
- 形象化：这次只检查“坐标表”，不检查“传感器表”；没传感器就应该是空表，不该乱填。

### S02 — 带 analog 的 materialize

- 预设：存在 analog 通道（例如 EMG / 力台）。
- 契约：analogs 数组形状与索引一致（`layout="CN"`），且存在匹配的 `*_valid` mask。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S02`
- 结果：PASS 是预期；不含 analog 的文件会被跳过。
- 形象化：除了“坐标表”，还要交付一张“连续传感器采样表”；行列对齐、缺失要标出来。

### S03 — Selector 语义

- 预设：按 label 请求一小组 markers（包含“反向顺序”的请求）。
- 契约：`None` 表示 ALL，`[]` 表示 empty，并且按 label 选择是**保序**的。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S03`
- 结果：PASS 表示“label selection 可预测”（也对应 `user_guide_selectors.md`）。
- 形象化：点菜按你点的顺序上菜；`None` 像“来一份全家桶”，`[]` 像“什么都不点”。

### S04 — 窗口读取与切片等价

- 预设：只读取一个窗口（`start_frame`, `frame_count`），而不是一次读完整条 trial。
- 契约：窗口读取 == “整条 materialize 后再切片”，包括边界值（`frame_count=0`, `-1`）。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S04`
- 结果：PASS 表示“窗口读取是安全的优化”（也对应 `user_guide_materialize_vs_streaming.md`）。
- 形象化：只解码视频的某一段，不应该导致画面内容变了（只是少读了一段）。

### S05 — indices vs labels

- 预设：用 indices 与 labels 两种方式选择 points，并期待读到同一组数据。
- 契约：两条路径在 values 与 index-space 映射（`point_indices_total`）上完全一致。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S05`
- 结果：PASS 表示“不存在 off-by-one 或错 index space”（也对应 `user_guide_selectors.md`）。
- 形象化：同一栋楼既可以按“房间号”找，也可以按“名字”找；换一种叫法不应走错门。

### S06 — Label 归一化

- 预设：labels 存在空格 / 大小写等差异。
- 契约：`label_norm` 策略行为与文档一致且稳定。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S06`
- 结果：PASS 表示“label 匹配规则可解释、可测试”。
- 形象化：同一个人名写法不统一（空格/大小写），先按规则统一，再去匹配才不容易踩坑。

### S07 — Units 与缩放

- 预设：包含 `POINT:UNITS=mm/cm/m`（或缺失/未知）的 C3D。
- 契约：默认点坐标尺度是**可解释**的；单位差异可通过 flags 变严格。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S07 --unit-contract auto`
- 结果：PASS 通常意味着“默认行为与你的 `ezc3d` 输出尺度一致”；不一致时会在报告中清楚说明差异与失败路径。
- 形象化：单位像尺子的刻度（mm/cm/m）；默认不偷偷换尺子，但会把“尺子不一致”写到报告里。

细节参见 `user_guide_units.md`。

### S08 — Validity 策略

- 预设：真实试次中存在 gaps / 丢点等情况。
- 契约：`*_valid` mask 自洽，且在约定的 validity 策略下对比数值。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S08`
- 结果：PASS 表示“你可以把 `*_valid` 作为唯一可信来源”。
- 形象化：每个样本旁边有一张“合格/不合格”贴纸（validity mask）；对比前先看贴纸，再看数值。

细节参见 `user_guide_valid.md`。

### S09 — Analog scaling

- 预设：analog 通道带有校准参数。
- 契约：在同一文件上，缩放后的 analog 数值与 `ezc3d` 一致。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S09`
- 结果：PASS 是预期；不含 analog 的文件会被跳过。
- 形象化：传感器原始读数要“乘比例/加偏置”才是可用物理量；缩放规则错了就会整体偏。

### S10 — `meta_tree`

- 预设：参数值是矩阵/多维数组（例如 corners/origin 等）。
- 契约：参数树结构与 value 的顺序稳定，并可与 `ezc3d` 对照。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S10`
- 结果：PASS 表示“参数树不会悄悄漂移”（需要 `SQZC3D_WITH_EZC3D=ON`）。
- 形象化：设置菜单里有二维表格（矩阵参数），行列一旦错位就会“看着像对了，其实不对”。

### S11 — Bundles roundtrip

- 预设：把 chunk 缓存成 bundle（`.sqzc3d`）并重新加载。
- 契约：export → load 往返保持 meta + payload；strict 模式能拒绝不兼容 bundle。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S11`
- 结果：PASS 表示“bundle 可安全用于缓存”。若 Python 构建缺少 `export_bundle`，该场景会降级为 WARN。
- 形象化：把结果打包成“缓存箱”，下次开箱即用；strict 模式像“验封条”，封条不对就拒收。

细节参见 `user_guide_bundles.md`。

### S12 — `open_memory`

- 预设：从 bytes 读取（例如网络/归档/WASM FS）。
- 契约：`open_memory` 与读取同一路径文件的行为一致。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S12`
- 结果：PASS 表示“bytes-in == file-in”。
- 形象化：同一份内容，从硬盘读还是从网络 bytes 读，读出来都应该是同样的“内容”。

### S13 — Streaming

- 预设：你确实想要“真 streaming”接口。
- 契约：Python 暂不暴露专门的 streaming API；窗口读取（S04）通常能覆盖多数采样/降采样需求。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S13`
- 结果：信息性（PASS）+ 使用建议（不会把“缺少 Python streaming API”当成兼容性硬门槛）。
- 形象化：真 streaming 像边播放边取帧；当前 Python 更像“按窗口取帧”（S04）。

### S14 — Feature gating

- 预设：构建时禁用部分可选组件。
- 契约：feature bits 可查询且与实际行为一致。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S14`
- 结果：PASS 表示“开关状态与行为一致”；禁用功能时应明确提示，而不是悄悄做错事。
- 形象化：像选装包——没装就明确说“没有这项功能”，而不是表面能点、实际乱来。

### S15 — 病理输入与诊断

- 预设：截断/损坏的 C3D。
- 契约：应当“失败得足够响”，并给出可行动的诊断信息（不允许静默成功）。
- Stress：`python samples/verify/verify.py stress --c3d <dir> --scenarios S15`
- 结果：PASS 表示“坏输入不会产出坏数据”。
- 形象化：拿到一本缺页的书，应该直接报错，而不是把缺页内容编出来。

细节参见 `user_guide_errors.md`。

## 当前版本快照

这是一份“某一时点”的快照，帮助读者建立预期。你可以把它理解为“我们当时跑过的一张体检单”。
最新结论以你在本地跑出的结果为准（尤其是你自己的数据集）。

快照元数据：

- 日期：**2026-02-27**
- `sqzc3d`：**v0.3.4**（Python bindings）
- `ezc3d`（Python oracle）：**1.6.0**
- 平台：Windows（native + WASM）

### 入口结果

| Check | 预期 | 快照 |
| --- | --- | --- |
| `verify.py native` | 在抽样文件上 PASS | PASS |
| `verify.py wasm` | `open_file` + `open_memory` smoke PASS | PASS |
| `verify.py stress` | PASS（可能带 skipped 备注） | PASS |

### 场景快照

| Scenario | 预期 | 快照 |
| --- | --- | --- |
| G0 | PASS | PASS |
| S01 | PASS（跳过非 points-only 文件） | PASS |
| S02 | PASS（跳过不含 analog 的文件） | PASS |
| S03 | PASS | PASS |
| S04 | PASS | PASS |
| S05 | PASS | PASS |
| S06 | PASS | PASS |
| S07 | PASS（或清晰报告；strict 可选） | PASS |
| S08 | PASS（或在严格策略不同情况下 WARN） | PASS |
| S09 | PASS（跳过不含 analog values 的文件） | PASS |
| S10 | PASS（需要 `WITH_EZC3D=ON`） | PASS |
| S11 | PASS（或 Python 缺 `export_bundle` 时 WARN） | PASS |
| S12 | PASS | PASS |
| S13 | 信息性 | PASS |
| S14 | PASS | PASS |
| S15 | PASS | PASS |
