# ezc3d patches

This folder contains local patches applied to ezc3d when sqzc3d fetches it via CMake.

## Why do we patch?

In some Emscripten/WASM builds, exception catching can be disabled or limited by toolchain flags.
ezc3d uses `try/catch` as control flow in a few query helpers (e.g., `isGroup`, `isParameter`).
When exceptions are not catchable, behavior can diverge from native builds and break parsing on
real-world C3D files.

## What we patch today

- `ezc3d_noexcept_isgroup_isparameter.patch`
  - removes try/catch control flow in:
    - `Group::isParameter`
    - `Parameters::isGroup`

## How patches are applied

`CMakeLists.txt` calls `cmake/patches/apply_git_patch.cmake` (which uses `git apply`) after
`FetchContent_MakeAvailable(ezc3d)`.

Control knobs:

- `SQZC3D_APPLY_EZC3D_PATCHES`
  - default: `ON` for `CMAKE_SYSTEM_NAME=Emscripten`, `OFF` otherwise.

