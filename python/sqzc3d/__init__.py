"""
sqzc3d (Python)

This is a placeholder package published to reserve the `sqzc3d` name on PyPI.
Python bindings (pybind11) for the underlying C/C++ library will be added next.
"""

from __future__ import annotations

__all__ = ["__version__", "version", "abi_version", "features"]

__version__ = "0.0.2"

try:
    from . import _core as _core
except ImportError:
    _core = None

if _core is not None:
    version = _core.version
    abi_version = _core.abi_version
    features = _core.features


def __getattr__(name: str):
    if _core is not None:
        raise AttributeError(name)
    raise ImportError(
        "sqzc3d native bindings are not available in this build. "
        "If you are developing from source, build/install the package so the compiled extension is present. "
        "Track progress at https://github.com/lshdlut/squeezc3d."
    )
