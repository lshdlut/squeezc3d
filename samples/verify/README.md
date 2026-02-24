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
- Default `--wasm-dir` is `local_tools/build-wasm-verify` (or `SQZC3D_WASM_DIR` if set).
- Rebuild requires an Emscripten toolchain (recommended: set `EMSDK_HOME`).
- WASM rebuild also requires ezc3d sources (auto-discovered from `local_tools/build/_deps/ezc3d-src` when available, or pass `--ezc3d-src` / set `EZC3D_SRC_DIR`).

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
