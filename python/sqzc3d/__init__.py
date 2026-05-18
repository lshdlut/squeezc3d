"""Python interface for sqzc3d."""

from __future__ import annotations

import importlib.machinery
import importlib.util
import sys
from pathlib import Path
from types import ModuleType

__version__ = "0.4.0"

_core_import_error = None


def _load_local_core() -> ModuleType:
    this_dir = Path(__file__).resolve().parent
    for suffix in importlib.machinery.EXTENSION_SUFFIXES:
        candidate = this_dir / f"_core{suffix}"
        if not candidate.is_file():
            continue
        spec = importlib.util.spec_from_file_location(f"{__name__}._core", candidate)
        if spec is None or spec.loader is None:
            raise ImportError(f"Failed to create an import spec for: {candidate}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        try:
            spec.loader.exec_module(module)
        except Exception:
            sys.modules.pop(spec.name, None)
            raise
        return module
    raise FileNotFoundError("Local sqzc3d._core extension module not found next to sqzc3d/__init__.py")


try:
    _core = _load_local_core()
except FileNotFoundError:
    try:
        from . import _core as _core
    except ImportError as e:
        _core = None
        _core_import_error = e
except ImportError as e:
    _core = None
    _core_import_error = e

if _core is not None:
    __version__ = _core.version()
    version = _core.version
    abi_version = _core.abi_version
    features = _core.features
    Decoder = _core.Decoder
    Chunk = _core.Chunk
    load_bundle = _core.load_bundle
    if hasattr(_core, "export_bundle"):
        export_bundle = _core.export_bundle

    from .easy import View, read  # noqa: E402

    from .query import (  # noqa: E402
        ChunkQuery,
        ChunkRecipe,
        indices_to_mask,
        mask_to_indices,
        mask_or_indices,
        mask_and_indices,
        mask_andnot_indices,
        type_group_indices,
    )
    Recipe = ChunkRecipe
    SQZC3D_VERSION = _core.SQZC3D_VERSION
    SQZC3D_VERSION_MAJOR = _core.SQZC3D_VERSION_MAJOR
    SQZC3D_VERSION_MINOR = _core.SQZC3D_VERSION_MINOR
    SQZC3D_VERSION_PATCH = _core.SQZC3D_VERSION_PATCH
    SQZC3D_ABI_VERSION = _core.SQZC3D_ABI_VERSION
    SQZC3D_FEATURE_OPEN_FILE = _core.SQZC3D_FEATURE_OPEN_FILE
    SQZC3D_FEATURE_OPEN_MEMORY = _core.SQZC3D_FEATURE_OPEN_MEMORY
    SQZC3D_FEATURE_BUILD_CHUNKS = _core.SQZC3D_FEATURE_BUILD_CHUNKS
    SQZC3D_FEATURE_BUNDLE = _core.SQZC3D_FEATURE_BUNDLE
    SQZC3D_FEATURE_ANALOG = _core.SQZC3D_FEATURE_ANALOG
    SQZC3D_FEATURE_POINT_RESIDUAL = _core.SQZC3D_FEATURE_POINT_RESIDUAL
    SQZC3D_LABEL_NORM_EXACT = _core.SQZC3D_LABEL_NORM_EXACT
    SQZC3D_LABEL_NORM_TRIM = _core.SQZC3D_LABEL_NORM_TRIM
    SQZC3D_LABEL_NORM_CASEFOLD_WS = _core.SQZC3D_LABEL_NORM_CASEFOLD_WS
    SQZC3D_VALID_POLICY_FINITE_XYZ = _core.SQZC3D_VALID_POLICY_FINITE_XYZ
    SQZC3D_VALID_POLICY_FINITE_XYZ_AND_RESIDUAL_GATE = _core.SQZC3D_VALID_POLICY_FINITE_XYZ_AND_RESIDUAL_GATE
    __all__ = [
        "Decoder",
        "Chunk",
        "ChunkQuery",
        "ChunkRecipe",
        "Recipe",
        "View",
        "load_bundle",
        "read",
        "version",
        "abi_version",
        "features",
        "__version__",
        "indices_to_mask",
        "mask_to_indices",
        "mask_or_indices",
        "mask_and_indices",
        "mask_andnot_indices",
        "type_group_indices",
        "SQZC3D_VERSION",
        "SQZC3D_VERSION_MAJOR",
        "SQZC3D_VERSION_MINOR",
        "SQZC3D_VERSION_PATCH",
        "SQZC3D_ABI_VERSION",
        "SQZC3D_FEATURE_OPEN_FILE",
        "SQZC3D_FEATURE_OPEN_MEMORY",
        "SQZC3D_FEATURE_BUILD_CHUNKS",
        "SQZC3D_FEATURE_BUNDLE",
        "SQZC3D_FEATURE_ANALOG",
        "SQZC3D_FEATURE_POINT_RESIDUAL",
        "SQZC3D_LABEL_NORM_EXACT",
        "SQZC3D_LABEL_NORM_TRIM",
        "SQZC3D_LABEL_NORM_CASEFOLD_WS",
        "SQZC3D_VALID_POLICY_FINITE_XYZ",
        "SQZC3D_VALID_POLICY_FINITE_XYZ_AND_RESIDUAL_GATE",
    ]
    if hasattr(_core, "export_bundle"):
        __all__.append("export_bundle")
else:
    __all__ = ["__version__"]


def __getattr__(name: str):
    if _core is not None:
        raise AttributeError(name)
    msg = (
        "sqzc3d native bindings are not available in this build. "
        "If you are developing from source, build/install the package so the compiled extension is present. "
        "Track progress at https://github.com/lshdlut/squeezc3d."
    )
    if _core_import_error is not None:
        msg += f" Original import error: {_core_import_error}"
    raise ImportError(msg) from _core_import_error
