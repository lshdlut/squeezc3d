#include "sqzc3d_c3d_stream.h"
#include "sqzc3d_error_internal.h"

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
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

using C3dStreamReader = sqzc3d::C3dStreamReader;
using sqzc3d_status = int;
#if sqzc3d_WITH_EZC3D

namespace sqzc3d {

struct C3dReadSource {
  virtual ~C3dReadSource() = default;
  virtual bool read_at(std::uint64_t offset, void* dst, std::size_t n_bytes) = 0;
  virtual std::int64_t size_bytes() const = 0;
};

struct C3dParameterValue {
  std::string name;
  std::string description;
  bool locked = false;
  int type = 10000;
  std::vector<std::size_t> dimensions;
  std::vector<std::string> string_values;
  std::vector<int> int_values;
  std::vector<double> double_values;
};

struct C3dParameterGroup {
  std::string name;
  std::string description;
  bool locked = false;
  std::vector<C3dParameterValue> parameters;

  const C3dParameterValue* find_parameter(const std::string& parameter_name) const {
    for (const auto& parameter : parameters) {
      if (parameter.name == parameter_name) return &parameter;
    }
    return nullptr;
  }
};

struct C3dParameterTree {
  ezc3d::PROCESSOR_TYPE processor_type = ezc3d::PROCESSOR_TYPE::INTEL;
  std::vector<C3dParameterGroup> groups;

  const C3dParameterGroup* find_group(const std::string& group_name) const {
    for (const auto& group : groups) {
      if (group.name == group_name) return &group;
    }
    return nullptr;
  }
};

C3dStreamReader::C3dStreamReader() = default;
C3dStreamReader::~C3dStreamReader() = default;

}  // namespace sqzc3d

namespace {

using C3dStreamMeta = sqzc3d::C3dStreamMeta;
using C3dParameterGroup = sqzc3d::C3dParameterGroup;
using C3dParameterTree = sqzc3d::C3dParameterTree;
using C3dParameterValue = sqzc3d::C3dParameterValue;

struct ApiCtx {
  const char* api = "";

  explicit ApiCtx(const char* in_api) : api(in_api ? in_api : "") {
    sqzc3d::internal::reset_global_error();
  }

  int fail(int status, const std::string& message, const char* section = nullptr, int index = -1) const {
    sqzc3d::internal::set_global_error(status, message, api, section, index);
    return status;
  }
};

class FileReadSource final : public sqzc3d::C3dReadSource {
 public:
  FileReadSource(std::ifstream stream, std::int64_t n_bytes)
      : stream_(std::move(stream)),
        n_bytes_(n_bytes) {}

  bool read_at(std::uint64_t offset, void* dst, std::size_t n_bytes) override {
    if (!dst && n_bytes > 0u) return false;
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) return false;
    if (n_bytes > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) return false;
    if (n_bytes_ >= 0) {
      const auto available = static_cast<std::uint64_t>(n_bytes_);
      if (offset > available || n_bytes > available - offset) return false;
    }
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_.good()) return false;
    stream_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n_bytes));
    return stream_.good();
  }

  std::int64_t size_bytes() const override { return n_bytes_; }

 private:
  std::ifstream stream_;
  std::int64_t n_bytes_ = -1;
};

class MemoryReadSource final : public sqzc3d::C3dReadSource {
 public:
  MemoryReadSource(const void* data, std::size_t n_bytes) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    storage_.assign(bytes, bytes + n_bytes);
  }

  bool read_at(std::uint64_t offset, void* dst, std::size_t n_bytes) override {
    if (!dst && n_bytes > 0u) return false;
    if (offset > static_cast<std::uint64_t>(storage_.size())) return false;
    const auto pos = static_cast<std::size_t>(offset);
    if (n_bytes > storage_.size() - pos) return false;
    if (n_bytes > 0u) {
      std::memcpy(dst, storage_.data() + pos, n_bytes);
    }
    return true;
  }

  std::int64_t size_bytes() const override {
    return static_cast<std::int64_t>(storage_.size());
  }

 private:
  std::vector<unsigned char> storage_;
};

struct C3dHeaderInfo {
  std::uint64_t zeros_before_header = 0;
  std::uint64_t parameters_address = 0;
  std::uint64_t parameter_start_offset = 0;
  std::uint64_t nb3d_points = 0;
  std::uint64_t nb_analog_measurements = 0;
  std::uint64_t first_frame = 0;
  std::uint64_t last_frame = 0;
  std::uint64_t data_start_blocks = 0;
  std::uint64_t nb_analog_by_frame = 0;
  float scale_factor = 1.0f;
  float frame_rate = 0.0f;
};

class SourceCursor {
 public:
  explicit SourceCursor(sqzc3d::C3dReadSource& source) : source_(source) {}

  std::uint64_t tell() const { return pos_; }
  void seek(std::uint64_t pos) { pos_ = pos; }

  void read_bytes(void* dst, std::size_t n_bytes) {
    if (!source_.read_at(pos_, dst, n_bytes)) {
      throw std::runtime_error("read failed");
    }
    pos_ += static_cast<std::uint64_t>(n_bytes);
  }

  std::uint8_t read_u8() {
    std::uint8_t value = 0;
    read_bytes(&value, 1u);
    return value;
  }

  int read_i8() { return static_cast<int>(static_cast<std::int8_t>(read_u8())); }

 private:
  sqzc3d::C3dReadSource& source_;
  std::uint64_t pos_ = 0;
};

static void reset_reader(C3dStreamReader* reader) {
  if (!reader) return;
  reader->source.reset();
  reader->parameter_tree.reset();
  reader->meta = {};
  reader->point_labels.clear();
  reader->analog_labels.clear();
  reader->type_group_names.clear();
  reader->type_group_starts.clear();
  reader->type_group_indices.clear();
  reader->read_scratch.clear();
}

static std::string sqzc3d_debug_hex_at(
    sqzc3d::C3dReadSource& source,
    std::int64_t offset,
    std::size_t nbytes) {
  if (offset < 0 || nbytes == 0) return std::string();

  std::vector<unsigned char> buf;
  buf.resize(nbytes);
  if (!source.read_at(static_cast<std::uint64_t>(offset), buf.data(), buf.size())) return std::string();
  const auto nread = buf.size();

  std::string out;
  out.reserve(nread * 2);
  char tmp[3] = {0, 0, 0};
  for (std::size_t i = 0; i < nread; ++i) {
    std::snprintf(tmp, sizeof(tmp), "%02x", static_cast<unsigned int>(buf[i]));
    out.append(tmp);
  }
  return out;
}

static std::string sqzc3d_debug_probe_params_layout(sqzc3d::C3dReadSource& source) {
  std::int64_t zeros = 0;
  unsigned int paddr = 0;
  for (;;) {
    unsigned char c = 0;
    if (!source.read_at(static_cast<std::uint64_t>(zeros), &c, 1u)) break;
    paddr = static_cast<unsigned int>(static_cast<unsigned char>(c));
    if (paddr != 0u) break;
    ++zeros;
    if (zeros > 4096) break;
  }

  const std::int64_t off_a =
      static_cast<std::int64_t>(paddr > 0u ? (paddr - 1u) : 0u) * 512 + zeros;
  const std::int64_t off_b = static_cast<std::int64_t>(paddr) * 512 + zeros;

  const auto head0 = sqzc3d_debug_hex_at(source, 0, 16);
  const auto headz = (zeros > 0) ? sqzc3d_debug_hex_at(source, zeros, 16) : std::string();
  const auto par_a = sqzc3d_debug_hex_at(source, off_a, 16);
  const auto par_b = sqzc3d_debug_hex_at(source, off_b, 16);

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

static bool is_supported_processor_type(ezc3d::PROCESSOR_TYPE p) {
  return p == ezc3d::PROCESSOR_TYPE::INTEL ||
         p == ezc3d::PROCESSOR_TYPE::DEC ||
         p == ezc3d::PROCESSOR_TYPE::MIPS;
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

static std::vector<int> parameter_values_as_int(const C3dParameterValue* p);
static std::vector<double> parameter_values_as_double(const C3dParameterValue* p);

static std::vector<std::string> collect_contiguous_char_parameter(
    const C3dParameterGroup& g,
    const char* base) {
  std::vector<std::string> out;
  for (int i = 1;; ++i) {
    std::string name(base);
    if (i > 1) {
      name += std::to_string(i);
    }
    const auto* p = g.find_parameter(name);
    if (!p) break;
    if (p->type == ezc3d::DATA_TYPE::CHAR) {
      out.insert(out.end(), p->string_values.begin(), p->string_values.end());
    }
  }
  return out;
}

static std::vector<double> collect_contiguous_double_parameter(
    const C3dParameterGroup& g,
    const char* base) {
  std::vector<double> out;
  for (int i = 1;; ++i) {
    std::string name(base);
    if (i > 1) {
      name += std::to_string(i);
    }
    const auto* p = g.find_parameter(name);
    if (!p) break;
    const auto values = parameter_values_as_double(p);
    out.insert(out.end(), values.begin(), values.end());
  }
  return out;
}

static std::vector<int> collect_contiguous_int_parameter(
    const C3dParameterGroup& g,
    const char* base) {
  std::vector<int> out;
  for (int i = 1;; ++i) {
    std::string name(base);
    if (i > 1) {
      name += std::to_string(i);
    }
    const auto* p = g.find_parameter(name);
    if (!p) break;
    const auto values = parameter_values_as_int(p);
    out.insert(out.end(), values.begin(), values.end());
  }
  return out;
}

static std::vector<int> parameter_values_as_int(const C3dParameterValue* p) {
  if (!p) return {};
  if (!p->int_values.empty()) return p->int_values;
  std::vector<int> out;
  out.reserve(p->double_values.size());
  for (double v : p->double_values) out.push_back(static_cast<int>(v));
  return out;
}

static std::vector<double> parameter_values_as_double(const C3dParameterValue* p) {
  if (!p) return {};
  if (!p->double_values.empty()) return p->double_values;
  std::vector<double> out;
  out.reserve(p->int_values.size());
  for (int v : p->int_values) out.push_back(static_cast<double>(v));
  return out;
}

static int parameter_first_int(const C3dParameterGroup* g, const char* name, int fallback) {
  if (!g) throw std::runtime_error("missing parameter group");
  if (!name) throw std::runtime_error("missing parameter name");
  const auto values = parameter_values_as_int(g->find_parameter(name));
  return values.empty() ? fallback : values[0];
}

static double parameter_first_double(const C3dParameterGroup* g, const char* name, double fallback) {
  if (!g) throw std::runtime_error("missing parameter group");
  if (!name) throw std::runtime_error("missing parameter name");
  const auto values = parameter_values_as_double(g->find_parameter(name));
  return values.empty() ? fallback : values[0];
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

static inline std::uint16_t read_u16_proc(ezc3d::PROCESSOR_TYPE p, const std::uint8_t* bytes) {
  if (p == ezc3d::PROCESSOR_TYPE::MIPS) return read_u16_be(bytes);
  return read_u16_le(bytes);
}

static bool source_read_u8(sqzc3d::C3dReadSource& source, std::uint64_t offset, std::uint8_t* out) {
  return source.read_at(offset, out, 1u);
}

static std::uint8_t require_u8(sqzc3d::C3dReadSource& source, std::uint64_t offset) {
  std::uint8_t value = 0;
  if (!source_read_u8(source, offset, &value)) {
    throw std::runtime_error("read_u8 failed");
  }
  return value;
}

static std::uint16_t require_u16(
    sqzc3d::C3dReadSource& source,
    std::uint64_t offset,
    ezc3d::PROCESSOR_TYPE processor_type) {
  std::uint8_t bytes[2] = {0, 0};
  if (!source.read_at(offset, bytes, sizeof(bytes))) {
    throw std::runtime_error("read_u16 failed");
  }
  return read_u16_proc(processor_type, bytes);
}

static float require_f32(
    sqzc3d::C3dReadSource& source,
    std::uint64_t offset,
    ezc3d::PROCESSOR_TYPE processor_type) {
  std::uint8_t bytes[4] = {0, 0, 0, 0};
  if (!source.read_at(offset, bytes, sizeof(bytes))) {
    throw std::runtime_error("read_f32 failed");
  }
  return read_f32_proc(processor_type, bytes);
}

static ezc3d::PROCESSOR_TYPE processor_type_from_id(std::uint8_t processor_id) {
  if (processor_id == 84u) return ezc3d::PROCESSOR_TYPE::INTEL;
  if (processor_id == 85u) return ezc3d::PROCESSOR_TYPE::DEC;
  if (processor_id == 86u) return ezc3d::PROCESSOR_TYPE::MIPS;
  throw std::runtime_error("Could not read the processor type");
}

static C3dHeaderInfo parse_header_info(sqzc3d::C3dReadSource& source) {
  C3dHeaderInfo out{};
  for (;;) {
    const std::uint8_t value = require_u8(source, out.zeros_before_header);
    if (value != 0u) {
      out.parameters_address = value;
      break;
    }
    ++out.zeros_before_header;
    if (out.zeros_before_header > 4096u) {
      throw std::runtime_error("invalid C3D header: too many leading zero bytes");
    }
  }
  const auto checksum = require_u8(source, out.zeros_before_header + 1u);
  if (checksum != 0x50u) {
    throw std::ios_base::failure("File must be a valid c3d file");
  }
  if (out.parameters_address == 0u) {
    throw std::runtime_error("invalid C3D header: parameter address is zero");
  }
  out.parameter_start_offset =
      out.zeros_before_header + (out.parameters_address - 1u) * 512u;
  const auto processor_id = require_u8(source, out.parameter_start_offset + 3u);
  const auto processor_type = processor_type_from_id(processor_id);

  out.nb3d_points = require_u16(source, out.zeros_before_header + 2u, processor_type);
  out.nb_analog_measurements = require_u16(source, out.zeros_before_header + 4u, processor_type);

  std::uint64_t first_frame = require_u16(source, out.zeros_before_header + 6u, processor_type);
  bool one_based = false;
  if (first_frame != 0u) {
    --first_frame;
    one_based = true;
  }
  std::uint64_t last_frame = require_u16(source, out.zeros_before_header + 8u, processor_type);
  if (last_frame != 0u && one_based) {
    --last_frame;
  }
  out.first_frame = first_frame;
  out.last_frame = last_frame;
  out.scale_factor = require_f32(source, out.zeros_before_header + 12u, processor_type);
  out.data_start_blocks = require_u16(source, out.zeros_before_header + 16u, processor_type);
  out.nb_analog_by_frame = require_u16(source, out.zeros_before_header + 18u, processor_type);
  out.frame_rate = require_f32(source, out.zeros_before_header + 20u, processor_type);
  return out;
}

static std::uint64_t header_nb_analogs(const C3dHeaderInfo& header) {
  if (header.nb_analog_by_frame == 0u) return header.nb_analog_measurements;
  return header.nb_analog_measurements / header.nb_analog_by_frame;
}

static bool header_has_frame_payload(const C3dHeaderInfo& header) {
  return header.nb3d_points != 0u || header_nb_analogs(header) != 0u;
}

static std::uint64_t header_nb_frames(const C3dHeaderInfo& header) {
  if (!header_has_frame_payload(header)) return 0u;
  if (header.last_frame < header.first_frame) return 0u;
  return header.last_frame - header.first_frame + 1u;
}

static std::string read_ascii(SourceCursor& cursor, std::size_t n_bytes) {
  std::vector<char> bytes(n_bytes + 1u, '\0');
  if (n_bytes > 0u) {
    cursor.read_bytes(bytes.data(), n_bytes);
  }
  return std::string(bytes.data());
}

static void trim_trailing_spaces_in_place(std::string& value) {
  while (!value.empty() && value.back() == ' ') {
    value.pop_back();
  }
}

static void read_char_matrix(
    const std::vector<std::size_t>& dims,
    SourceCursor& cursor,
    std::vector<std::string>& out) {
  out.clear();
  if (dims.empty()) return;
  if (dims[0] == 0u) {
    std::size_t n_values = (dims.size() > 1u) ? 1u : 0u;
    for (std::size_t i = 1u; i < dims.size(); ++i) {
      if (dims[i] == 0u) {
        n_values = 0u;
        break;
      }
      if (n_values > std::numeric_limits<std::size_t>::max() / dims[i]) {
        throw std::runtime_error("invalid CHAR parameter dimensions");
      }
      n_values *= dims[i];
    }
    out.assign(n_values, std::string());
    return;
  }
  std::size_t total_chars = 1u;
  for (const std::size_t dim : dims) {
    if (dim == 0u) {
      out.clear();
      return;
    }
    if (total_chars > std::numeric_limits<std::size_t>::max() / dim) {
      throw std::runtime_error("invalid CHAR parameter dimensions");
    }
    total_chars *= dim;
  }
  std::vector<char> chars(total_chars);
  cursor.read_bytes(chars.data(), chars.size());
  if (dims.size() == 1u) {
    std::string value;
    value.reserve(chars.size());
    for (char ch : chars) {
      if (ch != '\0') value.push_back(ch);
    }
    trim_trailing_spaces_in_place(value);
    out.push_back(value);
    return;
  }
  const std::size_t width = dims[0];
  const std::size_t n_values = total_chars / width;
  out.reserve(n_values);
  for (std::size_t i = 0; i < n_values; ++i) {
    const auto begin = chars.begin() + static_cast<std::ptrdiff_t>(i * width);
    const auto end = begin + static_cast<std::ptrdiff_t>(width);
    std::string value;
    value.reserve(width);
    for (auto it = begin; it != end; ++it) {
      if (*it != '\0') value.push_back(*it);
    }
    trim_trailing_spaces_in_place(value);
    out.push_back(value);
  }
}

static std::size_t parameter_value_count(const std::vector<std::size_t>& dims) {
  if (dims.empty()) return 0u;
  std::size_t total = 1u;
  for (const auto dim : dims) {
    if (dim == 0u) return 0u;
    if (total > std::numeric_limits<std::size_t>::max() / dim) {
      throw std::runtime_error("invalid parameter dimensions");
    }
    total *= dim;
  }
  return total;
}

static void parse_parameter_payload(
    C3dParameterValue& parameter,
    SourceCursor& cursor,
    ezc3d::PROCESSOR_TYPE processor_type) {
  const std::size_t count = parameter_value_count(parameter.dimensions);
  if (parameter.type == ezc3d::DATA_TYPE::CHAR) {
    read_char_matrix(parameter.dimensions, cursor, parameter.string_values);
    return;
  }
  if (parameter.type == ezc3d::DATA_TYPE::BYTE) {
    parameter.int_values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      parameter.int_values.push_back(cursor.read_i8());
    }
    return;
  }
  if (parameter.type == ezc3d::DATA_TYPE::INT) {
    parameter.int_values.reserve(count);
    std::uint8_t bytes[2] = {0, 0};
    for (std::size_t i = 0; i < count; ++i) {
      cursor.read_bytes(bytes, sizeof(bytes));
      parameter.int_values.push_back(static_cast<int>(read_i16_proc(processor_type, bytes)));
    }
    return;
  }
  if (parameter.type == ezc3d::DATA_TYPE::FLOAT) {
    parameter.double_values.reserve(count);
    std::uint8_t bytes[4] = {0, 0, 0, 0};
    for (std::size_t i = 0; i < count; ++i) {
      cursor.read_bytes(bytes, sizeof(bytes));
      parameter.double_values.push_back(static_cast<double>(read_f32_proc(processor_type, bytes)));
    }
    return;
  }
  throw std::runtime_error("unsupported parameter type");
}

static C3dParameterGroup& ensure_group(C3dParameterTree& tree, int id) {
  if (id <= 0) throw std::runtime_error("invalid parameter group id");
  while (tree.groups.size() < static_cast<std::size_t>(id)) {
    tree.groups.push_back(C3dParameterGroup{});
  }
  return tree.groups[static_cast<std::size_t>(id - 1)];
}

static void add_or_replace_parameter(C3dParameterGroup& group, C3dParameterValue parameter) {
  for (auto& existing : group.parameters) {
    if (existing.name == parameter.name) {
      existing = std::move(parameter);
      return;
    }
  }
  group.parameters.push_back(std::move(parameter));
}

static C3dParameterGroup& ensure_named_group(C3dParameterTree& tree, const char* name) {
  for (auto& group : tree.groups) {
    if (group.name == name) return group;
  }
  tree.groups.push_back(C3dParameterGroup{});
  tree.groups.back().name = name;
  return tree.groups.back();
}

static bool group_has_parameter(const C3dParameterGroup& group, const char* name) {
  return group.find_parameter(name) != nullptr;
}

static C3dParameterValue make_int_parameter(
    const char* name,
    std::vector<std::size_t> dimensions,
    std::vector<int> values,
    bool locked = false) {
  C3dParameterValue p{};
  p.name = name;
  p.locked = locked;
  p.type = ezc3d::DATA_TYPE::INT;
  p.dimensions = std::move(dimensions);
  p.int_values = std::move(values);
  return p;
}

static C3dParameterValue make_float_parameter(
    const char* name,
    std::vector<std::size_t> dimensions,
    std::vector<double> values,
    bool locked = false) {
  C3dParameterValue p{};
  p.name = name;
  p.locked = locked;
  p.type = ezc3d::DATA_TYPE::FLOAT;
  p.dimensions = std::move(dimensions);
  p.double_values = std::move(values);
  return p;
}

static C3dParameterValue make_char_parameter(
    const char* name,
    std::vector<std::string> values = {}) {
  C3dParameterValue p{};
  p.name = name;
  p.type = ezc3d::DATA_TYPE::CHAR;
  p.dimensions = values.empty()
                     ? std::vector<std::size_t>{0u, 0u}
                     : std::vector<std::size_t>{0u, values.size()};
  p.string_values = std::move(values);
  return p;
}

static void add_parameter_if_missing(C3dParameterGroup& group, C3dParameterValue parameter) {
  if (!group_has_parameter(group, parameter.name.c_str())) {
    group.parameters.push_back(std::move(parameter));
  }
}

static void set_mandatory_parameters(C3dParameterTree& tree) {
  auto& point = ensure_named_group(tree, "POINT");
  add_parameter_if_missing(point, make_int_parameter("USED", {1u}, {0}, true));
  add_parameter_if_missing(point, make_char_parameter("LABELS"));
  add_parameter_if_missing(point, make_char_parameter("DESCRIPTIONS"));
  add_parameter_if_missing(point, make_float_parameter("SCALE", {1u}, {-1.0}, true));
  add_parameter_if_missing(point, make_char_parameter("UNITS"));
  add_parameter_if_missing(point, make_float_parameter("RATE", {1u}, {0.0}, true));
  add_parameter_if_missing(point, make_int_parameter("DATA_START", {1u}, {0}, true));
  add_parameter_if_missing(point, make_int_parameter("FRAMES", {1u}, {0}, true));

  auto& analog = ensure_named_group(tree, "ANALOG");
  add_parameter_if_missing(analog, make_int_parameter("USED", {1u}, {0}, true));
  add_parameter_if_missing(analog, make_char_parameter("LABELS"));
  add_parameter_if_missing(analog, make_char_parameter("DESCRIPTIONS"));
  add_parameter_if_missing(analog, make_float_parameter("GEN_SCALE", {1u}, {1.0}));
  add_parameter_if_missing(analog, make_float_parameter("SCALE", {0u}, {}));
  add_parameter_if_missing(analog, make_int_parameter("OFFSET", {0u}, {}));
  add_parameter_if_missing(analog, make_char_parameter("UNITS"));
  add_parameter_if_missing(analog, make_float_parameter("RATE", {1u}, {0.0}, true));
  add_parameter_if_missing(analog, make_char_parameter("FORMAT"));
  add_parameter_if_missing(analog, make_int_parameter("BITS", {0u}, {}));

  auto& force_platform = ensure_named_group(tree, "FORCE_PLATFORM");
  add_parameter_if_missing(force_platform, make_int_parameter("USED", {1u}, {0}));
  add_parameter_if_missing(force_platform, make_int_parameter("TYPE", {0u}, {}));
  add_parameter_if_missing(force_platform, make_int_parameter("ZERO", {2u}, {1, 0}));
  add_parameter_if_missing(force_platform, make_float_parameter("CORNERS", {0u}, {}));
  add_parameter_if_missing(force_platform, make_float_parameter("ORIGIN", {0u}, {}));
  add_parameter_if_missing(force_platform, make_int_parameter("CHANNEL", {0u}, {}));
  add_parameter_if_missing(force_platform, make_float_parameter("CAL_MATRIX", {0u}, {}));
}

static C3dParameterTree parse_parameter_tree(
    sqzc3d::C3dReadSource& source,
    const C3dHeaderInfo& header) {
  C3dParameterTree tree{};
  SourceCursor cursor(source);
  cursor.seek(header.parameter_start_offset);
  const auto parameter_start = cursor.read_u8();
  (void)parameter_start;
  const auto checksum = cursor.read_u8();
  (void)checksum;
  const auto nb_param_block = cursor.read_u8();
  (void)nb_param_block;
  tree.processor_type = processor_type_from_id(cursor.read_u8());

  std::uint64_t next_param_pos = cursor.tell();
  while (next_param_pos != 0u) {
    if (cursor.tell() != next_param_pos) {
      throw std::ios_base::failure("The format is not standard");
    }
    const int nb_char_in_name = cursor.read_i8();
    if (nb_char_in_name == 0) break;
    const int id = cursor.read_i8();
    const bool locked = nb_char_in_name < 0;
    const int name_len = std::abs(nb_char_in_name);
    const std::string name = read_ascii(cursor, static_cast<std::size_t>(name_len));
    std::uint8_t offset_bytes[2] = {0, 0};
    cursor.read_bytes(offset_bytes, sizeof(offset_bytes));
    const auto offset_next = static_cast<std::uint64_t>(read_u16_proc(tree.processor_type, offset_bytes));
    if (offset_next == 0u) {
      next_param_pos = 0u;
    } else {
      next_param_pos = cursor.tell() + offset_next - 2u;
    }

    if (id < 0) {
      auto& group = ensure_group(tree, std::abs(id));
      group.name = name;
      group.locked = locked;
      const int desc_len = cursor.read_i8();
      if (desc_len > 0) {
        group.description = read_ascii(cursor, static_cast<std::size_t>(desc_len));
      }
    } else {
      auto& group = ensure_group(tree, id);
      C3dParameterValue parameter{};
      parameter.name = name;
      parameter.locked = locked;
      const int length_in_byte = cursor.read_i8();
      if (length_in_byte == -1) {
        parameter.type = ezc3d::DATA_TYPE::CHAR;
      } else if (length_in_byte == 1) {
        parameter.type = ezc3d::DATA_TYPE::BYTE;
      } else if (length_in_byte == 2) {
        parameter.type = ezc3d::DATA_TYPE::INT;
      } else if (length_in_byte == 4) {
        parameter.type = ezc3d::DATA_TYPE::FLOAT;
      } else {
        throw std::ios_base::failure("Parameter type unrecognized");
      }

      const int n_dimensions = cursor.read_i8();
      if (n_dimensions == 0 && parameter.type != ezc3d::DATA_TYPE::CHAR) {
        parameter.dimensions.push_back(1u);
      } else {
        for (int i = 0; i < n_dimensions; ++i) {
          parameter.dimensions.push_back(static_cast<std::size_t>(cursor.read_u8()));
        }
      }
      parse_parameter_payload(parameter, cursor, tree.processor_type);

      const int desc_len = cursor.read_i8();
      if (desc_len > 0) {
        parameter.description = read_ascii(cursor, static_cast<std::size_t>(desc_len));
      }
      add_or_replace_parameter(group, std::move(parameter));
    }
  }
  set_mandatory_parameters(tree);
  return tree;
}

static void json_write_quoted(std::ostream& out, const std::string& text) {
  out << "\"";
  for (char c : text) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20u) {
          out << "\\u"
              << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(c))
              << std::dec << std::setfill(' ');
        } else {
          out << c;
        }
        break;
    }
  }
  out << "\"";
}

static void json_write_bool(std::ostream& out, bool value) { out << (value ? "true" : "false"); }

static void json_write_size_t_list(std::ostream& out, const std::vector<std::size_t>& values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) out << ",";
    out << values[i];
  }
  out << "]";
}

template <typename T>
static void json_write_numeric_list(std::ostream& out, const std::vector<T>& values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) out << ",";
    out << values[i];
  }
  out << "]";
}

static void json_write_string_list(std::ostream& out, const std::vector<std::string>& values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) out << ",";
    json_write_quoted(out, values[i]);
  }
  out << "]";
}

static std::string build_meta_tree_json(
    const C3dParameterTree& parameters,
    const std::int64_t point_frames_override) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(std::numeric_limits<double>::max_digits10);
  out << "{";
  out << "\"groups\":{";
  bool first_group = true;
  for (const auto& group : parameters.groups) {
    if (group.name.empty()) continue;
    if (!first_group) out << ",";
    first_group = false;

    const std::string& group_name = group.name;
    json_write_quoted(out, group_name);
    out << ":{";
    out << "\"name\":";
    json_write_quoted(out, group_name);
    out << ",\"description\":";
    json_write_quoted(out, group.description);
    out << ",\"locked\":";
    json_write_bool(out, group.locked);
    out << ",\"parameters\":{";

    bool first_param = true;
    for (const auto& parameter : group.parameters) {
      if (!first_param) out << ",";
      first_param = false;

      const std::string& param_name = parameter.name;
      json_write_quoted(out, param_name);
      out << ":{";
      out << "\"name\":";
      json_write_quoted(out, param_name);
      out << ",\"description\":";
      json_write_quoted(out, parameter.description);
      out << ",\"locked\":";
      json_write_bool(out, parameter.locked);
      out << ",\"type\":" << static_cast<int>(parameter.type);
      out << ",\"dimensions\":";
      json_write_size_t_list(out, parameter.dimensions);
      out << ",\"values\":";

      const auto type = parameter.type;
      if (type == ezc3d::DATA_TYPE::CHAR) {
        json_write_string_list(out, parameter.string_values);
      } else if (type == ezc3d::DATA_TYPE::FLOAT) {
        std::vector<double> values = parameter.double_values;
        if (group_name == "POINT" && param_name == "FRAMES" && point_frames_override > 0) {
          values.assign(1u, static_cast<double>(point_frames_override));
        }
        json_write_numeric_list(out, values);
      } else {
        std::vector<int> values = parameter.int_values;
        if (group_name == "POINT" && param_name == "FRAMES" && point_frames_override > 0) {
          values.assign(1u, static_cast<int>(point_frames_override));
        }
        json_write_numeric_list(out, values);
      }
      out << "}";
    }

    out << "}";
    out << "}";
  }
  out << "}";
  out << "}";
  return out.str();
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

static inline void decode_point_record_mips(
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
    x = static_cast<sqzc3d_num_t>(read_f32_be(rec + 0u));
    y = static_cast<sqzc3d_num_t>(read_f32_be(rec + 4u));
    z = static_cast<sqzc3d_num_t>(read_f32_be(rec + 8u));
    const std::int16_t r_word = read_i16_be(rec + 14u);
    residual = static_cast<sqzc3d_num_t>(static_cast<int>(r_word)) * static_cast<sqzc3d_num_t>(-meta.point_scale);
  } else {
    const sqzc3d_num_t scale = static_cast<sqzc3d_num_t>(meta.point_scale);
    x = static_cast<sqzc3d_num_t>(static_cast<float>(read_i16_be(rec + 0u))) * scale;
    y = static_cast<sqzc3d_num_t>(static_cast<float>(read_i16_be(rec + 2u))) * scale;
    z = static_cast<sqzc3d_num_t>(static_cast<float>(read_i16_be(rec + 4u))) * scale;
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

static inline void decode_point_record_proc(
    const C3dStreamMeta& meta,
    const std::uint8_t* rec,
    sqzc3d_num_t* out_xyz3,
    sqzc3d_num_t* out_residual) {
  if (meta.processor_type == ezc3d::PROCESSOR_TYPE::INTEL) {
    decode_point_record_intel(meta, rec, out_xyz3, out_residual);
  } else if (meta.processor_type == ezc3d::PROCESSOR_TYPE::DEC) {
    decode_point_record_dec(meta, rec, out_xyz3, out_residual);
  } else if (meta.processor_type == ezc3d::PROCESSOR_TYPE::MIPS) {
    decode_point_record_mips(meta, rec, out_xyz3, out_residual);
  } else {
    throw std::runtime_error("unsupported processor type");
  }
}

static std::vector<std::uint8_t>& read_scratch_buffer(
    C3dStreamReader* reader,
    std::vector<std::uint8_t>* scratch) {
  return scratch ? *scratch : reader->read_scratch;
}

static sqzc3d_status read_point_span(
    C3dStreamReader* reader,
    int frame_idx,
    int first_point,
    int n_points,
    sqzc3d_num_t* out_xyz,
    int out_nscalar,
    unsigned char* out_valid,
    int out_valid_nscalar,
    sqzc3d_num_t* out_residual,
    int out_residual_nscalar,
    bool read_residual,
    std::vector<std::uint8_t>* scratch) {
  const auto& meta = reader->meta;
  if (frame_idx < 0 || frame_idx >= meta.n_frames || first_point < 0 || n_points <= 0 ||
      first_point > meta.n_points || n_points > meta.n_points - first_point) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  if (out_xyz && out_nscalar != n_points * 3) {
    return sqzc3d_STATUS_DIMENSION_MISMATCH;
  }
  if (out_valid && out_valid_nscalar != n_points) {
    return sqzc3d_STATUS_DIMENSION_MISMATCH;
  }
  if (read_residual && out_residual_nscalar != n_points) {
    return sqzc3d_STATUS_DIMENSION_MISMATCH;
  }
  if (!reader->source) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  if (!is_supported_processor_type(meta.processor_type)) {
    return sqzc3d_STATUS_NOT_IMPLEMENTED;
  }

  const std::size_t record_bytes = static_cast<std::size_t>(meta.point_record_bytes);
  if (record_bytes == 0u || static_cast<std::size_t>(n_points) >
                                std::numeric_limits<std::size_t>::max() / record_bytes) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  const std::size_t span_bytes = static_cast<std::size_t>(n_points) * record_bytes;
  const std::size_t frame_off =
      frame_start_offset(meta, frame_idx) + static_cast<std::size_t>(first_point) * record_bytes;
  auto& buf = read_scratch_buffer(reader, scratch);
  buf.resize(span_bytes);
  if (!reader->source->read_at(static_cast<std::uint64_t>(frame_off), buf.data(), span_bytes)) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }

  for (int i = 0; i < n_points; ++i) {
    const std::size_t rec_off = static_cast<std::size_t>(i) * record_bytes;
    sqzc3d_num_t xyz[3] = {
        std::numeric_limits<sqzc3d_num_t>::quiet_NaN(),
        std::numeric_limits<sqzc3d_num_t>::quiet_NaN(),
        std::numeric_limits<sqzc3d_num_t>::quiet_NaN()};
    sqzc3d_num_t residual = 0;
    decode_point_record_proc(meta, buf.data() + rec_off, out_xyz || out_valid ? xyz : nullptr, &residual);
    if (out_xyz) {
      const std::size_t out_o = static_cast<std::size_t>(i) * 3u;
      out_xyz[out_o] = xyz[0];
      out_xyz[out_o + 1u] = xyz[1];
      out_xyz[out_o + 2u] = xyz[2];
    }
    if (out_residual && read_residual) out_residual[static_cast<std::size_t>(i)] = residual;
    if (out_valid) {
      out_valid[i] = is_finite_xyz(xyz[0], xyz[1], xyz[2]) ? 1u : 0u;
    }
  }
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
    bool read_residual,
    std::vector<std::uint8_t>* scratch) {
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
  if (!reader->source) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  if (!is_supported_processor_type(meta.processor_type)) {
    return sqzc3d_STATUS_NOT_IMPLEMENTED;
  }

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
  if (record_bytes == 0u || span_points > std::numeric_limits<std::size_t>::max() / record_bytes) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  const std::size_t span_bytes = span_points * record_bytes;
  if (span_bytes == 0u) return sqzc3d_STATUS_INVALID_ARGUMENT;

  const std::size_t frame_off = frame_start_offset(meta, frame_idx) + span_start * record_bytes;
  auto& buf = read_scratch_buffer(reader, scratch);
  buf.resize(span_bytes);
  if (!reader->source->read_at(static_cast<std::uint64_t>(frame_off), buf.data(), span_bytes)) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }

  for (int i = 0; i < n_points_sel; ++i) {
    const int idx = point_indices[i];
    const std::size_t rec_off = static_cast<std::size_t>(idx - min_idx) * record_bytes;
    sqzc3d_num_t xyz[3] = {
        std::numeric_limits<sqzc3d_num_t>::quiet_NaN(),
        std::numeric_limits<sqzc3d_num_t>::quiet_NaN(),
        std::numeric_limits<sqzc3d_num_t>::quiet_NaN()};
    sqzc3d_num_t residual = 0;
    decode_point_record_proc(meta, buf.data() + rec_off, out_xyz || out_valid ? xyz : nullptr, &residual);
    if (out_xyz) {
      const std::size_t out_o = static_cast<std::size_t>(i) * 3u;
      out_xyz[out_o] = xyz[0];
      out_xyz[out_o + 1u] = xyz[1];
      out_xyz[out_o + 2u] = xyz[2];
    }
    if (out_residual && read_residual) out_residual[static_cast<std::size_t>(i)] = residual;
    if (out_valid) {
      out_valid[i] = is_finite_xyz(xyz[0], xyz[1], xyz[2]) ? 1u : 0u;
    }
  }
  return sqzc3d_STATUS_SUCCESS;
}

static void load_labels(const C3dStreamReader* source, C3dStreamReader* out) {
  if (!source || !out || !source->parameter_tree) {
    throw std::runtime_error("label parse requires an opened parameter tree");
  }
  const auto& params = *source->parameter_tree;

  const auto* point_group = params.find_group("POINT");
  const auto* analog_group = params.find_group("ANALOG");
  if (!point_group || !analog_group) {
    throw std::runtime_error("mandatory label parameter group missing");
  }
  out->point_labels = collect_contiguous_char_parameter(*point_group, "LABELS");
  out->analog_labels = collect_contiguous_char_parameter(*analog_group, "LABELS");
}

static void load_point_type_groups(const C3dStreamReader* source, C3dStreamReader* out) {
  if (!source || !out || !source->parameter_tree) {
    throw std::runtime_error("type-group parse requires an opened parameter tree");
  }
  const auto& params = *source->parameter_tree;
  out->type_group_names.clear();
  out->type_group_starts.clear();
  out->type_group_indices.clear();
  out->type_group_starts.push_back(0);
  const auto* group_point = params.find_group("POINT");
  if (!group_point) {
    throw std::runtime_error("mandatory POINT parameter group missing");
  }

  const auto* type_groups_param = group_point->find_parameter("TYPE_GROUPS");
  if (!type_groups_param || type_groups_param->type != ezc3d::DATA_TYPE::CHAR) return;

  const auto& type_groups = type_groups_param->string_values;
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
    const auto* gparam = group_point->find_parameter(gname);
    if (!gparam) {
      out->type_group_names.push_back(gname);
      out->type_group_starts.push_back(static_cast<int>(out->type_group_indices.size()));
      continue;
    }
    if (gparam->type == ezc3d::DATA_TYPE::CHAR) {
      for (const auto& label : gparam->string_values) {
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

static sqzc3d_status open_source(
    C3dStreamReader* reader,
    std::unique_ptr<sqzc3d::C3dReadSource> source,
    std::int64_t source_nbytes,
    const char* api_name,
    const char* op_name) {
  ApiCtx ctx(api_name);
  if (!reader || !source) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, std::string(op_name) + ": invalid argument", "args");
  }
  reset_reader(reader);
  reader->source = std::move(source);

  struct ReaderCleanup {
    C3dStreamReader* reader = nullptr;
    explicit ReaderCleanup(C3dStreamReader* r) : reader(r) {}
    void release() { reader = nullptr; }
    ~ReaderCleanup() {
      reset_reader(reader);
    }
  };
  ReaderCleanup cleanup(reader);

  const char* phase = "init";

  try {
    phase = "header";
    C3dHeaderInfo header{};
    C3dParameterTree parameter_tree{};
    try {
      header = parse_header_info(*reader->source);
      parameter_tree = parse_parameter_tree(*reader->source, header);
    } catch (const std::exception& e) {
      std::string probe;
      try {
        probe = sqzc3d_debug_probe_params_layout(*reader->source);
      } catch (const std::exception& pe) {
        probe = std::string("\n(probe failed: ") + pe.what() + ")";
      } catch (...) {
        probe = "\n(probe failed: unknown exception)";
      }
      return ctx.fail(
          sqzc3d_STATUS_INVALID_ARGUMENT,
          std::string(op_name) + ": header/parameter parse failed: " + e.what() + probe,
          phase);
    }

    phase = "check_point";
    if (!is_supported_processor_type(parameter_tree.processor_type)) {
      return ctx.fail(
          sqzc3d_STATUS_INVALID_ARGUMENT,
          std::string(op_name) + ": unsupported processor type: " +
              std::to_string(static_cast<int>(parameter_tree.processor_type)),
          phase);
    }
    if (header_has_frame_payload(header) && header.last_frame < header.first_frame) {
      return ctx.fail(
          sqzc3d_STATUS_INVALID_ARGUMENT,
          std::string(op_name) + ": invalid C3D header: last frame before first frame",
          phase);
    }

    C3dStreamMeta tmp{};
    bool has_point_units_param = false;
    bool has_shadow_group = false;
    std::string point_units_token;
    std::uint64_t effective_last_frame = header.last_frame;
    bool has_rotational = false;
    double effective_point_rate = static_cast<double>(header.frame_rate);
    phase = "meta";
    try {
      const auto* group_point = parameter_tree.find_group("POINT");
      const auto* group_analog = parameter_tree.find_group("ANALOG");
      if (!group_point || !group_analog) {
        throw std::runtime_error("mandatory POINT/ANALOG parameter group missing");
      }
      has_shadow_group = parameter_tree.find_group("SHADOW") != nullptr;

      tmp.processor_type = parameter_tree.processor_type;
      tmp.header_scale = header.scale_factor;
      tmp.point_scale = static_cast<double>(tmp.header_scale);
      int analog_record_bytes = (tmp.header_scale < 0.0) ? 4 : 2;

      const auto v = parameter_values_as_double(group_point->find_parameter("SCALE"));
      if (!v.empty()) tmp.point_scale = v[0];
      const auto units_values = collect_contiguous_char_parameter(*group_point, "UNITS");
      if (!units_values.empty()) {
        has_point_units_param = true;
        point_units_token = units_values[0];
        tmp.point_units_per_meter = units_per_meter_from_unit_token(point_units_token);
      }

      const auto scales = collect_contiguous_double_parameter(*group_analog, "SCALE");
      if (!scales.empty()) {
        tmp.analog_scale = scales[0];
        tmp.analog_scales.assign(scales.begin(), scales.end());
      }
      const auto gv = parameter_values_as_double(group_analog->find_parameter("GEN_SCALE"));
      if (!gv.empty() && std::isfinite(gv[0])) {
        tmp.analog_general_factor = gv[0];
      }
      const auto offsets = collect_contiguous_int_parameter(*group_analog, "OFFSET");
      if (!offsets.empty()) {
        tmp.analog_offsets.clear();
        tmp.analog_offsets.reserve(offsets.size());
        for (int offset : offsets) {
          tmp.analog_offsets.push_back(std::abs(offset));
        }
      }
      tmp.analog_record_bytes = analog_record_bytes;
      tmp.n_points = static_cast<int>(header.nb3d_points);
      tmp.n_analogs = static_cast<int>(header_nb_analogs(header));
      tmp.n_analog_by_frame = static_cast<int>(header.nb_analog_by_frame);

      double point_rate = static_cast<double>(header.frame_rate);
      const int point_used = parameter_first_int(group_point, "USED", tmp.n_points);
      const int point_frames = parameter_first_int(group_point, "FRAMES", 0);
      if (point_frames < 0) {
        throw std::runtime_error("invalid POINT:FRAMES: negative frame count");
      }
      if (point_frames > 0 && point_frames != static_cast<int>(header_nb_frames(header))) {
        effective_last_frame = header.first_frame + static_cast<std::uint64_t>(point_frames) - 1u;
      }
      const double point_rate_param = parameter_first_double(group_point, "RATE", point_rate);
      constexpr double rate_compare_scale = 10000.0;
      if (static_cast<int>(point_rate_param * rate_compare_scale) !=
          static_cast<int>(static_cast<double>(header.frame_rate) * rate_compare_scale)) {
        if (point_rate_param == 0.0 && point_used != 0) {
          point_rate = static_cast<double>(header.frame_rate);
        } else {
          point_rate = point_rate_param;
        }
      }
      if (point_used != tmp.n_points) {
        tmp.n_points = point_used;
      }

      const double analog_rate = parameter_first_double(
          group_analog,
          "RATE",
          point_rate * static_cast<double>(tmp.n_analog_by_frame));
      if (point_rate != 0.0) {
        const int analog_by_frame_from_rate = static_cast<int>(analog_rate / point_rate);
        if (analog_by_frame_from_rate != tmp.n_analog_by_frame) {
          if (!(tmp.n_analog_by_frame == 1 && has_shadow_group)) {
            tmp.n_analog_by_frame = analog_by_frame_from_rate;
          }
        }
      }
      const int analog_used = parameter_first_int(group_analog, "USED", tmp.n_analogs);
      if (analog_used != tmp.n_analogs) {
        tmp.n_analogs = analog_used;
      }
      effective_point_rate = point_rate;
      has_rotational = parameter_tree.find_group("ROTATION") != nullptr;
    } catch (const std::exception& e) {
      return ctx.fail(
          sqzc3d_STATUS_INVALID_ARGUMENT,
          std::string(op_name) + ": meta parse failed: " + e.what(),
          phase);
    }

    // By default, expose point coordinates in the raw C3D units (no scaling).
    if (!(tmp.point_units_per_meter > 0.0) || !std::isfinite(tmp.point_units_per_meter)) {
      // C3D files typically use mm; treat missing/unknown units as mm for robustness.
      tmp.point_units_per_meter = 1000.0;
      tmp.point_units_source = has_point_units_param ? 2 : 1;
    } else {
      tmp.point_units_source = 0;
    }
    tmp.target_units_per_meter = tmp.point_units_per_meter;
    tmp.point_unit_scale = 1.0;

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
    const auto data_start_blocks = header.data_start_blocks;
    if (data_start_blocks == 0u) {
      return ctx.fail(
          sqzc3d_STATUS_INVALID_ARGUMENT,
          std::string(op_name) + ": invalid C3D header: dataStart must be >= 1",
          phase);
    }
    const auto data_start_bytes_u = (data_start_blocks - 1u) * 512u;
    if (data_start_bytes_u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return ctx.fail(
          sqzc3d_STATUS_INVALID_ARGUMENT,
          std::string(op_name) + ": invalid C3D header: dataStart is too large",
          phase);
    }
    tmp.data_start_bytes = static_cast<std::int64_t>(data_start_bytes_u);

    const auto points_bytes = static_cast<std::int64_t>(tmp.n_points) * static_cast<std::int64_t>(tmp.point_record_bytes);
    const auto analog_bytes = static_cast<std::int64_t>(tmp.n_analog_by_frame) *
                             static_cast<std::int64_t>(tmp.n_analogs) *
                             static_cast<std::int64_t>(tmp.analog_record_bytes);
    const auto frame_bytes = points_bytes + analog_bytes;
    if (points_bytes < 0 || analog_bytes < 0 || frame_bytes < 0 ||
        frame_bytes > std::numeric_limits<int>::max()) {
      return ctx.fail(
          sqzc3d_STATUS_INVALID_ARGUMENT,
          std::string(op_name) + ": invalid point/analog shape: frame bytes overflow",
          phase);
    }
    tmp.frame_bytes = static_cast<int>(frame_bytes);

    // Prefer frame count from header first/last indices.
    // Special case: nbFrames==0xFFFF without rotational data means "read until EOF".
    const auto header_frames_effective =
        (!header_has_frame_payload(header) && !has_rotational)
            ? 0u
            : (effective_last_frame - header.first_frame + 1u);
    const bool all_of_file_frames = (header_frames_effective == 0xFFFFu && !has_rotational);
    int frames = 0;
    const auto header_first_frame = header.first_frame;
    {
      std::size_t header_frames = 0;
      if (!all_of_file_frames) {
        if (effective_last_frame > header_first_frame || (header_first_frame == 0 && effective_last_frame == 0)) {
          header_frames = effective_last_frame - header_first_frame + 1u;
        } else if (effective_last_frame >= header_first_frame) {
          header_frames = effective_last_frame - header_first_frame + 1u;
        }
        if (header_frames == 0) {
          header_frames = header_frames_effective;
        }
      } else {
        // Keep this as a plausible initial value; it will be replaced
        // by file-size-based frame count below.
        header_frames = effective_last_frame - header_first_frame + 1u;
      }
      if (header_frames > std::numeric_limits<int>::max()) {
        return ctx.fail(
            sqzc3d_STATUS_INVALID_ARGUMENT,
            std::string(op_name) + ": invalid C3D header: frame count overflow",
            phase);
      }
      frames = static_cast<int>(header_frames);
    }

    std::int64_t file_bytes = source_nbytes;
    if (file_bytes < 0) {
      file_bytes = reader->source->size_bytes();
    }

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
      return ctx.fail(
          sqzc3d_STATUS_INVALID_ARGUMENT,
          std::string(op_name) + ": invalid frame count",
          phase);
    }
    tmp.n_frames = frames;
    if (header_first_frame > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return ctx.fail(
          sqzc3d_STATUS_INVALID_ARGUMENT,
          std::string(op_name) + ": invalid C3D header: first frame index overflow",
          phase);
    }
    tmp.first_frame = static_cast<int>(header_first_frame);
    if (tmp.n_frames <= 0) {
      tmp.last_frame = tmp.first_frame - 1;
    } else {
      const auto last = static_cast<long long>(tmp.first_frame) + static_cast<long long>(tmp.n_frames) - 1LL;
      if (last < std::numeric_limits<int>::min() || last > std::numeric_limits<int>::max()) {
        return ctx.fail(
            sqzc3d_STATUS_INVALID_ARGUMENT,
            std::string(op_name) + ": invalid C3D header: last frame index overflow",
            phase);
      }
      tmp.last_frame = static_cast<int>(last);
    }

    {
      const double rate = effective_point_rate;
      if (std::isfinite(rate) && rate > 0.0) {
        tmp.point_rate_hz = rate;
        const double a_rate = rate * static_cast<double>(tmp.n_analog_by_frame);
        tmp.analog_rate_hz = (std::isfinite(a_rate) && a_rate >= 0.0) ? a_rate : 0.0;
      } else {
        tmp.point_rate_hz = 0.0;
        tmp.analog_rate_hz = 0.0;
      }
    }

    phase = "labels";
    reader->parameter_tree = std::make_unique<C3dParameterTree>(std::move(parameter_tree));
    reader->meta = tmp;
    try {
      load_labels(reader, reader);
      load_point_type_groups(reader, reader);
    } catch (const std::exception& e) {
      return ctx.fail(
          sqzc3d_STATUS_INVALID_ARGUMENT,
          std::string(op_name) + ": label/type-groups parse failed: " + e.what(),
          phase);
    }
    cleanup.release();
    return sqzc3d_STATUS_SUCCESS;
  } catch (const std::bad_alloc&) {
    return ctx.fail(
        sqzc3d_STATUS_INTERNAL_ERROR,
        std::string(op_name) + ": bad_alloc",
        phase);
  } catch (const std::exception& e) {
    return ctx.fail(
        sqzc3d_STATUS_INVALID_ARGUMENT,
        std::string(op_name) + ": exception: " + e.what(),
        phase);
  } catch (...) {
    return ctx.fail(
        sqzc3d_STATUS_INTERNAL_ERROR,
        std::string(op_name) + ": unknown exception",
        phase);
  }
}

sqzc3d_status sqzc3d_c3d_stream_open_file(
    C3dStreamReader* reader,
    const char* file_path) {
  ApiCtx ctx("sqzc3d_c3d_stream_open_file");
  if (!reader || !file_path) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "open_file: invalid argument", "args");
  }

  try {
    const auto fs_path = std::filesystem::u8path(file_path);
    std::ifstream stream(fs_path, std::ios::binary);
    if (!stream.is_open()) {
      return ctx.fail(
          sqzc3d_STATUS_INVALID_ARGUMENT,
          std::string("open_file: failed to open C3D file: ") + file_path,
          "open_source");
    }

    std::int64_t source_nbytes = -1;
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(fs_path, ec);
    if (!ec && file_size <= static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max())) {
      source_nbytes = static_cast<std::int64_t>(file_size);
    }
    auto source = std::make_unique<FileReadSource>(std::move(stream), source_nbytes);
    return open_source(
        reader,
        std::move(source),
        source_nbytes,
        "sqzc3d_c3d_stream_open_file",
        "open_file");
  } catch (const std::bad_alloc&) {
    return ctx.fail(sqzc3d_STATUS_INTERNAL_ERROR, "open_file: bad_alloc", "open_source");
  } catch (const std::exception& e) {
    return ctx.fail(
        sqzc3d_STATUS_INVALID_ARGUMENT,
        std::string("open_file: exception: ") + e.what(),
        "open_source");
  } catch (...) {
    return ctx.fail(sqzc3d_STATUS_INTERNAL_ERROR, "open_file: unknown exception", "open_source");
  }
}

sqzc3d_status sqzc3d_c3d_stream_open_memory(
    C3dStreamReader* reader,
    const void* data,
    std::size_t n_bytes) {
  ApiCtx ctx("sqzc3d_c3d_stream_open_memory");
  if (!reader || !data || n_bytes == 0u) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "open_memory: invalid argument", "args");
  }
  if (n_bytes > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "open_memory: buffer too large", "args");
  }

  try {
    auto source = std::make_unique<MemoryReadSource>(data, n_bytes);
    return open_source(
        reader,
        std::move(source),
        static_cast<std::int64_t>(n_bytes),
        "sqzc3d_c3d_stream_open_memory",
        "open_memory");
  } catch (const std::bad_alloc&) {
    return ctx.fail(sqzc3d_STATUS_INTERNAL_ERROR, "open_memory: bad_alloc", "open_source");
  } catch (const std::exception& e) {
    return ctx.fail(
        sqzc3d_STATUS_INVALID_ARGUMENT,
        std::string("open_memory: exception: ") + e.what(),
        "open_source");
  } catch (...) {
    return ctx.fail(sqzc3d_STATUS_INTERNAL_ERROR, "open_memory: unknown exception", "open_source");
  }
}

sqzc3d_status sqzc3d_c3d_stream_set_target_unit(
    C3dStreamReader* reader,
    const char* unit_token) {
  ApiCtx ctx("sqzc3d_c3d_stream_set_target_unit");
  if (!reader || !reader->source || !unit_token) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "set_target_unit: invalid argument", "args");
  }
  const double upm = units_per_meter_from_unit_token(unit_token);
  if (!(upm > 0.0) || !std::isfinite(upm)) {
    return ctx.fail(
        sqzc3d_STATUS_INVALID_ARGUMENT,
        std::string("set_target_unit: unknown unit token: ") + unit_token,
        "args");
  }
  return sqzc3d_c3d_stream_set_target_units_per_meter(reader, upm);
}

sqzc3d_status sqzc3d_c3d_stream_set_target_units_per_meter(
    C3dStreamReader* reader,
    double units_per_meter) {
  ApiCtx ctx("sqzc3d_c3d_stream_set_target_units_per_meter");
  if (!reader || !reader->source) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "set_target_units_per_meter: invalid argument", "args");
  }
  if (!(units_per_meter > 0.0) || !std::isfinite(units_per_meter)) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "set_target_units_per_meter: units_per_meter must be > 0", "args");
  }
  auto& meta = reader->meta;
  meta.target_units_per_meter = units_per_meter;
  meta.point_unit_scale = meta.target_units_per_meter / meta.point_units_per_meter;
  return sqzc3d_STATUS_SUCCESS;
}

void sqzc3d_c3d_stream_close(C3dStreamReader* reader) {
  reset_reader(reader);
}

bool sqzc3d_c3d_stream_is_open(const C3dStreamReader* reader) {
  return reader && reader->source != nullptr;
}

sqzc3d_status sqzc3d_c3d_stream_meta_tree_json(
    const C3dStreamReader* reader,
    std::int64_t point_frames_override,
    std::string* out_json) {
  ApiCtx ctx("sqzc3d_c3d_stream_meta_tree_json");
  if (!reader || !reader->parameter_tree || !out_json) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "meta_tree_json: invalid argument", "args");
  }
  try {
    *out_json = build_meta_tree_json(*reader->parameter_tree, point_frames_override);
    return sqzc3d_STATUS_SUCCESS;
  } catch (const std::bad_alloc&) {
    return ctx.fail(sqzc3d_STATUS_INTERNAL_ERROR, "meta_tree_json: bad_alloc");
  } catch (const std::exception& e) {
    return ctx.fail(
        sqzc3d_STATUS_INTERNAL_ERROR,
        std::string("meta_tree_json: exception: ") + e.what());
  } catch (...) {
    return ctx.fail(sqzc3d_STATUS_INTERNAL_ERROR, "meta_tree_json: unknown exception");
  }
}

sqzc3d_status sqzc3d_c3d_stream_point_indices_for_labels(
    const C3dStreamReader* reader,
    const char* const* labels,
    int n_labels,
    int* out_indices,
    int miss_idx,
    int norm_mode) {
  ApiCtx ctx("sqzc3d_c3d_stream_point_indices_for_labels");
  if (!reader || !reader->source) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "point_indices_for_labels: reader is not opened", "args");
  }
  if (n_labels < 0) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "point_indices_for_labels: n_labels must be >= 0", "args");
  }
  if (n_labels > 0 && (!labels || !out_indices)) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "point_indices_for_labels: labels/out_indices must not be null", "args");
  }

  for (int i = 0; i < n_labels; ++i) out_indices[i] = miss_idx;

  const auto& haystack = reader->point_labels;
  for (int i = 0; i < n_labels; ++i) {
    const char* want = labels[i];
    if (!want) {
      return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "point_indices_for_labels: null label pointer", "args", i);
    }
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
      return ctx.fail(
          sqzc3d_STATUS_INVALID_ARGUMENT,
          std::string("point_indices_for_labels: label selector is ambiguous under norm_mode=") +
              std::to_string(norm_mode) + ": '" + want + "'",
          "match",
          i);
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
    int out_valid_nscalar,
    std::vector<std::uint8_t>* scratch) {
  ApiCtx ctx("sqzc3d_c3d_stream_read_frame_xyz_sel");
  if (!reader || !reader->source || !out_target || !point_indices) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_xyz_sel: invalid argument", "args");
  }
  if (out_nscalar != n_points_sel * 3) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_xyz_sel: out_nscalar mismatch", "args");
  }
  const auto st = read_point_block(
      reader,
      frame_idx,
      point_indices,
      n_points_sel,
      out_target,
      out_valid,
      out_valid_nscalar,
      nullptr,
      0,
      false,
      scratch);
  if (st != sqzc3d_STATUS_SUCCESS) {
    return ctx.fail(st, "read_frame_xyz_sel: read failed", "read");
  }
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_xyz_residual_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* point_indices,
    int n_points_sel,
    sqzc3d_num_t* out_target,
    int out_nscalar,
    unsigned char* out_valid,
    int out_valid_nscalar,
    sqzc3d_num_t* out_residual,
    int out_residual_nscalar,
    std::vector<std::uint8_t>* scratch) {
  ApiCtx ctx("sqzc3d_c3d_stream_read_frame_xyz_residual_sel");
  if (!reader || !reader->source || !out_target || !out_residual || !point_indices || n_points_sel <= 0) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_xyz_residual_sel: invalid argument", "args");
  }
  if (out_nscalar != n_points_sel * 3) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_xyz_residual_sel: out_nscalar mismatch", "args");
  }
  const auto st = read_point_block(
      reader,
      frame_idx,
      point_indices,
      n_points_sel,
      out_target,
      out_valid,
      out_valid_nscalar,
      out_residual,
      out_residual_nscalar,
      true,
      scratch);
  if (st != sqzc3d_STATUS_SUCCESS) {
    return ctx.fail(st, "read_frame_xyz_residual_sel: read failed", "read");
  }
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_residual_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* point_indices,
    int n_points_sel,
    sqzc3d_num_t* out_residual,
    int out_nscalar,
    std::vector<std::uint8_t>* scratch) {
  ApiCtx ctx("sqzc3d_c3d_stream_read_frame_residual_sel");
  if (!reader || !reader->source || !out_residual || !point_indices || n_points_sel <= 0) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_residual_sel: invalid argument", "args");
  }
  const auto st = read_point_block(
      reader,
      frame_idx,
      point_indices,
      n_points_sel,
      nullptr,
      nullptr,
      0,
      out_residual,
      out_nscalar,
      true,
      scratch);
  if (st != sqzc3d_STATUS_SUCCESS) {
    return ctx.fail(st, "read_frame_residual_sel: read failed", "read");
  }
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_all_xyz(
    C3dStreamReader* reader,
    int frame_idx,
    sqzc3d_num_t* out_target,
    int out_nscalar,
    std::vector<std::uint8_t>* scratch) {
  ApiCtx ctx("sqzc3d_c3d_stream_read_frame_all_xyz");
  if (!reader || !reader->source || !out_target) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_all_xyz: invalid argument", "args");
  }
  const int n_points = reader->meta.n_points;
  if (n_points <= 0) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_all_xyz: no points in stream", "args");
  }
  if (out_nscalar != n_points * 3) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_all_xyz: out_nscalar mismatch", "args");
  }

  const auto st = read_point_span(
      reader,
      frame_idx,
      0,
      n_points,
      out_target,
      out_nscalar,
      nullptr,
      0,
      nullptr,
      0,
      false,
      scratch);
  if (st != sqzc3d_STATUS_SUCCESS) {
    return ctx.fail(st, "read_frame_all_xyz: read failed", "read");
  }
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_all_xyz_residual(
    C3dStreamReader* reader,
    int frame_idx,
    sqzc3d_num_t* out_target,
    int out_nscalar,
    unsigned char* out_valid,
    int out_valid_nscalar,
    sqzc3d_num_t* out_residual,
    int out_residual_nscalar,
    std::vector<std::uint8_t>* scratch) {
  ApiCtx ctx("sqzc3d_c3d_stream_read_frame_all_xyz_residual");
  if (!reader || !reader->source || !out_target || !out_residual) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_all_xyz_residual: invalid argument", "args");
  }
  const int n_points = reader->meta.n_points;
  if (n_points <= 0) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_all_xyz_residual: no points in stream", "args");
  }
  if (out_nscalar != n_points * 3) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_all_xyz_residual: out_nscalar mismatch", "args");
  }

  const auto st = read_point_span(
      reader,
      frame_idx,
      0,
      n_points,
      out_target,
      out_nscalar,
      out_valid,
      out_valid_nscalar,
      out_residual,
      out_residual_nscalar,
      true,
      scratch);
  if (st != sqzc3d_STATUS_SUCCESS) {
    return ctx.fail(st, "read_frame_all_xyz_residual: read failed", "read");
  }
  return sqzc3d_STATUS_SUCCESS;
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
    int out_valid_nscalar,
    std::vector<std::uint8_t>* scratch) {
  ApiCtx ctx("sqzc3d_c3d_stream_read_traj_xyz_sel");
  if (!reader || !reader->source || !point_indices || !out_traj || n_points_sel <= 0) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_traj_xyz_sel: invalid argument", "args");
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
  if (start >= end) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_traj_xyz_sel: empty frame range", "args");
  }

  const int span = end - start;
  const int expected = span * n_points_sel * 3;
  if (out_nscalar != expected) {
    return ctx.fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "read_traj_xyz_sel: out_nscalar mismatch", "args");
  }
  if (out_valid && out_valid_nscalar != span * n_points_sel) {
    return ctx.fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "read_traj_xyz_sel: out_valid_nscalar mismatch", "args");
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
        false,
        scratch);
    if (st != sqzc3d_STATUS_SUCCESS) {
      return ctx.fail(st, "read_traj_xyz_sel: read failed", "read", f);
    }
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
    int out_nscalar,
    std::vector<std::uint8_t>* scratch) {
  ApiCtx ctx("sqzc3d_c3d_stream_read_frame_analogs_sel");
  if (!reader || !reader->source || !analog_indices || !out_analog || n_analog_sel <= 0 ||
      n_samples < 0 || start_sample < 0) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_analogs_sel: invalid argument", "args");
  }
  const auto& meta = reader->meta;
  if (frame_idx < 0 || frame_idx >= meta.n_frames) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_analogs_sel: frame_idx out of range", "args");
  }

  for (int i = 0; i < n_analog_sel; ++i) {
    if (analog_indices[i] < 0 || analog_indices[i] >= meta.n_analogs) {
      return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_analogs_sel: analog index out of range", "args", i);
    }
  }

  const int end_sample = start_sample + n_samples;
  if (start_sample >= meta.n_analog_by_frame || end_sample > meta.n_analog_by_frame) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_analogs_sel: invalid sample range", "args");
  }
  if (out_nscalar != n_samples * n_analog_sel) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_analogs_sel: out_nscalar mismatch", "args");
  }
  if (meta.analog_record_bytes != 4 && meta.analog_record_bytes != 2) {
    return ctx.fail(
        sqzc3d_STATUS_NOT_IMPLEMENTED,
        "read_frame_analogs_sel: unsupported analog_record_bytes",
        "meta");
  }

  const std::size_t frame_base = frame_start_offset(meta, frame_idx);
  const std::size_t points_bytes = static_cast<std::size_t>(meta.n_points) *
                                   static_cast<std::size_t>(meta.point_record_bytes);
  const std::size_t sample_stride =
      static_cast<std::size_t>(meta.n_analogs) * static_cast<std::size_t>(meta.analog_record_bytes);
  const std::size_t start_off =
      frame_base + points_bytes + static_cast<std::size_t>(start_sample) * sample_stride;
  const std::size_t bytes_to_read = static_cast<std::size_t>(n_samples) * sample_stride;
  if (bytes_to_read == 0u) return sqzc3d_STATUS_SUCCESS;

  auto& buf = read_scratch_buffer(reader, scratch);
  buf.resize(bytes_to_read);
  if (!reader->source->read_at(static_cast<std::uint64_t>(start_off), buf.data(), bytes_to_read)) {
    return ctx.fail(sqzc3d_STATUS_INVALID_ARGUMENT, "read_frame_analogs_sel: file read failed", "io");
  }

  for (int s = 0; s < n_samples; ++s) {
    const std::size_t sample_off = static_cast<std::size_t>(s) * sample_stride;
    for (int c = 0; c < n_analog_sel; ++c) {
      const int idx = analog_indices[c];
      const std::size_t idxu = static_cast<std::size_t>(idx);
      const std::size_t rec_off =
          sample_off + idxu * static_cast<std::size_t>(meta.analog_record_bytes);
      const std::size_t out_o =
          static_cast<std::size_t>(s) * static_cast<std::size_t>(n_analog_sel) +
          static_cast<std::size_t>(c);

      double scale = meta.analog_scale_default;
      if (idxu < meta.analog_scales.size()) {
        scale = meta.analog_scales[idxu];
      }
      double analog_offset = 0.0;
      if (idxu < meta.analog_offsets.size()) {
        analog_offset = static_cast<double>(meta.analog_offsets[idxu]);
      }

      if (meta.analog_record_bytes == 4) {
        const float raw = read_f32_proc(meta.processor_type, buf.data() + rec_off);
        out_analog[out_o] = static_cast<sqzc3d_num_t>(
            (static_cast<double>(raw) - analog_offset) * scale * meta.analog_general_factor);
      } else {
        const int raw = static_cast<int>(read_i16_proc(meta.processor_type, buf.data() + rec_off));
        out_analog[out_o] = static_cast<sqzc3d_num_t>(
            (static_cast<double>(raw) - analog_offset) * scale * meta.analog_general_factor);
      }
    }
  }
  return sqzc3d_STATUS_SUCCESS;
}

}  // namespace sqzc3d
#else

namespace sqzc3d {

struct C3dReadSource {};
struct C3dParameterTree {};

C3dStreamReader::C3dStreamReader() = default;
C3dStreamReader::~C3dStreamReader() = default;

sqzc3d_status sqzc3d_c3d_stream_open_file(
    C3dStreamReader* reader,
    const char* file_path) {
  (void)reader;
  (void)file_path;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

sqzc3d_status sqzc3d_c3d_stream_open_memory(
    C3dStreamReader* reader,
    const void* data,
    std::size_t n_bytes) {
  (void)reader;
  (void)data;
  (void)n_bytes;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

void sqzc3d_c3d_stream_close(C3dStreamReader* reader) {
  if (reader) {
    reader->source.reset();
    reader->parameter_tree.reset();
    reader->meta = {};
    reader->point_labels.clear();
    reader->analog_labels.clear();
    reader->type_group_names.clear();
    reader->type_group_starts.clear();
    reader->type_group_indices.clear();
    reader->read_scratch.clear();
  }
}

bool sqzc3d_c3d_stream_is_open(const C3dStreamReader* reader) {
  return reader && reader->source != nullptr;
}

sqzc3d_status sqzc3d_c3d_stream_meta_tree_json(
    const C3dStreamReader* reader,
    std::int64_t point_frames_override,
    std::string* out_json) {
  (void)reader;
  (void)point_frames_override;
  (void)out_json;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
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
    int out_valid_nscalar,
    std::vector<std::uint8_t>* scratch) {
  (void)reader;
  (void)frame_idx;
  (void)point_indices;
  (void)n_points_sel;
  (void)out_target;
  (void)out_nscalar;
  (void)out_valid;
  (void)out_valid_nscalar;
  (void)scratch;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_xyz_residual_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* point_indices,
    int n_points_sel,
    sqzc3d_num_t* out_target,
    int out_nscalar,
    unsigned char* out_valid,
    int out_valid_nscalar,
    sqzc3d_num_t* out_residual,
    int out_residual_nscalar,
    std::vector<std::uint8_t>* scratch) {
  (void)reader;
  (void)frame_idx;
  (void)point_indices;
  (void)n_points_sel;
  (void)out_target;
  (void)out_nscalar;
  (void)out_valid;
  (void)out_valid_nscalar;
  (void)out_residual;
  (void)out_residual_nscalar;
  (void)scratch;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_residual_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* point_indices,
    int n_points_sel,
    sqzc3d_num_t* out_residual,
    int out_nscalar,
    std::vector<std::uint8_t>* scratch) {
  (void)reader;
  (void)frame_idx;
  (void)point_indices;
  (void)n_points_sel;
  (void)out_residual;
  (void)out_nscalar;
  (void)scratch;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_all_xyz(
    C3dStreamReader* reader,
    int frame_idx,
    sqzc3d_num_t* out_target,
    int out_nscalar,
    std::vector<std::uint8_t>* scratch) {
  (void)reader;
  (void)frame_idx;
  (void)out_target;
  (void)out_nscalar;
  (void)scratch;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

sqzc3d_status sqzc3d_c3d_stream_read_frame_all_xyz_residual(
    C3dStreamReader* reader,
    int frame_idx,
    sqzc3d_num_t* out_target,
    int out_nscalar,
    unsigned char* out_valid,
    int out_valid_nscalar,
    sqzc3d_num_t* out_residual,
    int out_residual_nscalar,
    std::vector<std::uint8_t>* scratch) {
  (void)reader;
  (void)frame_idx;
  (void)out_target;
  (void)out_nscalar;
  (void)out_valid;
  (void)out_valid_nscalar;
  (void)out_residual;
  (void)out_residual_nscalar;
  (void)scratch;
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
    int out_valid_nscalar,
    std::vector<std::uint8_t>* scratch) {
  (void)reader;
  (void)point_indices;
  (void)n_points_sel;
  (void)start_frame;
  (void)end_frame;
  (void)out_traj;
  (void)out_nscalar;
  (void)out_valid;
  (void)out_valid_nscalar;
  (void)scratch;
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
    int out_nscalar,
    std::vector<std::uint8_t>* scratch) {
  (void)reader;
  (void)frame_idx;
  (void)analog_indices;
  (void)n_analog_sel;
  (void)start_sample;
  (void)n_samples;
  (void)out_analog;
  (void)out_nscalar;
  (void)scratch;
  return static_cast<sqzc3d_status>(sqzc3d_STATUS_NOT_IMPLEMENTED);
}

}  // namespace sqzc3d
#endif

