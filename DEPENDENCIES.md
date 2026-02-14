# Dependencies

## Mandatory build dependencies
- CMake >= 3.16
- C++ compiler with C++17 support

## Optional third-party dependency
- `ezc3d` is optional and required only when `SQZC3D_WITH_EZC3D=ON`.
- When `SQZC3D_WITH_EZC3D=ON`:
  - You must provide an `ezc3d` CMake target (`ezc3d` or `ezc3d::ezc3d`) or
  - set `SQZC3D_FETCH_EZC3D=ON` so CMake will fetch from  
    `https://github.com/pyomeca/ezc3d.git` (ref `SQZC3D_EZC3D_GIT_TAG`).
- When `SQZC3D_WITH_EZC3D=OFF`, this project only requires bundle APIs (no ezc3d integration at build time).

## Distribution and notices
- `Squeezed C3D (sqzc3d)` does not embed ezc3d source code.
- Distributors combining with ezc3d are responsible for carrying ezc3d notices and license text.
- `Squeezed C3D (sqzc3d)` uses MIT license (see `LICENSE`).

## See also
- `NOTICE`
