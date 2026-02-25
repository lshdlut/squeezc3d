#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Iterable, List, Optional


def _repo_root() -> Path:
    # samples/verify/_impl/build_wasm_fs.py -> repo root
    return Path(__file__).resolve().parents[3]


def _sha256_files(paths: Iterable[Path]) -> str:
    h = hashlib.sha256()
    for p in paths:
        h.update(Path(p).read_bytes())
    return h.hexdigest()


def _default_empp() -> str:
    override = os.environ.get("EMPP") or os.environ.get("EMPP_EXE")
    if override:
        return str(override)
    emsdk = os.environ.get("EMSDK_HOME") or os.environ.get("EMSDK")
    if emsdk:
        base = Path(emsdk)
        if os.name == "nt":
            cand = base / "upstream" / "emscripten" / "em++.py"
            if cand.exists():
                return str(cand)
            cand = base / "upstream" / "emscripten" / "em++.bat"
            if cand.exists():
                return str(cand)
        else:
            cand = base / "upstream" / "emscripten" / "em++"
            if cand.exists():
                return str(cand)
    return "em++"


def _empp_argv(empp: str) -> List[str]:
    resolved = shutil.which(empp) or empp
    p = Path(resolved)
    suf = p.suffix.lower()
    if suf == ".py":
        return [sys.executable, resolved]
    if os.name == "nt" and suf in (".bat", ".cmd"):
        return ["cmd", "/c", resolved]
    return [resolved]


def _find_ezc3d_src(root: Path, override: Optional[Path]) -> Path:
    if override is not None:
        p = Path(override)
        if not p.exists():
            raise RuntimeError(f"Missing --ezc3d-src: {p}")
        return p

    env = os.environ.get("EZC3D_SRC_DIR")
    if env:
        p = Path(env)
        if p.exists():
            return p

    dev_root = os.environ.get("DEV_ROOT_WIN") or os.environ.get("DEV_ROOT")
    dev = (Path(dev_root) / root.name) if dev_root else None
    if dev is None and os.name == "nt":
        cand = Path(r"C:\dev") / root.name
        if cand.exists():
            dev = cand

    candidates: List[Path] = []
    if dev is not None:
        candidates += [
            dev / "build" / "_deps" / "ezc3d-src",
            dev / "build-ezc3d" / "_deps" / "ezc3d-src",
        ]
    candidates += [
        root / "local_tools" / "build" / "_deps" / "ezc3d-src",
        root / "local_tools" / "build-ezc3d" / "_deps" / "ezc3d-src",
        root / "build" / "_deps" / "ezc3d-src",
        root / "build-ezc3d" / "_deps" / "ezc3d-src",
        root / "local_tools" / "temp_pkg" / "ezc3d_src",
        root / "temp_pkg" / "ezc3d_src",
    ]
    for c in candidates:
        if (c / "src").exists() and (c / "include").exists():
            return c

    raise RuntimeError(
        "Could not locate ezc3d source dir. Set EZC3D_SRC_DIR or pass --ezc3d-src, "
        "or build a native CMake tree so <build_dir>/_deps/ezc3d-src exists."
    )


def _iter_ezc3d_cpp(ezc3d_src: Path) -> List[Path]:
    src_dir = ezc3d_src / "src"
    out: List[Path] = []
    for p in src_dir.rglob("*.cpp"):
        s = str(p).replace("\\", "/")
        if "/binding" in s or "/examples" in s or "/test" in s:
            continue
        out.append(p)
    return out


def _stamp_expected(root: Path) -> dict:
    sqz_files = [
        root / "src" / "sqzc3d.cpp",
        root / "src" / "sqzc3d_c3d_stream.cpp",
        root / "include" / "sqzc3d.h",
        root / "include" / "sqzc3d_c3d_stream.h",
        root / "include" / "sqzc3d_types.h",
    ]
    build_script = Path(__file__).resolve()
    return {
        "schema": 1,
        "sqzc3d_sources": [str(p.relative_to(root)).replace("\\", "/") for p in sqz_files],
        "sqzc3d_sources_sha256": _sha256_files(sqz_files),
        "build_script": str(build_script.relative_to(root)).replace("\\", "/"),
        "build_script_sha256": _sha256_files([build_script]),
    }


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Build a sqzc3d WASM module (filesystem-enabled, ezc3d-backed).")
    p.add_argument("--out-dir", type=Path, required=True)
    p.add_argument("--empp", type=Path, default=None)
    p.add_argument("--ezc3d-src", type=Path, default=None)
    return p.parse_args()


def main() -> None:
    args = _parse_args()
    root = _repo_root()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    empp = str(args.empp) if args.empp else _default_empp()
    ezc3d_src = _find_ezc3d_src(root, args.ezc3d_src)
    empp_argv = _empp_argv(empp)

    build_inc: Optional[Path] = None
    build_inc_env = os.environ.get("EZC3D_BUILD_INCLUDE_DIR")
    if build_inc_env:
        build_inc = Path(build_inc_env)
    else:
        dev_root = os.environ.get("DEV_ROOT_WIN") or os.environ.get("DEV_ROOT")
        dev = (Path(dev_root) / root.name) if dev_root else None
        if dev is None and os.name == "nt":
            cand = Path(r"C:\dev") / root.name
            if cand.exists():
                dev = cand

        candidates: List[Path] = []
        if dev is not None:
            candidates += [
                dev / "build" / "_deps" / "ezc3d-build" / "include",
                dev / "build-ezc3d" / "_deps" / "ezc3d-build" / "include",
            ]
        candidates += [
            root / "local_tools" / "build" / "_deps" / "ezc3d-build" / "include",
            root / "local_tools" / "build-ezc3d" / "_deps" / "ezc3d-build" / "include",
            root / "build" / "_deps" / "ezc3d-build" / "include",
            root / "build-ezc3d" / "_deps" / "ezc3d-build" / "include",
        ]
        for cand in candidates:
            if cand.exists():
                build_inc = cand
                break
    if build_inc is not None and not build_inc.exists():
        build_inc = None

    exported = [
        "_malloc",
        "_free",
        "_sqzc3d_default_open_opt",
        "_sqzc3d_default_build_opt",
        "_sqzc3d_open_file",
        "_sqzc3d_open_memory",
        "_sqzc3d_close_dec",
        "_sqzc3d_last_error",
        "_sqzc3d_last_error_detail",
        "_sqzc3d_build_chunks",
        "_sqzc3d_free_chunk",
        "_sqzc3d_chunk_num_frames",
        "_sqzc3d_chunk_num_points",
        "_sqzc3d_chunk_num_scalar",
        "_sqzc3d_points_view_frames",
        "_sqzc3d_points_view_points",
        "_sqzc3d_chunk_point_indices_total",
    ]

    src: List[Path] = [
        root / "src" / "sqzc3d.cpp",
        root / "src" / "sqzc3d_c3d_stream.cpp",
    ]
    src += _iter_ezc3d_cpp(ezc3d_src)

    # Note: Pass list syntax directly to Emscripten without extra escaping, since we don't invoke a shell.
    exported_list = "[" + ",".join([f"'{e}'" for e in exported]) + "]"
    runtime_methods = "[" + ",".join(
        [f"'{m}'" for m in ["ccall", "cwrap", "setValue", "getValue", "UTF8ToString", "stringToUTF8", "lengthBytesUTF8"]]
    ) + "]"

    cmd = [
        *empp_argv,
        "-O2",
        "-std=c++17",
        "-Wall",
        "--no-entry",
        "-fexceptions",
        "-s",
        "DISABLE_EXCEPTION_CATCHING=0",
        "-s",
        "MODULARIZE=1",
        "-s",
        "EXPORT_NAME=SQZC3DModule",
        "-s",
        "NO_EXIT_RUNTIME=1",
        "-s",
        "ALLOW_MEMORY_GROWTH=1",
        "-s",
        "FORCE_FILESYSTEM=1",
        "-s",
        f"EXPORTED_FUNCTIONS={exported_list}",
        "-s",
        f"EXPORTED_RUNTIME_METHODS={runtime_methods}",
        "-I",
        str(root / "include"),
        "-I",
        str(ezc3d_src / "include"),
        "-I",
        str(ezc3d_src / "src"),
    ]
    if build_inc is not None:
        cmd += ["-I", str(build_inc)]
    cmd += [
        "-D",
        "SQZC3D_WITH_EZC3D=1",
        "-D",
        "sqzc3d_WITH_EZC3D=1",
    ]
    cmd += [str(p) for p in src]
    cmd += ["-o", str(out_dir / "sqzc3d.js")]

    print("[sqzc3d][wasm] building...")
    print("[sqzc3d][wasm] out_dir:", out_dir)
    print("[sqzc3d][wasm] empp:", empp)
    if empp_argv != [empp]:
        print("[sqzc3d][wasm] empp_argv:", empp_argv)
    print("[sqzc3d][wasm] ezc3d_src:", ezc3d_src)
    if build_inc is not None:
        print("[sqzc3d][wasm] ezc3d_build_include:", build_inc)

    proc = subprocess.run(cmd, cwd=str(root))
    if int(proc.returncode) != 0:
        raise SystemExit(int(proc.returncode))

    stamp = _stamp_expected(root)
    stamp.update(
        {
            "built_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "out_dir": str(out_dir),
            "empp": empp,
            "ezc3d_src": str(ezc3d_src),
        }
    )
    stamp_path = out_dir / "sqzc3d_wasm_stamp.json"
    stamp_path.write_text(json.dumps(stamp, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print("[sqzc3d][wasm] wrote stamp:", stamp_path)


if __name__ == "__main__":
    main()
