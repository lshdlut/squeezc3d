#include <pybind11/pybind11.h>

#include <string>

#include "sqzc3d.h"

namespace py = pybind11;

PYBIND11_MODULE(_core, m) {
  m.doc() = "sqzc3d core bindings (minimal scaffold; API is WIP)";

  m.def("version", []() { return std::string(sqzc3d_version()); });
  m.def("abi_version", []() { return sqzc3d_abi_version(); });
  m.def("features", []() { return sqzc3d_get_features(); });
}

