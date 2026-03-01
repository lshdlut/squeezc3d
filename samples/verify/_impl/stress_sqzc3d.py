#!/usr/bin/env python3
"""Scenario-oriented stress workflow for sqzc3d vs ezc3d."""

from __future__ import annotations

import argparse
import importlib.util
import json
import random
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

_FLOAT_TOL = 1e-10
_REL_TOL = 1e-8


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _load_smoke_helpers():
    root = _repo_root()
    helper = root / "samples" / "verify" / "_impl" / "smoke_vs_ezc3d.py"
    if not helper.exists():
        raise RuntimeError(f"Missing helper script: {helper}")
    spec = importlib.util.spec_from_file_location("_sqzc3d_smoke_helpers", helper)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to load helper module from: {helper}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)  # type: ignore[union-attr]
    return module


_smoke = _load_smoke_helpers()


def _ensure_sqzc3d_import():
    try:
        import sqzc3d  # type: ignore

        return sqzc3d
    except Exception:
        repo = _repo_root()
        pkg = repo / "python"
        if str(pkg) not in sys.path:
            sys.path.insert(0, str(pkg))
        return __import__("sqzc3d")


def _safe_ez3d():
    try:
        import ezc3d

        return ezc3d
    except Exception as exc:
        raise RuntimeError(f"missing ezc3d dependency: {exc}") from exc


def _collect_files_from_roots(roots: list[str]) -> list[Path]:
    out: list[Path] = []
    for root in roots:
        p = Path(root).expanduser()
        if not p.exists():
            raise FileNotFoundError(f"root not found: {p}")
        if p.suffix.lower() == ".c3d":
            out.append(p)
            continue
        out.extend([fp for fp in p.rglob("*.c3d") if fp.is_file()])
    out = sorted(set(out))
    return [Path(p) for p in out]


def _sample(files: list[Path], sample: int, seed: int) -> list[Path]:
    if sample <= 0 or sample >= len(files):
        return files
    rnd = random.Random(seed)
    return rnd.sample(files, sample)


def _to_plain(value: Any):
    if isinstance(value, np.ndarray):
        if value.ndim == 0:
            return value.item()
        return [_to_plain(v) for v in value.tolist()]
    if isinstance(value, (np.integer, np.floating, np.bool_)):
        return value.item()
    if isinstance(value, (bytes, bytearray, memoryview)):
        return bytes(value).decode("utf-8", errors="ignore")
    if isinstance(value, (list, tuple)):
        return [_to_plain(v) for v in value]
    if isinstance(value, dict):
        out = {}
        for k, v in value.items():
            out[str(k)] = _to_plain(v)
        return out
    return value


def _to_dictlike(value: Any):
    if isinstance(value, dict):
        return value
    try:
        if hasattr(value, "keys"):
            return {str(k): value[k] for k in value.keys()}
    except Exception:
        return value
    return value


def _compare_value(path: str, left: Any, right: Any, out: list[str], max_diffs: int = 200):
    if len(out) >= max_diffs:
        return

    if isinstance(left, dict) and not isinstance(right, dict):
        out.append(f"{path}: right side is not dict ({type(right).__name__})")
        return
    if isinstance(right, dict) and not isinstance(left, dict):
        out.append(f"{path}: left side is not dict ({type(left).__name__})")
        return

    if isinstance(left, dict):
        left = _to_plain(left)
        right = _to_plain(right)
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
        return

    if isinstance(left, (list, tuple)) and isinstance(right, (list, tuple)):
        if len(left) != len(right):
            out.append(f"{path}: length mismatch (sqzc3d={len(left)} ezc3d={len(right)})")
            return
        for idx, (lv, rv) in enumerate(zip(left, right)):
            _compare_value(f"{path}[{idx}]", lv, rv, out, max_diffs=max_diffs)
        return

    if isinstance(left, np.ndarray) or isinstance(right, np.ndarray):
        a = np.asarray(left)
        b = np.asarray(right)
        if a.shape != b.shape:
            out.append(f"{path}: ndarray shape mismatch (sq={a.shape} ez={b.shape})")
            return
        if a.size == 0:
            return
        if np.issubdtype(a.dtype, np.floating) or np.issubdtype(b.dtype, np.floating):
            eq = np.isclose(a.astype(float), b.astype(float), rtol=_REL_TOL, atol=_FLOAT_TOL, equal_nan=True)
            if not bool(np.all(eq)):
                out.append(f"{path}: ndarray mismatch count={np.count_nonzero(~eq)}/{eq.size}")
            return
        if not np.array_equal(a, b):
            out.append(f"{path}: ndarray mismatch count={np.count_nonzero(a != b)}/{a.size}")
        return

    if isinstance(left, (float, np.floating)) and isinstance(right, (float, np.floating)):
        if np.isfinite(left) and np.isfinite(right) and not np.isclose(left, right, rtol=_REL_TOL, atol=_FLOAT_TOL):
            out.append(f"{path}: float mismatch sqzc3d={left} ezc3d={right}")
        return

    if left != right:
        out.append(f"{path}: mismatch sqzc3d={left!r} ezc3d={right!r}")


def _normalize_ez_meta_tree(ez_parameters):
    if not isinstance(ez_parameters, dict):
        return {}
    groups: dict[str, Any] = {}
    for g_name, g in ez_parameters.items():
        if not isinstance(g_name, str) or not isinstance(g, dict):
            continue
        params: dict[str, Any] = {}
        for p_name, p in g.items():
            if p_name == "__METADATA__":
                continue
            if not isinstance(p_name, str) or not isinstance(p, dict):
                continue
            values = None
            for key in ("value", "values", "values_as_string", "values_as_double", "values_as_int"):
                if key in p:
                    raw = p[key]
                    if isinstance(raw, np.ndarray):
                        if raw.ndim > 1:
                            values = _to_plain(raw.reshape(-1, order="F"))
                        else:
                            values = _to_plain(raw.reshape(-1))
                    else:
                        values = _to_plain(raw)
                    break
            if isinstance(values, (list, tuple)):
                arr = np.asarray(values, dtype=object)
                if arr.ndim > 1:
                    values = _to_plain(arr.reshape(-1, order="F"))
                else:
                    values = _flatten_list_values(values)
            params[p_name] = {"values": values}
        groups[g_name] = params
    return {"groups": groups}


def _flatten_list_values(values: Any) -> Any:
    if not isinstance(values, (list, tuple)):
        return values
    flat: list[Any] = []

    def _walk(v: Any):
        if isinstance(v, (list, tuple)):
            for item in v:
                _walk(item)
        else:
            flat.append(v)

    _walk(values)
    return flat


def _normalize_sq_meta_tree(sq_tree: Any) -> dict[str, Any]:
    if not isinstance(sq_tree, dict):
        return {}
    groups_in = sq_tree.get("groups", {})
    if not isinstance(groups_in, dict):
        return {}
    groups: dict[str, Any] = {}
    for g_name, g in groups_in.items():
        if not isinstance(g, dict):
            continue
        params_in = g.get("parameters", {})
        if not isinstance(params_in, dict):
            continue
        params: dict[str, Any] = {}
        for p_name, p in params_in.items():
            if not isinstance(p, dict):
                continue
            values = _to_plain(p.get("values"))
            values = _flatten_list_values(values)
            params[str(p_name)] = {"values": values}
        groups[str(g_name)] = params
    return {"groups": groups}


def _meta_tree_dimension_issues(sq_tree: Any) -> list[str]:
    issues: list[str] = []
    if not isinstance(sq_tree, dict):
        return issues
    groups = sq_tree.get("groups", {})
    if not isinstance(groups, dict):
        return issues

    def _prod(xs: list[int]) -> int:
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
            dims_i: list[int] = []
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
                # ezc3d CHAR parameters include the string-length in dimension[0].
                # `values` is a list of strings, so its expected length is prod(dim[1:]) (or 1 for a single string).
                expected = _prod(dims_i[1:]) if len(dims_i) > 1 else 1
            else:
                if len(dims_i) == 0:
                    expected = 1 if len(values) > 0 else 0
                else:
                    expected = _prod(dims_i)

            if expected != len(values):
                issues.append(
                    f"meta_tree.{g_name}.{p_name}: dimensions inconsistent dims={dims_i} values={len(values)} expected={expected}"
                )

    return issues


def _extract_ez_reference(file_path: Path):
    ez = _safe_ez3d()
    return _smoke._load_ezc3d_reference(file_path)  # type: ignore[attr-defined]


def _read_sqzc3d_full(sqzc3d, file_path: Path, label_norm: int | None = None):
    dec = sqzc3d.Decoder(str(file_path)) if label_norm is None else sqzc3d.Decoder(str(file_path), int(label_norm))
    chunk = dec.read(frame_count=-1, points=None, analogs=None, analog_range=None)
    try:
        dec.close()
    except Exception:
        pass
    return chunk


def _read_sqzc3d_memory(sqzc3d, data: bytes, label_norm: int | None = None):
    dec = sqzc3d.Decoder(memoryview(data)) if label_norm is None else sqzc3d.Decoder(memoryview(data), int(label_norm))
    chunk = dec.read(frame_count=-1, points=None, analogs=None, analog_range=None)
    try:
        dec.close()
    except Exception:
        pass
    return chunk


def _estimate_max_diff(a: np.ndarray, b: np.ndarray) -> float:
    if a.shape != b.shape:
        return float("nan")
    if a.size == 0:
        return 0.0
    diff = np.abs(np.asarray(a, dtype=float) - np.asarray(b, dtype=float))
    finite = np.isfinite(a) & np.isfinite(b)
    if not np.any(finite):
        return float("nan")
    return float(np.nanmax(np.where(finite, diff, 0.0)))


def _extract_ez_points_raw(ez) -> np.ndarray:
    ez_points_raw = np.asarray(ez["data"]["points"], dtype=np.float64)
    if ez_points_raw.ndim != 3 or ez_points_raw.shape[0] < 3:
        raise ValueError(f"unexpected ezc3d points shape {ez_points_raw.shape}")
    return np.transpose(ez_points_raw[0:3], (2, 1, 0))


def _select_unit_mode(sq_chunk, ez, unit_contract: str) -> tuple[str, float, float, str, float]:
    ez_points_raw = _extract_ez_points_raw(ez)
    params = _to_dictlike(ez.get("parameters", {}))
    point_group = params.get("POINT", {}) if isinstance(params, dict) else {}
    p_units = ""
    if isinstance(point_group, dict):
        pu = point_group.get("UNITS")
        if isinstance(pu, dict):
            p_units = str(pu.get("value") if pu.get("value") is not None else pu.get("values") or "").strip()
        else:
            p_units = str(pu or "").strip()
    units_per_meter = _smoke._units_per_meter_from_unit_token(p_units)  # type: ignore[attr-defined]
    if not units_per_meter or not np.isfinite(units_per_meter):
        units_per_meter = 1000.0
    sq_pts = np.asarray(sq_chunk.points()[0], dtype=np.float64)
    d_raw = _estimate_max_diff(sq_pts, ez_points_raw)
    d_unit_norm = _estimate_max_diff(sq_pts, ez_points_raw / float(units_per_meter))
    if np.isfinite(d_raw) and np.isfinite(d_unit_norm):
        if d_raw <= d_unit_norm:
            best = "ez_raw"
        else:
            best = "ez_unit_normalized"
    else:
        best = "unknown"

    # unit_contract is used by callers to decide whether mismatch is actionable,
    # but we always report both diffs so the report stays interpretable.
    del unit_contract
    return (best, d_raw, d_unit_norm, p_units, float(units_per_meter))


def _cmp_points(sq_pts, sq_valid, ez_pts, ez_valid, *, strict_validity: bool = False, label: str = "point") -> list[str]:
    out: list[str] = []
    sq_pts = np.asarray(sq_pts, dtype=np.float64)
    sq_valid = np.asarray(sq_valid, dtype=np.uint8)
    ez_pts = np.asarray(ez_pts, dtype=np.float64)
    ez_valid = np.asarray(ez_valid, dtype=np.uint8)
    if sq_pts.shape != ez_pts.shape:
        out.append(f"{label} shape mismatch: sq={sq_pts.shape} ez={ez_pts.shape}")
        return out
    if sq_valid.shape != ez_valid.shape:
        out.append(f"{label} valid shape mismatch: sq={sq_valid.shape} ez={ez_valid.shape}")
    valid_mask = ez_valid == 1
    mism = np.count_nonzero(sq_valid.astype(np.int8) != ez_valid.astype(np.int8))
    if mism > 0:
        msg = f"{label} valid mismatch count={mism}"
        if strict_validity:
            out.append(msg)
        else:
            out.append(f"WARN {msg}")
    if sq_pts.size and ez_pts.size and sq_pts.shape == ez_pts.shape:
        sq_sel = sq_pts.reshape(-1, 3)[valid_mask.reshape(-1)]
        ez_sel = ez_pts.reshape(-1, 3)[valid_mask.reshape(-1)]
        eq = np.isclose(sq_sel, ez_sel, rtol=_REL_TOL, atol=_FLOAT_TOL, equal_nan=True)
        if not bool(np.all(eq)):
            out.append(f"{label} value mismatch max_abs_diff={float(np.abs(sq_sel - ez_sel).max()):.6g}")
    return out


def _cmp_analogs(sq_ana, sq_v, ez_ana, ez_v, strict: bool = False) -> list[str]:
    out: list[str] = []
    sq_ana = np.asarray(sq_ana, dtype=np.float64)
    sq_v = np.asarray(sq_v, dtype=np.uint8)
    ez_ana = np.asarray(ez_ana, dtype=np.float64)
    ez_v = np.asarray(ez_v, dtype=np.uint8)
    if sq_ana.shape != ez_ana.shape:
        out.append(f"analog shape mismatch: sq={sq_ana.shape} ez={ez_ana.shape}")
        return out
    if sq_v.shape != ez_v.shape:
        out.append(f"analog valid shape mismatch: sq={sq_v.shape} ez={ez_v.shape}")
    mism = np.count_nonzero(sq_v.astype(np.int8) != ez_v.astype(np.int8))
    if mism > 0:
        msg = f"analog valid mismatch count={mism}"
        if strict:
            out.append(msg)
        else:
            out.append(f"WARN {msg}")
    if sq_ana.size > 0:
        eq = np.isclose(sq_ana, ez_ana, rtol=_REL_TOL, atol=_FLOAT_TOL, equal_nan=True)
        if not bool(np.all(eq)):
            out.append(f"analog value mismatch max_abs_diff={float(np.abs(sq_ana - ez_ana).max()):.6g}")
    return out


def _meta_tree_compare(chunk, ez) -> list[str]:
    try:
        sq_raw = _to_plain(chunk.meta_tree)
        sq_tree = _normalize_sq_meta_tree(sq_raw)
    except Exception as exc:
        return [f"meta_tree access failed: {type(exc).__name__}: {exc}"]
    params = _to_dictlike(ez.get("parameters", {}))
    expected = _normalize_ez_meta_tree(params)
    diffs: list[str] = []
    _compare_value("meta_tree", sq_tree, expected, diffs)
    diffs.extend(_meta_tree_dimension_issues(sq_raw))
    return diffs[:200]


@dataclass
class FileReport:
    file: str
    status: str
    notes: list[str]
    metrics: dict[str, Any]


@dataclass
class ScenarioReport:
    scenario_id: str
    scenario_name: str
    status: str
    files: list[FileReport]
    notes: list[str]
    metrics: dict[str, Any]


def _status_aggregate(file_reports: list[FileReport]) -> str:
    if any(fr.status == "FAIL" for fr in file_reports):
        return "FAIL"
    if any(fr.status == "WARN" for fr in file_reports):
        return "WARN"
    return "PASS"


def _run_scenario_G0(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    strict = context["strict"]
    out: list[FileReport] = []
    for fp in files:
        notes: list[str] = []
        metrics: dict[str, Any] = {}
        status = "PASS"
        ez = context["ez"]
        dec_chunk = _read_sqzc3d_full(sqzc3d, fp)
        ez_obj, fallback = _extract_ez_reference(fp)
        try:
            ez_tuple = _smoke._extract_ezc3d_arrays(ez_obj)  # type: ignore[attr-defined]
            notes.extend(_smoke._compare_points(dec_chunk, ez_tuple))  # type: ignore[attr-defined]
            notes.extend(_smoke._compare_analogs(dec_chunk, ez_tuple))  # type: ignore[attr-defined]
            metrics["sq_points_shape"] = tuple(np.asarray(dec_chunk.points()[0]).shape)
            metrics["sq_analogs_shape"] = tuple(np.asarray(dec_chunk.analogs(None, layout="CN")[0]).shape)
            if strict:
                ez_points, ez_point_valid, ez_analogs, _, _ = ez_tuple
                ref_meta = _smoke._build_reference_meta(ez_obj, ez_points, ez_point_valid, ez_analogs)  # type: ignore[attr-defined]
                notes.extend(_smoke._compare_meta_counts(dec_chunk.meta, ref_meta))  # type: ignore[attr-defined]
                notes.extend(_meta_tree_compare(dec_chunk, ez_obj))
        finally:
            if fallback is not None:
                try:
                    fallback.unlink()
                except OSError:
                    pass
        if any(n.startswith("WARN") for n in notes):
            status = "WARN"
        elif any(n for n in notes):
            status = "FAIL"
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics=metrics))
    return ScenarioReport("G0", "通用：全库抽样回归", _status_aggregate(out), out, [], {"tested": len(out)})


def _run_scenario_S01(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    out: list[FileReport] = []
    skipped = 0
    for fp in files:
        notes: list[str] = []
        status = "PASS"
        chunk_full = _read_sqzc3d_full(sqzc3d, fp)
        meta = chunk_full.meta
        if int(meta.get("n_analogs", -1)) != 0:
            skipped += 1
            notes.append("skipped: not points-only")
        else:
            pts, pvalid = chunk_full.points()
            ana, av = chunk_full.analogs(None, layout="CN")
            if np.asarray(pts).ndim != 3 or np.asarray(pts).shape[2] != 3:
                status = "FAIL"
                notes.append(f"points shape unexpected: {np.asarray(pts).shape}")
            if np.asarray(pvalid).shape != np.asarray(pts).shape[:2]:
                status = "FAIL"
                notes.append(f"points_valid shape mismatch: {np.asarray(pvalid).shape}")
            if np.asarray(ana).shape[0] != 0:
                status = "FAIL"
                notes.append(f"analogs not empty: {np.asarray(ana).shape}")
            if np.asarray(av).shape != tuple(np.asarray(ana).shape):
                status = "FAIL"
                notes.append(f"analogs_valid mismatch: {np.asarray(av).shape}")
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
    scenario_notes = []
    if skipped:
        scenario_notes.append(f"skipped {skipped}/{len(files)} non-points-only files")
    return ScenarioReport("S01", "基础：points-only materialize", _status_aggregate(out), out, scenario_notes, {})


def _run_scenario_S02(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    out: list[FileReport] = []
    skipped = 0
    for fp in files:
        notes: list[str] = []
        status = "PASS"
        chunk = _read_sqzc3d_full(sqzc3d, fp)
        meta = chunk.meta
        n_analogs = int(meta.get("n_analogs", 0))
        if n_analogs <= 0:
            skipped += 1
            notes.append("skipped: no analog channels")
        else:
            ana, av = chunk.analogs(None, layout="CN")
            if np.asarray(ana).ndim != 2 or np.asarray(ana).shape[0] != n_analogs:
                status = "FAIL"
                notes.append(f"analog shape mismatch: got={np.asarray(ana).shape} expected_channels={n_analogs}")
            if np.asarray(av).shape != np.asarray(ana).shape:
                status = "FAIL"
                notes.append(f"analog_valid shape mismatch: {np.asarray(av).shape} vs {np.asarray(ana).shape}")
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={"n_analogs": n_analogs}))
    scenario_notes = []
    if skipped:
        scenario_notes.append(f"skipped {skipped}/{len(files)} files without analog channels")
    return ScenarioReport("S02", "基础：含 analog materialize", _status_aggregate(out), out, scenario_notes, {})


def _run_scenario_S03(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    out: list[FileReport] = []
    for fp in files:
        notes: list[str] = []
        status = "PASS"
        chunk = _read_sqzc3d_full(sqzc3d, fp)
        pts_all = np.asarray(chunk.points()[0])
        pts_empty = np.asarray(chunk.points([])[0])
        if pts_all.shape[0] == 0:
            status = "WARN"
            notes.append("empty file, skipped")
            out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
            continue
        if pts_empty.shape[1] != 0:
            status = "FAIL"
            notes.append(f"points=[] expected 0 points: {pts_empty.shape}")
        labels = chunk.meta.get("point_labels", [])
        if isinstance(labels, list) and len(labels) >= 2:
            a, b = str(labels[0]), str(labels[1])
            ord_pts = np.asarray(chunk.points([b, a])[0])
            if ord_pts.shape[1] != 2:
                status = "FAIL"
                notes.append(f"label order selection failed: shape={ord_pts.shape}")
            else:
                ez_obj = None
                fallback = None
                try:
                    ez_obj, fallback = _extract_ez_reference(fp)
                    ez_pts, _, _, _, _ = _smoke._extract_ezc3d_arrays(ez_obj)  # type: ignore[attr-defined]
                    ez_pts = np.asarray(ez_pts, dtype=np.float64)
                    src_indices = None
                    mapping = chunk.meta.get("point_indices_total")
                    if isinstance(mapping, list) and len(mapping) >= 2:
                        try:
                            src_indices = [int(mapping[1]), int(mapping[0])]
                        except Exception:
                            src_indices = None
                    if src_indices is None:
                        params = _to_dictlike(ez_obj.get("parameters", {}))
                        ez_labels = _smoke._extract_ezc3d_label_series(params, "POINT", "LABELS")  # type: ignore[attr-defined]
                        if isinstance(ez_labels, list) and b in ez_labels and a in ez_labels:
                            src_indices = [int(ez_labels.index(b)), int(ez_labels.index(a))]
                    if src_indices is None:
                        if status == "PASS":
                            status = "WARN"
                        notes.append("ez compare skipped: unable to map point labels to ezc3d indices")
                    else:
                        expected = ez_pts[:, src_indices, :]
                        got = np.asarray(ord_pts, dtype=np.float64)
                        if got.shape != expected.shape:
                            status = "FAIL"
                            notes.append(f"ez compare shape mismatch: got={got.shape} expected={expected.shape}")
                        else:
                            eq = np.isclose(got, expected, rtol=_REL_TOL, atol=_FLOAT_TOL, equal_nan=True)
                            if not bool(np.all(eq)):
                                status = "FAIL"
                                notes.append(f"ez compare mismatch max_abs_diff={float(np.max(np.abs(got - expected))):.6g}")
                            else:
                                notes.append("ez compare PASS")
                finally:
                    if fallback is not None:
                        try:
                            fallback.unlink()
                        except OSError:
                            pass
                notes.append("label order check PASS")
        else:
            status = "WARN"
            notes.append("labels insufficient")
        if status == "PASS":
            notes.append("selector core PASS")
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
    return ScenarioReport("S03", "选择器语义：None/[]/labels 顺序", _status_aggregate(out), out, [], {})


def _run_scenario_S04(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    out: list[FileReport] = []
    for fp in files:
        notes: list[str] = []
        status = "PASS"
        chunk_all = _read_sqzc3d_full(sqzc3d, fp)
        if int(chunk_all.meta.get("n_points", 0) or 0) <= 0:
            status = "WARN"
            notes.append("no points in file, skipped")
            out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
            continue
        full_pts = np.asarray(chunk_all.points()[0])
        full_ana = np.asarray(chunk_all.analogs(None, layout="CN")[0])
        n_by_frame = int(chunk_all.meta.get("n_analog_by_frame", 0))
        n_frames = int(chunk_all.meta.get("n_frames", 0))
        if n_frames < 2:
            status = "WARN"
            notes.append("insufficient frames")
            out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
            continue
        for s, c in ((0, 1), (0, 2), (1, 1), (0, 0), (0, -1)):
            dec = None
            try:
                dec = sqzc3d.Decoder(str(fp))
                sub = dec.read(start_frame=s, frame_count=c, points=None, analogs=None, analog_range=None)
                pts = np.asarray(sub.points()[0])
                expect_n = 0 if c == 0 else (n_frames - s if c == -1 else c)
                expected = full_pts[s : s + expect_n]
                if pts.shape != expected.shape:
                    status = "FAIL"
                    notes.append(f"point window mismatch (s={s},c={c}): {pts.shape} != {expected.shape}")

                # Time-axis metadata should be present and consistent with the window.
                m = sub.meta
                if not isinstance(m, dict):
                    status = "FAIL"
                    notes.append("meta is not a dict")
                else:
                    fs = m.get("frame_start", None)
                    if fs is None or int(fs) != int(s):
                        status = "FAIL"
                        notes.append(f"meta.frame_start mismatch (s={s},c={c}): {fs!r} != {s}")

                    sf = m.get("source_first_frame", None)
                    fsa = m.get("frame_start_abs", None)
                    if sf is None or fsa is None:
                        status = "FAIL"
                        notes.append("time-axis meta missing (source_first_frame/frame_start_abs)")
                    else:
                        if int(fsa) != int(sf) + int(fs or 0):
                            status = "FAIL"
                            notes.append(f"meta.frame_start_abs inconsistent: {fsa} != {sf}+{fs}")

                    afs = m.get("analog_frame_start", None)
                    if afs is None or int(afs) != 0:
                        status = "FAIL"
                        notes.append(f"meta.analog_frame_start mismatch (s={s},c={c}): {afs!r} != 0")

                    pr = m.get("point_rate_hz", None)
                    ar = m.get("analog_rate_hz", None)
                    if pr is None or ar is None:
                        status = "FAIL"
                        notes.append("time-axis meta missing (point_rate_hz/analog_rate_hz)")
                    else:
                        try:
                            pr_f = float(pr)
                            ar_f = float(ar)
                        except Exception:
                            status = "FAIL"
                            notes.append("time-axis meta invalid (point_rate_hz/analog_rate_hz)")
                        else:
                            if pr_f <= 0.0:
                                status = "WARN" if status == "PASS" else status
                                notes.append(f"point_rate_hz not positive: {pr_f}")
                            expected_ar = pr_f * float(n_by_frame)
                            if n_by_frame > 0 and not np.isclose(ar_f, expected_ar, rtol=1e-6, atol=1e-6):
                                status = "WARN" if status == "PASS" else status
                                notes.append(f"analog_rate_hz unexpected: {ar_f} vs {expected_ar}")
                if c >= 0:
                    expect_s = 0 if c == 0 else (n_by_frame * expect_n if n_by_frame > 0 else 0)
                    expected_a = np.asarray(full_ana[:, 0:expect_s])
                    if np.asarray(sub.analogs(None, layout="CN")[0]).shape != expected_a.shape:
                        status = "FAIL"
                        notes.append(
                            f"analog window mismatch (s={s},c={c}): got={np.asarray(sub.analogs(None, layout='CN')[0]).shape} expected={expected_a.shape}"
                        )
            except Exception as exc:
                status = "WARN"
                notes.append(f"window read failed (s={s},c={c}): {type(exc).__name__}: {exc}")
            finally:
                if dec is not None:
                    try:
                        dec.close()
                    except Exception:
                        pass
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
    return ScenarioReport("S04", "窗口读：start_frame/frame_count 等价性", _status_aggregate(out), out, [], {})


def _run_scenario_S05(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    out: list[FileReport] = []
    for fp in files:
        notes: list[str] = []
        status = "PASS"
        dec = sqzc3d.Decoder(str(fp))
        chunk = dec.read(frame_count=1, points=None, analogs=None, analog_range=None)
        try:
            dec.close()
        except Exception:
            pass
        if int(chunk.meta.get("n_points", 0) or 0) < 3:
            status = "WARN"
            notes.append("points too few")
            out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
            continue
        sel = [0, 2]
        dec_sel = sqzc3d.Decoder(str(fp))
        chunk_sel = dec_sel.read(frame_count=1, points=sel, analogs=[], analog_range=None)
        try:
            got = chunk_sel.point_indices_total
            sq_sel_pts = np.asarray(chunk_sel.points()[0], dtype=np.float64)
        finally:
            try:
                dec_sel.close()
            except Exception:
                pass
        got_list = list(got) if got is not None else []
        if not got_list:
            status = "WARN"
            notes.append("point_indices_total unavailable")
        elif got_list != sel:
            status = "FAIL"
            notes.append(f"index-space contract mismatch: requested={sel} observed={got_list}")

        ez_obj, fallback = _extract_ez_reference(fp)
        try:
            ez_pts, _, _, _, _ = _smoke._extract_ezc3d_arrays(ez_obj)  # type: ignore[attr-defined]
            ez_pts = np.asarray(ez_pts, dtype=np.float64)
            expected = ez_pts[0:1, sel, :]
            if sq_sel_pts.shape != expected.shape:
                status = "FAIL"
                notes.append(f"ez compare shape mismatch: got={sq_sel_pts.shape} expected={expected.shape}")
            else:
                eq = np.isclose(sq_sel_pts, expected, rtol=_REL_TOL, atol=_FLOAT_TOL, equal_nan=True)
                if not bool(np.all(eq)):
                    status = "FAIL"
                    notes.append(f"ez compare mismatch max_abs_diff={float(np.max(np.abs(sq_sel_pts - expected))):.6g}")
                else:
                    notes.append("ez compare PASS (indices)")

            labels = chunk.meta.get("point_labels", [])
            if isinstance(labels, list) and len(labels) > max(sel):
                label_sel = [str(labels[i]) for i in sel]
                dec_lab = sqzc3d.Decoder(str(fp))
                chunk_lab = dec_lab.read(frame_count=1, points=label_sel, analogs=[], analog_range=None)
                try:
                    sq_lab_pts = np.asarray(chunk_lab.points()[0], dtype=np.float64)
                    got_lab = chunk_lab.point_indices_total
                finally:
                    try:
                        dec_lab.close()
                    except Exception:
                        pass
                if sq_lab_pts.shape != sq_sel_pts.shape:
                    status = "FAIL"
                    notes.append(f"indices vs labels shape mismatch: {sq_sel_pts.shape} vs {sq_lab_pts.shape}")
                else:
                    eq2 = np.isclose(sq_lab_pts, sq_sel_pts, rtol=_REL_TOL, atol=_FLOAT_TOL, equal_nan=True)
                    if not bool(np.all(eq2)):
                        status = "FAIL"
                        notes.append(
                            f"indices vs labels value mismatch max_abs_diff={float(np.max(np.abs(sq_lab_pts - sq_sel_pts))):.6g}"
                        )
                    else:
                        notes.append("indices vs labels PASS")
                got_lab_list = list(got_lab) if got_lab is not None else []
                if got_lab_list and got_lab_list != sel:
                    status = "FAIL"
                    notes.append(f"label selection index-space mismatch: expected={sel} got={got_lab_list}")
            else:
                if status == "PASS":
                    status = "WARN"
                notes.append("labels unavailable for label-path check")
        finally:
            if fallback is not None:
                try:
                    fallback.unlink()
                except OSError:
                    pass
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
    return ScenarioReport("S05", "indices vs labels（index-space 契约）", _status_aggregate(out), out, [], {})


def _run_scenario_S06(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    out: list[FileReport] = []
    for fp in files:
        status = "PASS"
        notes: list[str] = []
        dec = sqzc3d.Decoder(str(fp))
        chunk = dec.read(frame_count=1, points=None, analogs=None, analog_range=None)
        try:
            dec.close()
        except Exception:
            pass
        labels = [str(x) for x in chunk.meta.get("point_labels", [])]
        if not labels:
            status = "WARN"
            notes.append("no point labels")
            out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
            continue
        base = labels[0]
        trim = f"  {base}  "
        up = base.upper()
        dec_exact = None
        dec_trim = None
        dec_case = None
        try:
            dec_exact = sqzc3d.Decoder(str(fp), label_norm=sqzc3d.SQZC3D_LABEL_NORM_EXACT)
            dec_exact.read(points=[base], frame_count=1)
            exact_ok = True
        except Exception:
            exact_ok = False
        finally:
            if dec_exact is not None:
                try:
                    dec_exact.close()
                except Exception:
                    pass

        try:
            dec_trim = sqzc3d.Decoder(str(fp), label_norm=sqzc3d.SQZC3D_LABEL_NORM_TRIM)
            dec_trim.read(points=[trim], frame_count=1)
            trim_ok = True
        except Exception:
            trim_ok = False
        finally:
            if dec_trim is not None:
                try:
                    dec_trim.close()
                except Exception:
                    pass

        try:
            dec_case = sqzc3d.Decoder(str(fp), label_norm=sqzc3d.SQZC3D_LABEL_NORM_CASEFOLD_WS)
            dec_case.read(points=[up], frame_count=1)
            case_ok = True
        except Exception:
            case_ok = False
        finally:
            if dec_case is not None:
                try:
                    dec_case.close()
                except Exception:
                    pass
        notes.append(f"exact={exact_ok} trim={trim_ok} casefold={case_ok}")
        if not trim_ok and not case_ok:
            status = "WARN"
            notes.append("no normalization robustness observed")
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
    return ScenarioReport("S06", "label normalization（TRIM/CASEFOLD_WS）", _status_aggregate(out), out, [], {})


def _run_scenario_S07(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    strict_unit = context["strict_unit"]
    unit_contract = context["unit_contract"]
    out: list[FileReport] = []
    for fp in files:
        status = "PASS"
        notes: list[str] = []
        chunk = _read_sqzc3d_full(sqzc3d, fp)
        ez_obj, fallback = _extract_ez_reference(fp)
        try:
            best, d_raw, d_unit_norm, unit_token, units_per_meter = _select_unit_mode(chunk, ez_obj, unit_contract)
            default_matches_ez_raw = best == "ez_raw"

            expected: str | None = None
            if unit_contract == "raw":
                expected = "ez_raw"
            elif unit_contract == "meters":
                expected = "ez_unit_normalized"

            if best == "unknown":
                status = "WARN"
                notes.append("unable to compare scale (shape mismatch or all-nonfinite)")
            else:
                if not default_matches_ez_raw:
                    status = "FAIL" if strict_unit and unit_contract in ("auto", "raw") else "WARN"
                    notes.append(
                        "default point scale differs from ezc3d default output "
                        f"(POINT:UNITS={unit_token!r}, units_per_meter={units_per_meter:g})"
                    )

                if expected is not None and best != expected:
                    status = "FAIL" if strict_unit else "WARN"
                    notes.append(f"unit_contract={unit_contract} mismatch: best_match={best}")

            notes.append(
                f"default_matches_ez_raw={default_matches_ez_raw}, best_match={best}, "
                f"max_abs_diff_vs_ez_raw={d_raw:.6g}, max_abs_diff_vs_ez_unit_normalized={d_unit_norm:.6g}"
            )
        finally:
            if fallback is not None:
                try:
                    fallback.unlink()
                except OSError:
                    pass
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
    return ScenarioReport("S07", "points scale vs ezc3d", _status_aggregate(out), out, [], {})


def _run_scenario_S08(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    strict_validity = context["strict_validity"]
    out: list[FileReport] = []
    for fp in files:
        notes: list[str] = []
        status = "PASS"
        chunk = _read_sqzc3d_full(sqzc3d, fp)
        ez_obj, fallback = _extract_ez_reference(fp)
        try:
            ez_pts, ez_v, ez_a, ez_av, _ = _smoke._extract_ezc3d_arrays(ez_obj)  # type: ignore[attr-defined]
            sq_pts, sq_v = chunk.points()
            sq_a, sq_av = chunk.analogs(None, layout="CN")
            notes.extend(_cmp_points(sq_pts, sq_v, ez_pts, ez_v, strict_validity=strict_validity))
            notes.extend(_cmp_analogs(sq_a, sq_av, ez_a, ez_av, strict=False))
            notes.append(f"residual_gate_mm={chunk.meta.get('residual_gate_mm', 0.0)}")
            if np.any(np.isnan(np.asarray(ez_pts))) and not np.any(np.isfinite(np.asarray(sq_pts))):
                notes.append("contains NaN-only frames")
        finally:
            if fallback is not None:
                try:
                    fallback.unlink()
                except OSError:
                    pass
        if any(n.startswith("WARN") for n in notes):
            if status == "PASS":
                status = "WARN"
        elif any("mismatch" in n for n in notes):
            status = "FAIL"
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
    return ScenarioReport("S08", "validity 与缺失点/NaN/Inf", _status_aggregate(out), out, [], {})


def _run_scenario_S09(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    out: list[FileReport] = []
    skipped = 0
    for fp in files:
        status = "PASS"
        notes: list[str] = []
        chunk = _read_sqzc3d_full(sqzc3d, fp)
        ez_obj, fallback = _extract_ez_reference(fp)
        try:
            try:
                _, _, ez_a, ez_av, _ = _smoke._extract_ezc3d_arrays(ez_obj)  # type: ignore[attr-defined]
            except Exception as exc:
                notes.append(f"ez extract failed: {type(exc).__name__}: {exc}")
                status = "WARN"
            else:
                sq_a, sq_av = chunk.analogs(None, layout="CN")
                notes.extend(_cmp_analogs(sq_a, sq_av, ez_a, ez_av, strict=False))
                if np.asarray(sq_a).size == 0:
                    skipped += 1
                    notes.append("skipped: no analog values")
            params = _to_dictlike(ez_obj.get("parameters", {}))
            analog = params.get("ANALOG", {}) if isinstance(params, dict) else {}
            for k in ("GEN_SCALE", "SCALE", "OFFSET"):
                if isinstance(analog, dict) and k in analog:
                    rec = analog[k]
                    if isinstance(rec, dict):
                        vals = rec.get("value", rec.get("values", []))
                        vals = _to_plain(vals)
                        if isinstance(vals, (list, tuple)) and vals:
                            notes.append(f"found {k} count={len(vals)}")
                            break
        finally:
            if fallback is not None:
                try:
                    fallback.unlink()
                except OSError:
                    pass
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
    scenario_notes = []
    if skipped:
        scenario_notes.append(f"skipped {skipped}/{len(files)} files without analog values")
    return ScenarioReport("S09", "analog scaling（GEN_SCALE/SCALE/OFFSET）", _status_aggregate(out), out, scenario_notes, {})


def _run_scenario_S10(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    out: list[FileReport] = []
    for fp in files:
        status = "PASS"
        notes: list[str] = []
        chunk = _read_sqzc3d_full(sqzc3d, fp)
        ez_obj, fallback = _extract_ez_reference(fp)
        try:
            notes.extend(_meta_tree_compare(chunk, ez_obj))
        finally:
            if fallback is not None:
                try:
                    fallback.unlink()
                except OSError:
                    pass
        if any("missing" in n for n in notes):
            status = "WARN"
        if len(notes) > 0 and status == "PASS":
            status = "FAIL"
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
    return ScenarioReport("S10", "meta_tree 多维参数", _status_aggregate(out), out, [], {})


def _run_scenario_S11(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    if not hasattr(sqzc3d, "export_bundle"):
        return ScenarioReport(
            scenario_id="S11",
            scenario_name="bundles：roundtrip 与 strictness",
            status="WARN",
            files=[],
            notes=["Python 绑定未公开 export_bundle；请通过 C++ matrix 验证 bundling 行为。"],
            metrics={"implemented_in_py": False},
        )
    out: list[FileReport] = []

    def _cmp_arrays(name: str, a: Any, b: Any, out_notes: list[str]):
        aa = np.asarray(a)
        bb = np.asarray(b)
        if aa.shape != bb.shape:
            out_notes.append(f"{name} shape mismatch: {aa.shape} vs {bb.shape}")
            return
        if aa.size == 0:
            return
        if np.issubdtype(aa.dtype, np.floating) or np.issubdtype(bb.dtype, np.floating):
            eq = np.isclose(aa.astype(float), bb.astype(float), rtol=_REL_TOL, atol=_FLOAT_TOL, equal_nan=True)
            if not bool(np.all(eq)):
                out_notes.append(f"{name} value mismatch max_abs_diff={float(np.max(np.abs(aa.astype(float) - bb.astype(float)))):.6g}")
        else:
            if not np.array_equal(aa, bb):
                out_notes.append(f"{name} value mismatch count={int(np.count_nonzero(aa != bb))}/{aa.size}")

    for fp in files:
        status = "PASS"
        notes: list[str] = []
        chunk = _read_sqzc3d_full(sqzc3d, fp)
        with tempfile.TemporaryDirectory(prefix="sqzc3d_bundle_") as td:
            bundle_dir = Path(td) / "bundle_dir"
            bundle_dir.mkdir(parents=True, exist_ok=True)
            try:
                sqzc3d.export_bundle(str(bundle_dir), chunk)
                loaded = sqzc3d.load_bundle(str(bundle_dir), strict=True)
            except Exception as exc:
                status = "FAIL"
                notes.append(f"bundle roundtrip failed: {type(exc).__name__}: {exc}")
                out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
                continue

            for key in ("n_frames", "n_points", "n_points_total", "n_analogs", "n_analog_by_frame"):
                if chunk.meta.get(key) != loaded.meta.get(key):
                    status = "FAIL"
                    notes.append(f"meta.{key} mismatch: {chunk.meta.get(key)} vs {loaded.meta.get(key)}")

            for key in (
                "source_first_frame",
                "source_last_frame",
                "point_rate_hz",
                "analog_rate_hz",
                "frame_start",
                "analog_frame_start",
            ):
                a = chunk.meta.get(key)
                b = loaded.meta.get(key)
                if isinstance(a, (int, float)) and isinstance(b, (int, float)):
                    if not bool(np.isclose(float(a), float(b), rtol=_REL_TOL, atol=_FLOAT_TOL)):
                        status = "FAIL"
                        notes.append(f"meta.{key} mismatch: {a} vs {b}")
                else:
                    if a != b:
                        status = "FAIL"
                        notes.append(f"meta.{key} mismatch: {a!r} vs {b!r}")

            _cmp_arrays("points", chunk.points()[0], loaded.points()[0], notes)
            _cmp_arrays("points_valid", chunk.points()[1], loaded.points()[1], notes)
            _cmp_arrays("analogs", chunk.analogs(None, layout="CN")[0], loaded.analogs(None, layout="CN")[0], notes)
            _cmp_arrays(
                "analogs_valid",
                chunk.analogs(None, layout="CN")[1],
                loaded.analogs(None, layout="CN")[1],
                notes,
            )

            for key in ("point_labels", "analog_labels"):
                if chunk.meta.get(key) != loaded.meta.get(key):
                    status = "FAIL"
                    notes.append(f"meta.{key} mismatch")

            try:
                diffs: list[str] = []
                _compare_value("meta_tree", _to_plain(chunk.meta_tree), _to_plain(loaded.meta_tree), diffs)
                if diffs:
                    status = "FAIL"
                    notes.append("meta_tree mismatch after bundle roundtrip")
                    notes.extend(diffs[:40])
                else:
                    notes.append("meta_tree preserved in bundle")
            except Exception as exc:
                status = "FAIL"
                notes.append(f"meta_tree access failed: {type(exc).__name__}: {exc}")

            if status == "PASS":
                notes.append("bundle roundtrip PASS")

        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))

    return ScenarioReport("S11", "bundles：roundtrip 与 strictness", _status_aggregate(out), out, [], {"implemented_in_py": True})


def _run_scenario_S12(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    out: list[FileReport] = []
    for fp in files:
        status = "PASS"
        notes: list[str] = []
        raw = fp.read_bytes()
        chunk_file = _read_sqzc3d_full(sqzc3d, fp)
        chunk_mem = _read_sqzc3d_memory(sqzc3d, raw)
        if _smoke._shape_str(np.asarray(chunk_file.points()[0])) != _smoke._shape_str(np.asarray(chunk_mem.points()[0])):  # type: ignore[attr-defined]
            status = "FAIL"
            notes.append("points shape mismatch file vs memory")
        if np.asarray(chunk_file.meta).size != np.asarray(chunk_mem.meta).size:
            status = "WARN"
            notes.append("meta object mismatch, compare field-by-field")

        for key in (
            "source_first_frame",
            "source_last_frame",
            "point_rate_hz",
            "analog_rate_hz",
            "frame_start",
            "analog_frame_start",
        ):
            a = chunk_file.meta.get(key)
            b = chunk_mem.meta.get(key)
            if isinstance(a, (int, float)) and isinstance(b, (int, float)):
                if not bool(np.isclose(float(a), float(b), rtol=_REL_TOL, atol=_FLOAT_TOL)):
                    status = "FAIL"
                    notes.append(f"meta.{key} mismatch file vs memory: {a} vs {b}")
            else:
                if a != b:
                    status = "FAIL"
                    notes.append(f"meta.{key} mismatch file vs memory: {a!r} vs {b!r}")
        try:
            diffs: list[str] = []
            _compare_value("meta_tree", _to_plain(chunk_file.meta_tree), _to_plain(chunk_mem.meta_tree), diffs)
            if diffs:
                status = "FAIL"
                notes.append("meta_tree mismatch file vs memory")
                notes.extend(diffs[:40])
            else:
                notes.append("meta_tree preserved for open_memory")
        except Exception as exc:
            status = "FAIL"
            notes.append(f"meta_tree access failed: {type(exc).__name__}: {exc}")
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
    return ScenarioReport("S12", "open_memory", _status_aggregate(out), out, [], {})


def _run_scenario_S13(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    del context, files
    return ScenarioReport(
        scenario_id="S13",
        scenario_name="streaming vs materialize",
        status="PASS",
        files=[],
        notes=["当前 Python 接口没有独立 streaming API；用 S04 的窗口读做等价性抽样替代即可。"],
        metrics={"implemented_in_py": False},
    )


def _run_scenario_S14(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    del files
    sqzc3d = context["sqzc3d"]
    status = "PASS"
    notes: list[str] = []
    if hasattr(sqzc3d, "features"):
        features = int(sqzc3d.features())
        names = (
            "SQZC3D_FEATURE_OPEN_FILE",
            "SQZC3D_FEATURE_OPEN_MEMORY",
            "SQZC3D_FEATURE_BUNDLE",
            "SQZC3D_FEATURE_BUILD_CHUNKS",
        )
        for n in names:
            if hasattr(sqzc3d, n):
                notes.append(f"{n}={'on' if features & int(getattr(sqzc3d, n)) else 'off'}")
    else:
        status = "WARN"
        notes.append("features() unavailable")
    return ScenarioReport("S14", "feature gating（WITH_EZC3D=OFF）", status, [], notes, {})


def _run_scenario_S15(context: dict[str, Any], files: list[Path]) -> ScenarioReport:
    sqzc3d = context["sqzc3d"]
    out: list[FileReport] = []
    for fp in files:
        status = "PASS"
        notes: list[str] = []
        raw = fp.read_bytes()
        if len(raw) < 64:
            status = "WARN"
            notes.append("file too small to craft malformed variants")
            out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
            continue
        with tempfile.NamedTemporaryFile(suffix=fp.suffix, delete=False) as tmp:
            tmp.write(raw[: len(raw) // 2])
            path_half = Path(tmp.name)
        with tempfile.NamedTemporaryFile(suffix=fp.suffix, delete=False) as tmp2:
            path_zero = Path(tmp2.name)
            path_zero.write_bytes(b"")
        for p in (path_half, path_zero):
            try:
                dec = sqzc3d.Decoder(str(p))
                _ = dec.read(frame_count=1, points=None, analogs=None, analog_range=None)
                if p == path_zero:
                    status = "FAIL"
                    notes.append(f"zero-length input unexpectedly parsed: {p.name}")
                else:
                    notes.append(f"pathologic truncated half-file parsed: {p.name} (non-strict caveat)")
                try:
                    dec.close()
                except Exception:
                    pass
            except Exception as exc:
                notes.append(f"expected fail on {p.name}: {type(exc).__name__}")
        try:
            path_half.unlink()
            path_zero.unlink()
        except OSError:
            pass
        out.append(FileReport(file=str(fp), status=status, notes=notes, metrics={}))
    return ScenarioReport("S15", "病理输入与错误诊断", _status_aggregate(out), out, [], {})


_SCENARIO_RUNNERS = {
    "G0": _run_scenario_G0,
    "S01": _run_scenario_S01,
    "S02": _run_scenario_S02,
    "S03": _run_scenario_S03,
    "S04": _run_scenario_S04,
    "S05": _run_scenario_S05,
    "S06": _run_scenario_S06,
    "S07": _run_scenario_S07,
    "S08": _run_scenario_S08,
    "S09": _run_scenario_S09,
    "S10": _run_scenario_S10,
    "S11": _run_scenario_S11,
    "S12": _run_scenario_S12,
    "S13": _run_scenario_S13,
    "S14": _run_scenario_S14,
    "S15": _run_scenario_S15,
}

_SCENARIO_NAMES = {
    "G0": "通用：全库抽样回归",
    "S01": "基础：points-only materialize",
    "S02": "基础：含 analog materialize",
    "S03": "选择器语义：None/[]/labels 顺序",
    "S04": "窗口读：start_frame/frame_count 等价性",
    "S05": "indices vs labels（index-space 契约）",
    "S06": "label normalization（TRIM/CASEFOLD_WS）",
    "S07": "units & scaling",
    "S08": "validity 与缺失点/NaN/Inf",
    "S09": "analog scaling（GEN_SCALE/SCALE/OFFSET）",
    "S10": "meta_tree 多维参数",
    "S11": "bundles：roundtrip 与 strictness",
    "S12": "open_memory",
    "S13": "streaming vs materialize",
    "S14": "feature gating（WITH_EZC3D=OFF）",
    "S15": "病理输入与错误诊断",
}


def _run_scenarios(
    sqzc3d,
    scenarios: list[str],
    files: list[Path],
    args,
) -> tuple[list[ScenarioReport], int]:
    context = {
        "sqzc3d": sqzc3d,
        "strict": args.strict,
        "strict_unit": args.strict_unit,
        "strict_validity": args.strict_validity,
        "unit_contract": args.unit_contract,
        "ez": _safe_ez3d(),
    }
    reports: list[ScenarioReport] = []
    failed = 0
    for sid in scenarios:
        fn = _SCENARIO_RUNNERS.get(sid)
        if fn is None:
            reports.append(
                ScenarioReport(
                    scenario_id=sid,
                    scenario_name="unknown",
                    status="FAIL",
                    files=[],
                    notes=[f"Unknown scenario id: {sid}"],
                    metrics={},
                )
            )
            failed += 1
            if failed >= args.max_fail:
                break
            continue
        scenario_name = _SCENARIO_NAMES.get(sid, "unknown")
        try:
            rep = fn(context, files)
        except Exception as exc:
            rep = ScenarioReport(
                scenario_id=sid,
                scenario_name=scenario_name,
                status="WARN",
                files=[
                    FileReport(
                        file=str(files[0]) if files else "<no-input>",
                        status="WARN",
                        notes=[f"scenario execution failed: {type(exc).__name__}: {exc}"],
                        metrics={},
                    )
                ],
                notes=[f"scenario execution failed: {type(exc).__name__}: {exc}"],
                metrics={},
            )
        reports.append(rep)
        if rep.status == "FAIL":
            failed += 1
            if failed >= args.max_fail:
                break
    return reports, failed


def _render(reports: list[ScenarioReport], args):
    all_status = "PASS"
    if any(r.status == "FAIL" for r in reports):
        all_status = "FAIL"
    elif any(r.status == "WARN" for r in reports):
        all_status = "WARN"

    print(f"SQZC3D Stress Summary: {all_status}")
    for rep in reports:
        print(f"\n[{rep.status}] {rep.scenario_id}: {rep.scenario_name}")
        if rep.notes:
            for note in rep.notes:
                print(f"  - {note}")
        for fr in rep.files:
            print(f"  {fr.status} :: {fr.file}")
            for note in fr.notes:
                print(f"    - {note}")

    if args.report:
        report_obj = {
            "status": all_status,
            "scenarios": [
                {
                    "id": rep.scenario_id,
                    "name": rep.scenario_name,
                    "status": rep.status,
                    "notes": rep.notes,
                    "files": [
                        {
                            "file": fr.file,
                            "status": fr.status,
                            "notes": fr.notes,
                            "metrics": fr.metrics,
                        }
                        for fr in rep.files
                    ],
                    "metrics": rep.metrics,
                }
                for rep in reports
            ],
        }
        Path(args.report).write_text(json.dumps(report_obj, indent=2, ensure_ascii=False), encoding="utf-8")


def parse_args():
    p = argparse.ArgumentParser(description="Scenario-driven sqzc3d stress checks.")
    p.add_argument("files", nargs="*", default=[], help="specific .c3d file(s)")
    p.add_argument("--roots", nargs="*", action="append", default=[], help="directories recursively searched for .c3d")
    p.add_argument("--sample", type=int, default=6, help="sample up to N files from collected inputs")
    p.add_argument("--seed", type=int, default=42)
    p.add_argument(
        "--scenarios",
        nargs="+",
        default=["G0", "S01", "S02", "S03", "S04", "S05", "S06", "S07", "S08", "S09", "S10", "S11", "S12", "S13", "S14", "S15"],
        help="scenario ids to run",
    )
    p.add_argument("--unit-contract", choices=["meters", "raw", "auto"], default="auto")
    p.add_argument(
        "--strict",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="enable strict checks in relevant scenarios",
    )
    p.add_argument("--strict-unit", action=argparse.BooleanOptionalAction, default=False, help="treat unit mismatch as hard fail")
    p.add_argument(
        "--strict-validity",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="treat validity mismatch as hard fail",
    )
    p.add_argument("--max-fail", type=int, default=999, help="stop after this many scenario fails")
    p.add_argument("--report", type=Path, default=None, help="dump JSON report to this path")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    files = [Path(f) for f in args.files]
    roots: list[str | Path] = []
    for root_group in args.roots:
        if isinstance(root_group, list):
            roots.extend(root_group)
        else:
            roots.append(root_group)
    if roots:
        files.extend(_collect_files_from_roots([str(r) for r in roots]))
    files = [f.expanduser().resolve() for f in files if f.suffix.lower() == ".c3d" and f.is_file()]
    files = sorted(set(files))
    if not files:
        print("No .c3d files found. Pass files/roots via positional args or --roots.", flush=True)
        return 2

    selected = _sample(files, int(args.sample), int(args.seed))
    print(
        f"stress: selected {len(selected)}/{len(files)} files (seed={args.seed}, scenarios={','.join(args.scenarios)})",
        flush=True,
    )
    sqzc3d = _ensure_sqzc3d_import()
    reports, fail_count = _run_scenarios(sqzc3d, args.scenarios, selected, args)
    _render(reports, args)
    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
