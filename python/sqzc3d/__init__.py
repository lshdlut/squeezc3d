"""Python interface for sqzc3d."""

from __future__ import annotations

__version__ = "0.2.0"

_core_import_error = None
try:
    from . import _core as _core
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
    SQZC3D_LABEL_NORM_EXACT = _core.SQZC3D_LABEL_NORM_EXACT
    SQZC3D_LABEL_NORM_TRIM = _core.SQZC3D_LABEL_NORM_TRIM
    SQZC3D_LABEL_NORM_CASEFOLD_WS = _core.SQZC3D_LABEL_NORM_CASEFOLD_WS
    __all__ = [
        "Decoder",
        "Chunk",
        "load_bundle",
        "version",
        "abi_version",
        "features",
        "__version__",
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
        "SQZC3D_LABEL_NORM_EXACT",
        "SQZC3D_LABEL_NORM_TRIM",
        "SQZC3D_LABEL_NORM_CASEFOLD_WS",
    ]
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
