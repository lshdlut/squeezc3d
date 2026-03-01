#include "sqzc3d_c3d_stream.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using C3dStreamReader = sqzc3d::C3dStreamReader;
using sqzc3d_status = int;
#if sqzc3d_WITH_EZC3D

namespace {

using C3dStreamMeta = sqzc3d::C3dStreamMeta;

class C3dHeaderOnly final : public ezc3d::c3d {
 public:
  C3dHeaderOnly() = default;

  void load_header_only(std::fstream& file, const std::string& file_path) {
    _filePath = file_path;
    _data.reset();
    _header = std::make_shared<ezc3d::Header>(*this, file);
    _parameters = std::make_shared<ezc3d::ParametersNS::Parameters>(*this, file);

    // Keep header fields aligned with parameters, consistent with ezc3d::c3d full parsing.
    // Some real-world files rely on ANALOG:USED/RATE and POINT:RATE to derive analog shapes.
    updateHeader();  // protected in ezc3d::c3d; accessible from this derived class.
  }

  void normalize_point_frames(std::size_t effective_n_frames) {
    // Match ezc3d read-file flow:
    // - Data may imply a different frame count than the preloaded params/header.
    // - updateParameters() will sync POINT:FRAMES to data().nbFrames(), then updateHeader().
    //
    // sqzc3d does not build ezc3d::Data in streaming/materialize mode, so we normalize the same
    // field using our effective frame count derived from header+file size.
    if (!_parameters) return;
    if (!parameters().isGroup("POINT")) return;
    auto& grp_point = _parameters->group(parameters().groupIdx("POINT"));
    if (!grp_point.isParameter("FRAMES")) return;

    const auto frames = grp_point.parameter("FRAMES").valuesConvertedAsInt();
    const int current = frames.empty() ? 0 : frames[0];
    const int desired = static_cast<int>(effective_n_frames);
    if (current == desired) return;

    grp_point.parameter("FRAMES").set(static_cast<std::size_t>(effective_n_frames));
    updateHeader();
  }
};

static std::string sqzc3d_debug_hex_at(
    std::fstream& file,
    std::int64_t offset,
    std::size_t nbytes) {
  if (offset < 0 || nbytes == 0) return std::string();
  const auto saved = file.tellg();
  file.clear();
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file.good()) {
    file.clear();
    if (saved != std::streampos(-1)) file.seekg(saved);
    return std::string();
  }

  std::vector<unsigned char> buf;
  buf.resize(nbytes);
  file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
  const auto nread = static_cast<std::size_t>(file.gcount());

  std::string out;
  out.reserve(nread * 2);
  char tmp[3] = {0, 0, 0};
  for (std::size_t i = 0; i < nread; ++i) {
    std::snprintf(tmp, sizeof(tmp), "%02x", static_cast<unsigned int>(buf[i]));
    out.append(tmp);
  }

  file.clear();
  if (saved != std::streampos(-1)) file.seekg(saved);
  return out;
}

static std::string sqzc3d_debug_probe_params_layout(std::fstream& file) {
  const auto saved = file.tellg();

  std::int64_t zeros = 0;
  unsigned int paddr = 0;
  file.clear();
  file.seekg(0, std::ios::beg);
  for (;;) {
    char c = 0;
    file.read(&c, 1);
    if (!file.good()) break;
    paddr = static_cast<unsigned int>(static_cast<unsigned char>(c));
    if (paddr != 0u) break;
    ++zeros;
    if (zeros > 4096) break;
  }

  const std::int64_t off_a =
      static_cast<std::int64_t>(paddr > 0u ? (paddr - 1u) : 0u) * 512 + zeros;
  const std::int64_t off_b = static_cast<std::int64_t>(paddr) * 512 + zeros;

  const auto head0 = sqzc3d_debug_hex_at(file, 0, 16);
  const auto headz = (zeros > 0) ? sqzc3d_debug_hex_at(file, zeros, 16) : std::string();
  const auto par_a = sqzc3d_debug_hex_at(file, off_a, 16);
  const auto par_b = sqzc3d_debug_hex_at(file, off_b, 16);

  file.clear();
  if (saved != std::streampos(-1)) file.seekg(saved);

  char msg[512];
  std::snprintf(
      msg,
      sizeof(msg),
      " | probe:char_signed=%d paddr=%u zeros=%lld offA=%lld offB=%lld head0=%s headZ=%s parA=%s parB=%s",
      std::numeric_limits<char>::is_signed ? 1 : 0,
      paddr,
      static_cast<long long>(zeros),
      static_cast<long long>(off_a),
      static_cast<long long>(off_b),
      head0.c_str(),
      headz.c_str(),
      par_a.c_str(),
      par_b.c_str());
  return std::string(msg);
}

static bool is_finite_xyz(sqzc3d_num_t x, sqzc3d_num_t y, sqzc3d_num_t z) {
  return x == x && x != std::numeric_limits<sqzc3d_num_t>::infinity() &&
         x != -std::numeric_limits<sqzc3d_num_t>::infinity() &&
         y == y && y != std::numeric_limits<sqzc3d_num_t>::infinity() &&
         y != -std::numeric_limits<sqzc3d_num_t>::infinity() &&
         z == z && z != std::numeric_limits<sqzc3d_num_t>::infinity() &&
         z != -std::numeric_limits<sqzc3d_num_t>::infinity();
}

static void normalize_processor_type(ezc3d::PROCESSOR_TYPE& p) {
  if (p == ezc3d::PROCESSOR_TYPE::INTEL || p == ezc3d::PROCESSOR_TYPE::DEC ||
      p == ezc3d::PROCESSOR_TYPE::MIPS) {
    return;
  }
  p = ezc3d::PROCESSOR_TYPE::INTEL;
}

static std::string trim_label(const std::string& src) {
  auto begin = src.begin();
  auto end = src.end();
  while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
    ++begin;
  }
  while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
    --end;
  }
  return std::string(begin, end);
}

static std::string normalize_unit_token(const std::string& raw) {
  const auto trimmed = trim_label(raw);
  std::string out;
  out.reserve(trimmed.size());
  for (unsigned char ch : trimmed) {
    if (std::isspace(ch)) continue;
    out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

static std::vector<std::string> collect_contiguous_char_parameter(
    const ezc3d::ParametersNS::GroupNS::Group& g,
    const char* base) {
  std::vector<std::string> out;
  for (int i = 1;; ++i) {
    std::string name(base);
    if (i > 1) {
      name += std::to_string(i);
    }
    if (!g.isParameter(name)) break;
    const auto& p = g.parameter(name);
    if (p.type() == ezc3d::DATA_TYPE::CHAR) {
      const auto values = p.valuesAsString();
      out.insert(out.end(), values.begin(), values.end());
    }
  }
  return out;
}

static double units_per_meter_from_unit_token(const std::string& raw) {
  const auto u = normalize_unit_token(raw);
  if (u.empty()) return 0.0;
  if (u == "mm" || u == "millimeter" || u == "millimeters") return 1000.0;
  if (u == "cm" || u == "centimeter" || u == "centimeters") return 100.0;
  if (u == "m" || u == "meter" || u == "meters") return 1.0;
  if (u == "km" || u == "kilometer" || u == "kilometers") return 1e-3;
  return 0.0;
}

static std::string normalize_label(const char* s, int mode) {
  if (!s) return {};
  std::string out(s);
  if (mode & 2 /* sqzc3d_LABEL_NORM_TRIM */) {
    out = trim_label(out);
  }
  if (mode & 4 /* sqzc3d_LABEL_NORM_CASEFOLD_WS */) {
    std::string collapsed;
    collapsed.reserve(out.size());
    bool prev_ws = false;
    for (unsigned char ch : out) {
      if (std::isspace(ch)) {
        if (!prev_ws) collapsed.push_back(' ');
        prev_ws = true;
      } else {
        collapsed.push_back(static_cast<char>(std::tolower(ch)));
        prev_ws = false;
      }
    }
    while (!collapsed.empty() && collapsed.back() == ' ') {
      collapsed.pop_back();
    }
    out = std::move(collapsed);
  }
  return out;
}

static std::string normalize_label(const std::string& s, int mode) { return normalize_label(s.c_str(), mode); }

static std::size_t frame_start_offset(const sqzc3d::C3dStreamMeta& meta, int frame_idx) {
  const auto offset = static_cast<std::int64_t>(meta.data_start_bytes) +
                      static_cast<std::int64_t>(frame_idx) * static_cast<std::int64_t>(meta.frame_bytes);
  return static_cast<std::size_t>(std::max<std::int64_t>(0, offset));
}

static std::size_t point_record_offset(const sqzc3d::C3dStreamMeta& meta,
                                      int frame_idx,
                                      int point_idx) {
  return frame_start_offset(meta, frame_idx) +
         static_cast<std::size_t>(point_idx) * static_cast<std::size_t>(meta.point_record_bytes);
}

[[maybe_unused]] static std::size_t analog_record_offset(const sqzc3d::C3dStreamMeta& meta,
                                        int frame_idx,
                                        int sample_idx,
                                        int analog_idx) {
  const std::size_t frame_base = frame_start_offset(meta, frame_idx);
  const std::size_t points_bytes = static_cast<std::size_t>(meta.n_points) *
                                  static_cast<std::size_t>(meta.point_record_bytes);
  const std::size_t sample_stride =
      static_cast<std::size_t>(meta.n_analogs) * static_cast<std::size_t>(meta.analog_record_bytes);
  return frame_base + points_bytes + static_cast<std::size_t>(sample_idx) * sample_stride +
         static_cast<std::size_t>(analog_idx) * static_cast<std::size_t>(meta.analog_record_bytes);
}

static sqzc3d_status read_point_record(
    C3dStreamReader* reader,
    std::size_t offset,
    sqzc3d_num_t* out_xyz,
    sqzc3d_num_t* out_residual) {
  auto& file = reader->file;
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file.good()) return sqzc3d_STATUS_INVALID_ARGUMENT;

  const auto& meta = reader->meta;
  sqzc3d_num_t xyz[3] = {0, 0, 0};
  sqzc3d_num_t residual = 0;

  if (meta.point_record_scalar == 2) {
    xyz[0] = static_cast<sqzc3d_num_t>(reader->c3d->readFloat(meta.processor_type, file));
    xyz[1] = static_cast<sqzc3d_num_t>(reader->c3d->readFloat(meta.processor_type, file));
    xyz[2] = static_cast<sqzc3d_num_t>(reader->c3d->readFloat(meta.processor_type, file));
    if (meta.processor_type == ezc3d::PROCESSOR_TYPE::INTEL) {
      file.seekg(sizeof(std::uint16_t), std::ios::cur);
      residual = static_cast<sqzc3d_num_t>(
          static_cast<float>(reader->c3d->readInt(meta.processor_type, file, 2)) *
          static_cast<sqzc3d_num_t>(-meta.point_scale));
    } else if (meta.processor_type == ezc3d::PROCESSOR_TYPE::DEC) {
      residual = static_cast<sqzc3d_num_t>(
          static_cast<float>(reader->c3d->readInt(meta.processor_type, file, 2)) *
          static_cast<sqzc3d_num_t>(-meta.point_scale));
      file.seekg(sizeof(std::uint16_t), std::ios::cur);
    } else {
      return sqzc3d_STATUS_NOT_IMPLEMENTED;
    }
  } else {
    xyz[0] = static_cast<sqzc3d_num_t>(
                static_cast<float>(reader->c3d->readInt(meta.processor_type, file, 2)) *
                static_cast<sqzc3d_num_t>(meta.point_scale));
    xyz[1] = static_cast<sqzc3d_num_t>(
                static_cast<float>(reader->c3d->readInt(meta.processor_type, file, 2)) *
                static_cast<sqzc3d_num_t>(meta.point_scale));
    xyz[2] = static_cast<sqzc3d_num_t>(
                static_cast<float>(reader->c3d->readInt(meta.processor_type, file, 2)) *
                static_cast<sqzc3d_num_t>(meta.point_scale));
    file.seekg(sizeof(std::uint8_t), std::ios::cur);
    residual = static_cast<sqzc3d_num_t>(
                  static_cast<float>(reader->c3d->readInt(meta.processor_type, file, 1)) *
                  static_cast<sqzc3d_num_t>(meta.point_scale));
  }

  if (residual < 0) {
    const auto nan = std::numeric_limits<sqzc3d_num_t>::quiet_NaN();
    xyz[0] = nan;
    xyz[1] = nan;
    xyz[2] = nan;
  }

  const sqzc3d_num_t unit_scale = static_cast<sqzc3d_num_t>(meta.point_unit_scale);
  xyz[0] *= unit_scale;
  xyz[1] *= unit_scale;
  xyz[2] *= unit_scale;

  if (out_xyz) {
    out_xyz[0] = xyz[0];
    out_xyz[1] = xyz[1];
    out_xyz[2] = xyz[2];
  }
  if (out_residual) {
    *out_residual = residual;
  }
  return sqzc3d_STATUS_SUCCESS;
}

static inline std::uint16_t read_u16_le(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                    (static_cast<std::uint16_t>(p[1]) << 8));
}

static inline std::uint16_t read_u16_be(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                    static_cast<std::uint16_t>(p[1]));
}

static inline std::int16_t read_i16_le(const std::uint8_t* p) {
  return static_cast<std::int16_t>(read_u16_le(p));
}

static inline std::int16_t read_i16_be(const std::uint8_t* p) {
  return static_cast<std::int16_t>(read_u16_be(p));
}

static inline float read_f32_le(const std::uint8_t* p) {
  const std::uint32_t bits = static_cast<std::uint32_t>(p[0]) |
                             (static_cast<std::uint32_t>(p[1]) << 8) |
                             (static_cast<std::uint32_t>(p[2]) << 16) |
                             (static_cast<std::uint32_t>(p[3]) << 24);
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

static inline float read_f32_be(const std::uint8_t* p) {
  const std::uint32_t bits = static_cast<std::uint32_t>(p[3]) |
                             (static_cast<std::uint32_t>(p[2]) << 8) |
                             (static_cast<std::uint32_t>(p[1]) << 16) |
                             (static_cast<std::uint32_t>(p[0]) << 24);
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

static inline float read_f32_dec(const std::uint8_t* p) {
  // Match ezc3d's DEC float conversion:
  //   out[0]=in[2], out[1]=in[3], out[2]=in[0], out[3]=in[1]-1 (if nonzero)
  const std::uint8_t b0 = p[2];
  const std::uint8_t b1 = p[3];
  const std::uint8_t b2 = p[0];
  const std::uint8_t b3 = (p[1] != 0u) ? static_cast<std::uint8_t>(p[1] - 1u) : p[1];
  const std::uint32_t bits = static_cast<std::uint32_t>(b0) |
                             (static_cast<std::uint32_t>(b1) << 8) |
                             (static_cast<std::uint32_t>(b2) << 16) |
                             (static_cast<std::uint32_t>(b3) << 24);
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

static inline float read_f32_proc(ezc3d::PROCESSOR_TYPE p, const std::uint8_t* bytes) {
  if (p == ezc3d::PROCESSOR_TYPE::INTEL) return read_f32_le(bytes);
  if (p == ezc3d::PROCESSOR_TYPE::DEC) return read_f32_dec(bytes);
  if (p == ezc3d::PROCESSOR_TYPE::MIPS) return read_f32_be(bytes);
  return read_f32_le(bytes);
}

static inline std::int16_t read_i16_proc(ezc3d::PROCESSOR_TYPE p, const std::uint8_t* bytes) {
  if (p == ezc3d::PROCESSOR_TYPE::MIPS) return read_i16_be(bytes);
  return read_i16_le(bytes);
}

static inline void decode_point_record_intel(
    const C3dStreamMeta& meta,
    const std::uint8_t* rec,
    sqzc3d_num_t* out_xyz3,
    sqzc3d_num_t* out_residual) {
  const auto nan = std::numeric_limits<sqzc3d_num_t>::quiet_NaN();
  sqzc3d_num_t x = 0;
  sqzc3d_num_t y = 0;
  sqzc3d_num_t z = 0;
  sqzc3d_num_t residual = 0;

  if (meta.point_record_scalar == 2) {
    x = static_cast<sqzc3d_num_t>(read_f32_le(rec + 0u));
    y = static_cast<sqzc3d_num_t>(read_f32_le(rec + 4u));
    z = static_cast<sqzc3d_num_t>(read_f32_le(rec + 8u));
    const std::int16_t r_word = read_i16_le(rec + 14u);
    residual = static_cast<sqzc3d_num_t>(static_cast<int>(r_word)) * static_cast<sqzc3d_num_t>(-meta.point_scale);
  } else {
    const sqzc3d_num_t scale = static_cast<sqzc3d_num_t>(meta.point_scale);
    x = static_cast<sqzc3d_num_t>(static_cast<float>(read_i16_le(rec + 0u))) * scale;
    y = static_cast<sqzc3d_num_t>(static_cast<float>(read_i16_le(rec + 2u))) * scale;
    z = static_cast<sqzc3d_num_t>(static_cast<float>(read_i16_le(rec + 4u))) * scale;
    const auto r_byte = static_cast<std::int8_t>(rec[7u]);
    residual = static_cast<sqzc3d_num_t>(static_cast<int>(r_byte)) * scale;
  }

  if (residual < 0) {
    x = nan;
    y = nan;
    z = nan;
  }

  const sqzc3d_num_t unit_scale = static_cast<sqzc3d_num_t>(meta.point_unit_scale);
  x *= unit_scale;
  y *= unit_scale;
  z *= unit_scale;

  if (out_xyz3) {
    out_xyz3[0] = x;
    out_xyz3[1] = y;
    out_xyz3[2] = z;
  }
  if (out_residual) {
    *out_residual = residual;
  }
}

static inline void decode_point_record_dec(
    const C3dStreamMeta& meta,
    const std::uint8_t* rec,
    sqzc3d_num_t* out_xyz3,
    sqzc3d_num_t* out_residual) {
  const auto nan = std::numeric_limits<sqzc3d_num_t>::quiet_NaN();
  sqzc3d_num_t x = 0;
  sqzc3d_num_t y = 0;
  sqzc3d_num_t z = 0;
  sqzc3d_num_t residual = 0;

  if (meta.point_record_scalar == 2) {
    x = static_cast<sqzc3d_num_t>(read_f32_dec(rec + 0u));
    y = static_cast<sqzc3d_num_t>(read_f32_dec(rec + 4u));
    z = static_cast<sqzc3d_num_t>(read_f32_dec(rec + 8u));
    // DEC float record stores residual WORD then camera mask WORD.
    const std::int16_t r_word = read_i16_le(rec + 12u);
    residual = static_cast<sqzc3d_num_t>(static_cast<int>(r_word)) * static_cast<sqzc3d_num_t>(-meta.point_scale);
  } else {
    const sqzc3d_num_t scale = static_cast<sqzc3d_num_t>(meta.point_scale);
    x = static_cast<sqzc3d_num_t>(static_cast<float>(read_i16_le(rec + 0u))) * scale;
    y = static_cast<sqzc3d_num_t>(static_cast<float>(read_i16_le(rec + 2u))) * scale;
    z = static_cast<sqzc3d_num_t>(static_cast<float>(read_i16_le(rec + 4u))) * scale;
    const auto r_byte = static_cast<std::int8_t>(rec[7u]);
    residual = static_cast<sqzc3d_num_t>(static_cast<int>(r_byte)) * scale;
  }

  if (residual < 0) {
    x = nan;
    y = nan;
    z = nan;
  }

  const sqzc3d_num_t unit_scale = static_cast<sqzc3d_num_t>(meta.point_unit_scale);
  x *= unit_scale;
  y *= unit_scale;
  z *= unit_scale;

  if (out_xyz3) {
    out_xyz3[0] = x;
    out_xyz3[1] = y;
    out_xyz3[2] = z;
  }
  if (out_residual) {
    *out_residual = residual;
  }
}

[[maybe_unused]] static sqzc3d_status read_analog_record(
    C3dStreamReader* reader,
    std::size_t offset,
    int analog_idx,
    sqzc3d_num_t* out_value) {
  auto& file = reader->file;
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file.good()) return sqzc3d_STATUS_INVALID_ARGUMENT;

  const auto& meta = reader->meta;
  if (meta.analog_record_bytes == 4) {
    double scale = meta.analog_scale_default;
    if (analog_idx >= 0 && analog_idx < static_cast<int>(meta.analog_scales.size())) {
      scale = meta.analog_scales[static_cast<std::size_t>(analog_idx)];
    }
    double analog_offset = 0.0;
    if (analog_idx >= 0 && analog_idx < static_cast<int>(meta.analog_offsets.size())) {
      analog_offset = static_cast<double>(meta.analog_offsets[static_cast<std::size_t>(analog_idx)]);
    }
    *out_value = static_cast<sqzc3d_num_t>(
        (static_cast<float>(reader->c3d->readFloat(meta.processor_type, file)) - analog_offset) *
        scale * meta.analog_general_factor);
    return sqzc3d_STATUS_SUCCESS;
  }
  if (meta.analog_record_bytes != 2) {
    return sqzc3d_STATUS_NOT_IMPLEMENTED;
  }
  double scale = meta.analog_scale_default;
  if (analog_idx >= 0 && analog_idx < static_cast<int>(meta.analog_scales.size())) {
    scale = meta.analog_scales[static_cast<std::size_t>(analog_idx)];
  }
  const double analog_offset =
      (analog_idx >= 0 && analog_idx < static_cast<int>(meta.analog_offsets.size()))
          ? static_cast<double>(meta.analog_offsets[static_cast<std::size_t>(analog_idx)])
          : 0.0;
  *out_value = static_cast<sqzc3d_num_t>(
      (static_cast<float>(reader->c3d->readInt(meta.processor_type, file, 2)) - analog_offset) *
      static_cast<sqzc3d_num_t>(scale) * static_cast<sqzc3d_num_t>(meta.analog_general_factor));
  return sqzc3d_STATUS_SUCCESS;
}

static sqzc3d_status read_point_block(
    C3dStreamReader* reader,
    int frame_idx,
    const int* point_indices,
    int n_points_sel,
    sqzc3d_num_t* out_xyz,
    unsigned char* out_valid,
    int out_valid_nscalar,
    sqzc3d_num_t* out_residual,
    int out_residual_nscalar,
    bool read_residual) {
  const auto& meta = reader->meta;
  if (frame_idx < 0 || frame_idx >= meta.n_frames || n_points_sel <= 0 || !point_indices) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  if (out_valid && out_valid_nscalar != n_points_sel) {
    return sqzc3d_STATUS_DIMENSION_MISMATCH;
  }
  if (read_residual && out_residual_nscalar != n_points_sel) {
    return sqzc3d_STATUS_DIMENSION_MISMATCH;
  }

  if (meta.processor_type == ezc3d::PROCESSOR_TYPE::INTEL || meta.processor_type == ezc3d::PROCESSOR_TYPE::DEC) {
    int min_idx = std::numeric_limits<int>::max();
    int max_idx = std::numeric_limits<int>::min();
    for (int i = 0; i < n_points_sel; ++i) {
      const int idx = point_indices[i];
      if (idx < 0 || idx >= meta.n_points) return sqzc3d_STATUS_INVALID_ARGUMENT;
      min_idx = (idx < min_idx) ? idx : min_idx;
      max_idx = (idx > max_idx) ? idx : max_idx;
    }
    if (min_idx > max_idx) return sqzc3d_STATUS_INVALID_ARGUMENT;
    const std::size_t span_start = static_cast<std::size_t>(min_idx);
    const std::size_t span_points = static_cast<std::size_t>(max_idx - min_idx + 1);
    const std::size_t record_bytes = static_cast<std::size_t>(meta.point_record_bytes);
    const std::size_t span_bytes = span_points * record_bytes;
    if (span_bytes == 0u) return sqzc3d_STATUS_INVALID_ARGUMENT;

    const std::size_t frame_off = frame_start_offset(meta, frame_idx) + span_start * record_bytes;
    auto& file = reader->file;
    file.seekg(static_cast<std::streamoff>(frame_off), std::ios::beg);
    if (!file.good()) return sqzc3d_STATUS_INVALID_ARGUMENT;

    thread_local std::vector<std::uint8_t> buf;
    buf.resize(span_bytes);
    file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(span_bytes));
    if (!file.good()) return sqzc3d_STATUS_INVALID_ARGUMENT;

    for (int i = 0; i < n_points_sel; ++i) {
      const int idx = point_indices[i];
      const std::size_t rec_off = static_cast<std::size_t>(idx - min_idx) * record_bytes;
      sqzc3d_num_t xyz3[3];
      sqzc3d_num_t residual = 0;
      if (meta.processor_type == ezc3d::PROCESSOR_TYPE::INTEL) {
        decode_point_record_intel(meta, buf.data() + rec_off, out_xyz ? xyz3 : nullptr, &residual);
      } else {
        decode_point_record_dec(meta, buf.data() + rec_off, out_xyz ? xyz3 : nullptr, &residual);
      }
      if (out_xyz) {
        const std::size_t out_o = static_cast<std::size_t>(i) * 3u;
        out_xyz[out_o] = xyz3[0];
        out_xyz[out_o + 1u] = xyz3[1];
        out_xyz[out_o + 2u] = xyz3[2];
      }
      if (out_residual && read_residual) out_residual[static_cast<std::size_t>(i)] = residual;
      if (out_valid && out_xyz) out_valid[i] = is_finite_xyz(xyz3[0], xyz3[1], xyz3[2]) ? 1u : 0u;
      if (out_valid && !out_xyz) out_valid[i] = (residual >= 0) ? 1u : 0u;
    }
    return sqzc3d_STATUS_SUCCESS;
  }

  for (int i = 0; i < n_points_sel; ++i) {
    const int idx = point_indices[i];
    if (idx < 0 || idx >= meta.n_points) return sqzc3d_STATUS_INVALID_ARGUMENT;
    const std::size_t out_o = static_cast<std::size_t>(i) * 3u;
    const std::size_t off = point_record_offset(meta, frame_idx, idx);
    sqzc3d_num_t xyz[3];
    sqzc3d_num_t residual = 0;
    const auto st = read_point_record(reader, off, xyz, read_residual ? &residual : nullptr);
    if (st != sqzc3d_STATUS_SUCCESS) return st;
    if (out_xyz) {
      out_xyz[out_o] = xyz[0];
      out_xyz[out_o + 1u] = xyz[1];
      out_xyz[out_o + 2u] = xyz[2];
    }
    if (out_residual) out_residual[static_cast<std::size_t>(i)] = residual;
    if (out_valid) out_valid[i] = is_finite_xyz(xyz[0], xyz[1], xyz[2]) ? 1u : 0u;
  }
  return sqzc3d_STATUS_SUCCESS;
}

static void load_labels(const C3dStreamReader* source, C3dStreamReader* out) {
  const auto& params = source->c3d->parameters();

  if (params.isGroup("POINT")) {
    const auto& g = params.group("POINT");
    const auto point_labels = collect_contiguous_char_parameter(g, "LABELS");
    out->point_labels = point_labels;
  }

  if (params.isGroup("ANALOG")) {
    const auto& g = params.group("ANALOG");
    const auto analog_labels = collect_contiguous_char_parameter(g, "LABELS");
    out->analog_labels = analog_labels;
  }
}

static void load_point_type_groups(const C3dStreamReader* source, C3dStreamReader* out) {
  const auto& params = source->c3d->parameters();
  out->type_group_names.clear();
  out->type_group_starts.clear();
  out->type_group_indices.clear();
  out->type_group_starts.push_back(0);
  if (!params.isGroup("POINT")) return;

  const auto& group_point = params.group("POINT");
  if (!group_point.isParameter("TYPE_GROUPS")) return;
  const auto& type_groups_param = group_point.parameter("TYPE_GROUPS");
  if (type_groups_param.type() != ezc3d::DATA_TYPE::CHAR) return;

  const auto type_groups = type_groups_param.valuesAsString();
  if (type_groups.empty()) return;

  std::unordered_map<std::string, int> point_label_to_index;
  point_label_to_index.reserve(source->point_labels.size() * 2 + 1);
  for (int i = 0; i < static_cast<int>(source->point_labels.size()); ++i) {
    point_label_to_index[source->point_labels[i]] = i;
  }

  for (const auto& raw_name : type_groups) {
    const auto& gname = raw_name;
    if (gname.empty()) continue;
    std::vector<int> indices;
    if (!group_point.isParameter(gname.c_str())) {
      out->type_group_names.push_back(gname);
      out->type_group_starts.push_back(static_cast<int>(out->type_group_indices.size()));
      continue;
    }
    const auto& gparam = group_point.parameter(gname.c_str());
    if (gparam.type() == ezc3d::DATA_TYPE::CHAR) {
      const auto labels = gparam.valuesAsString();
      for (const auto& label : labels) {
        const auto it = point_label_to_index.find(label);
        if (it != point_label_to_index.end()) {
          indices.push_back(it->second);
        }
      }
    }
    out->type_group_names.push_back(gname);
    for (const int idx : indices) {
      out->type_group_indices.push_back(idx);
    }
    out->type_group_starts.push_back(static_cast<int>(out->type_group_indices.size()));
  }
}

}  // namespace

namespace sqzc3d {

sqzc3d_status sqzc3d_c3d_stream_open_file(
    C3dStreamReader* reader,
    const char* file_path) {
  if (!reader || !file_path) return sqzc3d_STATUS_INVALID_ARGUMENT;
  reader->file.close();
  reader->c3d.reset();
  reader->point_labels.clear();
  reader->analog_labels.clear();
  reader->type_group_names.clear();
  reader->type_group_starts.clear();
  reader->type_group_indices.clear();

  std::unique_ptr<C3dHeaderOnly> c3d;
  const char* phase = "init";

  try {
    phase = "open_stream";
    const auto fs_path = std::filesystem::u8path(file_path);
    reader->file.open(fs_path, std::ios::in | std::ios::binary);
    if (!reader->file.is_open()) {
      fprintf(stderr, "[sqzc3d] ERROR: failed to open C3D file: %s\n", file_path);
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
    reader->file.seekg(0, std::ios::beg);

    phase = "header_only";
    c3d = std::make_unique<C3dHeaderOnly>();
    try {
      c3d->load_header_only(reader->file, file_path);
    } catch (const std::exception& e) {
      std::string probe;
      try {
        probe = sqzc3d_debug_probe_params_layout(reader->file);
      } catch (...) {
        probe.clear();
      }
      throw std::runtime_error(
          std::string("sqzc3d_c3d_stream_open_file: header-only parse failed: ") + e.what() +
          probe);
    }
    reader->file.clear();
    reader->file.seekg(0, std::ios::beg);

    phase = "check_point";
    if (!c3d->parameters().isGroup("POINT")) {
      fprintf(stderr,
              "[sqzc3d] WARNING: C3D file has no POINT group: %s\n",
              file_path);
    }

    C3dStreamMeta meta{};
    C3dStreamMeta tmp{};
    bool has_point_units_param = false;
    bool has_shadow_group = false;
    std::string point_units_token;
    phase = "meta";
    try {
      tmp.processor_type = c3d->parameters().processorType();
      normalize_processor_type(tmp.processor_type);
      tmp.header_scale = c3d->header().scaleFactor();
      tmp.point_scale = static_cast<double>(tmp.header_scale);
      int analog_record_bytes = (tmp.header_scale < 0.0) ? 4 : 2;
      has_shadow_group = c3d->parameters().isGroup("SHADOW");

      if (c3d->parameters().isGroup("POINT")) {
        const auto& g = c3d->parameters().group("POINT");
        if (g.isParameter("SCALE")) {
          const auto& v = g.parameter("SCALE").valuesAsDouble();
          if (!v.empty()) tmp.point_scale = v[0];
        }
        const auto units_values = collect_contiguous_char_parameter(g, "UNITS");
        if (!units_values.empty()) {
          has_point_units_param = true;
          point_units_token = units_values[0];
          tmp.point_units_per_meter = units_per_meter_from_unit_token(point_units_token);
        }
      }

      if (c3d->parameters().isGroup("ANALOG")) {
        const auto& g = c3d->parameters().group("ANALOG");
        if (g.isParameter("SCALE")) {
          const auto& v = g.parameter("SCALE").valuesAsDouble();
          if (!v.empty()) tmp.analog_scale = v[0];
          if (!v.empty()) tmp.analog_scales.assign(v.begin(), v.end());
        }
        if (g.isParameter("GEN_SCALE")) {
          const auto& gv = g.parameter("GEN_SCALE").valuesAsDouble();
          if (!gv.empty() && std::isfinite(gv[0])) {
            tmp.analog_general_factor = gv[0];
          }
        }
        const auto& c3d_scale = c3d->channelScales();
        if (!c3d_scale.empty()) tmp.analog_scales.assign(c3d_scale.begin(), c3d_scale.end());
        const auto& c3d_offsets = c3d->channelOffsets();
        if (!c3d_offsets.empty()) {
          tmp.analog_offsets.clear();
          tmp.analog_offsets.reserve(c3d_offsets.size());
          for (int offset : c3d_offsets) {
            tmp.analog_offsets.push_back(std::abs(offset));
          }
        }
      }
      tmp.analog_record_bytes = analog_record_bytes;
    } catch (const std::exception& e) {
      throw std::runtime_error(
          std::string("sqzc3d_c3d_stream_open_file: meta parse failed: ") + e.what());
    }

    // By default, expose point coordinates in the raw C3D units (no scaling).
    if (!(tmp.point_units_per_meter > 0.0) || !std::isfinite(tmp.point_units_per_meter)) {
      // C3D files typically use mm; treat missing/unknown units as mm for robustness.
      tmp.point_units_per_meter = 1000.0;
      tmp.point_units_source = has_point_units_param ? 2 : 1;
      if (tmp.point_units_source == 1) {
        fprintf(stderr, "[sqzc3d] WARNING: C3D POINT:UNITS missing; assuming mm for xyz.\n");
      } else {
        const auto token_norm = normalize_unit_token(point_units_token);
        if (!token_norm.empty()) {
          fprintf(stderr,
                  "[sqzc3d] WARNING: C3D POINT:UNITS unknown (\"%s\"); assuming mm for xyz.\n",
                  token_norm.c_str());
        } else {
          fprintf(stderr, "[sqzc3d] WARNING: C3D POINT:UNITS unknown; assuming mm for xyz.\n");
        }
      }
    } else {
      tmp.point_units_source = 0;
    }
    tmp.target_units_per_meter = tmp.point_units_per_meter;
    tmp.point_unit_scale = 1.0;

    tmp.n_points = static_cast<int>(c3d->header().nb3dPoints());
    tmp.n_analogs = static_cast<int>(c3d->header().nbAnalogs());
    tmp.n_analog_by_frame = static_cast<int>(c3d->header().nbAnalogByFrame());
    if (tmp.n_analogs > 0 && has_shadow_group) {
      if (tmp.analog_scales.empty()) {
        tmp.analog_scales.assign(tmp.n_analogs, 1.0);
      }
      if (tmp.analog_offsets.empty()) {
        tmp.analog_offsets.assign(tmp.n_analogs, 0);
      }
    }
    tmp.analog_scale_default = tmp.analog_scale;
    if (tmp.analog_scales.empty()) {
      tmp.analog_scales.assign(std::max(tmp.n_analogs, 1), tmp.analog_scale_default);
    } else if (static_cast<int>(tmp.analog_scales.size()) > tmp.n_analogs) {
      tmp.analog_scales.resize(static_cast<std::size_t>(tmp.n_analogs));
    } else if (static_cast<int>(tmp.analog_scales.size()) < tmp.n_analogs && tmp.n_analogs > 0) {
      tmp.analog_scales.resize(static_cast<std::size_t>(tmp.n_analogs), tmp.analog_scale_default);
    }
    if (tmp.analog_offsets.empty()) {
      tmp.analog_offsets.assign(std::max(tmp.n_analogs, 0), 0);
    } else if (static_cast<int>(tmp.analog_offsets.size()) > tmp.n_analogs) {
      tmp.analog_offsets.resize(static_cast<std::size_t>(tmp.n_analogs));
    } else if (static_cast<int>(tmp.analog_offsets.size()) < tmp.n_analogs && tmp.n_analogs > 0) {
      tmp.analog_offsets.resize(static_cast<std::size_t>(tmp.n_analogs), 0);
    }
    if (!std::isfinite(tmp.analog_general_factor)) {
      tmp.analog_general_factor = 1.0;
    }
    tmp.point_record_scalar = (tmp.point_scale < 0.0) ? 2 : 1;
    tmp.point_record_bytes = (tmp.point_record_scalar == 2) ? 16 : 8;
    tmp.data_start_bytes = static_cast<std::int64_t>(c3d->header().dataStart() - 1) * 512;

    const auto points_bytes = static_cast<std::int64_t>(tmp.n_points) * static_cast<std::int64_t>(tmp.point_record_bytes);
    const auto analog_bytes = static_cast<std::int64_t>(tmp.n_analog_by_frame) *
                             static_cast<std::int64_t>(tmp.n_analogs) *
                             static_cast<std::int64_t>(tmp.analog_record_bytes);
    const auto frame_bytes = points_bytes + analog_bytes;
    if (points_bytes < 0 || analog_bytes < 0 || frame_bytes < 0 ||
        frame_bytes > std::numeric_limits<int>::max()) {
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
    tmp.frame_bytes = static_cast<int>(frame_bytes);

    // Prefer frame count from header first/last indices.
    // Special case: nbFrames==0xFFFF without rotational data means "read until EOF".
    const auto has_rotational = c3d->header().hasRotationalData();
    const bool all_of_file_frames =
        (c3d->header().nbFrames() == 0xFFFF && !has_rotational);
    int frames = 0;
    const auto header_first_frame = c3d->header().firstFrame();
    {
      const auto last_frame = c3d->header().lastFrame();
      std::size_t header_frames = 0;
      if (!all_of_file_frames) {
        if (last_frame > header_first_frame || (header_first_frame == 0 && last_frame == 0)) {
          header_frames = last_frame - header_first_frame + 1;
        } else if (last_frame >= header_first_frame) {
          header_frames = last_frame - header_first_frame + 1;
        }
        if (header_frames == 0) {
          header_frames = c3d->header().nbFrames();
        }
      } else {
        // Keep this as a plausible initial value; it will be replaced
        // by file-size-based frame count below.
        header_frames = last_frame - header_first_frame + 1;
      }
      if (header_frames > std::numeric_limits<int>::max()) {
        return sqzc3d_STATUS_INVALID_ARGUMENT;
      }
      frames = static_cast<int>(header_frames);
    }

    std::int64_t file_bytes = -1;
    {
      std::error_code ec;
      const auto file_size = std::filesystem::file_size(fs_path, ec);
      if (!ec) {
        file_bytes = static_cast<std::int64_t>(static_cast<std::uintmax_t>(file_size));
      }
    }
    if (file_bytes < 0) {
      reader->file.clear();
      reader->file.seekg(0, std::ios::end);
      if (reader->file.good()) {
        const auto end_pos = reader->file.tellg();
        if (end_pos >= 0) file_bytes = static_cast<std::int64_t>(end_pos);
      }
    }
    reader->file.clear();
    reader->file.seekg(0, std::ios::beg);

    if (file_bytes > tmp.data_start_bytes && tmp.frame_bytes > 0) {
      const std::int64_t data_bytes = file_bytes - tmp.data_start_bytes;
      const std::int64_t frames_by_size = data_bytes / static_cast<std::int64_t>(tmp.frame_bytes);
      if (frames_by_size >= 0 && frames_by_size <= std::numeric_limits<int>::max()) {
        if (!all_of_file_frames) {
          if (frames_by_size < static_cast<std::int64_t>(frames)) {
            frames = static_cast<int>(frames_by_size);
          }
        } else {
          frames = static_cast<int>(frames_by_size);
        }
      }
    } else {
      frames = 0;
    }
    if (frames < 0) {
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
    tmp.n_frames = frames;
    if (header_first_frame > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
    tmp.first_frame = static_cast<int>(header_first_frame);
    if (tmp.n_frames <= 0) {
      tmp.last_frame = tmp.first_frame - 1;
    } else {
      const auto last = static_cast<long long>(tmp.first_frame) + static_cast<long long>(tmp.n_frames) - 1LL;
      if (last < std::numeric_limits<int>::min() || last > std::numeric_limits<int>::max()) {
        return sqzc3d_STATUS_INVALID_ARGUMENT;
      }
      tmp.last_frame = static_cast<int>(last);
    }

    {
      const double rate = static_cast<double>(c3d->header().frameRate());
      if (std::isfinite(rate) && rate > 0.0) {
        tmp.point_rate_hz = rate;
        const double a_rate = rate * static_cast<double>(tmp.n_analog_by_frame);
        tmp.analog_rate_hz = (std::isfinite(a_rate) && a_rate >= 0.0) ? a_rate : 0.0;
      } else {
        tmp.point_rate_hz = 0.0;
        tmp.analog_rate_hz = 0.0;
      }
    }

    // Normalize POINT:FRAMES and re-run updateHeader() to keep header/params consistent with
    // our effective frame count (closest equivalent to ezc3d::c3d::updateParameters()).
    c3d->normalize_point_frames(static_cast<std::size_t>(tmp.n_frames));

    phase = "labels";
    reader->c3d = std::move(c3d);
    reader->meta = tmp;
    try {
      load_labels(reader, reader);
      load_point_type_groups(reader, reader);
    } catch (const std::exception& e) {
      throw std::runtime_error(
          std::string("sqzc3d_c3d_stream_open_file: label/type-groups parse failed: ") +
          e.what());
    }
    return sqzc3d_STATUS_SUCCESS;
  } catch (const std::bad_alloc&) {
    reader->file.close();
    reader->c3d.reset();
    return sqzc3d_STATUS_INTERNAL_ERROR;
  } catch (const std::exception& e) {
    const int char_signed = std::numeric_limits<char>::is_signed ? 1 : 0;
    fprintf(stderr,
            "[sqzc3d] ERROR: exception while opening C3D (%s) [phase=%s] char_signed=%d: %s\n",
            file_path,
            phase ? phase : "unknown",
            char_signed,
            e.what());
    if (c3d) {
      try {
        const auto& params = c3d->parameters();
        fprintf(stderr, "[sqzc3d] DEBUG: param groups (%zu):", params.nbGroups());
        for (std::size_t i = 0; i < params.nbGroups(); ++i) {
          fprintf(stderr, " %s", params.group(i).name().c_str());
        }
        fprintf(stderr, "\n");
      } catch (...) {
        // Best-effort debug only.
      }
    }
    reader->file.close();
    reader->c3d.reset();
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  } catch (...) {
    reader->file.close();
    reader->c3d.reset();
    return sqzc3d_STATUS_INTERNAL_ERROR;
  }
}

sqzc3d_status sqzc3d_c3d_stream_set_target_unit(
    C3dStreamReader* reader,
    const char* unit_token) {
  if (!reader || !reader->c3d || !unit_token) return sqzc3d_STATUS_INVALID_ARGUMENT;
  const double upm = units_per_meter_from_unit_token(unit_token);
  if (!(upm > 0.0) || !std::isfinite(upm)) return sqzc3d_STATUS_INVALID_ARGUMENT;
  return sqzc3d_c3d_stream_set_target_units_per_meter(reader, upm);
}

sqzc3d_status sqzc3d_c3d_stream_set_target_units_per_meter(
    C3dStreamReader* reader,
    double units_per_meter) {
  if (!reader || !reader->c3d) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (!(units_per_meter > 0.0) || !std::isfinite(units_per_meter)) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  auto& meta = reader->meta;
  meta.target_units_per_meter = units_per_meter;
  meta.point_unit_scale = meta.target_units_per_meter / meta.point_units_per_meter;
  return sqzc3d_STATUS_SUCCESS;
}

void sqzc3d_c3d_stream_close(C3dStreamReader* reader) {
  if (!reader) return;
  if (reader->file.is_open()) reader->file.close();
  reader->c3d.reset();
  reader->point_labels.clear();
  reader->analog_labels.clear();
  reader->type_group_names.clear();
  reader->type_group_starts.clear();
  reader->type_group_indices.clear();
}

sqzc3d_status sqzc3d_c3d_stream_point_indices_for_labels(
    const C3dStreamReader* reader,
    const char* const* labels,
    int n_labels,
    int* out_indices,
    int miss_idx,
    int norm_mode) {
  if (!reader || !reader->c3d) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (n_labels < 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (n_labels > 0 && (!labels || !out_indices)) return sqzc3d_STATUS_INVALID_ARGUMENT;

  for (int i = 0; i < n_labels; ++i) out_indices[i] = miss_idx;

  const auto& haystack = reader->point_labels;
  for (int i = 0; i < n_labels; ++i) {
    const char* want = labels[i];
    if (!want) return sqzc3d_STATUS_INVALID_ARGUMENT;
    const auto target = normalize_label(want, norm_mode);
    int match_index = -1;
    int match_count = 0;
    for (int j = 0; j < static_cast<int>(haystack.size()); ++j) {
      if (normalize_label(haystack[static_cast<std::size_t>(j)], norm_mode) == target) {
        if (match_count == 0) {
          match_index = j;
        }
        ++match_count;
        if (match_count > 1) break;
      }
    }
    if (match_count == 1) {
      out_indices[i] = match_index;
    } else if (match_count > 1) {
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
  }
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_xyz_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* point_indices,
    int n_points_sel,
    sqzc3d_num_t* out_target,
    int out_nscalar,
    unsigned char* out_valid,
    int out_valid_nscalar) {
  if (!reader || !reader->c3d || !out_target || !point_indices) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  if (out_nscalar != n_points_sel * 3) return sqzc3d_STATUS_INVALID_ARGUMENT;
  return read_point_block(
      reader,
      frame_idx,
      point_indices,
      n_points_sel,
      out_target,
      out_valid,
      out_valid_nscalar,
      nullptr,
      0,
      false);
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_residual_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* point_indices,
    int n_points_sel,
    sqzc3d_num_t* out_residual,
    int out_nscalar) {
  if (!reader || !reader->c3d || !out_residual || !point_indices || n_points_sel <= 0) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  return read_point_block(
      reader,
      frame_idx,
      point_indices,
      n_points_sel,
      nullptr,
      nullptr,
      0,
      out_residual,
      out_nscalar,
      true);
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_all_xyz(
    C3dStreamReader* reader,
    int frame_idx,
    sqzc3d_num_t* out_target,
    int out_nscalar) {
  if (!reader || !reader->c3d || !out_target) return sqzc3d_STATUS_INVALID_ARGUMENT;
  const int n_points = reader->meta.n_points;
  if (n_points <= 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (out_nscalar != n_points * 3) return sqzc3d_STATUS_INVALID_ARGUMENT;

  // Avoid per-call allocations in the hot path (sqzc3d_build_chunks dense reads).
  thread_local std::vector<int> all;
  if (static_cast<int>(all.size()) != n_points) {
    all.resize(static_cast<std::size_t>(n_points));
    for (int i = 0; i < n_points; ++i) all[static_cast<std::size_t>(i)] = i;
  }
  return read_point_block(
      reader,
      frame_idx,
      all.data(),
      n_points,
      out_target,
      nullptr,
      0,
      nullptr,
      0,
      false);
}

sqzc3d_status sqzc3d_c3d_stream_read_traj_xyz_sel(
    C3dStreamReader* reader,
    const int* point_indices,
    int n_points_sel,
    int start_frame,
    int end_frame,
    sqzc3d_num_t* out_traj,
    int out_nscalar,
    unsigned char* out_valid,
    int out_valid_nscalar) {
  if (!reader || !reader->c3d || !point_indices || !out_traj || n_points_sel <= 0) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  const auto& meta = reader->meta;
  int start = start_frame;
  int end = end_frame;
  if (start < 0) start += meta.n_frames;
  if (end <= 0) {
    if (end < 0) end += meta.n_frames;
    else end = meta.n_frames;
  }
  if (start < 0) start = 0;
  if (end > meta.n_frames) end = meta.n_frames;
  if (start >= end) return sqzc3d_STATUS_INVALID_ARGUMENT;

  const int span = end - start;
  const int expected = span * n_points_sel * 3;
  if (out_nscalar != expected) return sqzc3d_STATUS_DIMENSION_MISMATCH;
  if (out_valid && out_valid_nscalar != span * n_points_sel) {
    return sqzc3d_STATUS_DIMENSION_MISMATCH;
  }

  const int per_frame = n_points_sel * 3;
  for (int t = 0; t < span; ++t) {
    const int f = start + t;
    sqzc3d_num_t* row = out_traj + static_cast<std::size_t>(t) * static_cast<std::size_t>(per_frame);
    unsigned char* row_valid = out_valid ? out_valid + static_cast<std::size_t>(t) * n_points_sel : nullptr;
    const auto st = read_point_block(
        reader,
        f,
        point_indices,
        n_points_sel,
        row,
        row_valid,
        n_points_sel,
        nullptr,
        0,
        false);
    if (st != sqzc3d_STATUS_SUCCESS) return st;
  }
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_analogs_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* analog_indices,
    int n_analog_sel,
    int start_sample,
    int n_samples,
    sqzc3d_num_t* out_analog,
    int out_nscalar) {
  if (!reader || !reader->c3d || !analog_indices || !out_analog || n_analog_sel <= 0 ||
      n_samples < 0 || start_sample < 0) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  const auto& meta = reader->meta;
  if (frame_idx < 0 || frame_idx >= meta.n_frames) return sqzc3d_STATUS_INVALID_ARGUMENT;

  for (int i = 0; i < n_analog_sel; ++i) {
    if (analog_indices[i] < 0 || analog_indices[i] >= meta.n_analogs) {
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
  }

  const int end_sample = start_sample + n_samples;
  if (start_sample >= meta.n_analog_by_frame || end_sample > meta.n_analog_by_frame) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  if (out_nscalar != n_samples * n_analog_sel) return sqzc3d_STATUS_INVALID_ARGUMENT;

  const std::size_t frame_base = frame_start_offset(meta, frame_idx);
  const std::size_t points_bytes = static_cast<std::size_t>(meta.n_points) *
                                   static_cast<std::size_t>(meta.point_record_bytes);
  const std::size_t sample_stride =
      static_cast<std::size_t>(meta.n_analogs) * static_cast<std::size_t>(meta.analog_record_bytes);
  const std::size_t start_off =
      frame_base + points_bytes + static_cast<std::size_t>(start_sample) * sample_stride;
  const std::size_t bytes_to_read = static_cast<std::size_t>(n_samples) * sample_stride;
  if (bytes_to_read == 0u) return sqzc3d_STATUS_SUCCESS;

  auto& file = reader->file;
  file.seekg(static_cast<std::streamoff>(start_off), std::ios::beg);
  if (!file.good()) return sqzc3d_STATUS_INVALID_ARGUMENT;

  thread_local std::vector<std::uint8_t> buf;
  buf.resize(bytes_to_read);
  file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(bytes_to_read));
  if (!file.good()) return sqzc3d_STATUS_INVALID_ARGUMENT;

  for (int s = 0; s < n_samples; ++s) {
    const std::size_t sample_off = static_cast<std::size_t>(s) * sample_stride;
    for (int c = 0; c < n_analog_sel; ++c) {
      const int idx = analog_indices[c];
      const std::size_t rec_off =
          sample_off + static_cast<std::size_t>(idx) * static_cast<std::size_t>(meta.analog_record_bytes);
      const std::size_t out_o =
          static_cast<std::size_t>(s) * static_cast<std::size_t>(n_analog_sel) +
          static_cast<std::size_t>(c);

      double scale = meta.analog_scale_default;
      if (idx >= 0 && idx < static_cast<int>(meta.analog_scales.size())) {
        scale = meta.analog_scales[static_cast<std::size_t>(idx)];
      }
      double analog_offset = 0.0;
      if (idx >= 0 && idx < static_cast<int>(meta.analog_offsets.size())) {
        analog_offset = static_cast<double>(meta.analog_offsets[static_cast<std::size_t>(idx)]);
      }

      if (meta.analog_record_bytes == 4) {
        const float raw = read_f32_proc(meta.processor_type, buf.data() + rec_off);
        out_analog[out_o] = static_cast<sqzc3d_num_t>(
            (static_cast<double>(raw) - analog_offset) * scale * meta.analog_general_factor);
      } else if (meta.analog_record_bytes == 2) {
        const int raw = static_cast<int>(read_i16_proc(meta.processor_type, buf.data() + rec_off));
        out_analog[out_o] = static_cast<sqzc3d_num_t>(
            (static_cast<double>(raw) - analog_offset) * scale * meta.analog_general_factor);
      } else {
        return sqzc3d_STATUS_NOT_IMPLEMENTED;
      }
    }
  }
  return sqzc3d_STATUS_SUCCESS;
}

}  // namespace sqzc3d
#else

namespace sqzc3d {

sqzc3d_status sqzc3d_c3d_stream_open_file(
    C3dStreamReader* reader,
    const char* file_path) {
  (void)reader;
  (void)file_path;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

void sqzc3d_c3d_stream_close(C3dStreamReader* reader) {
  if (reader) {
    reader->file.close();
    reader->c3d.reset();
    reader->point_labels.clear();
    reader->analog_labels.clear();
    reader->type_group_names.clear();
    reader->type_group_starts.clear();
    reader->type_group_indices.clear();
  }
}

sqzc3d_status sqzc3d_c3d_stream_set_target_unit(
    C3dStreamReader* reader,
    const char* unit_token) {
  (void)reader;
  (void)unit_token;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

sqzc3d_status sqzc3d_c3d_stream_set_target_units_per_meter(
    C3dStreamReader* reader,
    double units_per_meter) {
  (void)reader;
  (void)units_per_meter;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

sqzc3d_status sqzc3d_c3d_stream_point_indices_for_labels(
    const C3dStreamReader* reader,
    const char* const* labels,
    int n_labels,
    int* out_indices,
    int miss_idx,
    int norm_mode) {
  (void)reader;
  (void)labels;
  (void)n_labels;
  (void)out_indices;
  (void)miss_idx;
  (void)norm_mode;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_xyz_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* point_indices,
    int n_points_sel,
    sqzc3d_num_t* out_target,
    int out_nscalar,
    unsigned char* out_valid,
    int out_valid_nscalar) {
  (void)reader;
  (void)frame_idx;
  (void)point_indices;
  (void)n_points_sel;
  (void)out_target;
  (void)out_nscalar;
  (void)out_valid;
  (void)out_valid_nscalar;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_residual_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* point_indices,
    int n_points_sel,
    sqzc3d_num_t* out_residual,
    int out_nscalar) {
  (void)reader;
  (void)frame_idx;
  (void)point_indices;
  (void)n_points_sel;
  (void)out_residual;
  (void)out_nscalar;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_all_xyz(
    C3dStreamReader* reader,
    int frame_idx,
    sqzc3d_num_t* out_target,
    int out_nscalar) {
  (void)reader;
  (void)frame_idx;
  (void)out_target;
  (void)out_nscalar;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

sqzc3d_status sqzc3d_c3d_stream_read_traj_xyz_sel(
    C3dStreamReader* reader,
    const int* point_indices,
    int n_points_sel,
    int start_frame,
    int end_frame,
    sqzc3d_num_t* out_traj,
    int out_nscalar,
    unsigned char* out_valid,
    int out_valid_nscalar) {
  (void)reader;
  (void)point_indices;
  (void)n_points_sel;
  (void)start_frame;
  (void)end_frame;
  (void)out_traj;
  (void)out_nscalar;
  (void)out_valid;
  (void)out_valid_nscalar;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_analogs_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* analog_indices,
    int n_analog_sel,
    int start_sample,
    int n_samples,
    sqzc3d_num_t* out_analog,
    int out_nscalar) {
  (void)reader;
  (void)frame_idx;
  (void)analog_indices;
  (void)n_analog_sel;
  (void)start_sample;
  (void)n_samples;
  (void)out_analog;
  (void)out_nscalar;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

}  // namespace sqzc3d
#endif

