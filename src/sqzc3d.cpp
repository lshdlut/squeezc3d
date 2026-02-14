#include "sqzc3d.h"

#include "sqzc3d_c3d_stream.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <ostream>
#include <sstream>
#include <filesystem>
#include <string>
#include <vector>

#ifdef sqzc3d_DEBUG_BUNDLE
#  include <iostream>
#  define sqzc3d_DEBUG_BUNDLE_LOG(MSG) std::cerr << "[sqzc3d_load_bundle] " << MSG << std::endl
#else
#  define sqzc3d_DEBUG_BUNDLE_LOG(MSG) ((void)0)
#endif

namespace {

using C3dStreamReader = sqzc3d::C3dStreamReader;

struct sqzc3dDec {
  struct ErrorDetail {
    int status = sqzc3d_STATUS_SUCCESS;
    std::string api;
    std::string section;
    int index = -1;
    std::string message;
  };

  std::unique_ptr<C3dStreamReader> reader;
  std::string last_error;
  std::string temp_path;
  ErrorDetail last_error_detail;
  int label_norm = sqzc3d_LABEL_NORM_EXACT | sqzc3d_LABEL_NORM_TRIM;
};

struct sqzc3dChunk {
  int n_frames = 0;
  int n_points = 0;
  int n_points_total = 0;
  int n_analogs = 0;
  int n_analog_by_frame = 0;

  int n_scalar = 0;
  int valid_nscalar = 0;
  int n_analog_scalar = 0;

  int points_layout = sqzc3d_POINTS_LAYOUT_FRAME_MAJOR;
  int read_policy = sqzc3d_READ_POLICY_AUTO;
  int points_pack = sqzc3d_POINTS_PACK_AOS_XYZ_VALID;
  int valid_policy = sqzc3d_VALID_POLICY_FINITE_XYZ;

  double residual_gate_mm = 0.0;
  double point_scale = 1.0;
  double header_scale = 1.0;

  std::vector<sqzc3d_num_t> points_xyz_storage;
  std::vector<unsigned char> points_valid_storage;
  std::vector<sqzc3d_num_t> analog_storage;
  std::vector<unsigned char> analog_valid_storage;
  std::vector<std::string> point_labels_storage;
  std::vector<std::string> analog_labels_storage;
  std::vector<const char*> point_label_ptrs;
  std::vector<const char*> analog_label_ptrs;
  std::vector<sqzc3d_byte_t> raw_params;
  std::string reason;
  int label_norm = sqzc3d_LABEL_NORM_EXACT | sqzc3d_LABEL_NORM_TRIM;
};

static bool is_finite(sqzc3d_num_t value) {
  return value == value && value != std::numeric_limits<sqzc3d_num_t>::infinity() &&
         value != -std::numeric_limits<sqzc3d_num_t>::infinity();
}

static std::string trim(const std::string& src) {
  auto begin = src.begin();
  auto end = src.end();
  while (begin != end && isspace(static_cast<unsigned char>(*begin))) {
    ++begin;
  }
  while (begin != end && isspace(static_cast<unsigned char>(*(end - 1)))) {
    --end;
  }
  return std::string(begin, end);
}

static std::string normalize_label(const char* s, int mode) {
  if (!s) return {};
  std::string out(s);
  if (mode & sqzc3d_LABEL_NORM_TRIM) {
    out = trim(out);
  }
  if (mode & sqzc3d_LABEL_NORM_CASEFOLD_WS) {
    std::string collapsed;
    collapsed.reserve(out.size());
    bool prev_ws = false;
    for (unsigned char ch : out) {
      if (isspace(ch)) {
        if (!prev_ws) collapsed.push_back(' ');
        prev_ws = true;
      } else {
        collapsed.push_back(static_cast<char>(tolower(ch)));
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

static const char* kInvalidDecoderError = "invalid decoder";
static const char* kArgError = "invalid argument";

static std::string json_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 16);
  for (const unsigned char ch : text) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(static_cast<char>(ch));
        break;
    }
  }
  return out;
}

static const char* points_layout_name(int value) {
  return (value == sqzc3d_POINTS_LAYOUT_FRAME_MAJOR) ? "frame_major" : "unknown";
}

static const char* points_pack_name(int value) {
  return (value == sqzc3d_POINTS_PACK_AOS_XYZ_VALID) ? "aos_xyz_valid" : "unknown";
}

static const char* read_policy_name(int value) {
  if (value == sqzc3d_READ_POLICY_DENSE) return "dense";
  if (value == sqzc3d_READ_POLICY_SPARSE) return "sparse";
  return "auto";
}

static void skip_ws(const std::string& text, size_t& pos) {
  while (pos < text.size() && isspace(static_cast<unsigned char>(text[pos]))) ++pos;
}

static bool parse_json_key_colon(
    const std::string& text, const char* key, size_t begin_pos, size_t& colon_pos) {
  if (!key) return false;
  const std::string target_key = std::string("\"") + key + "\"";
  bool in_str = false;
  bool escape = false;
  size_t i = begin_pos;
  while (i < text.size()) {
    const char c = text[i];
    if (in_str) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_str = false;
      }
      ++i;
      continue;
    }
    if (c != '"') {
      ++i;
      continue;
    }
    ++i;
    std::string found_key;
    bool found_escape = false;
    while (i < text.size()) {
      const char qc = text[i++];
      if (found_escape) {
        found_key.push_back(qc);
        found_escape = false;
      } else if (qc == '\\') {
        found_key.push_back(qc);
        found_escape = true;
      } else if (qc == '"') {
        break;
      } else {
        found_key.push_back(qc);
      }
    }
    if (i >= text.size()) return false;
    size_t pos = i;
    skip_ws(text, pos);
    if (pos < text.size() && text[pos] == ':' &&
        found_key == target_key.substr(1, target_key.size() - 2)) {
      colon_pos = pos;
      return true;
    }
  }
  return false;
}

static bool parse_quoted_string(const std::string& text, size_t& pos, std::string& out) {
  if (pos >= text.size() || text[pos] != '"') return false;
  ++pos;
  std::string value;
  bool escape = false;
  while (pos < text.size()) {
    const char c = text[pos++];
    if (escape) {
      if (c == '"') value.push_back('"');
      else if (c == '\\') value.push_back('\\');
      else if (c == '/') value.push_back('/');
      else if (c == 'b') value.push_back('\b');
      else if (c == 'f') value.push_back('\f');
      else if (c == 'n') value.push_back('\n');
      else if (c == 'r') value.push_back('\r');
      else if (c == 't') value.push_back('\t');
      else return false;
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      out = std::move(value);
      return true;
    }
    value.push_back(c);
  }
  return false;
}

static bool parse_bundle_name_value(
    const std::string& text, const char* key, std::string* out_value, size_t begin_pos = 0) {
  size_t colon_pos = 0;
  if (!parse_json_key_colon(text, key, begin_pos, colon_pos)) return false;
  if (colon_pos == std::string::npos) return false;
  size_t pos = colon_pos + 1;
  skip_ws(text, pos);
  return parse_quoted_string(text, pos, *out_value);
}

static bool parse_bundle_int_value(
    const std::string& text, const char* key, int64_t* out_value, size_t begin_pos = 0) {
  size_t colon_pos = 0;
  if (!parse_json_key_colon(text, key, begin_pos, colon_pos)) return false;
  if (colon_pos == std::string::npos) return false;
  size_t pos = colon_pos + 1;
  skip_ws(text, pos);
  if (pos >= text.size()) return false;
  size_t end = pos;
  while (end < text.size() && (text[end] == '-' || text[end] == '+' ||
                               text[end] == '.' || isdigit(static_cast<unsigned char>(text[end])) ||
                               text[end] == 'e' || text[end] == 'E')) {
    ++end;
  }
  if (end == pos) return false;
  try {
    *out_value = std::stoll(text.substr(pos, end - pos));
  } catch (...) {
    return false;
  }
  return true;
}

static bool parse_bundle_uint64_value(
    const std::string& text, const char* key, std::uint64_t* out_value, size_t begin_pos = 0) {
  size_t colon_pos = 0;
  if (!parse_json_key_colon(text, key, begin_pos, colon_pos)) return false;
  if (colon_pos == std::string::npos) return false;
  size_t pos = colon_pos + 1;
  skip_ws(text, pos);
  if (pos >= text.size()) return false;
  size_t end = pos;
  while (end < text.size() && isdigit(static_cast<unsigned char>(text[end]))) {
    ++end;
  }
  if (end == pos) return false;
  try {
    *out_value = std::stoull(text.substr(pos, end - pos));
  } catch (...) {
    return false;
  }
  return true;
}

static bool parse_bundle_double_value(
    const std::string& text, const char* key, double* out_value, size_t begin_pos = 0) {
  size_t colon_pos = 0;
  if (!parse_json_key_colon(text, key, begin_pos, colon_pos)) return false;
  if (colon_pos == std::string::npos) return false;
  size_t pos = colon_pos + 1;
  skip_ws(text, pos);
  if (pos >= text.size()) return false;
  size_t end = pos;
  while (end < text.size() && (text[end] == '-' || text[end] == '+' ||
                               text[end] == '.' || isdigit(static_cast<unsigned char>(text[end])) ||
                               text[end] == 'e' || text[end] == 'E')) {
    ++end;
  }
  if (end == pos) return false;
  try {
    *out_value = std::stod(text.substr(pos, end - pos));
  } catch (...) {
    return false;
  }
  return true;
}

static bool parse_bundle_string_list(
    const std::string& text, const char* key, std::vector<std::string>& out_values) {
  out_values.clear();
  size_t colon_pos = 0;
  if (!parse_json_key_colon(text, key, 0, colon_pos)) return false;
  if (colon_pos == std::string::npos) return false;
  size_t pos = colon_pos + 1;
  skip_ws(text, pos);
  if (pos >= text.size() || text[pos] != '[') return false;
  ++pos;
  while (pos < text.size()) {
    skip_ws(text, pos);
    if (pos >= text.size()) return false;
    if (text[pos] == ']') return true;
    std::string token;
    if (!parse_quoted_string(text, pos, token)) return false;
    out_values.push_back(token);
    skip_ws(text, pos);
    if (pos < text.size() && text[pos] == ',') {
      ++pos;
      continue;
    }
    if (pos < text.size() && text[pos] == ']') return true;
    return false;
  }
  return false;
}

struct BundleSection {
  bool present = false;
  std::uint64_t offset = 0;
  std::uint64_t count = 0;
  std::uint64_t byte_size = 0;
  std::uint64_t checksum = 0;
  bool checksum_present = false;
  std::string dtype;
};

static bool extract_object_by_key(
    const std::string& text, const char* key, size_t start_pos, std::string* out) {
  size_t colon_pos = 0;
  if (!parse_json_key_colon(text, key, start_pos, colon_pos)) return false;
  if (colon_pos == std::string::npos) return false;
  size_t brace_pos = text.find('{', colon_pos);
  if (brace_pos == std::string::npos) return false;

  int depth = 0;
  bool in_str = false;
  bool escape = false;
  for (size_t i = brace_pos; i < text.size(); ++i) {
    const char c = text[i];
    if (escape) {
      escape = false;
    } else if (c == '\\') {
      escape = true;
    } else if (c == '"') {
      in_str = !in_str;
    } else if (!in_str) {
      if (c == '{') {
        ++depth;
      } else if (c == '}') {
        --depth;
        if (depth == 0) {
          *out = text.substr(brace_pos, i - brace_pos + 1);
          return true;
        }
      }
    }
  }
  return false;
}

static bool parse_bundle_section(
    const std::string& text, const char* section_name, BundleSection* out_section) {
  size_t sections_pos = 0;
  if (!parse_json_key_colon(text, "sections", 0, sections_pos)) return false;
  if (sections_pos == std::string::npos) return false;
  size_t section_key_pos = 0;
  if (!parse_json_key_colon(text, section_name, sections_pos, section_key_pos)) return false;
  if (section_key_pos == std::string::npos) return false;
  const size_t colon_pos = section_key_pos;
  if (colon_pos == std::string::npos) return false;

  size_t pos = colon_pos + 1;
  skip_ws(text, pos);
  if (pos >= text.size()) return false;
  if (text.find("null", pos) == pos) {
    out_section->present = false;
    out_section->offset = 0;
    out_section->count = 0;
    out_section->dtype.clear();
    return true;
  }
  if (text[pos] != '{') return false;
  std::string obj_text;
  if (!extract_object_by_key(text, section_name, sections_pos, &obj_text)) return false;

  int64_t offset = 0;
  int64_t count = 0;
  std::uint64_t byte_size = 0;
  std::uint64_t checksum = 0;
  bool checksum_present = false;
  if (!parse_bundle_int_value(obj_text, "offset", &offset)) return false;
  if (!parse_bundle_int_value(obj_text, "count", &count)) return false;
  std::string dtype;
  if (!parse_bundle_name_value(obj_text, "dtype", &dtype)) return false;
  if (parse_bundle_uint64_value(obj_text, "byte_size", &byte_size)) {
    out_section->byte_size = byte_size;
  } else {
    out_section->byte_size = 0;
  }
  if (parse_bundle_uint64_value(obj_text, "checksum", &checksum)) {
    out_section->checksum = checksum;
    checksum_present = true;
  }
  if (offset < 0 || count < 0) return false;
  out_section->present = true;
  out_section->offset = static_cast<std::uint64_t>(offset);
  out_section->count = static_cast<std::uint64_t>(count);
  out_section->dtype = std::move(dtype);
  out_section->checksum = checksum;
  out_section->checksum_present = checksum_present;
  return true;
}

static bool parse_bundle_optional_int_value(
    const std::string& text,
    const char* key,
    int& out_value,
    int default_value,
    size_t begin_pos = 0) {
  int64_t value = 0;
  if (!parse_bundle_int_value(text, key, &value, begin_pos)) {
    out_value = default_value;
    return false;
  }
  out_value = static_cast<int>(value);
  return true;
}

static bool parse_bundle_optional_name_value(
    const std::string& text,
    const char* key,
    std::string* out_value,
    size_t begin_pos = 0) {
  if (!out_value) return false;
  if (!parse_bundle_name_value(text, key, out_value, begin_pos)) {
    out_value->clear();
    return false;
  }
  return true;
}

static bool parse_bundle_schema_version(
    const std::string& text,
    const bool strict,
    int64_t* out_schema_version,
    bool* out_has_schema) {
  if (!out_schema_version || !out_has_schema) return false;
  int64_t value = 0;
  const bool has_schema = parse_bundle_int_value(text, "schema_version", &value);
  if (!has_schema) {
    if (strict) {
      return false;
    }
    *out_schema_version = 0;
    *out_has_schema = false;
    return true;
  }
  if (value < 0) return false;
  *out_schema_version = value;
  *out_has_schema = true;
  return true;
}

static bool parse_bundle_section_if_present(
    const std::string& text, const char* section_name, BundleSection* out_section) {
  if (out_section) {
    *out_section = BundleSection{};
  }
  size_t sections_pos = 0;
  if (!parse_json_key_colon(text, "sections", 0, sections_pos)) return false;
  if (sections_pos == std::string::npos) return false;
  size_t section_key_pos = 0;
  if (!parse_json_key_colon(text, section_name, sections_pos, section_key_pos)) {
    if (out_section) {
      out_section->present = false;
      out_section->offset = 0;
      out_section->count = 0;
      out_section->dtype.clear();
    }
    return true;
  }
  if (section_key_pos == std::string::npos) {
    if (out_section) {
      out_section->present = false;
      out_section->offset = 0;
      out_section->count = 0;
      out_section->dtype.clear();
    }
    return true;
  }
  return parse_bundle_section(text, section_name, out_section);
}

static bool is_sqzc3d_container_magic(std::istream& in) {
  char magic[8]{};
  in.read(magic, 8);
  if (!in.good()) return false;
  const char kExpected[8] = {'S', 'Q', 'C', '3', 'D', '0', '1', '\0'};
  return std::memcmp(magic, kExpected, sizeof(kExpected)) == 0;
}

static bool write_sic_bundle_magic(std::ostream& out) {
  const char magic[8] = {'S', 'Q', 'C', '3', 'D', '0', '1', '\0'};
  out.write(magic, 8);
  return !!out.good();
}

static std::size_t write_little_u64(std::ostream& out, std::uint64_t value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(value));
  return out.good() ? sizeof(value) : 0u;
}

static bool read_little_u64(std::istream& in, std::uint64_t* out) {
  if (!out) return false;
  in.read(reinterpret_cast<char*>(out), sizeof(*out));
  return in.good();
}

static bool write_u64_le(std::ostream& out, std::uint64_t value) {
  return write_little_u64(out, value) == sizeof(value);
}

static std::uint64_t checksum_fnv1a_64(const void* data, std::size_t nbytes) {
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  std::uint64_t h = 14695981039346656037ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  for (std::size_t i = 0; i < nbytes; ++i) {
    h ^= static_cast<std::uint64_t>(bytes[i]);
    h *= prime;
  }
  return h;
}

static std::uint64_t dtype_bytes(const std::string& dtype) {
  if (dtype == "float64") return sizeof(sqzc3d_num_t);
  if (dtype == "uint8") return sizeof(std::uint8_t);
  return 0;
}

template <typename T>
static std::uint64_t section_count_max() {
  return std::numeric_limits<std::size_t>::max() / sizeof(T);
}

static int parse_points_layout_name(const std::string& name) {
  if (name == "frame_major") return sqzc3d_POINTS_LAYOUT_FRAME_MAJOR;
  return -1;
}

static int parse_points_pack_name(const std::string& name) {
  if (name == "aos_xyz_valid") return sqzc3d_POINTS_PACK_AOS_XYZ_VALID;
  return -1;
}

static int parse_read_policy_name_to_enum(const std::string& name) {
  if (name == "dense") return sqzc3d_READ_POLICY_DENSE;
  if (name == "sparse") return sqzc3d_READ_POLICY_SPARSE;
  return sqzc3d_READ_POLICY_AUTO;
}

static void write_num(std::ostream& out, const void* ptr, std::size_t bytes) {
  out.write(reinterpret_cast<const char*>(ptr), static_cast<std::streamsize>(bytes));
}

static bool cast_metadata_int(int64_t value, int* out) {
  if (!out) return false;
  if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
    return false;
  }
  *out = static_cast<int>(value);
  return true;
}

template <typename T>
static bool read_section_data(
    std::ifstream& data_in,
    const BundleSection& section,
    const char* expected_dtype,
    bool strict,
    const uintmax_t file_size,
    const std::uint64_t base_offset,
    std::vector<T>& out) {
  if (!expected_dtype || section.dtype != expected_dtype) return false;
  if (strict) {
    if (section.byte_size == 0u && section.count > 0u) return false;
    if (section.count > section_count_max<T>()) {
      return false;
    }
    const std::uint64_t expected_bytes =
        static_cast<std::uint64_t>(section.count) * static_cast<std::uint64_t>(sizeof(T));
    if (section.byte_size != 0u && section.byte_size != expected_bytes) return false;
    if (section.count > 0u && !section.checksum_present) return false;
  }
  if (section.count == 0) {
    out.clear();
    return true;
  }
  const auto max_count = std::numeric_limits<size_t>::max() / sizeof(T);
  if (section.count > max_count) return false;
  const uintmax_t bytes = static_cast<uintmax_t>(section.count) * static_cast<uintmax_t>(sizeof(T));
  const auto absolute_offset = base_offset + section.offset;
  if (absolute_offset > std::numeric_limits<uintmax_t>::max() - bytes) return false;
  if (absolute_offset + bytes > file_size) return false;
  if (absolute_offset > static_cast<uintmax_t>(std::numeric_limits<std::streamoff>::max())) return false;
  if (bytes > std::numeric_limits<std::streamsize>::max()) return false;
  data_in.seekg(static_cast<std::streamoff>(absolute_offset), std::ios::beg);
  if (!data_in.good()) return false;
  out.resize(static_cast<size_t>(section.count));
  data_in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(bytes));
  if (!data_in && static_cast<std::streamsize>(bytes) != data_in.gcount()) {
    return false;
  }
  return true;
}

static void set_error(
    sqzc3d_dec_t* dec,
    const std::string& message,
    const char* api = nullptr,
    const char* section = nullptr,
    int index = -1) {
  if (!dec) return;
  auto* impl = static_cast<sqzc3dDec*>(dec->impl);
  if (!impl) return;
  impl->last_error = message;
  impl->last_error_detail.status = sqzc3d_STATUS_INVALID_ARGUMENT;
  impl->last_error_detail.api = api ? api : "";
  impl->last_error_detail.section = section ? section : "";
  impl->last_error_detail.index = index;
  impl->last_error_detail.message = message;
  dec->last_error = impl->last_error.c_str();
}

static void reset_error(sqzc3d_dec_t* dec) {
  if (!dec) return;
  auto* impl = static_cast<sqzc3dDec*>(dec->impl);
  if (!impl) return;
  impl->last_error.clear();
  impl->last_error_detail = {};
  impl->last_error_detail.status = sqzc3d_STATUS_SUCCESS;
  dec->last_error = impl->last_error.c_str();
}

static bool normalize_range(int n_total, int start, int count, int* out_start, int* out_count) {
  if (n_total < 0) return false;
  if (start < 0) start += n_total;
  if (count < 0) count = n_total - start;
  if (start < 0) start = 0;
  if (start > n_total) start = n_total;
  if (count < 0) count = 0;
  if (start + count > n_total) count = n_total - start;
  if (out_start) *out_start = start;
  if (out_count) *out_count = count;
  return count >= 0;
}

static int map_labels_to_indices(
    const std::vector<std::string>& haystack,
    const char* const* labels,
    int n_labels,
    std::vector<int>& out_indices,
    int miss_value,
    int norm_mode) {
  if (n_labels < 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (n_labels > 0 && !labels) return sqzc3d_STATUS_INVALID_ARGUMENT;
  out_indices.assign(static_cast<std::size_t>(n_labels), miss_value);
  for (int i = 0; i < n_labels; ++i) {
    if (!labels[i]) return sqzc3d_STATUS_INVALID_ARGUMENT;
    const auto target = normalize_label(labels[i], norm_mode);
    for (int j = 0; j < static_cast<int>(haystack.size()); ++j) {
      if (normalize_label(haystack[j], norm_mode) == target) {
        out_indices[static_cast<std::size_t>(i)] = j;
        break;
      }
    }
    if (out_indices[static_cast<std::size_t>(i)] == miss_value) return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  return sqzc3d_STATUS_SUCCESS;
}

static int build_point_selection(
    const sqzc3d_build_opt_t* opt,
    const std::vector<std::string>& source_point_labels,
    int n_points_total,
    std::vector<int>& out_indices,
    int label_norm) {
  out_indices.clear();
  if (opt->point_sel_mode == sqzc3d_POINT_SEL_ALL || opt->point_sel_count == 0) {
    out_indices.resize(static_cast<std::size_t>(n_points_total));
    for (int i = 0; i < n_points_total; ++i) out_indices[static_cast<std::size_t>(i)] = i;
    return sqzc3d_STATUS_SUCCESS;
  }
  if (opt->point_sel_mode == sqzc3d_POINT_SEL_INDICES) {
    if (!opt->point_sel || opt->point_sel_count <= 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
    out_indices.assign(opt->point_sel, opt->point_sel + opt->point_sel_count);
  } else if (opt->point_sel_mode == sqzc3d_POINT_SEL_LABELS) {
    const int label_count =
        opt->point_labels_count > 0 ? opt->point_labels_count : opt->point_sel_count;
    if (label_count <= 0 || !opt->point_labels) return sqzc3d_STATUS_INVALID_ARGUMENT;
    return map_labels_to_indices(
        source_point_labels, opt->point_labels, label_count, out_indices, -1, label_norm);
  } else {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  for (const int idx : out_indices) {
    if (idx < 0 || idx >= n_points_total) return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  return sqzc3d_STATUS_SUCCESS;
}

static int build_analog_selection(
    const sqzc3d_build_opt_t* opt,
    const std::vector<std::string>& source_analog_labels,
    int n_analogs_total,
    std::vector<int>& out_indices,
    int label_norm) {
  out_indices.clear();
  if (n_analogs_total <= 0 || opt->analog_enable == sqzc3d_ANALOG_EN_OFF) return sqzc3d_STATUS_SUCCESS;

  if (opt->analog_sel_mode == sqzc3d_ANALOG_SEL_ALL || opt->analog_sel_count == 0) {
    out_indices.resize(static_cast<std::size_t>(n_analogs_total));
    for (int i = 0; i < n_analogs_total; ++i) out_indices[static_cast<std::size_t>(i)] = i;
    return sqzc3d_STATUS_SUCCESS;
  }
  if (opt->analog_sel_mode == sqzc3d_ANALOG_SEL_INDICES) {
    if (!opt->analog_sel || opt->analog_sel_count <= 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
    out_indices.assign(opt->analog_sel, opt->analog_sel + opt->analog_sel_count);
    for (const int idx : out_indices) {
      if (idx < 0 || idx >= n_analogs_total) return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
    return sqzc3d_STATUS_SUCCESS;
  }
  if (opt->analog_sel_mode == sqzc3d_ANALOG_SEL_LABELS) {
    const int label_count =
        opt->analog_labels_count > 0 ? opt->analog_labels_count : opt->analog_sel_count;
    if (label_count <= 0 || !opt->analog_labels) return sqzc3d_STATUS_INVALID_ARGUMENT;
    return map_labels_to_indices(
        source_analog_labels, opt->analog_labels, label_count, out_indices, -1, label_norm);
  }
  return sqzc3d_STATUS_INVALID_ARGUMENT;
}

static int select_read_policy(
    const sqzc3d_build_opt_t* opt,
    int n_points_total,
    int n_points_sel,
    int64_t span_bytes,
    int64_t io_buffer_bytes,
    std::string* out_reason = nullptr) {
  const auto set_reason = [out_reason](const char* reason) {
    if (out_reason) {
      *out_reason = reason ? reason : "";
    }
  };
  if (!opt) return sqzc3d_READ_POLICY_AUTO;
  if (opt->read_policy == sqzc3d_READ_POLICY_SPARSE) {
    set_reason("read_policy_sparse_explicit");
    return sqzc3d_READ_POLICY_SPARSE;
  }
  if (opt->read_policy == sqzc3d_READ_POLICY_DENSE) {
    set_reason("read_policy_dense_explicit");
    return sqzc3d_READ_POLICY_DENSE;
  }
  if (n_points_sel >= n_points_total) {
    set_reason("read_policy_auto_dense_all_points");
    return sqzc3d_READ_POLICY_DENSE;
  }
  if (n_points_total <= 0) {
    set_reason("read_policy_auto_sparse_empty_total");
    return sqzc3d_READ_POLICY_SPARSE;
  }
  const double ratio = static_cast<double>(n_points_sel) / static_cast<double>(n_points_total);
  if (ratio >= opt->dense_threshold_ratio) {
    set_reason("read_policy_auto_dense_ratio");
    return sqzc3d_READ_POLICY_DENSE;
  }
  if (io_buffer_bytes > 0 && span_bytes > 0 &&
      span_bytes <= 2 * static_cast<int64_t>(io_buffer_bytes)) {
    set_reason("read_policy_auto_dense_span");
    return sqzc3d_READ_POLICY_DENSE;
  }
  set_reason("read_policy_auto_sparse");
  return sqzc3d_READ_POLICY_SPARSE;
}

}  // namespace

sqzc3d_API void sqzc3d_default_open_opt(sqzc3d_open_opt_t* out_opt) {
  if (!out_opt) return;
  out_opt->struct_size = static_cast<int>(sizeof(*out_opt));
  out_opt->use_ezc3d_params = 1;
  out_opt->preserve_raw_params = 0;
  out_opt->open_mode = sqzc3d_FILE;
  out_opt->enable_mmap = 1;
  out_opt->cache_labels = 1;
  out_opt->label_norm = sqzc3d_LABEL_NORM_EXACT | sqzc3d_LABEL_NORM_TRIM;
  out_opt->reserved = 0;
}

sqzc3d_API void sqzc3d_default_build_opt(sqzc3d_build_opt_t* out_opt) {
  if (!out_opt) return;
  out_opt->struct_size = static_cast<int>(sizeof(*out_opt));
  out_opt->frame_range = {0, -1};
  out_opt->point_sel_mode = sqzc3d_POINT_SEL_ALL;
  out_opt->point_sel = nullptr;
  out_opt->point_sel_count = 0;
  out_opt->point_labels = nullptr;
  out_opt->point_labels_count = 0;
  out_opt->analog_enable = sqzc3d_ANALOG_EN_AUTO;
  out_opt->analog_range = {0, -1};
  out_opt->analog_sel_mode = sqzc3d_ANALOG_SEL_ALL;
  out_opt->analog_sel = nullptr;
  out_opt->analog_sel_count = 0;
  out_opt->analog_labels = nullptr;
  out_opt->analog_labels_count = 0;
  out_opt->points_layout = sqzc3d_POINTS_LAYOUT_FRAME_MAJOR;
  out_opt->points_pack = sqzc3d_POINTS_PACK_AOS_XYZ_VALID;
  out_opt->valid_policy = sqzc3d_VALID_POLICY_FINITE_XYZ;
  out_opt->residual_gate_mm = 0.0;
  out_opt->read_policy = sqzc3d_READ_POLICY_AUTO;
  out_opt->dense_threshold_ratio = 0.25;
  out_opt->io_buffer_bytes = 4u << 20;
  out_opt->analog_size_soft_limit_bytes = 64LL << 20;
}

sqzc3d_API void sqzc3d_default_bundle_load_opt(sqzc3d_bundle_load_opt_t* out_opt) {
  if (!out_opt) return;
  out_opt->struct_size = static_cast<int>(sizeof(*out_opt));
  out_opt->strict = 1;
  out_opt->reserved = 0;
}

sqzc3d_API int sqzc3d_get_features(void) {
  int features = SQZC3D_FEATURE_BUNDLE;
#if sqzc3d_WITH_EZC3D
  features |= SQZC3D_FEATURE_OPEN_FILE | SQZC3D_FEATURE_OPEN_MEMORY | SQZC3D_FEATURE_BUILD_CHUNKS;
#endif
#if sqzc3d_WITH_EZC3D
  features |= SQZC3D_FEATURE_ANALOG;
#endif
  return features;
}

sqzc3d_API void sqzc3d_default_error_detail(sqzc3d_error_detail_t* out_detail) {
  if (!out_detail) return;
  out_detail->status = sqzc3d_STATUS_SUCCESS;
  out_detail->api = "";
  out_detail->section = "";
  out_detail->index = -1;
  out_detail->message = "";
}

sqzc3d_API int sqzc3d_last_error_detail(
    const sqzc3d_dec_t* dec,
    sqzc3d_error_detail_t* out_detail) {
  if (!out_detail) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (!dec || !dec->impl) return sqzc3d_STATUS_INVALID_ARGUMENT;
  const auto* impl = static_cast<const sqzc3dDec*>(dec->impl);
  out_detail->status = impl->last_error_detail.status;
  out_detail->api = impl->last_error_detail.api.empty() ? "" : impl->last_error_detail.api.c_str();
  out_detail->section = impl->last_error_detail.section.empty() ? "" : impl->last_error_detail.section.c_str();
  out_detail->index = impl->last_error_detail.index;
  out_detail->message = impl->last_error_detail.message.empty() ? "" : impl->last_error_detail.message.c_str();
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_open_file(
    sqzc3d_dec_t** out_dec,
    const char* file_path,
    const sqzc3d_open_opt_t* opt) {
#if sqzc3d_WITH_EZC3D == 0
  (void)opt;
  if (out_dec) *out_dec = nullptr;
  return sqzc3d_STATUS_NOT_IMPLEMENTED;
#endif
  if (!out_dec || !file_path) return sqzc3d_STATUS_INVALID_ARGUMENT;
  sqzc3d_open_opt_t opt_v{};
  if (!opt) {
    sqzc3d_default_open_opt(&opt_v);
    opt = &opt_v;
  }
  if (opt->struct_size > 0 && opt->struct_size < static_cast<int>(sizeof(sqzc3d_open_opt_t))) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  *out_dec = nullptr;
  auto* dec = new (std::nothrow) sqzc3d_dec_t{};
  if (!dec) return sqzc3d_STATUS_INTERNAL_ERROR;
  dec->impl = nullptr;
  dec->last_error = kArgError;
  auto impl = std::make_unique<sqzc3dDec>();
  impl->reader = std::make_unique<C3dStreamReader>();
  const int label_norm = (opt->label_norm > 0)
                            ? (opt->label_norm & (sqzc3d_LABEL_NORM_EXACT |
                                                 sqzc3d_LABEL_NORM_TRIM |
                                                 sqzc3d_LABEL_NORM_CASEFOLD_WS))
                            : (sqzc3d_LABEL_NORM_EXACT | sqzc3d_LABEL_NORM_TRIM);
  impl->label_norm = label_norm;
  if (opt->open_mode != sqzc3d_FILE) {
    set_error(dec, "open_file called with non-file open_mode");
    delete dec;
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  if (sqzc3d::sqzc3d_c3d_stream_open_file(
          impl->reader.get(), file_path, opt->preserve_raw_params != 0) !=
      sqzc3d_STATUS_SUCCESS) {
    const std::string msg = "failed to open c3d file: " + std::string(file_path);
    set_error(dec, msg);
    delete dec;
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  dec->impl = impl.release();
  *out_dec = dec;
  if (opt->cache_labels == 0) {
    auto* impl_out = static_cast<sqzc3dDec*>(dec->impl);
    if (impl_out && impl_out->reader) {
      impl_out->reader->point_labels.clear();
      impl_out->reader->analog_labels.clear();
    }
  }
  reset_error(dec);
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_open_memory(
    sqzc3d_dec_t** out_dec,
    const void* data,
    int n_bytes,
    const sqzc3d_open_opt_t* opt) {
#if sqzc3d_WITH_EZC3D == 0
  (void)data;
  (void)n_bytes;
  (void)opt;
  if (out_dec) *out_dec = nullptr;
  return sqzc3d_STATUS_NOT_IMPLEMENTED;
#endif
  if (!out_dec || !data || n_bytes <= 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
  *out_dec = nullptr;
  sqzc3d_open_opt_t opt_v{};
  if (!opt) {
    sqzc3d_default_open_opt(&opt_v);
    opt = &opt_v;
  }
  if (opt->struct_size > 0 && opt->struct_size < static_cast<int>(sizeof(sqzc3d_open_opt_t))) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }

  const auto* bytes = static_cast<const char*>(data);
  const auto tmp_dir = std::filesystem::temp_directory_path();
  auto tmp_path = tmp_dir / ("sqzc3d_mem_" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + ".c3d");
  {
    std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
    ofs.write(bytes, static_cast<std::streamsize>(n_bytes));
    if (!ofs.good()) {
      std::error_code ec;
      std::filesystem::remove(tmp_path, ec);
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
  }

  const int st = sqzc3d_open_file(out_dec, tmp_path.string().c_str(), opt);
  if (st != sqzc3d_STATUS_SUCCESS) {
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
    return st;
  }
  auto* impl = static_cast<sqzc3dDec*>((*out_dec)->impl);
  impl->temp_path = tmp_path.string();
  const int label_norm = (opt->label_norm > 0)
                            ? (opt->label_norm & (sqzc3d_LABEL_NORM_EXACT |
                                                 sqzc3d_LABEL_NORM_TRIM |
                                                 sqzc3d_LABEL_NORM_CASEFOLD_WS))
                            : (sqzc3d_LABEL_NORM_EXACT | sqzc3d_LABEL_NORM_TRIM);
  impl->label_norm = label_norm;
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_close_dec(sqzc3d_dec_t* dec) {
  if (!dec) return sqzc3d_STATUS_SUCCESS;
  auto* impl = static_cast<sqzc3dDec*>(dec->impl);
  if (impl) {
    if (!impl->temp_path.empty()) {
      std::error_code ec;
      std::filesystem::remove(impl->temp_path, ec);
      impl->temp_path.clear();
    }
    if (impl->reader) sqzc3d::sqzc3d_c3d_stream_close(impl->reader.get());
    delete impl;
  }
  delete dec;
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API const char* sqzc3d_last_error(const sqzc3d_dec_t* dec) {
  if (!dec || !dec->impl) return kInvalidDecoderError;
  const auto* impl = static_cast<const sqzc3dDec*>(dec->impl);
  return impl->last_error.empty() ? "" : impl->last_error.c_str();
}

sqzc3d_API int sqzc3d_build_chunks(
    const sqzc3d_dec_t* dec,
    const sqzc3d_build_opt_t* opt,
    sqzc3d_chunk_t** out_chunk) {
#if sqzc3d_WITH_EZC3D == 0
  (void)dec;
  (void)opt;
  if (out_chunk) *out_chunk = nullptr;
  return sqzc3d_STATUS_NOT_IMPLEMENTED;
#endif
  if (opt->struct_size > 0 && opt->struct_size < static_cast<int>(sizeof(sqzc3d_build_opt_t))) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  if (!dec || !opt || !out_chunk) return sqzc3d_STATUS_INVALID_ARGUMENT;
  const auto* dec_impl = static_cast<const sqzc3dDec*>(dec->impl);
  if (!dec_impl || !dec_impl->reader || !dec_impl->reader->c3d) return sqzc3d_STATUS_INVALID_ARGUMENT;
  const auto& meta = dec_impl->reader->meta;

  int frame_start = opt->frame_range.start;
  int frame_count = opt->frame_range.count;
  if (!normalize_range(meta.n_frames, frame_start, frame_count, &frame_start, &frame_count)) {
    set_error(const_cast<sqzc3d_dec_t*>(dec), "invalid frame range");
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }

  int analog_range_count = opt->analog_range.count;
  int analog_range_start = opt->analog_range.start;
  if (!normalize_range(meta.n_frames, analog_range_start, analog_range_count, &analog_range_start, &analog_range_count)) {
    analog_range_count = frame_count;
  }

  auto chunk = new (std::nothrow) sqzc3d_chunk_t();
  if (!chunk) return sqzc3d_STATUS_INTERNAL_ERROR;
  std::memset(chunk, 0, sizeof(sqzc3d_chunk_t));
  chunk->struct_size = static_cast<int>(sizeof(*chunk));
  auto chunk_impl = std::make_unique<sqzc3dChunk>();

  std::vector<int> point_indices;
  std::vector<int> analog_indices;

  const int st_points =
      build_point_selection(opt, dec_impl->reader->point_labels, meta.n_points, point_indices, dec_impl->label_norm);
  if (st_points != sqzc3d_STATUS_SUCCESS) {
    set_error(const_cast<sqzc3d_dec_t*>(dec), "invalid point selection");
    delete chunk;
    return st_points;
  }
  const int st_analog = build_analog_selection(
      opt, dec_impl->reader->analog_labels, meta.n_analogs, analog_indices, dec_impl->label_norm);
  if (st_analog != sqzc3d_STATUS_SUCCESS) {
    set_error(const_cast<sqzc3d_dec_t*>(dec), "invalid analog selection");
    delete chunk;
    return st_analog;
  }

  int analog_enable = opt->analog_enable;
  if (analog_enable == sqzc3d_ANALOG_EN_AUTO) {
    const int effective_analog_frames = (analog_range_count < frame_count) ? analog_range_count : frame_count;
    if (analog_range_count != frame_count) {
      chunk_impl->reason = "analog_range_count clipped to frame_range count";
    }
    const long long projected_bytes = static_cast<long long>(effective_analog_frames) *
                                      static_cast<long long>(meta.n_analog_by_frame) *
      static_cast<long long>(static_cast<int>(analog_indices.size())) *
      static_cast<long long>(sizeof(sqzc3d_num_t));
    if (projected_bytes > opt->analog_size_soft_limit_bytes || analog_indices.empty()) analog_enable = sqzc3d_ANALOG_EN_OFF;
    if (analog_enable != sqzc3d_ANALOG_EN_OFF && projected_bytes > opt->analog_size_soft_limit_bytes) {
      chunk_impl->reason = "analog skipped by size gate";
      analog_enable = sqzc3d_ANALOG_EN_OFF;
    }
  }
  if (opt->analog_enable == sqzc3d_ANALOG_EN_OFF) analog_enable = sqzc3d_ANALOG_EN_OFF;
  if (meta.n_analogs <= 0) analog_enable = sqzc3d_ANALOG_EN_OFF;

  chunk_impl->n_frames = frame_count;
  chunk_impl->n_points_total = meta.n_points;
  chunk_impl->n_points = static_cast<int>(point_indices.size());
  chunk_impl->n_analog_by_frame = meta.n_analog_by_frame;
  chunk_impl->points_layout = opt->points_layout;
  chunk_impl->read_policy = sqzc3d_READ_POLICY_DENSE;
  chunk_impl->points_pack = opt->points_pack;
  chunk_impl->valid_policy = opt->valid_policy;
  chunk_impl->residual_gate_mm = opt->residual_gate_mm;
  chunk_impl->point_scale = meta.point_scale;
  chunk_impl->header_scale = meta.header_scale;
  chunk_impl->label_norm = dec_impl->label_norm;
  chunk_impl->raw_params = dec_impl->reader->raw_params;

  chunk_impl->point_labels_storage.reserve(point_indices.size());
  for (int idx : point_indices) {
    if (idx >= 0 && idx < static_cast<int>(dec_impl->reader->point_labels.size())) {
      chunk_impl->point_labels_storage.push_back(dec_impl->reader->point_labels[static_cast<std::size_t>(idx)]);
    } else {
      chunk_impl->point_labels_storage.push_back(std::string());
    }
  }
  if (!chunk_impl->point_labels_storage.empty()) {
    chunk_impl->point_label_ptrs.resize(chunk_impl->point_labels_storage.size());
    for (std::size_t i = 0; i < chunk_impl->point_labels_storage.size(); ++i) {
      chunk_impl->point_label_ptrs[static_cast<std::size_t>(i)] = chunk_impl->point_labels_storage[i].c_str();
    }
  }

  if (analog_enable != sqzc3d_ANALOG_EN_OFF) {
    chunk_impl->n_analogs = static_cast<int>(analog_indices.size());
    chunk_impl->analog_labels_storage.reserve(analog_indices.size());
    for (int idx : analog_indices) {
      if (idx >= 0 && idx < static_cast<int>(dec_impl->reader->analog_labels.size())) {
        chunk_impl->analog_labels_storage.push_back(dec_impl->reader->analog_labels[static_cast<std::size_t>(idx)]);
      }
    }
    if (!chunk_impl->analog_labels_storage.empty()) {
      chunk_impl->analog_label_ptrs.resize(chunk_impl->analog_labels_storage.size());
      for (std::size_t i = 0; i < chunk_impl->analog_labels_storage.size(); ++i) {
        chunk_impl->analog_label_ptrs[static_cast<std::size_t>(i)] = chunk_impl->analog_labels_storage[i].c_str();
      }
    }
  }

  const int n_scalar = chunk_impl->n_frames * chunk_impl->n_points * 3;
  if (n_scalar > 0) {
    chunk_impl->points_xyz_storage.resize(static_cast<std::size_t>(n_scalar));
    chunk_impl->points_valid_storage.assign(static_cast<std::size_t>(chunk_impl->n_frames * chunk_impl->n_points), 0u);
  }

  const bool can_use_dense_read = (chunk_impl->n_points > 0 && chunk_impl->n_points_total > 0);
  int64_t span_points = 0;
  if (point_indices.size() > 0) {
    int min_idx = point_indices[0];
    int max_idx = point_indices[0];
    for (const int idx : point_indices) {
      min_idx = (idx < min_idx) ? idx : min_idx;
      max_idx = (idx > max_idx) ? idx : max_idx;
    }
    span_points = static_cast<int64_t>(max_idx - min_idx + 1);
  } else {
    span_points = chunk_impl->n_points_total;
  }
  if (span_points <= 0) span_points = chunk_impl->n_points_total;
  const int64_t span_bytes = span_points * static_cast<int64_t>(dec_impl->reader->meta.point_record_bytes);
  std::string read_policy_reason;
  chunk_impl->read_policy = select_read_policy(
      opt, chunk_impl->n_points_total, chunk_impl->n_points, span_bytes, opt->io_buffer_bytes, &read_policy_reason);
  if (!read_policy_reason.empty()) {
    if (!chunk_impl->reason.empty()) {
      chunk_impl->reason += " | ";
    }
    chunk_impl->reason += read_policy_reason;
  }

  std::vector<sqzc3d_num_t> residual;
  const int out_nscalar = chunk_impl->n_points * 3;
  std::vector<sqzc3d_num_t> full_points;
  if (chunk_impl->read_policy == sqzc3d_READ_POLICY_DENSE && can_use_dense_read && chunk_impl->n_points != meta.n_points) {
    full_points.resize(static_cast<std::size_t>(meta.n_points) * 3u);
  }
  const int full_nscalar = meta.n_points * 3;

  for (int fi = 0; fi < chunk_impl->n_frames; ++fi) {
    const int f = frame_start + fi;
    auto* xyz = chunk_impl->points_xyz_storage.data() + static_cast<std::size_t>(fi) * out_nscalar;
    auto* valid = chunk_impl->points_valid_storage.data() + static_cast<std::size_t>(fi) * chunk_impl->n_points;
    sqzc3d_status st = sqzc3d_STATUS_SUCCESS;
    if (chunk_impl->read_policy == sqzc3d_READ_POLICY_DENSE && can_use_dense_read) {
      if (chunk_impl->n_points == meta.n_points) {
        st = sqzc3d::sqzc3d_c3d_stream_read_frame_all_xyz(
            dec_impl->reader.get(), f, xyz, full_nscalar);
      } else {
        st = sqzc3d::sqzc3d_c3d_stream_read_frame_all_xyz(
            dec_impl->reader.get(), f, full_points.data(), full_nscalar);
        if (st == sqzc3d_STATUS_SUCCESS) {
          for (int p = 0; p < chunk_impl->n_points; ++p) {
            const int src_idx = point_indices[static_cast<std::size_t>(p)];
            const std::size_t src_o = static_cast<std::size_t>(src_idx) * 3u;
            const std::size_t dst_o = static_cast<std::size_t>(p) * 3u;
            xyz[dst_o] = full_points[src_o];
            xyz[dst_o + 1u] = full_points[src_o + 1u];
            xyz[dst_o + 2u] = full_points[src_o + 2u];
          }
        }
      }
    } else {
      if (chunk_impl->n_points == 0) {
        continue;
      }
      st = sqzc3d::sqzc3d_c3d_stream_read_frame_xyz_sel(
          dec_impl->reader.get(),
          f,
          point_indices.data(),
          chunk_impl->n_points,
          xyz,
          out_nscalar,
          valid,
          chunk_impl->n_points);
    }
    if (st != sqzc3d_STATUS_SUCCESS) {
      set_error(const_cast<sqzc3d_dec_t*>(dec), "read frame point data failed");
      delete chunk;
      return st;
    }

    for (int p = 0; p < chunk_impl->n_points; ++p) {
      const std::size_t i = static_cast<std::size_t>(p);
      if (chunk_impl->valid_policy != sqzc3d_VALID_POLICY_FINITE_XYZ) {
        valid[i] = 1u;
        continue;
      }
      const auto x = xyz[i * 3];
      const auto y = xyz[i * 3 + 1];
      const auto z = xyz[i * 3 + 2];
      valid[i] = (is_finite(x) && is_finite(y) && is_finite(z)) ? 1u : 0u;
    }
  }

  if (chunk_impl->residual_gate_mm > 0.0 && chunk_impl->n_points > 0) {
    residual.assign(static_cast<std::size_t>(chunk_impl->n_points), 0.0);
    for (int fi = 0; fi < chunk_impl->n_frames; ++fi) {
      const int f = frame_start + fi;
    auto* valid = chunk_impl->points_valid_storage.data() + static_cast<std::size_t>(fi) * chunk_impl->n_points;
    const auto st = sqzc3d::sqzc3d_c3d_stream_read_frame_residual_sel(
        dec_impl->reader.get(),
        f,
        point_indices.data(),
        chunk_impl->n_points,
        residual.data(),
        static_cast<int>(residual.size()));
      if (st != sqzc3d_STATUS_SUCCESS) {
        set_error(const_cast<sqzc3d_dec_t*>(dec), "read frame residual data failed");
        delete chunk;
        return st;
      }
      for (std::size_t p = 0; p < residual.size(); ++p) {
        if (residual[p] > chunk_impl->residual_gate_mm || residual[p] < 0.0) valid[p] = 0u;
      }
    }
  }

  const int analog_sample_count = chunk_impl->n_analogs * chunk_impl->n_analog_by_frame;
  if (analog_enable != sqzc3d_ANALOG_EN_OFF && analog_sample_count > 0) {
    chunk_impl->analog_storage.resize(static_cast<std::size_t>(chunk_impl->n_frames) *
                                     static_cast<std::size_t>(analog_sample_count));
    chunk_impl->analog_valid_storage.assign(chunk_impl->analog_storage.size(), 1u);
    chunk_impl->n_analog_scalar = static_cast<int>(chunk_impl->analog_storage.size());
    const int analog_frames = (analog_range_count < chunk_impl->n_frames) ? analog_range_count : chunk_impl->n_frames;
    for (int fi = 0; fi < chunk_impl->n_frames; ++fi) {
      if (fi >= analog_frames) {
        auto* valid_ptr = chunk_impl->analog_valid_storage.data() +
            static_cast<std::size_t>(fi) * static_cast<std::size_t>(analog_sample_count);
        std::fill(valid_ptr, valid_ptr + analog_sample_count, 0u);
        continue;
      }
      const int f = analog_range_start + fi;
      auto* analog_target = chunk_impl->analog_storage.data() +
          static_cast<std::size_t>(fi) * static_cast<std::size_t>(analog_sample_count);
      auto* analog_valid = chunk_impl->analog_valid_storage.data() +
          static_cast<std::size_t>(fi) * static_cast<std::size_t>(analog_sample_count);
      std::fill(analog_valid, analog_valid + analog_sample_count, 1u);
      const auto st = sqzc3d::sqzc3d_c3d_stream_read_frame_analogs_sel(
          dec_impl->reader.get(),
          f,
          analog_indices.data(),
          static_cast<int>(analog_indices.size()),
          0,
          meta.n_analog_by_frame,
          analog_target,
          analog_sample_count);
      if (st != sqzc3d_STATUS_SUCCESS) {
        set_error(const_cast<sqzc3d_dec_t*>(dec), "read frame analog data failed");
        delete chunk;
        return st;
      }
    }
  }

  chunk->n_frames = chunk_impl->n_frames;
  chunk->n_points = chunk_impl->n_points;
  chunk->n_points_total = chunk_impl->n_points_total;
  chunk->n_analogs = chunk_impl->n_analogs;
  chunk->n_analog_by_frame = chunk_impl->n_analog_by_frame;
  chunk->n_scalar = n_scalar;
  chunk->valid_nscalar = static_cast<int>(chunk_impl->points_valid_storage.size());
  chunk->n_analog_scalar = chunk_impl->n_analog_scalar;
  chunk->points_layout = chunk_impl->points_layout;
  chunk->read_policy = chunk_impl->read_policy;
  chunk->points_pack = chunk_impl->points_pack;
  chunk->valid_policy = chunk_impl->valid_policy;
  chunk->residual_gate_mm = chunk_impl->residual_gate_mm;
  chunk->point_scale = chunk_impl->point_scale;
  chunk->header_scale = chunk_impl->header_scale;
  chunk->raw_params_nbytes = static_cast<int>(chunk_impl->raw_params.size());
  chunk->points_xyz = chunk_impl->points_xyz_storage.empty() ? nullptr : chunk_impl->points_xyz_storage.data();
  chunk->points_valid = chunk_impl->points_valid_storage.empty() ? nullptr : chunk_impl->points_valid_storage.data();
  chunk->analog = chunk_impl->analog_storage.empty() ? nullptr : chunk_impl->analog_storage.data();
  chunk->analog_valid = chunk_impl->analog_valid_storage.empty() ? nullptr : chunk_impl->analog_valid_storage.data();
  chunk->point_labels = chunk_impl->point_label_ptrs.empty() ? nullptr : chunk_impl->point_label_ptrs.data();
  chunk->analog_labels = chunk_impl->analog_label_ptrs.empty() ? nullptr : chunk_impl->analog_label_ptrs.data();
  chunk->raw_params = chunk_impl->raw_params.empty() ? nullptr : chunk_impl->raw_params.data();
  chunk->reason = chunk_impl->reason.empty() ? nullptr : chunk_impl->reason.c_str();
  chunk->impl = chunk_impl.release();
  *out_chunk = chunk;
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_free_chunk(sqzc3d_chunk_t* chunk) {
  if (!chunk) return sqzc3d_STATUS_SUCCESS;
  if (chunk->impl == nullptr) {
    delete chunk;
    return sqzc3d_STATUS_SUCCESS;
  }
  auto* impl = static_cast<sqzc3dChunk*>(chunk->impl);
  delete impl;
  delete chunk;
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_export_bundle(
    const char* out_dir,
    const sqzc3d_chunk_t* chunk) {
  if (!out_dir || !out_dir[0] || !chunk) return sqzc3d_STATUS_INVALID_ARGUMENT;

  namespace fs = std::filesystem;
  const fs::path out_path = fs::path(out_dir);
  const auto ext = out_path.extension().string();
  const bool single_file = (ext == ".sqzc3d" || ext == ".sqzc3D");

  const bool has_points_xyz = chunk->points_xyz != nullptr && chunk->n_scalar > 0;
  const bool has_points_valid = chunk->points_valid != nullptr && chunk->valid_nscalar > 0;
  const bool has_analogs = chunk->analog != nullptr && chunk->n_analog_scalar > 0;
  const bool has_analog_valid = chunk->analog_valid != nullptr && chunk->n_analog_scalar > 0;
  const bool has_raw_params = chunk->raw_params != nullptr && chunk->raw_params_nbytes > 0;

  const std::uint64_t points_xyz_count = has_points_xyz ? static_cast<std::uint64_t>(chunk->n_scalar) : 0u;
  const std::uint64_t points_valid_count =
      has_points_valid ? static_cast<std::uint64_t>(chunk->valid_nscalar) : 0u;
  const std::uint64_t analog_count = has_analogs ? static_cast<std::uint64_t>(chunk->n_analog_scalar) : 0u;
  const std::uint64_t analog_valid_count = has_analog_valid ? static_cast<std::uint64_t>(chunk->n_analog_scalar) : 0u;
  const std::uint64_t raw_params_count = has_raw_params ? static_cast<std::uint64_t>(chunk->raw_params_nbytes) : 0u;

  const auto bytes_points_xyz = points_xyz_count * static_cast<std::uint64_t>(sizeof(sqzc3d_num_t));
  const auto bytes_points_valid = points_valid_count * static_cast<std::uint64_t>(sizeof(unsigned char));
  const auto bytes_analogs = analog_count * static_cast<std::uint64_t>(sizeof(sqzc3d_num_t));
  const auto bytes_analog_valid = analog_valid_count * static_cast<std::uint64_t>(sizeof(unsigned char));
  const auto bytes_raw_params = raw_params_count * static_cast<std::uint64_t>(sizeof(sqzc3d_byte_t));

  std::uint64_t cursor = 0u;
  const std::uint64_t points_xyz_offset = cursor;
  cursor += has_points_xyz ? bytes_points_xyz : 0u;
  const std::uint64_t points_valid_offset = cursor;
  cursor += has_points_valid ? bytes_points_valid : 0u;
  const std::uint64_t analog_offset = cursor;
  cursor += has_analogs ? bytes_analogs : 0u;
  const std::uint64_t analog_valid_offset = cursor;
  cursor += has_analog_valid ? bytes_analog_valid : 0u;
  const std::uint64_t raw_params_offset = cursor;
  cursor += has_raw_params ? bytes_raw_params : 0u;
  (void)cursor;

  std::ostringstream meta_out;
  const auto reason = chunk->reason ? chunk->reason : "";
  const auto n_point_labels = (chunk->point_labels == nullptr ? 0 : chunk->n_points);
  const auto n_analog_labels = (chunk->analog_labels == nullptr ? 0 : chunk->n_analogs);

  meta_out << "{\n";
  meta_out << "  \"format\": \"sqzc3d_bundle_v2\",\n";
  meta_out << "  \"schema_version\": 2,\n";
  meta_out << "  \"endianness\": \"little\",\n";
  meta_out << "  \"n_frames\": " << chunk->n_frames << ",\n";
  meta_out << "  \"n_points\": " << chunk->n_points << ",\n";
  meta_out << "  \"n_points_total\": " << chunk->n_points_total << ",\n";
  meta_out << "  \"n_analogs\": " << chunk->n_analogs << ",\n";
  meta_out << "  \"n_analog_by_frame\": " << chunk->n_analog_by_frame << ",\n";
  meta_out << "  \"n_scalar\": " << chunk->n_scalar << ",\n";
  meta_out << "  \"valid_nscalar\": " << chunk->valid_nscalar << ",\n";
  meta_out << "  \"n_analog_scalar\": " << chunk->n_analog_scalar << ",\n";
  meta_out << "  \"raw_params_nbytes\": " << chunk->raw_params_nbytes << ",\n";
  meta_out << "  \"points_layout\": \"" << points_layout_name(chunk->points_layout) << "\",\n";
  meta_out << "  \"points_pack\": \"" << points_pack_name(chunk->points_pack) << "\",\n";
  meta_out << "  \"read_policy\": \"" << read_policy_name(chunk->read_policy) << "\",\n";
  meta_out << "  \"valid_policy\": " << chunk->valid_policy << ",\n";
  meta_out << std::fixed << std::setprecision(std::numeric_limits<double>::max_digits10);
  meta_out << "  \"residual_gate_mm\": " << chunk->residual_gate_mm << ",\n";
  meta_out << "  \"point_scale\": " << chunk->point_scale << ",\n";
  meta_out << "  \"header_scale\": " << chunk->header_scale << ",\n";
  meta_out << "  \"reason\": \"" << json_escape(reason) << "\",\n";

  auto write_string_list = [&](const char** values, int n_values) {
    for (int i = 0; i < n_values; ++i) {
      if (i > 0) meta_out << ", ";
      const char* v = values && values[static_cast<std::size_t>(i)] ? values[static_cast<std::size_t>(i)] : "";
      meta_out << "\"" << json_escape(v) << "\"";
    }
  };

  auto write_section = [&](const char* name,
                          bool present,
                          std::uint64_t offset,
                          std::uint64_t count,
                          const char* dtype,
                          std::uint64_t byte_size,
                          std::uint64_t checksum,
                          bool trailing_comma) {
    meta_out << "    \"" << name << "\": ";
    if (!present) {
      meta_out << "null";
    } else {
      meta_out << "{\n      \"offset\": " << offset << ",\n      \"count\": " << count
                << ",\n      \"byte_size\": " << byte_size << ",\n      \"dtype\": \"" << dtype
                << "\",\n      \"checksum\": " << checksum << "\n    }";
    }
    meta_out << (trailing_comma ? ",\n" : "\n");
  };

  meta_out << "  \"sections\": {\n";
  const auto section_checksum = [&](const void* data, std::size_t nbytes) {
    if (!data || nbytes == 0u) return std::uint64_t{0u};
    return checksum_fnv1a_64(data, nbytes);
  };

  write_section(
      "points_xyz",
      has_points_xyz,
      points_xyz_offset,
      points_xyz_count,
      "float64",
      bytes_points_xyz,
      section_checksum(chunk->points_xyz, static_cast<std::size_t>(bytes_points_xyz)),
      true);
  write_section(
      "points_valid",
      has_points_valid,
      points_valid_offset,
      points_valid_count,
      "uint8",
      bytes_points_valid,
      section_checksum(chunk->points_valid, static_cast<std::size_t>(bytes_points_valid)),
      true);
  write_section(
      "analogs",
      has_analogs,
      analog_offset,
      analog_count,
      "float64",
      bytes_analogs,
      section_checksum(chunk->analog, static_cast<std::size_t>(bytes_analogs)),
      true);
  write_section(
      "analog_valid",
      has_analog_valid,
      analog_valid_offset,
      analog_valid_count,
      "uint8",
      bytes_analog_valid,
      section_checksum(chunk->analog_valid, static_cast<std::size_t>(bytes_analog_valid)),
      true);
  write_section(
      "raw_params",
      has_raw_params,
      raw_params_offset,
      raw_params_count,
      "uint8",
      bytes_raw_params,
      section_checksum(chunk->raw_params, static_cast<std::size_t>(bytes_raw_params)),
      false);
  meta_out << "  },\n";
  meta_out << "  \"point_labels\": [";
  write_string_list(chunk->point_labels, static_cast<int>(n_point_labels));
  meta_out << "],\n";
  meta_out << "  \"analog_labels\": [";
  write_string_list(chunk->analog_labels, static_cast<int>(n_analog_labels));
  meta_out << "]\n";
  meta_out << "}\n";

  const std::string meta_text = meta_out.str();
  if (!meta_out.good()) return sqzc3d_STATUS_INTERNAL_ERROR;

  const fs::path meta_path = out_path / "meta.json";
  const fs::path data_path = out_path / "data.bin";
  if (!single_file) {
    std::error_code ec;
    fs::create_directories(out_path, ec);
    if (ec) return sqzc3d_STATUS_INTERNAL_ERROR;

    std::ofstream data_out(data_path, std::ios::binary | std::ios::trunc);
    if (!data_out.good()) return sqzc3d_STATUS_INVALID_ARGUMENT;
    if (has_points_xyz) write_num(data_out, chunk->points_xyz, points_xyz_count * sizeof(sqzc3d_num_t));
    if (has_points_valid) write_num(data_out, chunk->points_valid, points_valid_count * sizeof(unsigned char));
    if (has_analogs) write_num(data_out, chunk->analog, analog_count * sizeof(sqzc3d_num_t));
    if (has_analog_valid) write_num(data_out, chunk->analog_valid, analog_valid_count * sizeof(unsigned char));
    if (has_raw_params) write_num(data_out, chunk->raw_params, raw_params_count * sizeof(sqzc3d_byte_t));
    if (!data_out.good()) return sqzc3d_STATUS_INTERNAL_ERROR;

    std::ofstream meta_file(meta_path, std::ios::binary | std::ios::trunc);
    if (!meta_file.good()) return sqzc3d_STATUS_INVALID_ARGUMENT;
    meta_file.write(meta_text.data(), static_cast<std::streamsize>(meta_text.size()));
    if (!meta_file.good()) return sqzc3d_STATUS_INTERNAL_ERROR;
    return sqzc3d_STATUS_SUCCESS;
  }

  std::ofstream container_out(out_path, std::ios::binary | std::ios::trunc);
  if (!container_out.good()) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (!write_sic_bundle_magic(container_out)) return sqzc3d_STATUS_INTERNAL_ERROR;
  if (!write_u64_le(container_out, static_cast<std::uint64_t>(meta_text.size()))) return sqzc3d_STATUS_INTERNAL_ERROR;
  container_out.write(meta_text.data(), static_cast<std::streamsize>(meta_text.size()));
  if (!container_out.good()) return sqzc3d_STATUS_INTERNAL_ERROR;
  if (has_points_xyz) write_num(container_out, chunk->points_xyz, points_xyz_count * sizeof(sqzc3d_num_t));
  if (has_points_valid) write_num(container_out, chunk->points_valid, points_valid_count * sizeof(unsigned char));
  if (has_analogs) write_num(container_out, chunk->analog, analog_count * sizeof(sqzc3d_num_t));
  if (has_analog_valid) write_num(container_out, chunk->analog_valid, analog_valid_count * sizeof(unsigned char));
  if (has_raw_params) write_num(container_out, chunk->raw_params, raw_params_count * sizeof(sqzc3d_byte_t));
  if (!container_out.good()) return sqzc3d_STATUS_INTERNAL_ERROR;
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_load_bundle(
    const char* bundle_dir,
    sqzc3d_chunk_t** out_chunk) {
  sqzc3d_bundle_load_opt_t opt{};
  sqzc3d_default_bundle_load_opt(&opt);
  opt.strict = 0;
  return sqzc3d_load_bundle_with_options(bundle_dir, &opt, out_chunk);
}

sqzc3d_API int sqzc3d_load_bundle_with_options(
    const char* bundle_dir,
    const sqzc3d_bundle_load_opt_t* opt,
    sqzc3d_chunk_t** out_chunk) {
  sqzc3d_bundle_load_opt_t opt_v{};
  if (!opt) {
    sqzc3d_default_bundle_load_opt(&opt_v);
    opt = &opt_v;
  }
  const bool strict = opt->strict != 0;
  auto fail = [](int code, const char* stage) {
    sqzc3d_DEBUG_BUNDLE_LOG(stage);
    return code;
  };
  if (opt->struct_size > 0 && opt->struct_size < static_cast<int>(sizeof(sqzc3d_bundle_load_opt_t))) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "invalid bundle load option struct");
  }
  if (!bundle_dir || !out_chunk || bundle_dir[0] == '\0') {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "invalid bundle path");
  }
  *out_chunk = nullptr;

  namespace fs = std::filesystem;
  const fs::path input_path = fs::path(bundle_dir);
  fs::path meta_path;
  fs::path data_path;
  std::string meta_text;
  std::uint64_t data_base_offset = 0u;
  uintmax_t data_size = 0u;

  if (fs::is_directory(input_path)) {
    meta_path = input_path / "meta.json";
    data_path = input_path / "data.bin";
    std::ifstream meta_in(meta_path, std::ios::binary);
    if (!meta_in.good()) return sqzc3d_STATUS_INVALID_ARGUMENT;
    meta_text.assign((std::istreambuf_iterator<char>(meta_in)), std::istreambuf_iterator<char>());
  } else if (fs::is_regular_file(input_path)) {
    const auto ext = input_path.extension().string();
    if (ext != ".sqzc3d" && ext != ".sqzc3D") {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "unsupported container suffix");
    }
    std::ifstream container_in(input_path, std::ios::binary);
    if (!container_in.good()) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "container open failed");
    }
    if (!is_sqzc3d_container_magic(container_in)) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "container magic mismatch");
    }
    std::uint64_t meta_nbytes = 0u;
    if (!read_little_u64(container_in, &meta_nbytes)) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "meta size parse failed");
    }
    std::error_code size_error;
    const auto total_size = fs::file_size(input_path, size_error);
    if (size_error) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "file size stat failed");
    }
    if (meta_nbytes + 16u > static_cast<uintmax_t>(total_size))
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "container header exceeds file size");
    meta_text.resize(static_cast<std::size_t>(meta_nbytes));
    container_in.read(meta_text.data(), static_cast<std::streamsize>(meta_nbytes));
    if (!container_in.good()) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "container meta read failed");
    }
    data_base_offset = 16u + meta_nbytes;
    data_size = static_cast<uintmax_t>(total_size);
    data_path = input_path;
  } else {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "unsupported bundle input type");
  }

  std::string format;
  const bool has_format = parse_bundle_name_value(meta_text, "format", &format);
  const bool is_v2 = has_format && format == "sqzc3d_bundle_v2";
  if (has_format && !(format == "sqzc3d_bundle_v1" || is_v2)) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "unsupported bundle format");
  }
  const bool strict_schema = strict && is_v2;
  if (strict_schema && !has_format) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "missing format under strict mode");
  }
  int64_t schema_version = 0;
  bool has_schema_version = false;
  if (!parse_bundle_schema_version(meta_text, strict_schema, &schema_version, &has_schema_version)) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "schema parse failed");
  }
  if (is_v2 && (!has_schema_version || schema_version != 2)) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "unsupported schema version");
  }

  int64_t n_frames = 0;
  int64_t n_points = 0;
  int64_t n_points_total = 0;
  int64_t n_analogs = 0;
  int64_t n_analog_by_frame = 0;
  int64_t n_scalar = 0;
  int64_t valid_nscalar = 0;
  int64_t n_analog_scalar = 0;
  if (!parse_bundle_int_value(meta_text, "n_frames", &n_frames) ||
      !parse_bundle_int_value(meta_text, "n_points", &n_points) ||
      !parse_bundle_int_value(meta_text, "n_points_total", &n_points_total) ||
      !parse_bundle_int_value(meta_text, "n_analogs", &n_analogs) ||
      !parse_bundle_int_value(meta_text, "n_analog_by_frame", &n_analog_by_frame) ||
      !parse_bundle_int_value(meta_text, "n_scalar", &n_scalar) ||
      !parse_bundle_int_value(meta_text, "valid_nscalar", &valid_nscalar) ||
      !parse_bundle_int_value(meta_text, "n_analog_scalar", &n_analog_scalar)) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "meta integer parse failed");
  }

  std::string points_layout_name;
  std::string points_pack_name;
  std::string read_policy_name;
  int64_t valid_policy = 0;
  double residual_gate_mm = 0.0;
  double point_scale = 1.0;
  double header_scale = 1.0;
  std::string endianness;
  std::string reason;
  if (!parse_bundle_name_value(meta_text, "points_layout", &points_layout_name) ||
      !parse_bundle_name_value(meta_text, "points_pack", &points_pack_name) ||
      !parse_bundle_name_value(meta_text, "read_policy", &read_policy_name) ||
      !parse_bundle_int_value(meta_text, "valid_policy", &valid_policy) ||
      !parse_bundle_double_value(meta_text, "residual_gate_mm", &residual_gate_mm) ||
      !parse_bundle_double_value(meta_text, "point_scale", &point_scale) ||
      !parse_bundle_double_value(meta_text, "header_scale", &header_scale) ||
      !parse_bundle_optional_name_value(meta_text, "reason", &reason) ||
      (strict_schema && !parse_bundle_optional_name_value(meta_text, "endianness", &endianness)) ||
      (strict_schema && endianness != "little")) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "meta value parse failed");
  }
  sqzc3d_DEBUG_BUNDLE_LOG(std::string("parsed read_policy_name='") + read_policy_name + "'");

  const int points_layout = parse_points_layout_name(points_layout_name);
  const int points_pack = parse_points_pack_name(points_pack_name);
  const int read_policy = parse_read_policy_name_to_enum(read_policy_name);
  sqzc3d_DEBUG_BUNDLE_LOG(std::string("parsed read_policy enum=") + std::to_string(read_policy));
  if (points_layout < 0 || points_pack < 0) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "layout/pack parse failed");
  }
  if (valid_policy < 0) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "valid policy parse failed");
  }

  std::vector<std::string> point_labels;
  std::vector<std::string> analog_labels;
  if (!parse_bundle_string_list(meta_text, "point_labels", point_labels) ||
      !parse_bundle_string_list(meta_text, "analog_labels", analog_labels)) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "label list parse failed");
  }

  BundleSection points_xyz{};
  BundleSection points_valid{};
  BundleSection analogs{};
  BundleSection analog_valid{};
  BundleSection raw_params{};
  if (!parse_bundle_section(meta_text, "points_xyz", &points_xyz) ||
      !parse_bundle_section(meta_text, "points_valid", &points_valid) ||
      !parse_bundle_section_if_present(meta_text, "analogs", &analogs) ||
      !parse_bundle_section_if_present(meta_text, "analog_valid", &analog_valid) ||
      !parse_bundle_section_if_present(meta_text, "raw_params", &raw_params)) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "sections parse failed");
  }

  int64_t raw_params_nbytes = 0;
  const bool has_raw_params_nbytes = parse_bundle_int_value(meta_text, "raw_params_nbytes", &raw_params_nbytes);
  if (has_raw_params_nbytes && raw_params_nbytes < 0) {
    return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "negative raw_params_nbytes");
  }
  if (raw_params.present && !has_raw_params_nbytes) {
    raw_params_nbytes = static_cast<int64_t>(raw_params.count);
  }
  if (raw_params.present && has_raw_params_nbytes &&
      static_cast<std::uint64_t>(raw_params_nbytes) != raw_params.count) {
    return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "raw section metadata mismatch");
  }

  if (n_analog_scalar != n_analogs * n_analog_by_frame * n_frames) {
    return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "analog scalar dimension mismatch");
  }
  if (!points_xyz.present && n_scalar > 0) {
    return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "missing points_xyz");
  }
  if (!points_valid.present && valid_nscalar > 0) {
    return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "missing points_valid");
  }
  if (!analogs.present && n_analog_scalar > 0) {
    return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "missing analogs");
  }
  if (!analog_valid.present && n_analog_scalar > 0) {
    return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "missing analog_valid");
  }
  if (static_cast<int>(point_labels.size()) != n_points) {
    return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "point labels count mismatch");
  }
  if (static_cast<int>(analog_labels.size()) != n_analogs) {
    return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "analog labels count mismatch");
  }

  if (fs::is_directory(input_path)) {
    if (!fs::exists(data_path)) return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "data.bin missing");
    std::error_code ec;
    data_size = fs::file_size(data_path, ec);
    if (ec) return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "data.bin stat failed");
  }
  if (data_size == 0u) return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "data size is zero");
  std::ifstream data_in(data_path, std::ios::binary);
  if (!data_in.good()) return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "data open failed");
  if (data_base_offset > 0u) {
    data_in.seekg(static_cast<std::streamoff>(data_base_offset), std::ios::beg);
    if (!data_in.good()) return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "data seek failed");
  }

  auto chunk = new (std::nothrow) sqzc3d_chunk_t();
  if (!chunk) return sqzc3d_STATUS_INTERNAL_ERROR;
  std::memset(chunk, 0, sizeof(sqzc3d_chunk_t));
  chunk->struct_size = static_cast<int>(sizeof(*chunk));
  auto chunk_impl = std::make_unique<sqzc3dChunk>();

  if (!cast_metadata_int(n_frames, &chunk->n_frames) ||
      !cast_metadata_int(n_points, &chunk->n_points) ||
      !cast_metadata_int(n_points_total, &chunk->n_points_total) ||
      !cast_metadata_int(n_analogs, &chunk->n_analogs) ||
      !cast_metadata_int(n_analog_by_frame, &chunk->n_analog_by_frame) ||
      !cast_metadata_int(n_scalar, &chunk->n_scalar) ||
      !cast_metadata_int(valid_nscalar, &chunk->valid_nscalar) ||
      !cast_metadata_int(n_analog_scalar, &chunk->n_analog_scalar) ||
      !cast_metadata_int(valid_policy, &chunk_impl->valid_policy)) {
    delete chunk;
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }

  chunk_impl->points_layout = points_layout;
  chunk->points_layout = chunk_impl->points_layout;
  chunk_impl->read_policy = read_policy;
  chunk->read_policy = chunk_impl->read_policy;
  chunk_impl->points_pack = points_pack;
  chunk->points_pack = chunk_impl->points_pack;
  chunk_impl->residual_gate_mm = residual_gate_mm;
  chunk->residual_gate_mm = chunk_impl->residual_gate_mm;
  chunk_impl->point_scale = point_scale;
  chunk->point_scale = chunk_impl->point_scale;
  chunk_impl->header_scale = header_scale;
  chunk->header_scale = chunk_impl->header_scale;
  chunk_impl->reason = reason;
  chunk_impl->label_norm = sqzc3d_LABEL_NORM_EXACT | sqzc3d_LABEL_NORM_TRIM;
  chunk->valid_policy = chunk_impl->valid_policy;

  const auto verify_section_checksum = [&](const BundleSection& section,
                                          const void* data,
                                          std::size_t nbytes) -> bool {
    if (!strict_schema) return true;
    if (section.byte_size != static_cast<std::uint64_t>(nbytes)) return false;
    if (nbytes > 0u && (!section.checksum_present || !data)) return false;
    if (nbytes == 0u) return true;
    const auto observed = checksum_fnv1a_64(data, nbytes);
    return observed == section.checksum;
  };

  if (n_scalar > 0) {
    if (!read_section_data(data_in,
                          points_xyz,
                          "float64",
                          strict_schema,
                          data_size,
                          data_base_offset,
                          chunk_impl->points_xyz_storage) ||
        static_cast<int64_t>(chunk_impl->points_xyz_storage.size()) != n_scalar) {
      delete chunk;
      return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "points_xyz read failed");
    }
    if (!verify_section_checksum(
            points_xyz,
            chunk_impl->points_xyz_storage.empty() ? nullptr : chunk_impl->points_xyz_storage.data(),
            chunk_impl->points_xyz_storage.size() * sizeof(sqzc3d_num_t))) {
      delete chunk;
      return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "points_xyz checksum mismatch");
    }
  } else {
    chunk_impl->points_xyz_storage.clear();
  }
  if (valid_nscalar > 0) {
    if (!read_section_data(data_in,
                          points_valid,
                          "uint8",
                          strict_schema,
                          data_size,
                          data_base_offset,
                          chunk_impl->points_valid_storage) ||
        static_cast<int64_t>(chunk_impl->points_valid_storage.size()) != valid_nscalar) {
      delete chunk;
      return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "points_valid read failed");
    }
    if (!verify_section_checksum(
            points_valid,
            chunk_impl->points_valid_storage.empty() ? nullptr : chunk_impl->points_valid_storage.data(),
            chunk_impl->points_valid_storage.size() * sizeof(unsigned char))) {
      delete chunk;
      return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "points_valid checksum mismatch");
    }
  } else {
    chunk_impl->points_valid_storage.clear();
  }
  if (n_analog_scalar > 0) {
    if (!read_section_data(data_in,
                          analogs,
                          "float64",
                          strict_schema,
                          data_size,
                          data_base_offset,
                          chunk_impl->analog_storage) ||
        static_cast<int64_t>(chunk_impl->analog_storage.size()) != n_analog_scalar) {
      delete chunk;
      return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "analogs read failed");
    }
    if (!verify_section_checksum(
            analogs,
            chunk_impl->analog_storage.empty() ? nullptr : chunk_impl->analog_storage.data(),
            chunk_impl->analog_storage.size() * sizeof(sqzc3d_num_t))) {
      delete chunk;
      return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "analogs checksum mismatch");
    }
    if (!read_section_data(data_in,
                          analog_valid,
                          "uint8",
                          strict_schema,
                          data_size,
                          data_base_offset,
                          chunk_impl->analog_valid_storage) ||
        static_cast<int64_t>(chunk_impl->analog_valid_storage.size()) != n_analog_scalar) {
      delete chunk;
      return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "analog_valid read failed");
    }
    if (!verify_section_checksum(
            analog_valid,
            chunk_impl->analog_valid_storage.empty() ? nullptr : chunk_impl->analog_valid_storage.data(),
            chunk_impl->analog_valid_storage.size() * sizeof(unsigned char))) {
      delete chunk;
      return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "analog_valid checksum mismatch");
    }
  } else {
    chunk_impl->analog_storage.clear();
    chunk_impl->analog_valid_storage.clear();
  }
  if ((raw_params.present || raw_params_nbytes > 0) &&
      (!read_section_data(data_in,
                          raw_params,
                          "uint8",
                          strict_schema,
                          data_size,
                          data_base_offset,
                          chunk_impl->raw_params) ||
       static_cast<int64_t>(chunk_impl->raw_params.size()) != raw_params_nbytes)) {
    delete chunk;
    return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "raw_params read failed");
  }
  if (raw_params_nbytes > 0 && !verify_section_checksum(
          raw_params,
          chunk_impl->raw_params.empty() ? nullptr : chunk_impl->raw_params.data(),
          chunk_impl->raw_params.size() * sizeof(sqzc3d_byte_t))) {
    delete chunk;
    return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "raw_params checksum mismatch");
  }

  chunk_impl->point_labels_storage = std::move(point_labels);
  chunk_impl->analog_labels_storage = std::move(analog_labels);
  chunk_impl->point_label_ptrs.resize(chunk_impl->point_labels_storage.size());
  for (std::size_t i = 0; i < chunk_impl->point_labels_storage.size(); ++i) {
    chunk_impl->point_label_ptrs[i] = chunk_impl->point_labels_storage[i].c_str();
  }
  chunk_impl->analog_label_ptrs.resize(chunk_impl->analog_labels_storage.size());
  for (std::size_t i = 0; i < chunk_impl->analog_labels_storage.size(); ++i) {
    chunk_impl->analog_label_ptrs[i] = chunk_impl->analog_labels_storage[i].c_str();
  }

  chunk->points_xyz = chunk_impl->points_xyz_storage.empty() ? nullptr : chunk_impl->points_xyz_storage.data();
  chunk->points_valid = chunk_impl->points_valid_storage.empty() ? nullptr : chunk_impl->points_valid_storage.data();
  chunk->analog = chunk_impl->analog_storage.empty() ? nullptr : chunk_impl->analog_storage.data();
  chunk->analog_valid = chunk_impl->analog_valid_storage.empty() ? nullptr : chunk_impl->analog_valid_storage.data();
  chunk->raw_params = chunk_impl->raw_params.empty() ? nullptr : chunk_impl->raw_params.data();
  chunk->point_labels = chunk_impl->point_label_ptrs.empty() ? nullptr : chunk_impl->point_label_ptrs.data();
  chunk->analog_labels = chunk_impl->analog_label_ptrs.empty() ? nullptr : chunk_impl->analog_label_ptrs.data();
  chunk->reason = chunk_impl->reason.empty() ? nullptr : chunk_impl->reason.c_str();
  if (chunk_impl->raw_params.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    delete chunk_impl.release();
    delete chunk;
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  chunk->raw_params_nbytes = static_cast<int>(chunk_impl->raw_params.size());
  chunk->impl = chunk_impl.release();
  *out_chunk = chunk;
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_chunk_num_frames(const sqzc3d_chunk_t* chunk) {
  return chunk ? chunk->n_frames : 0;
}

sqzc3d_API int sqzc3d_chunk_num_points(const sqzc3d_chunk_t* chunk) {
  return chunk ? chunk->n_points : 0;
}

sqzc3d_API int sqzc3d_chunk_num_scalar(const sqzc3d_chunk_t* chunk) {
  return chunk ? chunk->n_scalar : 0;
}

sqzc3d_API int sqzc3d_point_indices_for_labels(
    const sqzc3d_chunk_t* chunk,
    const char** labels,
    int n_labels,
    int* out_indices,
    int miss_idx) {
  if (!chunk || !out_indices || n_labels < 0 || (n_labels > 0 && !labels)) return sqzc3d_STATUS_INVALID_ARGUMENT;
  const auto* chunk_impl = static_cast<const sqzc3dChunk*>(chunk->impl);
  const int norm_mode = chunk_impl ? chunk_impl->label_norm : (sqzc3d_LABEL_NORM_EXACT | sqzc3d_LABEL_NORM_TRIM);
  for (int i = 0; i < n_labels; ++i) {
    out_indices[static_cast<std::size_t>(i)] = miss_idx;
    if (!labels[i]) continue;
    const auto target = normalize_label(labels[i], norm_mode);
    for (int j = 0; j < chunk->n_points; ++j) {
      const char* source = (chunk->point_labels && chunk->point_labels[j]) ? chunk->point_labels[j] : "";
      if (normalize_label(source, norm_mode) == target) {
        out_indices[static_cast<std::size_t>(i)] = j;
        break;
      }
    }
  }
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_analog_indices_for_labels(
    const sqzc3d_chunk_t* chunk,
    const char** labels,
    int n_labels,
    int* out_indices,
    int miss_idx) {
  if (!chunk || !out_indices || n_labels < 0 || (n_labels > 0 && !labels)) return sqzc3d_STATUS_INVALID_ARGUMENT;
  const auto* chunk_impl = static_cast<const sqzc3dChunk*>(chunk->impl);
  const int norm_mode = chunk_impl ? chunk_impl->label_norm : (sqzc3d_LABEL_NORM_EXACT | sqzc3d_LABEL_NORM_TRIM);
  for (int i = 0; i < n_labels; ++i) {
    out_indices[static_cast<std::size_t>(i)] = miss_idx;
    if (!labels[i]) continue;
    const auto target = normalize_label(labels[i], norm_mode);
    for (int j = 0; j < chunk->n_analogs; ++j) {
      const char* source = (chunk->analog_labels && chunk->analog_labels[j]) ? chunk->analog_labels[j] : "";
      if (normalize_label(source, norm_mode) == target) {
        out_indices[static_cast<std::size_t>(i)] = j;
        break;
      }
    }
  }
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_points_view_frames(
    const sqzc3d_chunk_t* chunk,
    int start,
    int count,
    sqzc3d_points_view_t* out_view) {
  if (!chunk || !out_view || start < 0 || count < 0 || start + count > chunk->n_frames) return sqzc3d_STATUS_INVALID_ARGUMENT;
  out_view->points_xyz = chunk->points_xyz ? chunk->points_xyz + static_cast<std::size_t>(start) *
                                                           static_cast<std::size_t>(chunk->n_points) * 3
                                         : nullptr;
  out_view->points_valid = chunk->points_valid ? chunk->points_valid + static_cast<std::size_t>(start) *
                                                                   static_cast<std::size_t>(chunk->n_points)
                                             : nullptr;
  out_view->n_frames = count;
  out_view->n_points = chunk->n_points;
  out_view->source_stride_points = chunk->n_points;
  out_view->source_point_offset = 0;
  out_view->point_indices = nullptr;
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_points_view_points(
    const sqzc3d_chunk_t* chunk,
    const int* point_indices,
    int n,
    sqzc3d_points_view_t* out_view) {
  if (!chunk || !out_view || n < 0 || (n > 0 && !point_indices)) return sqzc3d_STATUS_INVALID_ARGUMENT;
  for (int i = 0; i < n; ++i) {
    if (point_indices[i] < 0 || point_indices[i] >= chunk->n_points) return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  bool contiguous = n > 0;
  for (int i = 1; i < n; ++i) {
    if (point_indices[i] != point_indices[0] + i) {
      contiguous = false;
      break;
    }
  }
  out_view->points_xyz = chunk->points_xyz;
  out_view->points_valid = chunk->points_valid;
  out_view->n_frames = chunk->n_frames;
  out_view->n_points = n;
  out_view->source_stride_points = chunk->n_points;
  out_view->source_point_offset = contiguous ? (n > 0 ? point_indices[0] : 0) : 0;
  out_view->point_indices = contiguous ? nullptr : point_indices;
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_analogs_view_samples(
    const sqzc3d_chunk_t* chunk,
    int start_frame,
    int n_frames,
    sqzc3d_analogs_view_t* out_view) {
  if (!chunk || !out_view || start_frame < 0 || n_frames < 0 || start_frame + n_frames > chunk->n_frames) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  const int unit = chunk->n_analogs * chunk->n_analog_by_frame;
  out_view->analog = chunk->analog ? chunk->analog + static_cast<std::size_t>(start_frame) *
                                                     static_cast<std::size_t>(unit)
                                   : nullptr;
  out_view->n_frames = n_frames;
  out_view->n_analog_by_frame = chunk->n_analog_by_frame;
  out_view->n_analogs = chunk->n_analogs;
  out_view->source_n_analogs = chunk->n_analogs;
  out_view->channel_indices = nullptr;
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_analogs_view_channels(
    const sqzc3d_chunk_t* chunk,
    const int* channel_indices,
    int n,
    sqzc3d_analogs_view_t* out_view) {
  if (!chunk || !out_view || n < 0 || (n > 0 && !channel_indices)) return sqzc3d_STATUS_INVALID_ARGUMENT;
  for (int i = 0; i < n; ++i) {
    if (channel_indices[i] < 0 || channel_indices[i] >= chunk->n_analogs) return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  bool contiguous = n > 0;
  for (int i = 1; i < n; ++i) {
    if (channel_indices[i] != channel_indices[0] + i) {
      contiguous = false;
      break;
    }
  }
  out_view->analog = chunk->analog;
  out_view->n_frames = chunk->n_frames;
  out_view->n_analog_by_frame = chunk->n_analog_by_frame;
  out_view->n_analogs = n;
  out_view->source_n_analogs = chunk->n_analogs;
  out_view->channel_indices = contiguous ? nullptr : channel_indices;
  return sqzc3d_STATUS_SUCCESS;
}


