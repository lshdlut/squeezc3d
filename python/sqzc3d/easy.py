"""Pythonic "easy" interface for sqzc3d.

Goals:
- One entry point: read(...)
- Property-first access for data and validity
- Label-first selection (no indices in the easy surface)
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

from . import _core as _core
from .query import indices_to_mask, mask_to_indices, type_group_indices


def _normalize_label_list(value, *, field: str) -> list[str] | None:
    if value is None:
        return None
    if isinstance(value, str):
        return [value]
    if isinstance(value, (bytes, bytearray, memoryview)):
        raise TypeError(f"{field} expects labels (str), not bytes-like objects")
    try:
        seq = list(value)
    except TypeError as e:
        raise TypeError(f"{field} expects None/str/sequence[str]") from e
    out: list[str] = []
    for item in seq:
        if not isinstance(item, str):
            raise TypeError(f"{field} expects sequence[str], got {type(item)!r}")
        out.append(item)
    return out


def _is_contiguous(indices: Sequence[int]) -> bool:
    if not indices:
        return True
    base = int(indices[0])
    for i, v in enumerate(indices):
        if int(v) != base + i:
            return False
    return True


@dataclass
class View:
    """Stateful view over a single loaded Chunk."""

    _chunk: _core.Chunk
    point_labels: list[str] | None = None
    analog_labels: list[str] | None = None
    analog_layout: str = "CN"
    type_groups: list[str] | None = None
    type_groups_strict: bool = False
    type_groups_missing_meta: str = "all"

    _meta: dict = field(init=False, repr=False)
    _points_cache_key: tuple | None = field(init=False, default=None, repr=False)
    _points_cache: tuple | None = field(init=False, default=None, repr=False)
    _analogs_cache_key: tuple | None = field(init=False, default=None, repr=False)
    _analogs_cache: tuple | None = field(init=False, default=None, repr=False)

    def __post_init__(self) -> None:
        self._meta = self._chunk.meta
        self.point_labels = _normalize_label_list(self.point_labels, field="points")
        self.analog_labels = _normalize_label_list(self.analog_labels, field="analogs")
        if self.type_groups is None:
            self.type_groups = []
        else:
            # Normalize to str list (but keep as list for easy mutation).
            self.type_groups = [str(x) for x in self.type_groups]

    @property
    def meta(self) -> dict:
        return self._meta

    @property
    def meta_tree(self) -> dict:
        return self._chunk.meta_tree

    def describe(self) -> str:
        m = self.meta
        src = m.get("source_path") or ""
        return (
            "sqzc3d.View\\n"
            f"- source_path: {src}\\n"
            f"- n_frames: {m.get('n_frames')}\\n"
            f"- n_points: {m.get('n_points')} (total={m.get('n_points_total')})\\n"
            f"- n_analogs: {m.get('n_analogs')} (by_frame={m.get('n_analog_by_frame')})\\n"
            f"- points selector: {self.point_labels!r}\\n"
            f"- analogs selector: {self.analog_labels!r} (layout={self.analog_layout})\\n"
            f"- type_groups: {self.type_groups!r} (strict={self.type_groups_strict}, missing_meta={self.type_groups_missing_meta!r})\\n"
        )

    def _effective_point_indices(self) -> list[int] | None:
        m = self.meta
        n_points = int(m.get("n_points", 0) or 0)
        if n_points <= 0:
            return []
        if self.point_labels is None and not self.type_groups:
            return None

        mask: bytearray | None = None

        if self.point_labels is not None:
            idx = [int(x) for x in self._chunk._point_indices_for_labels(self.point_labels)]
            mask = indices_to_mask(idx, n_points)

        if self.type_groups:
            idx = type_group_indices(
                self._chunk,
                self.type_groups,
                strict=bool(self.type_groups_strict),
                missing_meta=str(self.type_groups_missing_meta),
            )
            m2 = indices_to_mask(idx, n_points) if idx else bytearray(n_points)
            if mask is None:
                mask = m2
            else:
                for i in range(n_points):
                    mask[i] = 1 if (mask[i] != 0 and m2[i] != 0) else 0

        if mask is None:
            return None
        return mask_to_indices(mask)

    def _points_query_key(self) -> tuple:
        pl = None if self.point_labels is None else tuple(self.point_labels)
        tg = tuple(self.type_groups or ())
        return (pl, tg, bool(self.type_groups_strict), str(self.type_groups_missing_meta))

    def _analogs_query_key(self) -> tuple:
        al = None if self.analog_labels is None else tuple(self.analog_labels)
        return (al, str(self.analog_layout))

    def _points_tuple(self):
        key = self._points_query_key()
        if self._points_cache_key == key and self._points_cache is not None:
            return self._points_cache
        idx = self._effective_point_indices()
        if idx is None:
            out = self._chunk.points(None, copy=False)
            self._points_cache_key = key
            self._points_cache = out
            return out
        copy = not _is_contiguous(idx)
        out = self._chunk.points(idx, copy=copy)
        self._points_cache_key = key
        self._points_cache = out
        return out

    def _analogs_tuple(self):
        key = self._analogs_query_key()
        if self._analogs_cache_key == key and self._analogs_cache is not None:
            return self._analogs_cache
        if self.analog_layout not in ("CN", "cn", "tcs", "TCS"):
            raise ValueError("analog_layout must be 'CN' or 'tcs'")
        if self.analog_labels is None:
            out = self._chunk.analogs(None, layout=self.analog_layout, copy=False)
            self._analogs_cache_key = key
            self._analogs_cache = out
            return out
        # Label selection can be non-contiguous; prefer safety.
        out = self._chunk.analogs(self.analog_labels, layout=self.analog_layout, copy=True)
        self._analogs_cache_key = key
        self._analogs_cache = out
        return out

    @property
    def points(self):
        values, _valid = self._points_tuple()
        return values

    @property
    def points_valid(self):
        _values, valid = self._points_tuple()
        return valid

    @property
    def analogs(self):
        values, _valid = self._analogs_tuple()
        return values

    @property
    def analogs_valid(self):
        _values, valid = self._analogs_tuple()
        return valid

    @property
    def point(self):
        return _PointAccessor(self, want_valid=False)

    @property
    def point_valid(self):
        return _PointAccessor(self, want_valid=True)

    @property
    def analog(self):
        return _AnalogAccessor(self, want_valid=False)

    @property
    def analog_valid(self):
        return _AnalogAccessor(self, want_valid=True)


class _PointAccessor:
    def __init__(self, view: View, *, want_valid: bool) -> None:
        self._view = view
        self._want_valid = want_valid

    def __getitem__(self, label: str):
        values, valid = self._view._chunk.points(label, copy=False)
        # values: (T, 1, 3), valid: (T, 1)
        if self._want_valid:
            return valid[:, 0]
        return values[:, 0, :]


class _AnalogAccessor:
    def __init__(self, view: View, *, want_valid: bool) -> None:
        self._view = view
        self._want_valid = want_valid

    def __getitem__(self, label: str):
        values, valid = self._view._chunk.analogs(label, layout=self._view.analog_layout, copy=False)
        if self._view.analog_layout in ("tcs", "TCS"):
            # values: (T, 1, S)
            if self._want_valid:
                return valid[:, 0, :]
            return values[:, 0, :]
        # values: (C, N) with C==1
        if self._want_valid:
            return valid[0, :]
        return values[0, :]


def read(
    source,
    *,
    start_frame: int = 0,
    frame_count: int = -1,
    points=None,
    analogs=None,
    analog_range=None,
    label_norm: int = int(_core.SQZC3D_LABEL_NORM_EXACT),
    recipe=None,
    bundle_strict: bool = True,
) -> View:
    """Read a C3D file/buffer (materialize) or load a .sqzc3d bundle.

    Selector semantics:
    - None: default (ALL)
    - []: empty selection
    - str / sequence[str]: label selection
    """
    if _core is None:
        raise ImportError("sqzc3d native bindings are not available in this build")

    points_norm = _normalize_label_list(points, field="points")
    analogs_norm = _normalize_label_list(analogs, field="analogs")

    if isinstance(source, (str, Path)):
        p = Path(source)
        if p.is_dir() or str(p).lower().endswith(".sqzc3d"):
            chunk = _core.load_bundle(str(p), strict=bool(bundle_strict))
            view = View(_chunk=chunk, point_labels=points_norm, analog_labels=analogs_norm)
        else:
            dec = _core.Decoder(str(p), int(label_norm))
            chunk = dec.read(
                start_frame=int(start_frame),
                frame_count=int(frame_count),
                points=points_norm,
                analogs=analogs_norm,
                analog_range=analog_range,
            )
            dec.close()
            view = View(_chunk=chunk, point_labels=points_norm, analog_labels=analogs_norm)
    else:
        dec = _core.Decoder(source, int(label_norm))
        chunk = dec.read(
            start_frame=int(start_frame),
            frame_count=int(frame_count),
            points=points_norm,
            analogs=analogs_norm,
            analog_range=analog_range,
        )
        dec.close()
        view = View(_chunk=chunk, point_labels=points_norm, analog_labels=analogs_norm)

    if recipe is not None:
        # Minimal bridge: apply type_groups fields if present.
        tg = getattr(recipe, "type_groups", ()) or ()
        view.type_groups = [str(x) for x in tg]
        view.type_groups_strict = bool(getattr(recipe, "type_groups_strict", False))
        view.type_groups_missing_meta = str(getattr(recipe, "type_groups_missing_meta", "all"))

    return view
