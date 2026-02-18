"""
sqzc3d (Python)

This is a placeholder package published to reserve the `sqzc3d` name on PyPI.
Python bindings (pybind11) for the underlying C/C++ library will be added next.
"""

from __future__ import annotations

__all__ = ["__version__"]

__version__ = "0.0.1"


def __getattr__(name: str):
    raise ImportError(
        "sqzc3d Python bindings are not available yet. "
        "This is a placeholder package published to reserve the name on PyPI. "
        "Track progress at https://github.com/lshdlut/squeezc3d."
    )

