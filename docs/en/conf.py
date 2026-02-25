# Configuration file for the Sphinx documentation builder.
#
# This is used for local builds and Read the Docs (RTD) hosting.

from __future__ import annotations

project = "squeezc3d"
language = "en"

extensions = [
    "myst_parser",
    "sphinx.ext.autosectionlabel",
]

myst_enable_extensions = [
    "colon_fence",
]

myst_heading_anchors = 3

source_suffix = {
    ".md": "markdown",
}

master_doc = "index"

exclude_patterns = [
    "_build",
]

autosectionlabel_prefix_document = True

html_theme = "sphinx_rtd_theme"
html_static_path: list[str] = []
