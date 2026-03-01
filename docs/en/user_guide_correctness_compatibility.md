# Correctness & Compatibility

This page answers a practical question:

> If I replace `ezc3d` with `sqzc3d` **for reading C3D into arrays**, will I silently get different data?

`sqzc3d` is intentionally *not* an "object-graph clone" of `ezc3d`. Instead, it focuses on delivering:

- predictable arrays (`points/analogs` + explicit `*_valid` masks),
- clear selector / index-space contracts,
- and stable behavior across native + WASM environments.

Correctness is defined **within that scope**.

## 30-second summary

For beginners:

If you only want to answer “can I switch safely?”, start here:

- **Do my arrays change if I switch?** → run `verify.py native` or stress `G0` (a direct `sqzc3d` vs `ezc3d` cross-check).
- **I select markers by label / only want some markers** → `S03/S05/S06` (selectors + index-space).
- **I read only a frame window** → `S04` (window reads must equal slicing).
- **I worry about units/scaling** → `S07` (reported clearly; use `--strict-unit` if you want a hard gate).
- **I worry about gaps / missing samples** → `S08` (`*_valid` must be reliable).
- **I want caching** → `S11` (bundle roundtrip).
- **I read from bytes** → `S12` (`open_memory`).
- **I worry about corrupted inputs** → `S15` (fail loudly; don’t produce bogus data).

## Mini glossary

No need to memorize:

| Term | Plain meaning |
| --- | --- |
| materialize | read a C3D fully into memory and produce arrays (think “C3D → NumPy tables”). |
| selector | “picking” which markers/channels you want (labels / indices). |
| index-space | the numbering scheme (source-total indices vs chunk-local indices). |
| window read | reading only a slice of frames (like decoding only a segment of a video). |
| `*_valid` (validity mask) | a per-sample “is this sample usable?” flag. |
| units / scaling | length unit metadata (`POINT:UNITS`) and whether numbers get converted. |
| `meta_tree` | the C3D parameter tree (think “settings/metadata dictionary”; matrix params include `dimensions` so you can reshape safely). |
| bundle | a cache artifact (`.sqzc3d`) you can export and load back. |
| stress | a scenario-based contract “health report” (PASS/WARN/FAIL + optional JSON report). |
| `--strict-*` | turn some “report-only differences” into hard gates (mismatch becomes FAIL). |
| oracle | the reference implementation used for comparison (here: `ezc3d`). |

## Expected outcomes

- Under the same interpretation policies (units, validity, etc.), `sqzc3d` points/analogs should match `ezc3d`
  (used as the oracle), avoiding silent numeric drift.
- For human-discernible policy differences (units / validity gate), stress defaults to clear reporting rather than
  surprising failures; use `--strict-unit` / `--strict-validity` to make them hard gates.
- Optional features (e.g. `meta_tree`, bundles) should degrade explicitly (WARN/skip/guidance) when dependencies or
  build options are missing.

## Scope

What we verify:

Covered (publicly documented contracts):

- C3D → materialize to arrays (Python/C/C++; i.e., “read into NumPy / contiguous arrays”).
- Open from file or from memory (`open_memory`; i.e., “read from bytes”).
- Selectors (`None`/`[]`/labels/indices; i.e., “pick which markers/channels”) and window reads (`start_frame`/`frame_count`; i.e., “read only a frame slice”).
- Validity masks (`*_valid`; i.e., “which samples are usable”) and missing samples.
- Units metadata behavior (`POINT:UNITS`) without surprising implicit scaling.
- Type-groups (marker-set semantics; i.e., “marker-set conventions”).
- `meta_tree` (EZ parameter tree / metadata dictionary): readable from C3D when built with `SQZC3D_WITH_EZC3D=ON`, and preserved in bundles.
- Bundle persistence (`export_bundle` / `load_bundle`; cache roundtrip, strict vs best-effort).

Not covered (by design):

- Writing/editing C3D files.
- Higher-level biomechanics semantics (force-plate post-processing, gait events, etc.).

## Verification entry points

From source:

All public verification tools live in the source repository under `samples/verify/`:

- `python samples/verify/verify.py native` — closest to daily workflows: read arrays and compare vs `ezc3d` (points/analogs/labels/meta/meta_tree).
- `python samples/verify/verify.py wasm` — browser/WASM smoke (Playwright) for `open_file` + `open_memory`.
- `python samples/verify/verify.py stress` — scenario-oriented “health report” checks (PASS/WARN/FAIL + optional JSON report).
- `python samples/verify/verify.py cpp` — C++ correctness matrix (best for bundle + index-space hard checks).

Quick reproduce:

```bash
python samples/verify/verify.py all --c3d path/to/c3d_dir --wasm-dir path/to/wasm_out --limit 5
python samples/verify/verify.py stress --c3d path/to/c3d_dir --sample 0 --report stress_report.json
```

## How to read results

You can think of stress output as a simple “health report”: each Scenario is one check.

- **PASS**: the scenario's contract held for the tested inputs (or the file was **skipped** because the scenario is not applicable).
- **PASS + "skipped: ..."**: normal, means "this file doesn't have that feature" (e.g. no analog channels).
- **WARN**: comparison could not be performed (missing dependency), or a potentially surprising behavior was observed.
- **FAIL**: contract violation or oracle mismatch.

Some differences (e.g. unit policies) are easy for humans to spot. For those, the default behavior is to **report clearly**
and let users make them strict via flags (`--strict-unit`, `--strict-validity`) rather than failing by default.

If you see **FAIL**, the fastest debugging loop is usually:

1. Add `--report stress_report.json` and check which file + which contract triggered.
2. Re-run only that scenario via `--scenarios <ID>` (don’t run everything while iterating).
3. If it’s a policy mismatch (units/validity), decide whether to enable `--strict-unit` / `--strict-validity` as hard gates.
4. If it’s about `meta_tree`, make sure you built with `SQZC3D_WITH_EZC3D=ON`.

## Stress scenarios: preset → contract → how to run → what it means

The full spec is in `samples/verify/STRESS_PLAN.md`. The runner is `python samples/verify/verify.py stress`.

### G0 — Mixed-dataset regression

- Preset: a folder of C3Ds from different sources (points-only + analog-heavy mixed).
- Contract: core payload matches `ezc3d` (points/analogs/labels + core meta + meta_tree).
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios G0`
- Result: PASS means "no silent numeric drift" under the same interpretation rules.
- Mental model: give the same trial to two “translators” (`sqzc3d`/`ezc3d`) and diff the output tables; PASS = same translation.

### S01 — Points-only materialize

- Preset: a trial with no analog channels.
- Contract: points arrays have stable shapes; analog arrays are empty (but well-formed).
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S01`
- Result: PASS is expected; files with analogs are skipped.
- Mental model: you only have a “marker table”; the “sensor table” should be an empty table, not garbage.

### S02 — Materialize with analogs

- Preset: analog channels exist (e.g. EMG / force plate).
- Contract: analog arrays are shaped and indexed consistently (`layout="CN"`), with a matching `*_valid` mask.
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S02`
- Result: PASS is expected; files without analogs are skipped.
- Mental model: besides marker positions, you also deliver a time-series “sensor table”; shapes + indexing must line up.

### S03 — Selector semantics

- Preset: you request a small set of markers by label (including a reversed order).
- Contract: `None` means ALL, `[]` means empty, and label selection is **order-preserving**.
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S03`
- Result: PASS means "label selection is predictable" (see also `user_guide_selectors.md`).
- Mental model: “ordering is what you asked for”; `None` is “give me everything”, `[]` is “give me nothing”.

### S04 — Window reads are equivalent to slicing

- Preset: you load a window (`start_frame`, `frame_count`) instead of the full trial.
- Contract: window read == full materialize then slice, including edge cases (`frame_count=0`, `-1`).
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S04`
- Result: PASS means "window reads are a safe optimization" (see also `user_guide_materialize_vs_streaming.md`).
- Mental model: decoding only a segment of a video should not change the frames you did decode.

### S05 — Indices vs labels

- Preset: you select points by indices vs labels and expect the same data.
- Contract: indices/labels paths agree on both values and the index-space mapping (`point_indices_total`).
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S05`
- Result: PASS means "no off-by-one / wrong index space" (see `user_guide_selectors.md`).
- Mental model: two naming systems for the same thing (numbers vs names) should still land on the same marker.

### S06 — Label normalization

- Preset: labels with whitespace / case variations.
- Contract: `label_norm` policies behave as documented and are stable.
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S06`
- Result: PASS means "label matching rules are explicit and testable".
- Mental model: normalize “names” first (trim/case), then match, so users aren’t surprised by trivial formatting.

### S07 — Units & scaling

- Preset: C3Ds with `POINT:UNITS` set to `mm/cm/m` (or missing/unknown).
- Contract: default point scale is **explainable** and does not surprise users; unit differences can be made strict.
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S07 --unit-contract auto`
- Result: PASS means "sqzc3d's default matches ezc3d raw output for your files"; otherwise the report tells you how it differs.
- Mental model: units are the ruler marks (mm/cm/m); default is “don’t secretly swap rulers”, but do report differences clearly.

For details, see `user_guide_units.md`.

### S08 — Validity policies

- Preset: real-world trials with gaps / missing markers.
- Contract: `*_valid` masks are consistent and values are compared under the agreed validity policy.
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S08`
- Result: PASS means "you can trust `*_valid` as the single source of truth".
- Mental model: every sample comes with a “usable / not usable” sticker; compare only after checking the sticker.

See `user_guide_valid.md`.

### S09 — Analog scaling

- Preset: analog channels with calibration parameters.
- Contract: scaled analog values match `ezc3d` for the same file.
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S09`
- Result: PASS is expected; files without analog values are skipped.
- Mental model: analog readings need calibration (scale/offset); if calibration differs, everything shifts.

### S10 — `meta_tree`

- Preset: parameters that are matrices (corners/origin/etc).
- Contract: structure is stable, value ordering is stable, and `dimensions` is present so callers can reshape reliably.
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S10`
- Result: PASS means "no subtle param-tree drift" (requires `SQZC3D_WITH_EZC3D=ON`).
- Mental model: a settings menu can contain matrices; row/column ordering matters even if values “look similar”.

### S11 — Bundles roundtrip

- Preset: cache a chunk as a bundle (`.sqzc3d`) and reload it.
- Contract: export → load roundtrip preserves meta + payload + `meta_tree`; strict mode rejects incompatible bundles.
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S11`
- Result: PASS means "bundle is safe for caching". If Python lacks `export_bundle`, the scenario degrades to WARN.
- Mental model: bundle is a sealed cache box; strict mode is “verify the seal” before trusting the contents.

See `user_guide_bundles.md`.

### S12 — `open_memory`

- Preset: load from bytes (e.g. network / archive / WASM FS).
- Contract: `open_memory` behaves like reading the same file path (including `meta_tree`).
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S12`
- Result: PASS means "bytes-in == file-in".
- Mental model: reading the same content from disk vs from bytes should produce the same tables.

### S13 — Streaming

- Preset: you want true streaming.
- Contract: Python does not expose a dedicated streaming API (yet); window reads (`S04`) cover most sampling needs.
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S13`
- Result: informational (PASS) with guidance (not a hard compatibility gate).
- Mental model: true streaming is “read while playing”; Python currently focuses on “read by window” (`S04`).

### S14 — Feature gating

- Preset: builds that disable optional components.
- Contract: feature bits are queryable and match behavior.
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S14`
- Result: PASS means “switches match behavior”; missing features should be explicit, not silent.
- Mental model: like optional add-ons—if you didn’t install it, the system should say so clearly.

### S15 — Pathological inputs & diagnostics

- Preset: truncated / corrupted C3D inputs.
- Contract: fail loudly with actionable diagnostics (no silent success).
- Stress: `python samples/verify/verify.py stress --c3d <dir> --scenarios S15`
- Result: PASS means "bad inputs don't produce bad data".
- Mental model: a torn/truncated book should trigger an error, not a made-up “successful” translation.

See `user_guide_errors.md`.

## Snapshot

This is a point-in-time snapshot to give readers a concrete expectation (think: “the health report we ran on that date”).
For up-to-date results, run the commands above.

Snapshot metadata:

- Date: **2026-02-27**
- `sqzc3d`: **v0.3.4** (Python bindings)
- `ezc3d` (Python oracle): **1.6.0**
- Platform: Windows (native + WASM)

### Entry points

| Check | Expected | Snapshot |
| --- | --- | --- |
| `verify.py native` | PASS on sampled files | PASS |
| `verify.py wasm` | PASS for `open_file` + `open_memory` smoke | PASS |
| `verify.py stress` | PASS (with possible "skipped" notes) | PASS |

### Scenario snapshot

| Scenario | Expected | Snapshot |
| --- | --- | --- |
| G0 | PASS | PASS |
| S01 | PASS (skips non-points-only files) | PASS |
| S02 | PASS (skips files without analogs) | PASS |
| S03 | PASS | PASS |
| S04 | PASS | PASS |
| S05 | PASS | PASS |
| S06 | PASS | PASS |
| S07 | PASS (or clear report; strict optional) | PASS |
| S08 | PASS (or WARN if strict policy differs) | PASS |
| S09 | PASS (skips files without analog values) | PASS |
| S10 | PASS (requires `WITH_EZC3D=ON`) | PASS |
| S11 | PASS (or WARN if Python lacks `export_bundle`) | PASS |
| S12 | PASS | PASS |
| S13 | informational | PASS |
| S14 | PASS | PASS |
| S15 | PASS | PASS |
