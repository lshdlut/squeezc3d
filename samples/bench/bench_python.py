#!/usr/bin/env python3
"""Python benchmark for sqzc3d vs ezc3d.

This is intentionally minimal and reports only:
- load time (materialize)
- frame copy time (K=all points)
- trajectory copy time (T=full, K=1 point)
- memory snapshots (RSS / peak RSS)
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Tuple

import numpy as np


@dataclass
class MemSnapshot:
    rss_mb: float = 0.0
    peak_rss_mb: float = 0.0


def _snapshot_memory() -> MemSnapshot:
    out = MemSnapshot()
    if sys.platform.startswith("win"):
        import ctypes
        from ctypes import wintypes

        class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD),
                ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
                ("PrivateUsage", ctypes.c_size_t),
            ]

        psapi = ctypes.WinDLL("psapi")
        kernel32 = ctypes.WinDLL("kernel32")
        GetCurrentProcess = kernel32.GetCurrentProcess
        GetCurrentProcess.restype = wintypes.HANDLE
        GetProcessMemoryInfo = psapi.GetProcessMemoryInfo
        GetProcessMemoryInfo.argtypes = [
            wintypes.HANDLE,
            ctypes.POINTER(PROCESS_MEMORY_COUNTERS_EX),
            wintypes.DWORD,
        ]
        GetProcessMemoryInfo.restype = wintypes.BOOL

        pmc = PROCESS_MEMORY_COUNTERS_EX()
        pmc.cb = ctypes.sizeof(pmc)
        if GetProcessMemoryInfo(GetCurrentProcess(), ctypes.byref(pmc), pmc.cb):
            out.rss_mb = float(pmc.WorkingSetSize) / (1024.0 * 1024.0)
            out.peak_rss_mb = float(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0)
        if out.peak_rss_mb <= 0.0:
            out.peak_rss_mb = out.rss_mb
        return out

    # POSIX-ish fallback (best-effort).
    try:
        import resource

        ru = resource.getrusage(resource.RUSAGE_SELF)
        if sys.platform == "darwin":
            out.peak_rss_mb = float(ru.ru_maxrss) / (1024.0 * 1024.0)
        else:
            out.peak_rss_mb = float(ru.ru_maxrss) / 1024.0
    except Exception:
        pass

    if sys.platform.startswith("linux"):
        try:
            with open("/proc/self/statm", "r", encoding="utf-8") as f:
                parts = f.read().strip().split()
            if len(parts) >= 2:
                resident_pages = int(parts[1])
                page_size = os.sysconf("SC_PAGESIZE")
                out.rss_mb = float(resident_pages * page_size) / (1024.0 * 1024.0)
        except OSError:
            pass

    if out.rss_mb <= 0.0:
        out.rss_mb = out.peak_rss_mb
    if out.peak_rss_mb <= 0.0:
        out.peak_rss_mb = out.rss_mb
    return out


def _elapsed_ms(t0: float, t1: float) -> float:
    return (t1 - t0) * 1000.0


def _time_ms_adaptive(
    fn: Callable[[int], float],
    *,
    target_total_ms: float,
    max_iters: int,
) -> Tuple[float, float, int]:
    iters = 1
    checksum = 0.0
    while True:
        checksum = 0.0
        t0 = time.perf_counter()
        for i in range(iters):
            x = float(fn(i))
            checksum += x if x == x else 0.0
        t1 = time.perf_counter()
        total_ms = _elapsed_ms(t0, t1)
        if total_ms >= target_total_ms or iters >= max_iters:
            per_op_ms = total_ms / float(max(iters, 1))
            return per_op_ms, checksum, iters
        iters = min(iters * 2, max_iters)


def _bench_sqzc3d(file_path: Path, repeat: int) -> dict:
    import sqzc3d  # type: ignore

    mem0 = _snapshot_memory()
    mem1 = mem0
    sum_load = 0.0
    chunk = None
    for _ in range(max(1, int(repeat))):
        dec = sqzc3d.Decoder(str(file_path))
        try:
            t0 = time.perf_counter()
            # Materialize with defaults (points=ALL, analogs=ALL).
            # Access-pattern microbench below focuses on point arrays.
            chunk = dec.read(frame_count=-1, points=None, analogs=None, analog_range=None)
            t1 = time.perf_counter()
            sum_load += _elapsed_ms(t0, t1)
        finally:
            dec.close()
        mem1 = _snapshot_memory()

    assert chunk is not None
    pts, valid = chunk.points(copy=False)
    n_frames = int(pts.shape[0])
    n_points = int(pts.shape[1])
    f0 = 0
    t256 = min(256, n_frames)
    start = 0 if n_frames <= t256 else max(0, (n_frames // 2) - (t256 // 2))

    def frame_copy(_it: int) -> float:
        f = f0 if n_frames <= 1 else (_it % n_frames)
        x = pts[f, :, :].copy()
        return float(x[_it % max(n_points, 1), 0])

    def traj_copy(_it: int) -> float:
        p = 0 if n_points <= 1 else (_it % n_points)
        x = pts[:, p, :].copy()
        return float(x[_it % max(n_frames, 1), 0])

    def window_copy(_it: int) -> float:
        if t256 <= 0:
            return 0.0
        x = pts[start : start + t256, :, :].copy()
        idx = (_it % max(t256 * max(n_points, 1), 1))
        return float(x[idx // max(n_points, 1), idx % max(n_points, 1), 0])

    frame_copy_ms_kall, chk0, _iters0 = _time_ms_adaptive(frame_copy, target_total_ms=150.0, max_iters=1 << 12)
    traj_strided_ms_Tfull_k1, chk1, _iters1 = _time_ms_adaptive(traj_copy, target_total_ms=250.0, max_iters=1 << 10)
    window_read_ms_T256_kall, chk2, _iters2 = _time_ms_adaptive(
        window_copy, target_total_ms=250.0, max_iters=1 << 10
    )
    mem2 = _snapshot_memory()

    # Keep a checksum to prevent accidental dead-code removal.
    checksum = float(
        chk0 + chk1 + chk2 + float(mem2.rss_mb) + float(valid[0, 0] if n_frames and n_points else 0.0)
    )

    return {
        "lib": "sqzc3d",
        "lang": "python",
        "mode": "materialize",
        "repeat": int(repeat),
        "input": str(file_path),
        "file_bytes": int(file_path.stat().st_size),
        "frames": n_frames,
        "points": n_points,
        "rss_baseline_mb": float(mem0.rss_mb),
        "peak_rss_mb": float(max(mem1.peak_rss_mb, mem2.peak_rss_mb)),
        "peak_rss_delta_mb": float(max(mem1.peak_rss_mb, mem2.peak_rss_mb) - mem0.rss_mb),
        "load_ms": float(sum_load / float(max(1, int(repeat)))),
        "frame_copy_ms_kall": float(frame_copy_ms_kall),
        "window_read_ms_T256_kall": float(window_read_ms_T256_kall),
        "traj_strided_ms_Tfull_k1": float(traj_strided_ms_Tfull_k1),
        "checksum": float(checksum),
        "sqzc3d_version": str(getattr(sqzc3d, "__version__", "")),
        "sqzc3d_abi_version": int(sqzc3d.abi_version()),
    }


def _bench_ezc3d(file_path: Path, repeat: int) -> dict:
    import ezc3d  # type: ignore

    mem0 = _snapshot_memory()
    mem1 = mem0
    sum_load = 0.0
    c3d = None
    for _ in range(max(1, int(repeat))):
        t0 = time.perf_counter()
        c3d = ezc3d.c3d(str(file_path))
        t1 = time.perf_counter()
        sum_load += _elapsed_ms(t0, t1)
        mem1 = _snapshot_memory()

    assert c3d is not None
    pts = np.asarray(c3d["data"]["points"])
    if pts.ndim != 3 or pts.shape[0] < 3:
        raise RuntimeError(f"unexpected ezc3d points shape: {pts.shape}")

    n_points = int(pts.shape[1])
    n_frames = int(pts.shape[2])
    f0 = 0
    t256 = min(256, n_frames)
    start = 0 if n_frames <= t256 else max(0, (n_frames // 2) - (t256 // 2))

    def frame_copy(_it: int) -> float:
        f = f0 if n_frames <= 1 else (_it % n_frames)
        x = np.ascontiguousarray(pts[:3, :, f].T)
        return float(x[_it % max(n_points, 1), 0])

    def traj_copy(_it: int) -> float:
        p = 0 if n_points <= 1 else (_it % n_points)
        x = np.ascontiguousarray(pts[:3, p, :].T)
        return float(x[_it % max(n_frames, 1), 0])

    def window_copy(_it: int) -> float:
        if t256 <= 0:
            return 0.0
        x = np.ascontiguousarray(pts[:3, :, start : start + t256].transpose(2, 1, 0))
        idx = (_it % max(t256 * max(n_points, 1), 1))
        return float(x[idx // max(n_points, 1), idx % max(n_points, 1), 0])

    frame_copy_ms_kall, chk0, _iters0 = _time_ms_adaptive(frame_copy, target_total_ms=150.0, max_iters=1 << 12)
    traj_strided_ms_Tfull_k1, chk1, _iters1 = _time_ms_adaptive(traj_copy, target_total_ms=250.0, max_iters=1 << 10)
    window_read_ms_T256_kall, chk2, _iters2 = _time_ms_adaptive(
        window_copy, target_total_ms=250.0, max_iters=1 << 10
    )
    mem2 = _snapshot_memory()
    checksum = float(chk0 + chk1 + chk2 + float(mem2.rss_mb))

    return {
        "lib": "ezc3d",
        "lang": "python",
        "mode": "materialize",
        "repeat": int(repeat),
        "input": str(file_path),
        "file_bytes": int(file_path.stat().st_size),
        "frames": n_frames,
        "points": n_points,
        "rss_baseline_mb": float(mem0.rss_mb),
        "peak_rss_mb": float(max(mem1.peak_rss_mb, mem2.peak_rss_mb)),
        "peak_rss_delta_mb": float(max(mem1.peak_rss_mb, mem2.peak_rss_mb) - mem0.rss_mb),
        "load_ms": float(sum_load / float(max(1, int(repeat)))),
        "frame_copy_ms_kall": float(frame_copy_ms_kall),
        "window_read_ms_T256_kall": float(window_read_ms_T256_kall),
        "traj_strided_ms_Tfull_k1": float(traj_strided_ms_Tfull_k1),
        "checksum": float(checksum),
        "ezc3d_version": str(getattr(ezc3d, "__version__", "")),
    }


def _print_kv(result: dict) -> None:
    def _fmt(v) -> str:
        if isinstance(v, float):
            return f"{v:.6f}"
        return str(v)

    for k, v in result.items():
        print(f"{k}={_fmt(v)}")


def _parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("c3d", type=Path, help="path to a .c3d file")
    ap.add_argument("--lib", choices=["sqzc3d", "ezc3d"], required=True)
    ap.add_argument("--repeat", type=int, default=1, help="repeat load for averaging (default: 1)")
    return ap.parse_args()


def main() -> int:
    args = _parse_args()
    file_path = Path(args.c3d).expanduser().resolve()
    if not file_path.is_file() or file_path.suffix.lower() != ".c3d":
        raise SystemExit(f"invalid c3d path: {file_path}")

    if int(args.repeat) < 1 or int(args.repeat) > 20:
        raise SystemExit("--repeat must be in [1, 20]")

    if args.lib == "sqzc3d":
        result = _bench_sqzc3d(file_path, int(args.repeat))
    else:
        result = _bench_ezc3d(file_path, int(args.repeat))

    _print_kv(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
