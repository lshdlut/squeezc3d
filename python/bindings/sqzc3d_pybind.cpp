#include "sqzc3d.h"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if sqzc3d_WITH_EZC3D
#  include <ezc3d/ezc3d_all.h>
#endif

namespace py = pybind11;

namespace {

struct DecoderHolder {
  sqzc3d_dec_t* dec = nullptr;
  std::string source_path;

  ~DecoderHolder() { close(); }

  void close() {
    if (dec) {
      sqzc3d_close_dec(dec);
      dec = nullptr;
    }
    source_path.clear();
  }
};

struct ChunkHolder {
  sqzc3d_chunk_t* chunk = nullptr;
  std::string source_path;

  ChunkHolder(sqzc3d_chunk_t* in_chunk, std::string src_path = {})
      : chunk(in_chunk), source_path(std::move(src_path)) {}
  ~ChunkHolder() {
    if (chunk) {
      sqzc3d_free_chunk(chunk);
      chunk = nullptr;
    }
  }
};

[[noreturn]] void RaiseStatusError(const char* api, int status, const sqzc3d_dec_t* dec = nullptr) {
  const char* msg = sqzc3d_last_error(dec);
  if (!msg || !msg[0]) msg = "unknown";
  std::ostringstream os;
  os << api << " failed with status=" << status << " : " << msg;
  throw std::runtime_error(os.str());
}

void CheckStatus(int status, const char* api, const sqzc3d_dec_t* dec = nullptr) {
  if (status != sqzc3d_STATUS_SUCCESS) {
    RaiseStatusError(api, status, dec);
  }
}

template <typename T>
py::array_t<T> MakeArrayNoCopy(
    const T* data,
    const std::vector<py::ssize_t>& shape,
    const std::vector<py::ssize_t>& strides,
    const py::object& owner) {
  return py::array_t<T>(shape, strides, data, owner);
}

enum class SelectorKind { kAll, kIndices, kLabels };

struct ParsedSelector {
  SelectorKind kind = SelectorKind::kAll;
  std::vector<int> indices;
  std::vector<std::string> labels;
};

ParsedSelector ParseSelectorArg(const py::handle& obj, const char* field_name) {
  ParsedSelector out;
  if (obj.is_none()) {
    return out;
  }
  if (py::isinstance<py::bool_>(obj)) {
    throw py::type_error(std::string(field_name) + " expects None/int/str or sequence");
  }
  if (py::isinstance<py::int_>(obj)) {
    out.kind = SelectorKind::kIndices;
    out.indices.push_back(static_cast<int>(py::cast<py::ssize_t>(obj)));
    return out;
  }
  if (py::isinstance<py::str>(obj)) {
    out.kind = SelectorKind::kLabels;
    out.labels.push_back(py::cast<std::string>(obj));
    return out;
  }
  if (py::isinstance<py::sequence>(obj)) {
    auto seq = obj.cast<py::sequence>();
    if (seq.size() == 0) {
      out.kind = SelectorKind::kAll;
      return out;
    }
    out.kind = SelectorKind::kIndices;
    bool saw_str = false;
    for (const auto item : seq) {
      if (py::isinstance<py::str>(item)) {
        saw_str = true;
        out.labels.push_back(py::cast<std::string>(item));
      } else if (py::isinstance<py::int_>(item)) {
        out.indices.push_back(static_cast<int>(py::cast<py::ssize_t>(item)));
      } else {
        throw py::type_error(std::string(field_name) + " accepts only int/str sequence");
      }
    }
    if (saw_str) {
      if (!out.indices.empty()) {
        throw py::type_error(std::string(field_name) + " mixed int and str selectors are not allowed");
      }
      out.kind = SelectorKind::kLabels;
    }
    return out;
  }
  throw py::type_error(std::string(field_name) + " expects None/int/str or sequence");
}

sqzc3d_range_t ParseRangeArg(const py::handle& obj, const char* field_name) {
  if (obj.is_none()) {
    return {0, -1};
  }
  if (py::isinstance<py::int_>(obj)) {
    return {0, static_cast<int>(py::cast<py::ssize_t>(obj))};
  }
  if (py::isinstance<py::sequence>(obj)) {
    auto seq = obj.cast<py::sequence>();
    if (seq.size() != 2) {
      throw py::type_error(std::string(field_name) + " expects (start, count)");
    }
    return {static_cast<int>(py::cast<py::ssize_t>(seq[0])),
            static_cast<int>(py::cast<py::ssize_t>(seq[1]))};
  }
  throw py::type_error(std::string(field_name) + " expects int or (start, count)");
}

std::vector<int> ResolvePointLabelIndices(const sqzc3d_chunk_t* chunk, const std::vector<std::string>& labels) {
  std::vector<const char*> ptrs;
  ptrs.reserve(labels.size());
  for (const auto& l : labels) ptrs.push_back(l.c_str());
  std::vector<int> out(labels.size(), -1);
  if (labels.empty()) return out;
  CheckStatus(
      sqzc3d_point_indices_for_labels(chunk, ptrs.data(), static_cast<int>(ptrs.size()), out.data(), -1),
      "sqzc3d_point_indices_for_labels");
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i] < 0) {
      throw py::key_error(std::string("point label not found: ") + labels[i]);
    }
  }
  return out;
}

std::vector<int> ResolveAnalogLabelIndices(const sqzc3d_chunk_t* chunk, const std::vector<std::string>& labels) {
  std::vector<const char*> ptrs;
  ptrs.reserve(labels.size());
  for (const auto& l : labels) ptrs.push_back(l.c_str());
  std::vector<int> out(labels.size(), -1);
  if (labels.empty()) return out;
  CheckStatus(
      sqzc3d_analog_indices_for_labels(chunk, ptrs.data(), static_cast<int>(ptrs.size()), out.data(), -1),
      "sqzc3d_analog_indices_for_labels");
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i] < 0) {
      throw py::key_error(std::string("analog label not found: ") + labels[i]);
    }
  }
  return out;
}

std::string PointsLayoutToString(int value) {
  return (value == sqzc3d_POINTS_LAYOUT_FRAME_MAJOR) ? "frame_major" : "unknown";
}

std::string ReadPolicyToString(int value) {
  if (value == sqzc3d_READ_POLICY_DENSE) return "dense";
  if (value == sqzc3d_READ_POLICY_SPARSE) return "sparse";
  return "auto";
}

std::string PointsPackToString(int value) {
  return (value == sqzc3d_POINTS_PACK_AOS_XYZ_VALID) ? "aos_xyz_valid" : "unknown";
}

struct PyChunk;

struct PyDecoder {
  DecoderHolder handle;

  PyDecoder(const std::string& source_path, bool preserve_raw_params = false, int label_norm = sqzc3d_LABEL_NORM_EXACT) {
    sqzc3d_open_opt_t opt{};
    sqzc3d_default_open_opt(&opt);
    opt.preserve_raw_params = preserve_raw_params ? 1 : 0;
    opt.label_norm = label_norm;
    CheckStatus(sqzc3d_open_file(&handle.dec, source_path.c_str(), &opt), "sqzc3d_open_file");
    handle.source_path = source_path;
  }

  PyDecoder(py::buffer buffer, bool preserve_raw_params = false, int label_norm = sqzc3d_LABEL_NORM_EXACT) {
    auto view = buffer.request();
    if (view.itemsize <= 0) {
      throw std::runtime_error("input buffer has invalid itemsize");
    }
    if (view.size < 0 || view.size > static_cast<std::ptrdiff_t>(std::numeric_limits<int>::max())) {
      throw std::runtime_error("input buffer too large");
    }
    const auto n_bytes = static_cast<int>(view.size * view.itemsize);
    sqzc3d_open_opt_t opt{};
    sqzc3d_default_open_opt(&opt);
    opt.preserve_raw_params = preserve_raw_params ? 1 : 0;
    opt.label_norm = label_norm;
    CheckStatus(sqzc3d_open_memory(&handle.dec, view.ptr, n_bytes, &opt), "sqzc3d_open_memory");
  }

  bool is_closed() const { return handle.dec == nullptr; }

  std::shared_ptr<PyChunk> read(
      int start_frame,
      int frame_count,
      py::object points_selector,
      py::object analog_selector,
      py::object analog_range) {
    if (!handle.dec) throw std::runtime_error("decoder closed");
    sqzc3d_build_opt_t opt{};
    sqzc3d_default_build_opt(&opt);
    opt.frame_range = {start_frame, frame_count};
    opt.analog_range = ParseRangeArg(analog_range, "analog_range");

    const auto p_sel = ParseSelectorArg(points_selector, "points");
    const auto a_sel = ParseSelectorArg(analog_selector, "analogs");

    std::vector<int> point_idx;
    std::vector<int> analog_idx;
    std::vector<std::string> point_label_storage;
    std::vector<std::string> analog_label_storage;
    std::vector<const char*> point_label_ptrs;
    std::vector<const char*> analog_label_ptrs;

    if (p_sel.kind == SelectorKind::kIndices) {
      point_idx = p_sel.indices;
    } else if (p_sel.kind == SelectorKind::kLabels) {
      point_label_storage = p_sel.labels;
      for (auto& s : point_label_storage) point_label_ptrs.push_back(s.c_str());
    }

    if (a_sel.kind == SelectorKind::kIndices) {
      analog_idx = a_sel.indices;
    } else if (a_sel.kind == SelectorKind::kLabels) {
      analog_label_storage = a_sel.labels;
      for (auto& s : analog_label_storage) analog_label_ptrs.push_back(s.c_str());
    }

    if (p_sel.kind == SelectorKind::kAll || (p_sel.kind == SelectorKind::kIndices && point_idx.empty()) ||
        (p_sel.kind == SelectorKind::kLabels && point_label_ptrs.empty())) {
      opt.point_sel_mode = sqzc3d_POINT_SEL_ALL;
      opt.point_sel = nullptr;
      opt.point_sel_count = 0;
    } else if (p_sel.kind == SelectorKind::kIndices) {
      opt.point_sel_mode = sqzc3d_POINT_SEL_INDICES;
      opt.point_sel = point_idx.data();
      opt.point_sel_count = static_cast<int>(point_idx.size());
    } else {
      opt.point_sel_mode = sqzc3d_POINT_SEL_LABELS;
      opt.point_labels = point_label_ptrs.data();
      opt.point_labels_count = static_cast<int>(point_label_ptrs.size());
    }

    if (a_sel.kind == SelectorKind::kAll || (a_sel.kind == SelectorKind::kIndices && analog_idx.empty()) ||
        (a_sel.kind == SelectorKind::kLabels && analog_label_ptrs.empty())) {
      opt.analog_sel_mode = sqzc3d_ANALOG_SEL_ALL;
      opt.analog_sel = nullptr;
      opt.analog_sel_count = 0;
    } else if (a_sel.kind == SelectorKind::kIndices) {
      opt.analog_sel_mode = sqzc3d_ANALOG_SEL_INDICES;
      opt.analog_sel = analog_idx.data();
      opt.analog_sel_count = static_cast<int>(analog_idx.size());
    } else {
      opt.analog_sel_mode = sqzc3d_ANALOG_SEL_LABELS;
      opt.analog_labels = analog_label_ptrs.data();
      opt.analog_labels_count = static_cast<int>(analog_label_ptrs.size());
    }

    sqzc3d_chunk_t* out = nullptr;
    CheckStatus(sqzc3d_build_chunks(handle.dec, &opt, &out), "sqzc3d_build_chunks", handle.dec);
    return std::make_shared<PyChunk>(std::make_shared<ChunkHolder>(out, handle.source_path));
  }

  void close() { handle.close(); }

  const std::string& source_path() const { return handle.source_path; }
};

struct PyChunk {
  explicit PyChunk(std::shared_ptr<ChunkHolder> holder) : holder_(std::move(holder)) {}

  py::tuple points(py::object selector, bool copy) const {
    const auto* chunk = holder_->chunk;
    if (!chunk) throw std::runtime_error("chunk is not available");

    const auto parsed = ParseSelectorArg(selector, "selector");
    std::vector<int> point_indices;
    if (parsed.kind == SelectorKind::kIndices) {
      point_indices = parsed.indices;
    } else if (parsed.kind == SelectorKind::kLabels) {
      point_indices = ResolvePointLabelIndices(chunk, parsed.labels);
    }
    if (!point_indices.empty()) {
      for (const auto idx : point_indices) {
        if (idx < 0 || idx >= chunk->n_points) {
          throw py::index_error("point index out of range");
        }
      }
    }

    sqzc3d_points_view_t view{};
    if (parsed.kind == SelectorKind::kAll || point_indices.empty()) {
      view.points_xyz = chunk->points_xyz;
      view.points_valid = chunk->points_valid;
      view.n_frames = chunk->n_frames;
      view.n_points = chunk->n_points;
      view.source_stride_points = chunk->n_points;
      view.source_point_offset = 0;
      view.point_indices = nullptr;
    } else {
      CheckStatus(sqzc3d_points_view_points(chunk, point_indices.data(), static_cast<int>(point_indices.size()), &view),
                  "sqzc3d_points_view_points");
    }
    if (!view.points_xyz) {
      throw std::runtime_error("points data is not available");
    }

    const bool contiguous = (view.point_indices == nullptr);
    if (!copy && !contiguous) {
      throw std::runtime_error("non-contiguous point selection requires copy=True");
    }

    std::vector<py::ssize_t> vshape = {view.n_frames, view.n_points, 3};
    std::vector<py::ssize_t> vstrides = {static_cast<py::ssize_t>(view.source_stride_points * 3 * sizeof(sqzc3d_num_t)),
                                          static_cast<py::ssize_t>(3 * sizeof(sqzc3d_num_t)),
                                          static_cast<py::ssize_t>(sizeof(sqzc3d_num_t))};
    std::vector<py::ssize_t> vshape_valid = {view.n_frames, view.n_points};
    std::vector<py::ssize_t> vstrides_valid = {static_cast<py::ssize_t>(view.source_stride_points * sizeof(unsigned char)),
                                               static_cast<py::ssize_t>(sizeof(unsigned char))};

    if (!contiguous) {
      if (copy) {
        auto values = py::array_t<sqzc3d_num_t>(vshape);
        auto* dst = static_cast<sqzc3d_num_t*>(values.mutable_data());
        const int n_frames = view.n_frames;
        const int n_points = view.n_points;
        for (int f = 0; f < n_frames; ++f) {
          const auto dst_base = static_cast<std::size_t>(f) * static_cast<std::size_t>(n_points) * 3u;
          const auto src_base = static_cast<std::size_t>(f) * static_cast<std::size_t>(view.source_stride_points) * 3u;
          for (int p = 0; p < n_points; ++p) {
            const auto src = src_base + static_cast<std::size_t>(view.point_indices[static_cast<std::size_t>(p)]) * 3u;
            const auto dst_base_point = dst_base + static_cast<std::size_t>(p) * 3u;
            dst[dst_base_point + 0] = view.points_xyz[src];
            dst[dst_base_point + 1] = view.points_xyz[src + 1];
            dst[dst_base_point + 2] = view.points_xyz[src + 2];
          }
        }
        auto valid = py::array_t<unsigned char>(vshape_valid);
        auto* vdst = static_cast<unsigned char*>(valid.mutable_data());
        if (view.points_valid) {
          for (int f = 0; f < n_frames; ++f) {
            const auto vdst_base = static_cast<std::size_t>(f) * static_cast<std::size_t>(n_points);
            const auto vsrc_base = static_cast<std::size_t>(f) * static_cast<std::size_t>(view.source_stride_points);
            for (int p = 0; p < n_points; ++p) {
              vdst[vdst_base + static_cast<std::size_t>(p)] =
                  view.points_valid[vsrc_base + static_cast<std::size_t>(view.point_indices[static_cast<std::size_t>(p)])];
            }
          }
        } else {
          std::fill(vdst, vdst + static_cast<std::size_t>(n_frames) * static_cast<std::size_t>(n_points),
                    static_cast<unsigned char>(1));
        }
        return py::make_tuple(values, valid);
      }
      throw std::runtime_error("non-contiguous point selection requires copy=True");
    }

    py::array_t<sqzc3d_num_t> values = MakeArrayNoCopy(view.points_xyz, vshape, vstrides, py::cast(*this));
    if (!view.points_valid) {
      auto filled = py::array_t<unsigned char>(vshape_valid);
      auto* vp = static_cast<unsigned char*>(filled.mutable_data());
      std::fill(vp, vp + static_cast<std::size_t>(view.n_frames) * static_cast<std::size_t>(view.n_points),
                static_cast<unsigned char>(1));
      return py::make_tuple(values, filled);
    }
    py::array_t<unsigned char> valid =
        MakeArrayNoCopy(view.points_valid, vshape_valid, vstrides_valid, py::cast(*this));
    return py::make_tuple(values, valid);
  }

  py::tuple analogs(py::object selector, const std::string& layout, bool copy) const {
    const auto* chunk = holder_->chunk;
    if (!chunk) throw std::runtime_error("chunk is not available");
    if (layout != "CN" && layout != "cn" && layout != "tcs" && layout != "TCS") {
      throw std::runtime_error("layout must be 'CN' or 'tcs'");
    }

    const auto parsed = ParseSelectorArg(selector, "selector");
    std::vector<int> analog_indices;
    if (parsed.kind == SelectorKind::kIndices) {
      analog_indices = parsed.indices;
    } else if (parsed.kind == SelectorKind::kLabels) {
      analog_indices = ResolveAnalogLabelIndices(chunk, parsed.labels);
    }
    if (!analog_indices.empty()) {
      for (const auto idx : analog_indices) {
        if (idx < 0 || idx >= chunk->n_analogs) {
          throw py::index_error("analog index out of range");
        }
      }
    }

    sqzc3d_analogs_view_t view{};
    if (parsed.kind == SelectorKind::kAll || analog_indices.empty()) {
      CheckStatus(sqzc3d_analogs_view_samples(chunk, 0, chunk->n_frames, &view), "sqzc3d_analogs_view_samples");
    } else {
      CheckStatus(sqzc3d_analogs_view_channels(
                      chunk, analog_indices.data(), static_cast<int>(analog_indices.size()), &view),
                  "sqzc3d_analogs_view_channels");
    }
    if (!view.analog && chunk->n_analogs > 0) {
      throw std::runtime_error("analog data is not available");
    }

    const bool tcs = (layout == "tcs" || layout == "TCS");
    const bool contiguous = (view.channel_indices == nullptr);
    if (!copy && !contiguous) {
      throw std::runtime_error("non-contiguous analog selection requires copy=True");
    }

    const py::ssize_t c = static_cast<py::ssize_t>(view.n_analogs);
    const py::ssize_t t = static_cast<py::ssize_t>(view.n_frames);
    const py::ssize_t s = static_cast<py::ssize_t>(view.n_analog_by_frame);
    const py::ssize_t n =
        (c == 0 && view.n_samples == 0) ? (view.n_analog_by_frame > 0 ? 1 : 0) : static_cast<py::ssize_t>(view.n_samples);
    const py::ssize_t source_stride_samples = static_cast<py::ssize_t>(view.source_stride_samples);

    if (tcs) {
      std::vector<py::ssize_t> vshape = {t, c, s};
      std::vector<py::ssize_t> vstrides = {s * static_cast<py::ssize_t>(sizeof(sqzc3d_num_t)),
                                           source_stride_samples * static_cast<py::ssize_t>(sizeof(sqzc3d_num_t)),
                                           static_cast<py::ssize_t>(sizeof(sqzc3d_num_t))};
      std::vector<py::ssize_t> vshape_valid = vshape;
      std::vector<py::ssize_t> vstrides_valid = {s * static_cast<py::ssize_t>(sizeof(unsigned char)),
                                                 source_stride_samples * static_cast<py::ssize_t>(sizeof(unsigned char)),
                                                 static_cast<py::ssize_t>(sizeof(unsigned char))};
      if (copy && !contiguous) {
        auto values = py::array_t<sqzc3d_num_t>(vshape);
        auto valid = py::array_t<unsigned char>(vshape_valid);
        auto* vdst = static_cast<sqzc3d_num_t*>(values.mutable_data());
        auto* vdstd = static_cast<unsigned char*>(valid.mutable_data());
        std::vector<int> map(static_cast<std::size_t>(c));
        for (py::ssize_t i = 0; i < c; ++i) {
          map[static_cast<std::size_t>(i)] = (view.channel_indices ? view.channel_indices[static_cast<std::size_t>(i)] : static_cast<int>(i));
        }
        for (py::ssize_t ti = 0; ti < t; ++ti) {
          for (py::ssize_t ci = 0; ci < c; ++ci) {
            const int src_c = map[static_cast<std::size_t>(ci)];
            for (py::ssize_t si = 0; si < s; ++si) {
              const py::ssize_t out_i = (ti * c + ci) * s + si;
              const py::ssize_t in_i = static_cast<py::ssize_t>(src_c) * source_stride_samples +
                                       ti * s + si;
              vdst[out_i] = view.analog ? view.analog[static_cast<std::size_t>(in_i)] : 0.0;
              vdstd[out_i] = view.analog_valid ? view.analog_valid[static_cast<std::size_t>(in_i)] : 1;
            }
          }
        }
        return py::make_tuple(values, valid);
      }
      py::array_t<sqzc3d_num_t> values = MakeArrayNoCopy(view.analog, vshape, vstrides, py::cast(*this));
      if (!view.analog_valid) {
        auto filled = py::array_t<unsigned char>(vshape_valid);
        auto* ptr = static_cast<unsigned char*>(filled.mutable_data());
        std::fill(ptr, ptr + static_cast<std::size_t>(t * c * s), static_cast<unsigned char>(1));
        return py::make_tuple(values, filled);
      }
      py::array_t<unsigned char> valid =
          MakeArrayNoCopy(view.analog_valid, vshape_valid, vstrides_valid, py::cast(*this));
      return py::make_tuple(values, valid);
    }

    std::vector<py::ssize_t> vshape = {c, n};
    std::vector<py::ssize_t> vstrides = {n * static_cast<py::ssize_t>(sizeof(sqzc3d_num_t)),
                                         static_cast<py::ssize_t>(sizeof(sqzc3d_num_t))};
    std::vector<py::ssize_t> vshape_valid = {c, n};
    std::vector<py::ssize_t> vstrides_valid = {n * static_cast<py::ssize_t>(sizeof(unsigned char)),
                                               static_cast<py::ssize_t>(sizeof(unsigned char))};
    if (copy && !contiguous) {
      auto values = py::array_t<sqzc3d_num_t>(vshape);
      auto valid = py::array_t<unsigned char>(vshape_valid);
      auto* vdst = static_cast<sqzc3d_num_t*>(values.mutable_data());
      auto* vdstd = static_cast<unsigned char*>(valid.mutable_data());
      std::vector<int> map(static_cast<std::size_t>(c));
      for (py::ssize_t i = 0; i < c; ++i) {
        map[static_cast<std::size_t>(i)] = (view.channel_indices ? view.channel_indices[static_cast<std::size_t>(i)] : static_cast<int>(i));
      }
      for (py::ssize_t ci = 0; ci < c; ++ci) {
        const int src_c = map[static_cast<std::size_t>(ci)];
        for (py::ssize_t si = 0; si < n; ++si) {
          const py::ssize_t out_i = ci * n + si;
          const py::ssize_t in_i = static_cast<py::ssize_t>(src_c) * source_stride_samples + si;
          vdst[out_i] = view.analog ? view.analog[static_cast<std::size_t>(in_i)] : 0.0;
          vdstd[out_i] = view.analog_valid ? view.analog_valid[static_cast<std::size_t>(in_i)] : 1;
        }
      }
      return py::make_tuple(values, valid);
    }
    py::array_t<sqzc3d_num_t> values = MakeArrayNoCopy(view.analog, vshape, vstrides, py::cast(*this));
    if (!view.analog_valid) {
      auto filled = py::array_t<unsigned char>(vshape_valid);
      auto* ptr = static_cast<unsigned char*>(filled.mutable_data());
      std::fill(ptr, ptr + static_cast<std::size_t>(c * n), static_cast<unsigned char>(1));
      return py::make_tuple(values, filled);
    }
    py::array_t<unsigned char> valid =
        MakeArrayNoCopy(view.analog_valid, vshape_valid, vstrides_valid, py::cast(*this));
    return py::make_tuple(values, valid);
  }

  py::object point_indices_total() const {
    const auto* chunk = holder_ ? holder_->chunk : nullptr;
    if (!chunk) return py::none();
    if (chunk->n_points == 0) return py::list();
    const int* indices = nullptr;
    int n = 0;
    const int st = sqzc3d_chunk_point_indices_total(chunk, &indices, &n);
    if (st != sqzc3d_STATUS_SUCCESS || !indices || n <= 0) return py::none();
    py::list out;
    for (int i = 0; i < n; ++i) out.append(indices[i]);
    return std::move(out);
  }

  py::dict meta() const {
    py::dict out;
    const auto* chunk = holder_->chunk;
    if (!chunk) return out;
    out["n_frames"] = chunk->n_frames;
    out["n_points"] = chunk->n_points;
    out["n_points_total"] = chunk->n_points_total;
    out["n_analogs"] = chunk->n_analogs;
    out["n_analog_by_frame"] = (chunk->n_analogs > 0) ? chunk->n_analog_by_frame : 0;
    out["n_scalar"] = chunk->n_scalar;
    out["valid_nscalar"] = chunk->valid_nscalar;
    out["n_analog_scalar"] = chunk->n_analog_scalar;
    out["n_type_groups"] = chunk->n_type_groups;
    out["analog_layout"] = "CN";
    out["points_layout"] = PointsLayoutToString(chunk->points_layout);
    out["read_policy"] = ReadPolicyToString(chunk->read_policy);
    out["points_pack"] = PointsPackToString(chunk->points_pack);
    out["valid_policy"] = chunk->valid_policy;
    out["residual_gate_mm"] = chunk->residual_gate_mm;
    out["point_scale"] = chunk->point_scale;
    out["header_scale"] = chunk->header_scale;
    out["reason"] = chunk->reason ? chunk->reason : "";
    py::list point_labels;
    if (chunk->point_labels) {
      for (int i = 0; i < chunk->n_points; ++i) {
        point_labels.append(chunk->point_labels[static_cast<std::size_t>(i)] ?
                                      chunk->point_labels[static_cast<std::size_t>(i)] :
                                      "");
      }
    }
    out["point_labels"] = std::move(point_labels);
    out["point_indices_total"] = point_indices_total();
    py::list analog_labels;
    if (chunk->analog_labels) {
      for (int i = 0; i < chunk->n_analogs; ++i) {
        analog_labels.append(chunk->analog_labels[static_cast<std::size_t>(i)] ?
                                       chunk->analog_labels[static_cast<std::size_t>(i)] :
                                       "");
      }
    }
    out["analog_labels"] = std::move(analog_labels);
    py::list type_group_names;
    if (chunk->type_group_names) {
      for (int i = 0; i < chunk->n_type_groups; ++i) {
        type_group_names.append(chunk->type_group_names[static_cast<std::size_t>(i)] ?
                                           chunk->type_group_names[static_cast<std::size_t>(i)] :
                                           "");
      }
    }
    out["type_group_names"] = std::move(type_group_names);
    py::list type_group_starts;
    if (chunk->type_group_starts) {
      const int n_start =
          (chunk->n_type_groups >= 0 ? chunk->n_type_groups + 1 : 0);
      for (int i = 0; i < n_start; ++i) {
        type_group_starts.append(chunk->type_group_starts[static_cast<std::size_t>(i)]);
      }
    }
    out["type_group_starts"] = std::move(type_group_starts);
    py::list type_group_indices;
    if (chunk->type_group_indices) {
      if (chunk->type_group_starts && chunk->n_type_groups >= 0) {
        const int end = chunk->type_group_starts[static_cast<std::size_t>(chunk->n_type_groups)];
        for (int i = 0; i < end; ++i) {
          type_group_indices.append(chunk->type_group_indices[static_cast<std::size_t>(i)]);
        }
      }
    }
    out["type_group_indices"] = std::move(type_group_indices);
    return out;
  }

  py::dict meta_tree() const;

  const std::string& source_path() const { return holder_->source_path; }

  std::shared_ptr<ChunkHolder> holder_;
};

#if sqzc3d_WITH_EZC3D
template <typename ParameterT>
py::dict EzcParameterToDict(const ParameterT& parameter) {
  py::dict out;
  out["name"] = parameter.name();
  out["description"] = parameter.description();
  out["locked"] = parameter.isLocked();
  out["type"] = static_cast<int>(parameter.type());
  if (parameter.type() == ezc3d::DATA_TYPE::CHAR) {
    py::list values;
    for (const auto& v : parameter.valuesAsString()) values.append(v);
    out["values"] = values;
  } else if (parameter.type() == ezc3d::DATA_TYPE::FLOAT) {
    py::list values;
    for (const auto v : parameter.valuesAsDouble()) values.append(v);
    out["values"] = values;
  } else {
    py::list values;
    for (const auto v : parameter.valuesAsInt()) values.append(v);
    out["values"] = values;
  }
  return out;
}

py::dict ParseMetaTreeFromSource(const std::string& path, const std::int64_t point_frames_override) {
  py::dict out;
  const auto adjust_point_frames = [](const std::string& name, py::dict& parameter, const std::int64_t frame_count) {
    if (frame_count <= 0 || name != "FRAMES") {
      return;
    }
    if (!parameter.contains("values")) {
      return;
    }
    py::list values;
    values.append(frame_count);
    parameter["values"] = values;
  };
  std::unique_ptr<ezc3d::c3d> c3d;
  try {
    c3d = std::make_unique<ezc3d::c3d>(path);
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("failed to parse meta_tree: ") + path + ": " + e.what());
  }
  py::dict groups;
  for (const auto& group : c3d->parameters().groups()) {
    py::dict g;
    g["name"] = group.name();
    g["description"] = group.description();
    g["locked"] = group.isLocked();
    py::dict parameters;
    for (const auto& parameter : group.parameters()) {
      auto p = EzcParameterToDict(parameter);
      if (group.name() == "POINT") {
        adjust_point_frames(parameter.name(), p, point_frames_override);
      }
      parameters[py::str(parameter.name())] = p;
    }
    g["parameters"] = parameters;
    groups[py::str(group.name())] = g;
  }
  out["groups"] = groups;
  return out;
}
#endif

py::dict PyChunk::meta_tree() const {
  py::dict out;
  if (!holder_ || holder_->source_path.empty()) {
    return out;
  }
#if sqzc3d_WITH_EZC3D
  const auto point_frames_override = static_cast<std::int64_t>(sqzc3d_chunk_num_frames(holder_->chunk));
  return ParseMetaTreeFromSource(holder_->source_path, point_frames_override);
#else
  return out;
#endif
}

PYBIND11_MODULE(_core, m) {
  m.attr("SQZC3D_VERSION") = py::str(SQZC3D_VERSION);
  m.attr("SQZC3D_VERSION_MAJOR") = static_cast<int>(SQZC3D_VERSION_MAJOR);
  m.attr("SQZC3D_VERSION_MINOR") = static_cast<int>(SQZC3D_VERSION_MINOR);
  m.attr("SQZC3D_VERSION_PATCH") = static_cast<int>(SQZC3D_VERSION_PATCH);
  m.attr("SQZC3D_ABI_VERSION") = static_cast<int>(SQZC3D_ABI_VERSION);
  m.attr("SQZC3D_FEATURE_OPEN_FILE") = static_cast<int>(SQZC3D_FEATURE_OPEN_FILE);
  m.attr("SQZC3D_FEATURE_OPEN_MEMORY") = static_cast<int>(SQZC3D_FEATURE_OPEN_MEMORY);
  m.attr("SQZC3D_FEATURE_BUILD_CHUNKS") = static_cast<int>(SQZC3D_FEATURE_BUILD_CHUNKS);
  m.attr("SQZC3D_FEATURE_BUNDLE") = static_cast<int>(SQZC3D_FEATURE_BUNDLE);
  m.attr("SQZC3D_FEATURE_ANALOG") = static_cast<int>(SQZC3D_FEATURE_ANALOG);
  m.attr("SQZC3D_LABEL_NORM_EXACT") = static_cast<int>(sqzc3d_LABEL_NORM_EXACT);
  m.attr("SQZC3D_LABEL_NORM_TRIM") = static_cast<int>(sqzc3d_LABEL_NORM_TRIM);
  m.attr("SQZC3D_LABEL_NORM_CASEFOLD_WS") = static_cast<int>(sqzc3d_LABEL_NORM_CASEFOLD_WS);

  m.def("version", &sqzc3d_version, "Get version string");
  m.def("abi_version", &sqzc3d_abi_version, "Get ABI version");
  m.def("features", &sqzc3d_get_features, "Get runtime feature bits");

  m.def(
      "load_bundle",
      [](const std::string& path, bool strict) {
        sqzc3d_bundle_load_opt_t opt{};
        sqzc3d_default_bundle_load_opt(&opt);
        opt.strict = strict ? 1 : 0;
        sqzc3d_chunk_t* chunk = nullptr;
        CheckStatus(
            sqzc3d_load_bundle_with_options(path.c_str(), &opt, &chunk),
            "sqzc3d_load_bundle_with_options");
        return std::make_shared<PyChunk>(std::make_shared<ChunkHolder>(chunk));
      },
      py::arg("path"),
      py::arg("strict") = true);

  py::class_<PyDecoder>(m, "Decoder")
      .def(py::init<const std::string&, bool, int>(), py::arg("source_path"), py::arg("preserve_raw_params") = false,
           py::arg("label_norm") = static_cast<int>(sqzc3d_LABEL_NORM_EXACT))
      .def(py::init<py::buffer, bool, int>(), py::arg("data"), py::arg("preserve_raw_params") = false,
           py::arg("label_norm") = static_cast<int>(sqzc3d_LABEL_NORM_EXACT))
      .def("read",
           &PyDecoder::read,
           py::arg("start_frame") = 0,
           py::arg("frame_count") = -1,
           py::arg("points") = py::none(),
           py::arg("analogs") = py::none(),
           py::arg("analog_range") = py::none())
      .def("close", &PyDecoder::close)
      .def_property_readonly("source_path", &PyDecoder::source_path)
      .def_property_readonly("closed", &PyDecoder::is_closed);

  py::class_<PyChunk, std::shared_ptr<PyChunk>>(m, "Chunk")
      .def("points", &PyChunk::points, py::arg("selector") = py::none(), py::arg("copy") = true)
      .def("analogs", &PyChunk::analogs, py::arg("selector") = py::none(), py::arg("layout") = "CN",
           py::arg("copy") = true)
      .def_property_readonly("meta", &PyChunk::meta)
      .def_property_readonly("meta_tree", &PyChunk::meta_tree)
      .def_property_readonly("point_indices_total", &PyChunk::point_indices_total)
      .def_property_readonly("source_path", &PyChunk::source_path);
}

}  // namespace
