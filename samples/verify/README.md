# Verify (single entrypoint)

This folder contains developer-facing correctness and integration checks.

## One entrypoint

Use a single runner:

```bash
python samples/verify/verify.py -h
```

Subcommands:

- `native`: native Python correctness smoke vs ezc3d
- `wasm`: standalone browser smoke for a `sqzc3d.js/.wasm` build (Playwright)
- `cpp`: run the C++ matrix executable (built via CMake)
- `all`: run `native` + `wasm`
- `stress`: scenario-oriented stress workflow (PASS/WARN/FAIL)，默认覆盖 G0,S01,S02,S03,S04,S05,S06,S07,S08,S09,S10,S11,S12,S13,S14,S15（`S11` 在 Python 绑定缺少 `export_bundle` 时会降级为 WARN；`S13` 为 streaming API 的信息性项）

## Recommended: verify all

Use a single command after refactors:

```bash
python samples/verify/verify.py all ^
  --c3d      path\to\c3d_dir ^
  --wasm-dir path\to\wasm_out ^
  --limit 5
```

Notes:
- `wasm` / `all` will auto-rebuild the WASM output under `--wasm-dir` when it is missing or stale.
- Default `--wasm-dir` comes from `SQZC3D_WASM_DIR` when set. Otherwise it uses `$DEV_ROOT_WIN/<repo>/build-wasm-verify` (when `DEV_ROOT_WIN` is set), or a temp directory.
- Rebuild requires an Emscripten toolchain (recommended: set `EMSDK_HOME`).
- WASM rebuild also requires ezc3d sources (auto-discovered from a native CMake FetchContent tree when available, or pass `--ezc3d-src` / set `EZC3D_SRC_DIR`).

Environment variable defaults (optional):

- `C3D_DIR`: directory containing `.c3d` files
- `SQZC3D_WASM_DIR`: directory containing `sqzc3d.js` + `sqzc3d.wasm`
- `C3D_LIMIT`: number of wasm samples (default: 5)

## Layout

- `samples/verify/verify.py`
  - the only entrypoint

- `samples/verify/_impl/`
  - internal implementations used by `verify.py` (do not run directly)

- `samples/verify/cpp/`
  - C++ validation sources (built via CMake)

- `samples/verify/STRESS_PLAN.md`
  - scenario-based stress plan (vs ezc3d)

## Quick usage

1. Standalone wasm smoke (requires a directory containing `sqzc3d.js` + `sqzc3d.wasm`):

```bash
python samples/verify/verify.py wasm ^
  --wasm-dir path\\to\\wasm_out ^
  --c3d-dir  path\\to\\c3d_dir ^
  --limit 5
```

2. C++ matrix verify (requires building the sample executable):

```bash
python samples/verify/verify.py cpp --exe path\\to\\verify_correctness_matrix_sqzc3d.exe --c3d path\\to\\file.c3d
```

3. Stress mode:

```bash
python samples/verify/verify.py stress --c3d path\\to\\c3d_dir --report stress_report.json
python samples/verify/verify.py stress --roots path\\to\\dataset_dir --scenarios G0 S07 S08 --unit-contract auto
python samples/verify/verify.py stress --c3d path\\to\\c3d_dir --strict-unit --strict-validity --max-fail 3
```
