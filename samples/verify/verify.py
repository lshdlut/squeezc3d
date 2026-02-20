#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


def _repo_root() -> Path:
    # samples/verify/verify.py -> repo root
    return Path(__file__).resolve().parents[2]


def _run_py(script: Path, args: list[str]) -> int:
    if not script.exists():
        raise RuntimeError(f"Missing script: {script}")
    cmd = [sys.executable, str(script), *args]
    proc = subprocess.run(cmd, cwd=str(_repo_root()))
    return int(proc.returncode)


def _find_cpp_verify_exe() -> Path | None:
    override = os.environ.get("SQZC3D_VERIFY_CPP_EXE")
    if override:
        p = Path(override)
        return p if p.exists() else None

    root = _repo_root()
    candidates = [
        root / "build" / "verify_correctness_matrix_sqzc3d",
        root / "build" / "verify_correctness_matrix_sqzc3d.exe",
        root / "build" / "Release" / "verify_correctness_matrix_sqzc3d.exe",
        root / "build" / "Debug" / "verify_correctness_matrix_sqzc3d.exe",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


def _run_cpp(exe: Path, c3d_files: list[Path]) -> int:
    if not exe.exists():
        raise RuntimeError(f"Missing exe: {exe}")
    if not c3d_files:
        raise RuntimeError("Missing C3D inputs for cpp mode.")
    cmd = [str(exe), *[str(p) for p in c3d_files]]
    proc = subprocess.run(cmd, cwd=str(_repo_root()))
    return int(proc.returncode)


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="samples/verify/verify.py",
        description="Single entrypoint for sqzc3d verification checks.",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("all", help="Run all checks (native + wasm).")

    p_native = sub.add_parser("native", help="Native correctness smoke vs ezc3d (Python).")
    p_native.add_argument("--c3d", type=Path, default=os.environ.get("C3D_FILE"))

    p_wasm = sub.add_parser("wasm", help="Standalone wasm browser smoke (Playwright).")
    p_wasm.add_argument("--wasm-dir", type=Path, default=os.environ.get("SQZC3D_WASM_DIR"))
    p_wasm.add_argument("--c3d-dir", type=Path, default=os.environ.get("C3D_DIR"))
    p_wasm.add_argument("--limit", type=int, default=int(os.environ.get("C3D_LIMIT", "5")))

    p_cpp = sub.add_parser("cpp", help="C++ correctness matrix executable (built via CMake).")
    p_cpp.add_argument("--exe", type=Path, default=os.environ.get("SQZC3D_VERIFY_CPP_EXE"))
    p_cpp.add_argument("--c3d", type=Path, action="append", default=[])

    return p.parse_args()


def main() -> None:
    args = _parse_args()
    vdir = Path(__file__).resolve().parent

    rc = 0
    if args.cmd in ("all", "native"):
        native_script = vdir / "_impl" / "smoke_vs_ezc3d.py"
        native_args: list[str] = []
        c3d = getattr(args, "c3d", None)
        if c3d:
            p = Path(c3d)
            if p.is_dir():
                native_args += ["--roots", str(p)]
            else:
                native_args += [str(p)]
        rc = _run_py(native_script, native_args)
        if rc != 0 and args.cmd != "all":
            raise SystemExit(rc)

    if args.cmd in ("all", "wasm"):
        wasm_script = vdir / "_impl" / "playwright_wasm_sqzc3d_open_modes.py"
        wasm_args: list[str] = []
        wasm_dir = getattr(args, "wasm_dir", None)
        if wasm_dir:
            wasm_args += ["--wasm-dir", str(Path(wasm_dir))]
        c3d_dir = getattr(args, "c3d_dir", None)
        if c3d_dir:
            wasm_args += ["--c3d-dir", str(Path(c3d_dir))]
        wasm_args += ["--limit", str(int(getattr(args, "limit", 5)))]
        rc = _run_py(wasm_script, wasm_args)
        if rc != 0 and args.cmd != "all":
            raise SystemExit(rc)

    if args.cmd == "cpp":
        exe = Path(args.exe) if args.exe else _find_cpp_verify_exe()
        if exe is None:
            raise RuntimeError("Missing cpp exe. Pass --exe or set SQZC3D_VERIFY_CPP_EXE.")
        c3d_files = [Path(p) for p in (args.c3d or [])]
        rc = _run_cpp(exe, c3d_files)
        raise SystemExit(rc)

    raise SystemExit(rc)


if __name__ == "__main__":
    main()
