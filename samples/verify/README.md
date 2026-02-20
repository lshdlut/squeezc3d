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
