#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def _repo_root() -> Path:
    # samples/verify/verify.py -> repo root
    return Path(__file__).resolve().parents[2]


def _dev_root_repo_dir() -> Path | None:
    env = os.environ.get("DEV_ROOT_WIN") or os.environ.get("DEV_ROOT")
    if not env:
        # Local convention fallback (Windows): C:\dev\<repo>
        if os.name == "nt":
            cand = Path(r"C:\dev") / _repo_root().name
            if cand.exists():
                return cand
        return None
    return Path(env) / _repo_root().name


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
    dev = _dev_root_repo_dir()
    candidates: list[Path] = []
    if dev is not None:
        candidates += [
            dev / "build" / "verify_correctness_matrix_sqzc3d",
            dev / "build" / "verify_correctness_matrix_sqzc3d.exe",
            dev / "build" / "Release" / "verify_correctness_matrix_sqzc3d.exe",
            dev / "build" / "Debug" / "verify_correctness_matrix_sqzc3d.exe",
        ]
    candidates += [
        root / "local_tools" / "build" / "verify_correctness_matrix_sqzc3d",
        root / "local_tools" / "build" / "verify_correctness_matrix_sqzc3d.exe",
        root / "local_tools" / "build" / "Release" / "verify_correctness_matrix_sqzc3d.exe",
        root / "local_tools" / "build" / "Debug" / "verify_correctness_matrix_sqzc3d.exe",
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


def _sha256_files(paths: list[Path]) -> str:
    h = hashlib.sha256()
    for p in paths:
        h.update(Path(p).read_bytes())
    return h.hexdigest()


def _expected_wasm_stamp() -> dict:
    root = _repo_root()
    sqz_files = [
        root / "src" / "sqzc3d.cpp",
        root / "src" / "sqzc3d_c3d_stream.cpp",
        root / "include" / "sqzc3d.h",
        root / "include" / "sqzc3d_c3d_stream.h",
        root / "include" / "sqzc3d_types.h",
    ]
    build_script = root / "samples" / "verify" / "_impl" / "build_wasm_fs.py"
    return {
        "schema": 1,
        "sqzc3d_sources_sha256": _sha256_files(sqz_files),
        "build_script_sha256": _sha256_files([build_script]),
    }


def _is_wasm_build_fresh(wasm_dir: Path) -> bool:
    wasm_dir = Path(wasm_dir)
    if not (wasm_dir / "sqzc3d.js").exists():
        return False
    if not (wasm_dir / "sqzc3d.wasm").exists():
        return False
    stamp_path = wasm_dir / "sqzc3d_wasm_stamp.json"
    if not stamp_path.exists():
        return False
    try:
        stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return False
    expected = _expected_wasm_stamp()
    if int(stamp.get("schema", -1)) != int(expected["schema"]):
        return False
    if str(stamp.get("sqzc3d_sources_sha256", "")) != str(expected["sqzc3d_sources_sha256"]):
        return False
    if str(stamp.get("build_script_sha256", "")) != str(expected["build_script_sha256"]):
        return False
    return True


def _ensure_wasm_built(wasm_dir: Path, rebuild: bool, empp: Path | None, ezc3d_src: Path | None) -> None:
    wasm_dir = Path(wasm_dir)
    wasm_dir.mkdir(parents=True, exist_ok=True)
    if _is_wasm_build_fresh(wasm_dir):
        return
    if not rebuild:
        raise RuntimeError(f"WASM build is missing or stale under: {wasm_dir} (rebuild disabled).")

    vdir = Path(__file__).resolve().parent
    build_script = vdir / "_impl" / "build_wasm_fs.py"
    build_args = ["--out-dir", str(wasm_dir)]
    if empp is not None:
        build_args += ["--empp", str(Path(empp))]
    if ezc3d_src is not None:
        build_args += ["--ezc3d-src", str(Path(ezc3d_src))]
    rc = _run_py(build_script, build_args)
    if rc != 0:
        raise SystemExit(rc)
    if not (wasm_dir / "sqzc3d.js").exists() or not (wasm_dir / "sqzc3d.wasm").exists():
        raise RuntimeError(f"WASM build script succeeded, but output is missing under: {wasm_dir}")


def _parse_args() -> argparse.Namespace:
    root = _repo_root()
    dev = _dev_root_repo_dir()
    default_wasm_dir = (
        Path(os.environ["SQZC3D_WASM_DIR"])
        if os.environ.get("SQZC3D_WASM_DIR")
        else (
            (dev / "build-wasm-verify")
            if dev is not None
            else (Path(tempfile.gettempdir()) / root.name / "build-wasm-verify")
        )
    )

    p = argparse.ArgumentParser(
        prog="samples/verify/verify.py",
        description="Single entrypoint for sqzc3d verification checks.",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    p_all = sub.add_parser("all", help="Run all checks (native + wasm).")
    p_all.add_argument("--c3d", type=Path, default=os.environ.get("C3D_DIR") or os.environ.get("C3D_FILE"))
    p_all.add_argument("--wasm-dir", type=Path, default=default_wasm_dir)
    p_all.add_argument("--limit", type=int, default=int(os.environ.get("C3D_LIMIT", "5")))
    p_all.add_argument("--rebuild-wasm", action=argparse.BooleanOptionalAction, default=True)
    p_all.add_argument("--empp", type=Path, default=None)
    p_all.add_argument("--ezc3d-src", type=Path, default=None)

    p_native = sub.add_parser("native", help="Native correctness smoke vs ezc3d (Python).")
    p_native.add_argument("--c3d", type=Path, default=os.environ.get("C3D_DIR") or os.environ.get("C3D_FILE"))

    p_stress = sub.add_parser("stress", help="Scenario-oriented stress workflow (self-reporting).")
    p_stress.add_argument("--c3d", type=Path, default=os.environ.get("C3D_DIR") or os.environ.get("C3D_FILE"))
    p_stress.add_argument("--roots", nargs="*", action="append", default=[])
    p_stress.add_argument("--sample", type=int, default=int(os.environ.get("C3D_STRESS_SAMPLE", "6")))
    p_stress.add_argument("--seed", type=int, default=42)
    p_stress.add_argument(
        "--scenarios",
        nargs="+",
        default=["G0", "S01", "S02", "S03", "S04", "S05", "S06", "S07", "S08", "S09", "S10", "S11", "S12", "S13", "S14", "S15"],
        help="scenario ids to run",
    )
    p_stress.add_argument("--unit-contract", choices=["meters", "raw", "auto"], default="auto")
    p_stress.add_argument(
        "--strict",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="strict core assertions (meta/meta_tree in scenarios that use them)",
    )
    p_stress.add_argument("--strict-unit", action=argparse.BooleanOptionalAction, default=False, help="treat unit mismatch as hard fail")
    p_stress.add_argument(
        "--strict-validity", action=argparse.BooleanOptionalAction, default=False, help="treat validity policy mismatch as hard fail"
    )
    p_stress.add_argument("--max-fail", type=int, default=999)
    p_stress.add_argument("--report", type=Path, default=None)

    p_wasm = sub.add_parser("wasm", help="Standalone wasm browser smoke (Playwright).")
    p_wasm.add_argument("--wasm-dir", type=Path, default=default_wasm_dir)
    p_wasm.add_argument("--c3d-dir", type=Path, default=os.environ.get("C3D_DIR"))
    p_wasm.add_argument("--limit", type=int, default=int(os.environ.get("C3D_LIMIT", "5")))
    p_wasm.add_argument("--rebuild-wasm", action=argparse.BooleanOptionalAction, default=True)
    p_wasm.add_argument("--empp", type=Path, default=None)
    p_wasm.add_argument("--ezc3d-src", type=Path, default=None)

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

    if args.cmd == "stress":
        stress_script = vdir / "_impl" / "stress_sqzc3d.py"
        stress_roots: list[str] = []
        stress_args = ["--sample", str(int(getattr(args, "sample", 6))), "--seed", str(int(getattr(args, "seed", 42)))]
        if getattr(args, "c3d", None) is not None:
            c3d = Path(args.c3d)
            if c3d.is_dir():
                stress_roots.append(str(c3d))
            else:
                stress_args.append(str(c3d))
        for root_group in getattr(args, "roots", []):
            if not root_group:
                continue
            if isinstance(root_group, list):
                stress_roots.extend(str(root) for root in root_group)
            else:
                stress_roots.append(str(root_group))
        if stress_roots:
            stress_args += ["--roots", *stress_roots]
        stress_args += ["--unit-contract", str(getattr(args, "unit_contract", "auto"))]
        stress_args += ["--scenarios", *getattr(args, "scenarios", ["G0"])]
        if args.strict is not None:
            stress_args += ["--strict" if bool(args.strict) else "--no-strict"]
        if args.strict_unit is not None:
            stress_args += ["--strict-unit" if bool(args.strict_unit) else "--no-strict-unit"]
        if args.strict_validity is not None:
            stress_args += ["--strict-validity" if bool(args.strict_validity) else "--no-strict-validity"]
        stress_args += ["--max-fail", str(int(getattr(args, "max_fail", 999)))]
        if getattr(args, "report", None):
            stress_args += ["--report", str(Path(args.report))]
        rc = _run_py(stress_script, stress_args)
        raise SystemExit(rc)

    if args.cmd in ("all", "wasm"):
        wasm_dir = getattr(args, "wasm_dir", None)
        if wasm_dir is None:
            raise RuntimeError("Missing --wasm-dir (or set SQZC3D_WASM_DIR).")
        _ensure_wasm_built(
            Path(wasm_dir),
            bool(getattr(args, "rebuild_wasm", True)),
            getattr(args, "empp", None),
            getattr(args, "ezc3d_src", None),
        )
        wasm_script = vdir / "_impl" / "playwright_wasm_sqzc3d_open_modes.py"
        wasm_args: list[str] = []
        if wasm_dir:
            wasm_args += ["--wasm-dir", str(Path(wasm_dir))]
        c3d_dir = getattr(args, "c3d_dir", None)
        c3d = getattr(args, "c3d", None)
        if c3d_dir:
            wasm_args += ["--c3d-dir", str(Path(c3d_dir))]
        elif c3d:
            p = Path(c3d)
            if p.is_dir():
                wasm_args += ["--c3d-dir", str(p)]
            else:
                wasm_args += ["--c3d-file", str(p)]
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
