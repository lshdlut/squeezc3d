#!/usr/bin/env python3
"""Smoke comparison of sqzc3d against ezc3d true values."""

from __future__ import annotations

import argparse
import random
import sys
import tempfile
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Sequence

import numpy as np


_FLOAT_TOL = 1e-10
_REL_TOL = 1e-8
_MAX_TREE_DIFF = 200


def _shape_str(a) -> str:
    try:
        return str(tuple(np.asarray(a).shape))
    except Exception:
        return "<unknown>"


def _check_empty_points_and_analogs(chunk, *, where: str) -> List[str]:
    issues: List[str] = []

    try:
        pts, pts_valid = chunk.points()
        pts = np.asarray(pts)
        pts_valid = np.asarray(pts_valid)
        if not (pts.ndim == 3 and pts.shape[1] == 0 and pts.shape[2] == 3):
            issues.append(f"{where}: chunk.points() expected (T, 0, 3), got {pts.shape}")
        if not (pts_valid.ndim == 2 and pts_valid.shape[1] == 0):
            issues.append(f"{where}: points_valid expected (T, 0), got {pts_valid.shape}")
    except Exception as exc:
        issues.append(f"{where}: chunk.points() raised: {type(exc).__name__}: {exc}")

    try:
        ana, ana_valid = chunk.analogs()
        ana = np.asarray(ana)
        ana_valid = np.asarray(ana_valid)
        if not (ana.ndim == 2 and ana.shape[0] == 0):
            issues.append(f"{where}: chunk.analogs() expected (0, N), got {ana.shape}")
        if not (ana_valid.ndim == 2 and ana_valid.shape[0] == 0):
            issues.append(f"{where}: analog_valid expected (0, N), got {ana_valid.shape}")
    except Exception as exc:
        issues.append(f"{where}: chunk.analogs() raised: {type(exc).__name__}: {exc}")

    return issues


def _check_python_selector_semantics(sqzc3d, file_path: Path, dec, expected_meta: dict) -> List[str]:
    issues: List[str] = []
    exp_points = int(expected_meta.get("n_points", 0) or 0)
    exp_analogs = int(expected_meta.get("n_analogs", 0) or 0)

    # None = ALL (should match the default full read counts).
    try:
        c_all = dec.read(frame_count=1, points=None, analogs=None, analog_range=None)
        meta_all = c_all.meta
        if exp_points > 0 and int(meta_all.get("n_points", -1)) != exp_points:
            issues.append(f"selector(None): n_points mismatch: got={meta_all.get('n_points')} expected={exp_points}")
        if exp_analogs >= 0 and int(meta_all.get("n_analogs", -1)) != exp_analogs:
            issues.append(f"selector(None): n_analogs mismatch: got={meta_all.get('n_analogs')} expected={exp_analogs}")
    except Exception as exc:
        issues.append(f"selector(None): Decoder.read failed: {type(exc).__name__}: {exc}")

    # [] = empty
    try:
        c_empty = dec.read(frame_count=1, points=[], analogs=[], analog_range=None)
        meta_empty = c_empty.meta
        if int(meta_empty.get("n_points", -1)) != 0:
            issues.append(f"selector([]): expected n_points=0, got {meta_empty.get('n_points')}")
        if int(meta_empty.get("n_analogs", -1)) != 0:
            issues.append(f"selector([]): expected n_analogs=0, got {meta_empty.get('n_analogs')}")
        issues.extend(_check_empty_points_and_analogs(c_empty, where="selector([])"))
    except Exception as exc:
        issues.append(f"selector([]): Decoder.read failed: {type(exc).__name__}: {exc}")

    # Easy layer smoke (should be available on the sqzc3d Python package).
    try:
        if not hasattr(sqzc3d, "read"):
            issues.append("easy: sqzc3d.read is missing")
        else:
            v = sqzc3d.read(str(file_path), frame_count=1)
            pts = np.asarray(v.points)
            if not (pts.ndim == 3 and pts.shape[2] == 3):
                issues.append(f"easy: view.points shape unexpected: {_shape_str(pts)}")
            v0 = sqzc3d.read(str(file_path), frame_count=1, points=[], analogs=[])
            pts0 = np.asarray(v0.points)
            ana0 = np.asarray(v0.analogs)
            if not (pts0.ndim == 3 and pts0.shape[1] == 0):
                issues.append(f"easy: points=[] expected (T, 0, 3), got {_shape_str(pts0)}")
            if not (ana0.ndim == 2 and ana0.shape[0] == 0):
                issues.append(f"easy: analogs=[] expected (0, N), got {_shape_str(ana0)}")
    except Exception as exc:
        issues.append(f"easy: read failed: {type(exc).__name__}: {exc}")

    return issues


def _find_repo_root() -> Path:
    candidate = Path(__file__).resolve().parent
    while candidate.parent != candidate:
        if (candidate / ".git").exists() or candidate.name == "squeezc3d":
            return candidate
        candidate = candidate.parent

    raise RuntimeError("failed to locate repository root for sqzc3d")


def _ensure_sqzc3d_import():
    try:
        import sqzc3d  # type: ignore

        return sqzc3d
    except Exception:
        repo = _find_repo_root()
        python_pkg = repo / "python"
        if str(python_pkg) not in sys.path:
            sys.path.insert(0, str(python_pkg))
        return __import__("sqzc3d")


def _safe_numpy_array(x) -> np.ndarray:
    arr = np.asarray(x)
    if arr.dtype == np.object_:
        return np.array([str(v) if v is not None else "" for v in arr], dtype=object)
    return arr


def _to_dict_like(value: Any) -> Any:
    if isinstance(value, dict):
        return value
    try:
        if hasattr(value, "keys"):
            return {str(k): value[k] for k in value.keys()}
    except Exception:
        return value
    return value


def _safe_number(value: Any, default=None):
    try:
        if isinstance(value, (list, tuple, np.ndarray)):
            if len(value) == 0:
                return default
            value = value[0]
        if value is None:
            return default
        return float(value)
    except Exception:
        return default


def _normalize_unit_token(raw: str) -> str:
    return "".join(str(raw or "").strip().lower().split())


def _units_per_meter_from_unit_token(raw: str) -> float:
    u = _normalize_unit_token(raw)
    if u in ("m", "meter", "meters"):
        return 1.0
    if u in ("cm", "centimeter", "centimeters"):
        return 100.0
    if u in ("mm", "millimeter", "millimeters"):
        return 1000.0
    if u in ("in", "inch", "inches"):
        return 39.37007874015748
    if u in ("ft", "foot", "feet"):
        return 3.280839895013123
    return 0.0


def _to_plain_value(value: Any):
    if isinstance(value, np.ndarray):
        if value.ndim == 0:
            return value.item()
        return [_to_plain_value(v) for v in value.tolist()]
    if isinstance(value, (np.integer, np.floating, np.bool_)):
        return value.item()
    if isinstance(value, (bytes, bytearray, memoryview)):
        return bytes(value).decode("utf-8", errors="ignore")
    if isinstance(value, (list, tuple)):
        return [_to_plain_value(v) for v in value]
    if isinstance(value, dict):
        out = {}
        for k, v in value.items():
            out[str(k)] = _to_plain_value(v)
        return out
    return value


def _flatten_nested_value(value: Any) -> List[Any]:
    flat: List[Any] = []

    def _walk(v):
        if isinstance(v, (list, tuple)):
            for item in v:
                _walk(item)
        else:
            flat.append(v)

    _walk(value)
    return flat


def _normalize_ezc3d_tree_param_value(param_like: Any):
    if isinstance(param_like, dict):
        key_lower = {str(k).lower(): v for k, v in param_like.items()}
        if "value" in key_lower:
            value = _to_plain_value(key_lower["value"])
            arr = np.asarray(value)
            if isinstance(arr, np.ndarray) and arr.ndim > 1:
                return arr.T.reshape(-1).tolist()
            if isinstance(value, (list, tuple)):
                return _flatten_nested_value(value)
            if isinstance(arr, np.ndarray):
                return arr.tolist()
            return value
        if "values" in key_lower:
            value = _to_plain_value(key_lower["values"])
            arr = np.asarray(value)
            if isinstance(arr, np.ndarray) and arr.ndim > 1:
                return arr.T.reshape(-1).tolist()
            if isinstance(value, (list, tuple)):
                return _flatten_nested_value(value)
            if isinstance(arr, np.ndarray):
                return arr.tolist()
            return value
        if "values_as_string" in key_lower:
            return _to_plain_value(key_lower["values_as_string"])
        if "values_as_double" in key_lower:
            value = _to_plain_value(key_lower["values_as_double"])
            if isinstance(value, (list, tuple)):
                return _flatten_nested_value(value)
            return value
        if "values_as_int" in key_lower:
            return _to_plain_value(key_lower["values_as_int"])
    return _to_plain_value(param_like)


def _normalize_ezc3d_meta_tree(ez_parameters) -> dict:
    groups: Dict[str, dict] = {}
    for g_name, g in ez_parameters.items():
        if not isinstance(g_name, str):
            continue
        g_dict = g if isinstance(g, dict) else {}
        g_meta = g_dict.get("__METADATA__", {}) if isinstance(g_dict, dict) else {}
        g_description = ""
        g_locked = False
        if isinstance(g_meta, dict):
            if "DESCRIPTION" in g_meta:
                g_description = str(g_meta["DESCRIPTION"] or "")
            elif "description" in g_meta:
                g_description = str(g_meta["description"] or "")
            if "IS_LOCKED" in g_meta:
                g_locked = bool(g_meta["IS_LOCKED"])
            elif "is_locked" in g_meta:
                g_locked = bool(g_meta["is_locked"])

        params = {}
        for p_name, p in g_dict.items():
            if p_name == "__METADATA__" or not isinstance(p_name, str) or not isinstance(p, dict):
                continue
            p_description = ""
            p_locked = False
            p_type = None
            if "description" in p:
                p_description = str(p["description"] or "")
            elif "DESCRIPTION" in p:
                p_description = str(p["DESCRIPTION"] or "")
            if "is_locked" in p:
                p_locked = bool(p["is_locked"])
            elif "IS_LOCKED" in p:
                p_locked = bool(p["IS_LOCKED"])
            if "type" in p:
                try:
                    p_type = int(p["type"])
                except Exception:
                    p_type = p["type"]
            elif "TYPE" in p:
                try:
                    p_type = int(p["TYPE"])
                except Exception:
                    p_type = p["TYPE"]

            params[p_name] = {
                "name": p_name,
                "description": p_description,
                "locked": bool(p_locked),
                "type": p_type,
                "values": _normalize_ezc3d_tree_param_value(p),
            }

        groups[g_name] = {
            "name": g_name,
            "description": g_description,
            "locked": bool(g_locked),
            "parameters": params,
        }
    return {"groups": groups}


def _compare_value(path: str, left: Any, right: Any, out: List[str], max_diffs: int = _MAX_TREE_DIFF):
    if len(out) >= max_diffs:
        return

    if isinstance(left, dict) and not isinstance(right, dict):
        out.append(f"{path}: right side is not dict ({type(right).__name__})")
        return
    if isinstance(right, dict) and not isinstance(left, dict):
        out.append(f"{path}: left side is not dict ({type(left).__name__})")
        return

    if isinstance(left, dict):
        left = _to_plain_value(left)
        right = _to_plain_value(right)
        if not isinstance(right, dict):
            out.append(f"{path}: type mismatch ({type(left).__name__} vs {type(right).__name__})")
            return
        lkeys = set(left.keys())
        rkeys = set(right.keys())
        for key in sorted(lkeys | rkeys):
            p = f"{path}.{key}"
            if key not in right:
                out.append(f"{p}: missing in ezc3d")
                continue
            if key not in left:
                out.append(f"{p}: missing in sqzc3d")
                continue
            _compare_value(p, left[key], right[key], out, max_diffs=max_diffs)
            if len(out) >= max_diffs:
                return
        return

    if isinstance(left, (list, tuple)) and isinstance(right, (list, tuple)):
        if len(left) != len(right):
            out.append(f"{path}: length mismatch (sqzc3d={len(left)} ezc3d={len(right)})")
            return
        for idx, (lv, rv) in enumerate(zip(left, right)):
            _compare_value(f"{path}[{idx}]", lv, rv, out, max_diffs=max_diffs)
            if len(out) >= max_diffs:
                return
        return

    if isinstance(left, np.ndarray) or isinstance(right, np.ndarray):
        a = np.asarray(left)
        b = np.asarray(right)
        if a.shape != b.shape:
            out.append(f"{path}: ndarray shape mismatch (sqzc3d={a.shape} ezc3d={b.shape})")
            return
        if a.size == 0:
            return
        if np.issubdtype(a.dtype, np.floating) or np.issubdtype(b.dtype, np.floating):
            eq = np.isclose(a, b, rtol=_REL_TOL, atol=_FLOAT_TOL, equal_nan=True)
            if not bool(np.all(eq)):
                diff = np.abs(a.astype(float) - b.astype(float))
                out.append(
                    f"{path}: ndarray mismatch count={np.count_nonzero(~eq)}/{eq.size}, "
                    f"max_abs_diff={float(np.max(diff)):.6g}"
                )
            return
        if not np.array_equal(a, b):
            out.append(f"{path}: ndarray mismatch count={np.count_nonzero(a != b)}/{a.size}")
        return

    if isinstance(left, (float, np.floating)) and isinstance(right, (float, np.floating)):
        if not (np.isfinite(left) and np.isfinite(right)):
            return
        if not np.isclose(left, right, rtol=_REL_TOL, atol=_FLOAT_TOL):
            out.append(f"{path}: float mismatch sqzc3d={left} ezc3d={right}")
        return

    if left != right:
        out.append(f"{path}: mismatch sqzc3d={left!r} ezc3d={right!r}")


def _collect_files_from_roots(roots: Sequence[str]) -> List[Path]:
    out: List[Path] = []
    for root in roots:
        p = Path(root).expanduser()
        if not p.exists():
            raise FileNotFoundError(f"root not found: {p}")
        if p.suffix.lower() == ".c3d":
            out.append(p)
            continue
        for fp in p.rglob("*.c3d"):
            if fp.is_file():
                out.append(fp)
    out.sort()
    seen = set()
    uniq: List[Path] = []
    for p in out:
        rp = str(p)
        if rp not in seen:
            seen.add(rp)
            uniq.append(p)
    return uniq


@dataclass
class CompareReport:
    file: str
    passed: bool
    details: List[str]


def _extract_ezc3d_arrays(ez) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, int]:
    # ez['data']['points'] expected shape: [4, n_points, n_frames].
    ez_points_raw = _safe_numpy_array(ez["data"]["points"]).astype(np.float64)
    if ez_points_raw.ndim != 3 or ez_points_raw.shape[0] < 3:
        raise ValueError(f"unexpected ezc3d points shape {ez_points_raw.shape}")
    point_count = int(ez_points_raw.shape[1])
    frame_count = int(ez_points_raw.shape[2])

    points = np.transpose(ez_points_raw[0:3], (2, 1, 0))
    if ez_points_raw.shape[0] > 3:
        residual = np.asarray(ez_points_raw[3], dtype=np.float64).T
    else:
        residual = np.full((frame_count, point_count), np.nan, dtype=np.float64)
    valid = np.isfinite(points).all(axis=2).astype(np.uint8)

    # ezc3d analogs usually [1, n_analogs, n_frames * n_analog_by_frame].
    ez_analogs_raw = _safe_numpy_array(ez["data"]["analogs"]).astype(np.float64)
    if ez_analogs_raw.ndim == 2:
        analogs = ez_analogs_raw
    elif ez_analogs_raw.ndim == 3 and ez_analogs_raw.shape[0] == 1:
        analogs = np.asarray(ez_analogs_raw[0])
    elif ez_analogs_raw.ndim == 3:
        analogs = np.moveaxis(ez_analogs_raw, 0, -1)
        analogs = np.moveaxis(analogs, 0, 0).reshape(analogs.shape[1], -1)
    else:
        raise ValueError(f"unexpected ezc3d analogs shape {ez_analogs_raw.shape}")

    analog_valid = np.isfinite(analogs).astype(np.uint8)

    params = _to_dict_like(ez.get("parameters", {}))
    point_group = params.get("POINT", {}) if isinstance(params, dict) else {}
    analog_group = params.get("ANALOG", {}) if isinstance(params, dict) else {}
    point_rate = _safe_number(point_group.get("RATE", {}).get("value")) if isinstance(point_group, dict) else None
    analog_rate = _safe_number(analog_group.get("RATE", {}).get("value")) if isinstance(analog_group, dict) else None

    if point_rate is None or analog_rate is None:
        header = _to_plain_value(_to_dict_like(ez.get("header", {})))
        if point_rate is None and isinstance(header.get("points"), dict):
            point_rate = _safe_number(header["points"].get("frame_rate"), point_rate)
        if analog_rate is None and isinstance(header.get("analogs"), dict):
            analog_rate = _safe_number(header["analogs"].get("frame_rate"), analog_rate)

    if point_rate and analog_rate and point_rate > 0:
        analog_by_frame = int(round(analog_rate / point_rate))
        if analog_by_frame <= 0:
            analog_by_frame = 1
    elif analogs.size > 0 and frame_count > 0:
        analog_by_frame = max(1, int(analogs.shape[1] / frame_count))
    else:
        analog_by_frame = 0

    # Keep points in ezc3d raw units to match sqzc3d default output.
    # (Unit normalization/scaling is explicitly stress-tested in stress_sqzc3d.py:S07.)

    return points, valid, analogs, analog_valid, max(analog_by_frame, 1 if analogs.shape[0] > 0 else 0)


def _build_reference_meta(ez, ez_points: np.ndarray, ez_point_valid: np.ndarray, ez_analogs: np.ndarray) -> dict:
    header = _to_plain_value(_to_dict_like(ez.get("header", {})))
    point_header = header.get("points", {}) if isinstance(header, dict) else {}
    analog_header = header.get("analogs", {}) if isinstance(header, dict) else {}
    params = _to_dict_like(ez.get("parameters", {}))

    point_first = _safe_number(point_header.get("first_frame"), None)
    point_last = _safe_number(point_header.get("last_frame"), None)
    if point_first is not None and point_last is not None:
        n_frames = max(0, int(point_last) - int(point_first) + 1)
    else:
        n_frames = int(point_header.get("size", ez_points.shape[0]))
    n_points = int(ez_points.shape[1]) if ez_points.ndim >= 2 else 0
    n_points_total = int(point_header.get("size", n_points))
    n_analogs = int(ez_analogs.shape[0]) if ez_analogs.ndim > 0 else 0
    n_analog_by_frame = 0
    if n_frames > 0 and n_analogs > 0 and ez_analogs.size > 0:
        n_analog_by_frame = max(1, int(ez_analogs.shape[1] / n_frames))

    n_scalar = int(ez_points.shape[0] * ez_points.shape[1] * 3) if ez_points.size else 0
    valid_nscalar = int(ez_point_valid.size)
    n_analog_scalar = int(ez_analogs.size)
    n_type_groups = 0
    point_group = params.get("POINT", {}) if isinstance(params, dict) else {}
    if isinstance(point_group, dict):
        type_groups = point_group.get("TYPE_GROUPS", {})
        if isinstance(type_groups, dict):
            names = type_groups.get("value") if "value" in type_groups else type_groups.get("values")
            if isinstance(names, (list, tuple)):
                n_type_groups = len(names)
            elif isinstance(names, str):
                n_type_groups = 1 if names else 0

    point_scale = 1.0
    if isinstance(point_group, dict):
        p_scale = point_group.get("SCALE", {})
        if isinstance(p_scale, dict):
            point_scale = _safe_number(p_scale.get("value"), point_scale) or point_scale

    return {
        "n_frames": n_frames,
        "n_points": n_points,
        "n_points_total": n_points_total,
        "n_analogs": n_analogs,
        "n_analog_by_frame": n_analog_by_frame,
        "n_scalar": n_scalar,
        "valid_nscalar": valid_nscalar,
        "n_analog_scalar": n_analog_scalar,
        "n_type_groups": n_type_groups,
        "point_scale": float(point_scale),
        "analog_layout": "CN",
    }


def _extract_sqzc3d_labels(chunk, key: str) -> List[str]:
    meta = chunk.meta
    labels = meta.get(key, [])
    return [str(v) if v is not None else "" for v in labels]


def _extract_ezc3d_label_series(params: Dict[str, Any], group: str, base: str) -> List[str]:
    group_obj = params.get(group, {}) if isinstance(params, dict) else {}
    if not isinstance(group_obj, dict):
        return []
    out: List[str] = []
    i = 1
    while True:
        key = base if i == 1 else f"{base}{i}"
        param = group_obj.get(key)
        if not isinstance(param, dict):
            break
        values = param.get("value", param.get("values", []))
        values_plain = _to_plain_value(values)
        if isinstance(values_plain, (list, tuple)):
            for item in values_plain:
                out.append("" if item is None else str(item))
        elif values_plain is not None:
            out.append("" if values_plain is None else str(values_plain))
        i += 1
    return out


def _compare_points(sq_chunk, ez_chunk) -> List[str]:
    sq_pts, sq_valid = sq_chunk.points(None, copy=True)
    ez_pts, ez_valid, _, _, ez_analog_by_frame = ez_chunk
    sq_pts = np.asarray(sq_pts, dtype=np.float64)
    sq_valid = np.asarray(sq_valid, dtype=np.uint8)
    ez_pts = np.asarray(ez_pts, dtype=np.float64)
    ez_valid = np.asarray(ez_valid, dtype=np.uint8)

    out: List[str] = []
    if sq_pts.shape != ez_pts.shape:
        out.append(f"point shape mismatch: sqzc3d={sq_pts.shape} ezc3d={ez_pts.shape}")
        return out
    if sq_valid.shape != ez_valid.shape:
        out.append(f"point valid shape mismatch: sqzc3d={sq_valid.shape} ezc3d={ez_valid.shape}")

    valid_mask = (sq_valid == 1) | (ez_valid == 1)
    if not np.array_equal(sq_valid, ez_valid):
        mism = np.count_nonzero(sq_valid.astype(np.int8) != ez_valid.astype(np.int8))
        out.append(f"point valid mismatch count={mism}/{sq_valid.size}")
    if np.any(~np.isfinite(ez_pts[valid_mask])):
        out.append("ezc3d point contains NaN/Inf in valid positions")

    if valid_mask.any():
        sq_sel = sq_pts.reshape(-1, 3)[valid_mask.reshape(-1)]
        ez_sel = ez_pts.reshape(-1, 3)[valid_mask.reshape(-1)]
        eq = np.isclose(
            sq_sel,
            ez_sel,
            rtol=_REL_TOL,
            atol=_FLOAT_TOL,
            equal_nan=True,
        )
        if not bool(np.all(eq)):
            diff = np.abs(sq_sel - ez_sel)
            out.append(
                f"point value mismatch at {np.count_nonzero(~np.all(eq, axis=1))} valid entries, "
                f"max_abs_diff={float(diff.max()):.6g}"
            )
    return out


def _compare_analogs(sq_chunk, ez_chunk) -> List[str]:
    sq_analog, sq_valid = sq_chunk.analogs(None, layout="CN", copy=True)
    _, _, ez_analog, ez_valid, ez_analog_by_frame = ez_chunk
    sq_analog = np.asarray(sq_analog, dtype=np.float64)
    sq_valid = np.asarray(sq_valid, dtype=np.uint8)
    ez_analog = np.asarray(ez_analog, dtype=np.float64)
    ez_valid = np.asarray(ez_valid, dtype=np.uint8)

    out: List[str] = []
    if sq_analog.shape != ez_analog.shape:
        out.append(f"analog shape mismatch: sqzc3d={sq_analog.shape} ezc3d={ez_analog.shape}")
        if sq_analog.ndim != 2 or ez_analog.ndim != 2:
            return out
    if sq_valid.shape != ez_valid.shape:
        out.append(f"analog valid shape mismatch: sqzc3d={sq_valid.shape} ezc3d={ez_valid.shape}")

    if sq_analog.size == 0 and ez_analog.size == 0:
        return out
    if sq_analog.size == 0:
        out.append("sqzc3d analog empty")
        return out
    if ez_analog.size == 0:
        out.append("ezc3d analog empty")
        return out

    if sq_analog.shape == ez_analog.shape:
        if not np.array_equal(sq_valid, ez_valid.astype(np.uint8)):
            mism = np.count_nonzero(sq_valid.astype(np.int8) != ez_valid.astype(np.int8))
            out.append(f"analog valid mismatch count={mism}/{sq_valid.size}")
        if np.any(~np.isfinite(ez_analog)):
            out.append("ezc3d analog contains NaN/Inf")

        eq = np.isclose(sq_analog, ez_analog, rtol=_REL_TOL, atol=_FLOAT_TOL, equal_nan=True)
        if not bool(np.all(eq)):
            diff = np.abs(sq_analog - ez_analog)
            out.append(
                f"analog value mismatch count={np.count_nonzero(~eq)}/{eq.size}, "
                f"max_abs_diff={float(diff.max()):.6g}, "
                f"n_channels={sq_analog.shape[0]}, n_samples={sq_analog.shape[1]}, "
                f"expected analog_by_frame_ezc3d={ez_analog_by_frame}"
            )
    return out


def _compare_meta_counts(sq_meta: dict, ez_meta: dict) -> List[str]:
    out: List[str] = []
    for key in sorted(ez_meta.keys()):
        if key not in sq_meta:
            out.append(f"meta missing key: {key}")
            continue
        s = _to_plain_value(sq_meta.get(key))
        e = _to_plain_value(ez_meta[key])
        if isinstance(s, (int, float)) and isinstance(e, (int, float)):
            if isinstance(s, int) or s.is_integer() if isinstance(s, float) else False:
                if int(s) != int(e):
                    out.append(f"meta[{key}] mismatch: sqzc3d={s} ezc3d={e}")
            elif not np.isclose(float(s), float(e), rtol=_REL_TOL, atol=_FLOAT_TOL):
                out.append(f"meta[{key}] mismatch: sqzc3d={s} ezc3d={e}")
        elif s != e:
            out.append(f"meta[{key}] mismatch: sqzc3d={s!r} ezc3d={e!r}")
    return out


def _drop_meta_tree_dimensions(value: Any) -> Any:
    if isinstance(value, dict):
        out: Dict[str, Any] = {}
        for k, v in value.items():
            if k == "dimensions":
                continue
            out[str(k)] = _drop_meta_tree_dimensions(v)
        return out
    if isinstance(value, (list, tuple)):
        return [_drop_meta_tree_dimensions(v) for v in value]
    return value


def _meta_tree_dimension_issues(sq_tree: Any) -> List[str]:
    issues: List[str] = []
    if not isinstance(sq_tree, dict):
        return issues
    groups = sq_tree.get("groups", {})
    if not isinstance(groups, dict):
        return issues

    def _prod(xs: List[int]) -> int:
        p = 1
        for v in xs:
            p *= v
        return p

    for g_name, g in groups.items():
        if not isinstance(g, dict):
            continue
        params = g.get("parameters", {})
        if not isinstance(params, dict):
            continue
        for p_name, p in params.items():
            if not isinstance(p, dict):
                continue
            if "values" not in p:
                continue
            if "dimensions" not in p:
                issues.append(f"meta_tree.{g_name}.{p_name}: missing dimensions")
                continue
            dims = p.get("dimensions")
            if not isinstance(dims, (list, tuple)):
                issues.append(f"meta_tree.{g_name}.{p_name}: dimensions not a list")
                continue
            dims_i: List[int] = []
            bad = False
            for d in dims:
                try:
                    di = int(d)
                except Exception:
                    bad = True
                    break
                # ezc3d may report zero-sized dimensions (e.g. unused/empty parameters),
                # and CHAR parameters may have dimension[0] == 0 when all strings are empty.
                if di < 0:
                    bad = True
                    break
                dims_i.append(di)
            if bad:
                issues.append(f"meta_tree.{g_name}.{p_name}: invalid dimensions={dims!r}")
                continue

            values = p.get("values")
            if not isinstance(values, (list, tuple)):
                issues.append(f"meta_tree.{g_name}.{p_name}: values not a list")
                continue

            p_type = p.get("type")
            try:
                p_type_i = int(p_type) if p_type is not None else None
            except Exception:
                p_type_i = None

            if p_type_i == -1:
                expected = _prod(dims_i[1:]) if len(dims_i) > 1 else 1
            else:
                expected = _prod(dims_i) if len(dims_i) > 0 else (1 if len(values) > 0 else 0)

            if expected != len(values):
                issues.append(
                    f"meta_tree.{g_name}.{p_name}: dimensions inconsistent dims={dims_i} values={len(values)} expected={expected}"
                )

    return issues


def _compare_meta_tree(sq_chunk, ez) -> List[str]:
    out: List[str] = []
    try:
        sq_tree = _to_plain_value(sq_chunk.meta_tree)
    except Exception as exc:
        return [f"meta_tree access failed: {type(exc).__name__}: {exc}"]

    ez_params = _to_dict_like(ez.get("parameters", {}))

    expected_tree = _normalize_ezc3d_meta_tree(ez_params)
    _compare_value("meta_tree", _drop_meta_tree_dimensions(sq_tree), expected_tree, out)
    out.extend(_meta_tree_dimension_issues(sq_tree))
    if len(out) > 0:
        return out[:_MAX_TREE_DIFF]
    return out


def _load_ezc3d_reference(file_path: Path):
    import ezc3d
    try:
        return ezc3d.c3d(str(file_path)), None
    except OSError:
        fallback_tmp = None
        try:
            with tempfile.NamedTemporaryFile(suffix=".c3d", delete=False) as tmp:
                fallback_tmp = Path(tmp.name)
            shutil.copy2(file_path, fallback_tmp)
            return ezc3d.c3d(str(fallback_tmp)), fallback_tmp
        except Exception:
            if fallback_tmp and fallback_tmp.exists():
                try:
                    fallback_tmp.unlink()
                except OSError:
                    pass
            raise


def run_one(file_path: Path, strict: bool = True) -> CompareReport:
    sqzc3d = _ensure_sqzc3d_import()
    dec = sqzc3d.Decoder(str(file_path))
    pre_issues: List[str] = []
    try:
        sq_chunk = dec.read(frame_count=-1, points=None, analogs=None, analog_range=None)
        pre_issues.extend(_check_python_selector_semantics(sqzc3d, file_path, dec, sq_chunk.meta))
    finally:
        try:
            dec.close()
        except Exception:
            pass

    ez, fallback_path = _load_ezc3d_reference(file_path)
    try:
        ez_points, ez_point_valid, ez_analog, ez_analog_valid, ez_analog_by_frame = _extract_ezc3d_arrays(ez)
    finally:
        if fallback_path is not None:
            try:
                fallback_path.unlink()
            except OSError:
                pass

    ez_meta_ref = _build_reference_meta(ez, ez_points, ez_point_valid, ez_analog)

    issues: List[str] = []
    issues.extend(pre_issues)
    issues.extend(_compare_points(sq_chunk, (ez_points, ez_point_valid, ez_analog, ez_analog_valid, ez_analog_by_frame)))
    issues.extend(_compare_analogs(sq_chunk, (ez_points, ez_point_valid, ez_analog, ez_analog_valid, ez_analog_by_frame)))

    point_labels_sq = _extract_sqzc3d_labels(sq_chunk, "point_labels")
    analog_labels_sq = _extract_sqzc3d_labels(sq_chunk, "analog_labels")
    ez_params = _to_dict_like(ez.get("parameters", {}))
    ez_point_labels = _extract_ezc3d_label_series(ez_params, "POINT", "LABELS")
    ez_analog_labels = _extract_ezc3d_label_series(ez_params, "ANALOG", "LABELS")
    if np.asarray(ez_analog).shape[0] == 0 and len(ez_analog_labels) == 1:
        # ezc3d sometimes exposes a placeholder label even when ANALOG:USED == 0.
        # Treat it as empty so points-only files don't fail strict label checks.
        if str(ez_analog_labels[0]).strip().lower() in ("nodata",):
            ez_analog_labels = []
    if [str(v) if v is not None else "" for v in point_labels_sq] != [str(v) if v is not None else "" for v in ez_point_labels]:
        issues.append(f"point labels mismatch (sq={point_labels_sq[:3]}... ez={list(ez_point_labels)[:3]}...)")
    if [str(v) if v is not None else "" for v in analog_labels_sq] != [str(v) if v is not None else "" for v in ez_analog_labels]:
        issues.append(f"analog labels mismatch (sq={analog_labels_sq[:3]}... ez={list(ez_analog_labels)[:3]}...)")

    if strict:
        issues.extend(_compare_meta_counts(sq_chunk.meta, ez_meta_ref))
        issues.extend(_compare_meta_tree(sq_chunk, ez))

    return CompareReport(file=str(file_path), passed=(len(issues) == 0), details=issues)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Compare sqzc3d against ezc3d on sampled .c3d files.")
    ap.add_argument("files", nargs="*", help="specific .c3d file(s)")
    ap.add_argument("--roots", nargs="*", default=[], help="directories to recursively discover .c3d files")
    ap.add_argument("--sample", type=int, default=6, help="how many files to sample (default: 6)")
    ap.add_argument("--seed", type=int, default=42, help="random seed for sampling")
    ap.add_argument(
        "--strict",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="enable full meta/eztree checks; use --no-strict to only do points+analogs+labels",
    )
    ap.add_argument("--max-fail", type=int, default=999, help="stop after N failed files")
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    files = [Path(p) for p in args.files]
    if args.roots:
        files.extend(_collect_files_from_roots(args.roots))
    files = [f.expanduser().resolve() for f in files if f.suffix.lower() == ".c3d" and f.is_file()]
    files = sorted(dict.fromkeys(files))
    if not files:
        print("No .c3d files found. Pass paths via positional args or --roots.", file=sys.stderr)
        return 2

    rnd = random.Random(args.seed)
    if args.sample <= 0 or args.sample >= len(files):
        selected = files
    else:
        selected = rnd.sample(files, args.sample)

    if len(selected) < len(files):
        print(f"smoke: selected {len(selected)}/{len(files)} files (seed={args.seed}, strict={args.strict})")
    else:
        print(f"smoke: selected all {len(selected)} files (strict={args.strict})")

    passed = 0
    failed = 0
    for fp in selected:
        try:
            rep = run_one(fp, strict=args.strict)
        except Exception as exc:
            failed += 1
            print(f"[FAIL] {fp} :: {type(exc).__name__}: {exc}")
            if failed >= args.max_fail:
                break
            continue

        if rep.passed:
            passed += 1
            print(f"[PASS] {fp}")
        else:
            failed += 1
            print(f"[FAIL] {fp}")
            for d in rep.details:
                print(f"  - {d}")
            if failed >= args.max_fail:
                break

    print(f"result: pass={passed}, fail={failed}, total={passed + failed}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
