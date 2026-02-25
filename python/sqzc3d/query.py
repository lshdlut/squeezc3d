"""Chunk-local filtering helpers for sqzc3d.

This module is intentionally small and explicit:
- A Query is bound to a specific Chunk.
- All filter slots combine by AND (set intersection).
- Missing TYPE_GROUPS metadata defaults to a no-op (all points), unless strict=True.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Sequence


def _normalize_str_list(names: str | Sequence[str]) -> list[str]:
    if isinstance(names, str):
        return [names]
    return [str(x) for x in names]


def _normalize_int_list(indices: int | Sequence[int]) -> list[int]:
    if isinstance(indices, int):
        return [int(indices)]
    return [int(x) for x in indices]


def indices_to_mask(indices: int | Sequence[int], n_total: int) -> bytearray:
    """Convert a list of indices to a uint8 mask (bytearray) of length n_total."""
    if n_total < 0:
        raise ValueError(f"n_total must be non-negative, got {n_total}")
    out = bytearray(n_total)
    idx_list = _normalize_int_list(indices)
    for idx in idx_list:
        if idx < 0 or idx >= n_total:
            raise ValueError(f"index out of bounds: idx={idx} (n_total={n_total})")
        out[idx] = 1
    return out


def mask_to_indices(mask: Sequence[int]) -> list[int]:
    """Convert a uint8-like mask to a list of indices (i where mask[i] != 0)."""
    out: list[int] = []
    for i, v in enumerate(mask):
        if int(v) != 0:
            out.append(i)
    return out


def mask_or_indices(a: Sequence[int], b: Sequence[int]) -> list[int]:
    """Return indices for (a OR b)."""
    if len(a) != len(b):
        raise ValueError(f"mask length mismatch: len(a)={len(a)} len(b)={len(b)}")
    out: list[int] = []
    for i in range(len(a)):
        if int(a[i]) != 0 or int(b[i]) != 0:
            out.append(i)
    return out


def mask_and_indices(a: Sequence[int], b: Sequence[int]) -> list[int]:
    """Return indices for (a AND b)."""
    if len(a) != len(b):
        raise ValueError(f"mask length mismatch: len(a)={len(a)} len(b)={len(b)}")
    out: list[int] = []
    for i in range(len(a)):
        if int(a[i]) != 0 and int(b[i]) != 0:
            out.append(i)
    return out


def mask_andnot_indices(a: Sequence[int], b: Sequence[int]) -> list[int]:
    """Return indices for (a AND (NOT b))."""
    if len(a) != len(b):
        raise ValueError(f"mask length mismatch: len(a)={len(a)} len(b)={len(b)}")
    out: list[int] = []
    for i in range(len(a)):
        if int(a[i]) != 0 and int(b[i]) == 0:
            out.append(i)
    return out


def type_group_indices(
    chunk,
    group_names: str | Sequence[str],
    *,
    strict: bool = False,
    missing_meta: str = "all",
) -> list[int]:
    """Resolve POINT:TYPE_GROUPS group names into a flat chunk-local point index list.

    Default semantics (strict=False):
    - If TYPE_GROUPS metadata is missing on this chunk: treat as no-op (all points) when missing_meta="all".
    - If metadata exists but a group name is missing: ignore it; if none match, return empty.

    strict=True enforces:
    - TYPE_GROUPS metadata must be present.
    - Every requested group name must exist.
    """
    want = _normalize_str_list(group_names)
    meta = chunk.meta
    n_points = int(meta.get("n_points", 0))
    n_type_groups = int(meta.get("n_type_groups", 0))
    tg_names = list(meta.get("type_group_names") or [])
    tg_starts = list(meta.get("type_group_starts") or [])
    tg_indices = list(meta.get("type_group_indices") or [])

    has_meta = (
        n_type_groups > 0
        and len(tg_names) == n_type_groups
        and len(tg_starts) == n_type_groups + 1
    )
    if not has_meta:
        if strict:
            raise ValueError("TYPE_GROUPS metadata is missing on this chunk")
        if missing_meta == "all":
            return list(range(n_points))
        if missing_meta == "empty":
            return []
        raise ValueError(f"invalid missing_meta={missing_meta!r} (expected 'all' or 'empty')")

    name_to_gi: dict[str, int] = {}
    for gi, name in enumerate(tg_names):
        s = str(name)
        if s not in name_to_gi:
            name_to_gi[s] = gi

    seen = bytearray(n_points)
    out: list[int] = []
    missing: list[str] = []
    for want_name in want:
        gi = name_to_gi.get(want_name)
        if gi is None:
            missing.append(want_name)
            continue
        s = int(tg_starts[gi])
        e = int(tg_starts[gi + 1])
        for raw_idx in tg_indices[s:e]:
            idx = int(raw_idx)
            if idx < 0 or idx >= n_points:
                # Source metadata is expected to be validated upstream, but stay robust here.
                continue
            if seen[idx] == 0:
                seen[idx] = 1
                out.append(idx)

    if strict and missing:
        raise ValueError(f"TYPE_GROUPS missing group name(s): {missing}")
    return out


class ChunkQuery:
    """Chunk-bound AND-only point query."""

    def __init__(self, chunk):
        self._chunk = chunk
        meta = chunk.meta
        self._n_points = int(meta.get("n_points", 0))

        self._sel_mask: bytearray | None = None
        self._type_mask: bytearray | None = None

    @property
    def chunk(self):
        return self._chunk

    @property
    def n_points(self) -> int:
        return self._n_points

    def sel_indices(self, indices: int | Sequence[int]) -> "ChunkQuery":
        self._sel_mask = indices_to_mask(indices, self._n_points)
        return self

    def sel_mask(self, mask: Sequence[int]) -> "ChunkQuery":
        if len(mask) != self._n_points:
            raise ValueError(f"mask length mismatch: len(mask)={len(mask)} expected={self._n_points}")
        out = bytearray(self._n_points)
        for i in range(self._n_points):
            out[i] = 1 if int(mask[i]) != 0 else 0
        self._sel_mask = out
        return self

    def type_groups(
        self,
        group_names: str | Sequence[str],
        *,
        strict: bool = False,
        missing_meta: str = "all",
    ) -> "ChunkQuery":
        idx = type_group_indices(self._chunk, group_names, strict=strict, missing_meta=missing_meta)
        self._type_mask = indices_to_mask(idx, self._n_points) if idx else bytearray(self._n_points)
        return self

    def mask(self) -> bytearray:
        out = bytearray(self._n_points)
        for i in range(self._n_points):
            out[i] = 1
        if self._sel_mask is not None:
            if len(self._sel_mask) != self._n_points:
                raise ValueError("internal sel mask length mismatch")
            for i in range(self._n_points):
                out[i] = 1 if (out[i] != 0 and self._sel_mask[i] != 0) else 0
        if self._type_mask is not None:
            if len(self._type_mask) != self._n_points:
                raise ValueError("internal type mask length mismatch")
            for i in range(self._n_points):
                out[i] = 1 if (out[i] != 0 and self._type_mask[i] != 0) else 0
        return out

    def indices(self) -> list[int]:
        return mask_to_indices(self.mask())

    def describe(self) -> str:
        idx = self.indices()
        return (
            "sqzc3d.ChunkQuery\n"
            f"- n_points: {self._n_points}\n"
            f"- selected: {len(idx)}\n"
            f"- sel_mask: {'set' if self._sel_mask is not None else 'unset'}\n"
            f"- type_mask: {'set' if self._type_mask is not None else 'unset'}\n"
        )

    def points(self, *, copy: bool = True):
        m = self.mask()
        if self._n_points == 0:
            return self._chunk.points([], copy=copy)
        all_selected = True
        for v in m:
            if v == 0:
                all_selected = False
                break
        if all_selected:
            return self._chunk.points(None, copy=copy)
        return self._chunk.points(mask_to_indices(m), copy=copy)


@dataclass(frozen=True)
class ChunkRecipe:
    """Reusable recipe that can be applied to any chunk (no explicit indices)."""

    type_groups: tuple[str, ...] = ()
    type_groups_strict: bool = False
    type_groups_missing_meta: str = "all"

    def apply(self, chunk) -> ChunkQuery:
        q = ChunkQuery(chunk)
        if self.type_groups:
            q.type_groups(
                self.type_groups,
                strict=self.type_groups_strict,
                missing_meta=self.type_groups_missing_meta,
            )
        return q

    def describe(self) -> str:
        return (
            "sqzc3d.ChunkRecipe\n"
            f"- type_groups: {list(self.type_groups)!r}\n"
            f"- type_groups_strict: {bool(self.type_groups_strict)}\n"
            f"- type_groups_missing_meta: {self.type_groups_missing_meta!r}\n"
        )
