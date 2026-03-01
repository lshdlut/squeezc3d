#include "sqzc3d.h"

#include "sqzc3d_c3d_stream.h"

#include <algorithm>
#include <atomic>
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

static std::string normalize_extension(std::string ext) {
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return ext;
}

static std::filesystem::path make_unique_tmp_c3d_path(const std::filesystem::path& dir) {
  static std::atomic<std::uint64_t> seq{0};
  const std::uint64_t now =
      static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  const std::uint64_t id = seq.fetch_add(1, std::memory_order_relaxed);
  std::ostringstream name;
  name << "sqzc3d_mem_" << std::hex << now << "_" << std::dec << id << ".c3d";
  return dir / name.str();
}

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
  int label_norm = sqzc3d_LABEL_NORM_EXACT;
};

struct sqzc3dChunk {
  int n_frames = 0;
  bool has_time_axis = false;
  int frame_start = 0;
  int analog_frame_start = 0;
  int source_first_frame = 0;
  int source_last_frame = -1;
  double point_rate_hz = 0.0;
  double analog_rate_hz = 0.0;
  int n_points = 0;
  int n_points_total = 0;
  int n_analogs = 0;
  int n_analog_by_frame = 0;
  int n_type_groups = 0;

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
  // Optional mapping from chunk-local point indices -> source total point indices.
  // - size == n_points when available.
  // - for chunks loaded from a bundle, this is present only if the bundle contained it.
  std::vector<int> point_indices_total_storage;
  std::vector<std::string> type_group_name_storage;
  std::vector<const char*> type_group_name_ptrs;
  std::vector<int> type_group_starts_storage;
  std::vector<int> type_group_indices_storage;
  std::string meta_tree_json;
  std::string reason;
  int label_norm = sqzc3d_LABEL_NORM_EXACT;
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

static thread_local std::string g_last_error;
static thread_local sqzc3dDec::ErrorDetail g_last_error_detail;

static void reset_global_error() {
  g_last_error.clear();
  g_last_error_detail = {};
  g_last_error_detail.status = sqzc3d_STATUS_SUCCESS;
}

static void set_global_error(
    int status,
    const std::string& message,
    const char* api = nullptr,
    const char* section = nullptr,
    int index = -1) {
  g_last_error = message;
  g_last_error_detail.status = status;
  g_last_error_detail.api = api ? api : "";
  g_last_error_detail.section = section ? section : "";
  g_last_error_detail.index = index;
  g_last_error_detail.message = message;
}

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
        if (ch < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[(ch >> 4) & 0x0f]);
          out.push_back(kHex[ch & 0x0f]);
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  return out;
}

#if sqzc3d_WITH_EZC3D
static void json_write_quoted(std::ostream& out, const std::string& text) {
  out << "\"" << json_escape(text) << "\"";
}

static void json_write_bool(std::ostream& out, bool value) {
  out << (value ? "true" : "false");
}

static void json_write_size_t_list(std::ostream& out, const std::vector<std::size_t>& values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) out << ",";
    out << static_cast<unsigned long long>(values[i]);
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
    const ezc3d::ParametersNS::Parameters& parameters,
    const std::int64_t point_frames_override) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(std::numeric_limits<double>::max_digits10);
  out << "{";
  out << "\"groups\":{";
  bool first_group = true;
  for (const auto& group : parameters.groups()) {
    if (!first_group) out << ",";
    first_group = false;

    const std::string& group_name = group.name();
    json_write_quoted(out, group_name);
    out << ":{";
    out << "\"name\":";
    json_write_quoted(out, group_name);
    out << ",\"description\":";
    json_write_quoted(out, group.description());
    out << ",\"locked\":";
    json_write_bool(out, group.isLocked());
    out << ",\"parameters\":{";

    bool first_param = true;
    for (const auto& parameter : group.parameters()) {
      if (!first_param) out << ",";
      first_param = false;

      const std::string& param_name = parameter.name();
      json_write_quoted(out, param_name);
      out << ":{";
      out << "\"name\":";
      json_write_quoted(out, param_name);
      out << ",\"description\":";
      json_write_quoted(out, parameter.description());
      out << ",\"locked\":";
      json_write_bool(out, parameter.isLocked());
      out << ",\"type\":" << static_cast<int>(parameter.type());
      out << ",\"dimensions\":";
      json_write_size_t_list(out, parameter.dimension());
      out << ",\"values\":";

      const auto type = parameter.type();
      if (type == ezc3d::DATA_TYPE::CHAR) {
        json_write_string_list(out, parameter.valuesAsString());
      } else if (type == ezc3d::DATA_TYPE::FLOAT) {
        std::vector<double> values = parameter.valuesAsDouble();
        if (group_name == "POINT" && param_name == "FRAMES" && point_frames_override > 0) {
          values.assign(1u, static_cast<double>(point_frames_override));
        }
        json_write_numeric_list(out, values);
      } else {
        std::vector<int> values = parameter.valuesConvertedAsInt();
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
#endif

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

static bool parse_bundle_int_list(
    const std::string& text, const char* key, std::vector<int>& out_values) {
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
    size_t end = pos;
    if (end >= text.size() ||
        !(text[end] == '-' || text[end] == '+' || isdigit(static_cast<unsigned char>(text[end])))) {
      return false;
    }
    while (
        end < text.size() && (text[end] == '-' || text[end] == '+' || isdigit(static_cast<unsigned char>(text[end])))) {
      ++end;
    }
    if (end == pos) return false;
    try {
      const auto value = std::stoll(text.substr(pos, end - pos));
      if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return false;
      }
      out_values.push_back(static_cast<int>(value));
    } catch (...) {
      return false;
    }
    pos = end;
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
  unsigned char buf[8]{};
  buf[0] = static_cast<unsigned char>((value >> 0) & 0xffu);
  buf[1] = static_cast<unsigned char>((value >> 8) & 0xffu);
  buf[2] = static_cast<unsigned char>((value >> 16) & 0xffu);
  buf[3] = static_cast<unsigned char>((value >> 24) & 0xffu);
  buf[4] = static_cast<unsigned char>((value >> 32) & 0xffu);
  buf[5] = static_cast<unsigned char>((value >> 40) & 0xffu);
  buf[6] = static_cast<unsigned char>((value >> 48) & 0xffu);
  buf[7] = static_cast<unsigned char>((value >> 56) & 0xffu);
  out.write(reinterpret_cast<const char*>(buf), sizeof(buf));
  return out.good() ? sizeof(buf) : 0u;
}

static bool read_little_u64(std::istream& in, std::uint64_t* out) {
  if (!out) return false;
  unsigned char buf[8]{};
  if (!in.read(reinterpret_cast<char*>(buf), sizeof(buf))) return false;
  *out = (static_cast<std::uint64_t>(buf[0]) << 0) |
         (static_cast<std::uint64_t>(buf[1]) << 8) |
         (static_cast<std::uint64_t>(buf[2]) << 16) |
         (static_cast<std::uint64_t>(buf[3]) << 24) |
         (static_cast<std::uint64_t>(buf[4]) << 32) |
         (static_cast<std::uint64_t>(buf[5]) << 40) |
         (static_cast<std::uint64_t>(buf[6]) << 48) |
         (static_cast<std::uint64_t>(buf[7]) << 56);
  return true;
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

[[maybe_unused]] static void set_error(
    sqzc3d_dec_t* dec,
    int status,
    const std::string& message,
    const char* api = nullptr,
    const char* section = nullptr,
    int index = -1) {
  set_global_error(status, message, api, section, index);
  if (!dec || !dec->impl) return;
  auto* impl = static_cast<sqzc3dDec*>(dec->impl);
  impl->last_error = message;
  impl->last_error_detail.status = status;
  impl->last_error_detail.api = api ? api : "";
  impl->last_error_detail.section = section ? section : "";
  impl->last_error_detail.index = index;
  impl->last_error_detail.message = message;
  dec->last_error = impl->last_error.c_str();
}

static void reset_error(sqzc3d_dec_t* dec) {
  reset_global_error();
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
  std::int64_t s = start;
  std::int64_t c = count;
  const std::int64_t total = static_cast<std::int64_t>(n_total);
  if (s < 0) s += total;
  if (c < 0) c = total - s;
  if (s < 0) s = 0;
  if (s > total) s = total;
  if (c < 0) c = 0;
  if (s + c > total) c = total - s;
  if (out_start) *out_start = static_cast<int>(s);
  if (out_count) *out_count = static_cast<int>(c);
  return c >= 0;
}

static int map_labels_to_indices(
    const std::vector<std::string>& haystack,
    const char* const* labels,
    int n_labels,
    std::vector<int>& out_indices,
    int miss_value,
    int norm_mode,
    std::string* out_error) {
  if (out_error) out_error->clear();
  if (n_labels < 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (n_labels > 0 && !labels) return sqzc3d_STATUS_INVALID_ARGUMENT;
  out_indices.assign(static_cast<std::size_t>(n_labels), miss_value);
  for (int i = 0; i < n_labels; ++i) {
    if (!labels[i]) return sqzc3d_STATUS_INVALID_ARGUMENT;
    const auto target = normalize_label(labels[i], norm_mode);
    int match_index = -1;
    int match_count = 0;
    std::ostringstream matches;
    for (int j = 0; j < static_cast<int>(haystack.size()); ++j) {
      if (normalize_label(haystack[j], norm_mode) == target) {
        if (match_count == 0) {
          match_index = j;
        }
        if (match_count < 5) {
          if (match_count > 0) matches << ", ";
          matches << "'" << haystack[static_cast<std::size_t>(j)] << "'@" << j;
        }
        ++match_count;
      }
    }
    if (match_count == 1) {
      out_indices[static_cast<std::size_t>(i)] = match_index;
      continue;
    }
    if (out_error) {
      std::ostringstream os;
      if (match_count == 0) {
        os << "label not found: '" << labels[i] << "'";
      } else {
        os << "label selector is ambiguous under label_norm=" << norm_mode << ": '" << labels[i]
           << "' matched " << match_count << " labels";
        const auto s = matches.str();
        if (!s.empty()) os << " (" << s << (match_count > 5 ? ", ..." : "") << ")";
      }
      *out_error = os.str();
    }
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  return sqzc3d_STATUS_SUCCESS;
}

static int build_point_selection(
    const sqzc3d_build_opt_t* opt,
    const std::vector<std::string>& source_point_labels,
    int n_points_total,
    std::vector<int>& out_indices,
    int label_norm,
    std::string* out_error) {
  out_indices.clear();
  if (out_error) out_error->clear();
  if (opt->point_sel_mode == sqzc3d_POINT_SEL_ALL) {
    out_indices.resize(static_cast<std::size_t>(n_points_total));
    for (int i = 0; i < n_points_total; ++i) out_indices[static_cast<std::size_t>(i)] = i;
    return sqzc3d_STATUS_SUCCESS;
  }
  if (opt->point_sel_mode == sqzc3d_POINT_SEL_INDICES) {
    if (opt->point_sel_count < 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
    if (opt->point_sel_count == 0) return sqzc3d_STATUS_SUCCESS;
    if (!opt->point_sel) return sqzc3d_STATUS_INVALID_ARGUMENT;
    out_indices.assign(opt->point_sel, opt->point_sel + opt->point_sel_count);
  } else if (opt->point_sel_mode == sqzc3d_POINT_SEL_LABELS) {
    const int label_count =
        opt->point_labels_count > 0 ? opt->point_labels_count : opt->point_sel_count;
    if (label_count < 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
    if (label_count == 0) return sqzc3d_STATUS_SUCCESS;
    if (!opt->point_labels) return sqzc3d_STATUS_INVALID_ARGUMENT;
    return map_labels_to_indices(
        source_point_labels, opt->point_labels, label_count, out_indices, -1, label_norm, out_error);
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
    int label_norm,
    std::string* out_error) {
  out_indices.clear();
  if (out_error) out_error->clear();
  if (n_analogs_total <= 0 || opt->analog_enable == sqzc3d_ANALOG_EN_OFF) return sqzc3d_STATUS_SUCCESS;

  if (opt->analog_sel_mode == sqzc3d_ANALOG_SEL_ALL) {
    out_indices.resize(static_cast<std::size_t>(n_analogs_total));
    for (int i = 0; i < n_analogs_total; ++i) out_indices[static_cast<std::size_t>(i)] = i;
    return sqzc3d_STATUS_SUCCESS;
  }
  if (opt->analog_sel_mode == sqzc3d_ANALOG_SEL_INDICES) {
    if (opt->analog_sel_count < 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
    if (opt->analog_sel_count == 0) return sqzc3d_STATUS_SUCCESS;
    if (!opt->analog_sel) return sqzc3d_STATUS_INVALID_ARGUMENT;
    out_indices.assign(opt->analog_sel, opt->analog_sel + opt->analog_sel_count);
    for (const int idx : out_indices) {
      if (idx < 0 || idx >= n_analogs_total) return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
    return sqzc3d_STATUS_SUCCESS;
  }
  if (opt->analog_sel_mode == sqzc3d_ANALOG_SEL_LABELS) {
    const int label_count =
        opt->analog_labels_count > 0 ? opt->analog_labels_count : opt->analog_sel_count;
    if (label_count < 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
    if (label_count == 0) return sqzc3d_STATUS_SUCCESS;
    if (!opt->analog_labels) return sqzc3d_STATUS_INVALID_ARGUMENT;
    return map_labels_to_indices(
        source_analog_labels, opt->analog_labels, label_count, out_indices, -1, label_norm, out_error);
  }
  return sqzc3d_STATUS_INVALID_ARGUMENT;
}

struct PointSelectionMap {
  int n_total = 0;
  std::vector<int> head;
  std::vector<int> next;

  static PointSelectionMap build(const std::vector<int>& local_to_total, int n_total_points) {
    PointSelectionMap map;
    map.n_total = n_total_points;
    if (n_total_points <= 0 || local_to_total.empty()) {
      return map;
    }
    map.head.assign(static_cast<std::size_t>(n_total_points), -1);
    map.next.assign(local_to_total.size(), -1);
    for (int local = static_cast<int>(local_to_total.size()) - 1; local >= 0; --local) {
      const int total = local_to_total[static_cast<std::size_t>(local)];
      if (total < 0 || total >= n_total_points) continue;
      map.next[static_cast<std::size_t>(local)] = map.head[static_cast<std::size_t>(total)];
      map.head[static_cast<std::size_t>(total)] = local;
    }
    return map;
  }

  template <typename Fn>
  void for_each_local_of_total(const int total, Fn&& fn) const {
    if (total < 0 || total >= n_total) return;
    if (head.empty()) return;
    for (int local = head[static_cast<std::size_t>(total)]; local >= 0;
         local = next[static_cast<std::size_t>(local)]) {
      fn(local);
    }
  }
};

static bool remap_type_groups_total_to_local(
    const std::vector<int>& src_starts,
    const std::vector<int>& src_indices,
    const PointSelectionMap& map,
    const std::size_t n_groups,
    std::vector<int>& out_starts,
    std::vector<int>& out_indices) {
  out_starts.assign(n_groups + 1u, 0);
  out_indices.clear();
  if (n_groups == 0u) return true;
  if (src_starts.size() != n_groups + 1u) return false;
  out_starts[0] = 0;
  for (std::size_t g = 0; g < n_groups; ++g) {
    const int s = src_starts[g];
    const int e = src_starts[g + 1u];
    if (s < 0 || e < s || e > static_cast<int>(src_indices.size())) {
      out_starts[g + 1u] = static_cast<int>(out_indices.size());
      continue;
    }
    for (int k = s; k < e; ++k) {
      const int total = src_indices[static_cast<std::size_t>(k)];
      map.for_each_local_of_total(total, [&](const int local) { out_indices.push_back(local); });
    }
    out_starts[g + 1u] = static_cast<int>(out_indices.size());
  }
  return true;
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
  out_opt->open_mode = sqzc3d_FILE;
  out_opt->cache_labels = 1;
  out_opt->label_norm = sqzc3d_LABEL_NORM_EXACT;
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
  out_opt->analog_enable = sqzc3d_ANALOG_EN_ON;
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
  out_opt->analog_size_soft_limit_bytes = 500LL << 20;
}

sqzc3d_API void sqzc3d_default_time_axis(sqzc3d_time_axis_t* out_axis) {
  if (!out_axis) return;
  std::memset(out_axis, 0, sizeof(*out_axis));
  out_axis->struct_size = static_cast<int>(sizeof(*out_axis));
  out_axis->source_last_frame = -1;
  out_axis->point_rate_hz = 0.0;
  out_axis->analog_rate_hz = 0.0;
}

sqzc3d_API void sqzc3d_apply_preset_stream_frame_all(sqzc3d_build_opt_t* out_opt) {
  if (!out_opt) return;
  sqzc3d_default_build_opt(out_opt);
  out_opt->analog_enable = sqzc3d_ANALOG_EN_OFF;
  out_opt->frame_range = {0, -1};
  out_opt->point_sel_mode = sqzc3d_POINT_SEL_ALL;
  out_opt->point_sel = nullptr;
  out_opt->point_sel_count = 0;
  out_opt->point_labels = nullptr;
  out_opt->point_labels_count = 0;
  out_opt->residual_gate_mm = 0.0;
  out_opt->read_policy = sqzc3d_READ_POLICY_AUTO;
}

sqzc3d_API void sqzc3d_apply_preset_stream_frame_sel(sqzc3d_build_opt_t* out_opt) {
  if (!out_opt) return;
  sqzc3d_default_build_opt(out_opt);
  out_opt->analog_enable = sqzc3d_ANALOG_EN_OFF;
  out_opt->frame_range = {0, -1};
  out_opt->point_sel_mode = sqzc3d_POINT_SEL_ALL;
  out_opt->residual_gate_mm = 0.0;
  out_opt->read_policy = sqzc3d_READ_POLICY_AUTO;
}

sqzc3d_API void sqzc3d_apply_preset_window_analysis(sqzc3d_build_opt_t* out_opt) {
  if (!out_opt) return;
  sqzc3d_default_build_opt(out_opt);
  out_opt->analog_enable = sqzc3d_ANALOG_EN_AUTO;
  out_opt->frame_range = {0, -1};
  out_opt->point_sel_mode = sqzc3d_POINT_SEL_ALL;
  out_opt->residual_gate_mm = 5.0;
  out_opt->read_policy = sqzc3d_READ_POLICY_AUTO;
  out_opt->valid_policy = sqzc3d_VALID_POLICY_FINITE_XYZ;
}

sqzc3d_API void sqzc3d_apply_preset_interpolation_ready(sqzc3d_build_opt_t* out_opt) {
  if (!out_opt) return;
  sqzc3d_default_build_opt(out_opt);
  out_opt->analog_enable = sqzc3d_ANALOG_EN_OFF;
  out_opt->frame_range = {0, -1};
  out_opt->point_sel_mode = sqzc3d_POINT_SEL_ALL;
  out_opt->residual_gate_mm = 5.0;
  out_opt->read_policy = sqzc3d_READ_POLICY_AUTO;
  out_opt->valid_policy = sqzc3d_VALID_POLICY_FINITE_XYZ;
}

sqzc3d_API void sqzc3d_default_bundle_load_opt(sqzc3d_bundle_load_opt_t* out_opt) {
  if (!out_opt) return;
  out_opt->struct_size = static_cast<int>(sizeof(*out_opt));
  out_opt->strict = 1;
  out_opt->reserved = 0;
}

sqzc3d_API int sqzc3d_get_features(void) {
  int features = SQZC3D_FEATURE_BUNDLE | SQZC3D_FEATURE_ANALOG;
#if sqzc3d_WITH_EZC3D
  features |= SQZC3D_FEATURE_OPEN_FILE | SQZC3D_FEATURE_OPEN_MEMORY | SQZC3D_FEATURE_BUILD_CHUNKS;
#endif
  return features;
}

sqzc3d_API const char* sqzc3d_version(void) {
  return SQZC3D_VERSION;
}

sqzc3d_API int sqzc3d_abi_version(void) {
  return SQZC3D_ABI_VERSION;
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
  if (!dec) {
    out_detail->status = g_last_error_detail.status;
    out_detail->api = g_last_error_detail.api.empty() ? "" : g_last_error_detail.api.c_str();
    out_detail->section = g_last_error_detail.section.empty() ? "" : g_last_error_detail.section.c_str();
    out_detail->index = g_last_error_detail.index;
    out_detail->message = g_last_error_detail.message.empty() ? "" : g_last_error_detail.message.c_str();
    return sqzc3d_STATUS_SUCCESS;
  }
  if (!dec->impl) return sqzc3d_STATUS_INVALID_ARGUMENT;
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
  if (!out_dec || !file_path) {
    set_error(nullptr, sqzc3d_STATUS_INVALID_ARGUMENT, "open_file called with invalid argument", "sqzc3d_open_file");
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  sqzc3d_open_opt_t opt_v{};
  sqzc3d_default_open_opt(&opt_v);
  const sqzc3d_open_opt_t* opt_in = &opt_v;
  if (opt) {
    if (opt->struct_size != static_cast<int>(sizeof(sqzc3d_open_opt_t))) {
      set_error(nullptr, sqzc3d_STATUS_INVALID_ARGUMENT, "open_file: opt struct_size mismatch", "sqzc3d_open_file");
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
    opt_v = *opt;
  }
  *out_dec = nullptr;
  auto* dec_raw = new (std::nothrow) sqzc3d_dec_t{};
  if (!dec_raw) return sqzc3d_STATUS_INTERNAL_ERROR;
  std::unique_ptr<sqzc3d_dec_t> dec(dec_raw);
  try {
    dec->impl = nullptr;
    dec->last_error = kArgError;
    auto impl = std::make_unique<sqzc3dDec>();
    impl->reader = std::make_unique<C3dStreamReader>();
    const int label_norm = (opt_in->label_norm > 0)
                          ? (opt_in->label_norm & (sqzc3d_LABEL_NORM_EXACT |
                                                   sqzc3d_LABEL_NORM_TRIM |
                                                   sqzc3d_LABEL_NORM_CASEFOLD_WS))
                          : sqzc3d_LABEL_NORM_EXACT;
    impl->label_norm = label_norm;
    if (opt_in->open_mode != sqzc3d_FILE) {
      set_error(dec.get(), sqzc3d_STATUS_INVALID_ARGUMENT, "open_file called with non-file open_mode", "sqzc3d_open_file");
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
    const int open_st = sqzc3d::sqzc3d_c3d_stream_open_file(
        impl->reader.get(), file_path);
    if (open_st != sqzc3d_STATUS_SUCCESS) {
      const std::string msg = "failed to open c3d file: " + std::string(file_path);
      set_error(dec.get(), open_st, msg, "sqzc3d_open_file");
      return open_st;
    }
    dec->impl = impl.release();
    if (opt_in->cache_labels == 0) {
      auto* impl_out = static_cast<sqzc3dDec*>(dec->impl);
      if (impl_out && impl_out->reader) {
        impl_out->reader->point_labels.clear();
        impl_out->reader->analog_labels.clear();
      }
    }
    reset_error(dec.get());
    *out_dec = dec.release();
    return sqzc3d_STATUS_SUCCESS;
  } catch (const std::bad_alloc&) {
    set_error(dec.get(), sqzc3d_STATUS_INTERNAL_ERROR, "open_file: bad_alloc", "sqzc3d_open_file");
    return sqzc3d_STATUS_INTERNAL_ERROR;
  } catch (const std::exception& e) {
    set_error(dec.get(), sqzc3d_STATUS_INTERNAL_ERROR, std::string("open_file: exception: ") + e.what(), "sqzc3d_open_file");
    return sqzc3d_STATUS_INTERNAL_ERROR;
  } catch (...) {
    set_error(dec.get(), sqzc3d_STATUS_INTERNAL_ERROR, "open_file: unknown exception", "sqzc3d_open_file");
    return sqzc3d_STATUS_INTERNAL_ERROR;
  }
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
  if (!out_dec || !data || n_bytes <= 0) {
    set_error(nullptr, sqzc3d_STATUS_INVALID_ARGUMENT, "open_memory called with invalid argument", "sqzc3d_open_memory");
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  *out_dec = nullptr;
  sqzc3d_open_opt_t opt_v{};
  sqzc3d_default_open_opt(&opt_v);
  const sqzc3d_open_opt_t* opt_in = &opt_v;
  if (opt) {
    if (opt->struct_size != static_cast<int>(sizeof(sqzc3d_open_opt_t))) {
      set_error(nullptr, sqzc3d_STATUS_INVALID_ARGUMENT, "open_memory: opt struct_size mismatch", "sqzc3d_open_memory");
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
    opt_v = *opt;
  }

  std::filesystem::path tmp_path;
  try {
    const auto* bytes = static_cast<const char*>(data);
    std::error_code ec;
    const auto tmp_dir = std::filesystem::temp_directory_path(ec);
    if (ec || tmp_dir.empty()) {
      set_error(nullptr,
                sqzc3d_STATUS_INVALID_ARGUMENT,
                std::string("open_memory: temp_directory_path failed: ") +
                    (ec ? ec.message() : std::string("invalid temp path")),
                "sqzc3d_open_memory");
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
    if (!std::filesystem::exists(tmp_dir, ec)) {
      set_error(nullptr,
                sqzc3d_STATUS_INVALID_ARGUMENT,
                "open_memory: temp directory does not exist",
                "sqzc3d_open_memory");
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }

    bool temp_written = false;
    for (int attempt = 0; attempt < 32; ++attempt) {
      tmp_path = make_unique_tmp_c3d_path(tmp_dir);
      if (std::filesystem::exists(tmp_path, ec)) continue;

      std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
      if (!ofs) {
        continue;
      }
      ofs.write(bytes, static_cast<std::streamsize>(n_bytes));
      if (!ofs.good()) {
        std::filesystem::remove(tmp_path, ec);
        tmp_path.clear();
        continue;
      }
      ofs.close();
      temp_written = true;
      break;
    }
    if (!temp_written) {
      set_error(nullptr,
                sqzc3d_STATUS_INVALID_ARGUMENT,
                "open_memory: failed to create/write temp file",
                "sqzc3d_open_memory");
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }

    sqzc3d_open_opt_t open_file_opt = *opt_in;
    open_file_opt.open_mode = sqzc3d_FILE;
    const int st = sqzc3d_open_file(out_dec, tmp_path.string().c_str(), &open_file_opt);
    if (st != sqzc3d_STATUS_SUCCESS) {
      const char* open_file_error = sqzc3d_last_error(nullptr);
      std::string err_msg = std::string("open_memory: open_file failed, path=") + tmp_path.string();
      if (open_file_error && *open_file_error) {
        err_msg += "; reason=" + std::string(open_file_error);
      }
      set_error(nullptr, st, err_msg, "sqzc3d_open_memory");
      std::error_code ec_rm;
      std::filesystem::remove(tmp_path, ec_rm);
      tmp_path.clear();
      return st;
    }
    auto* impl = static_cast<sqzc3dDec*>((*out_dec)->impl);
    impl->temp_path = tmp_path.string();
    const int label_norm = (opt_in->label_norm > 0)
                              ? (opt_in->label_norm & (sqzc3d_LABEL_NORM_EXACT |
                                                       sqzc3d_LABEL_NORM_TRIM |
                                                       sqzc3d_LABEL_NORM_CASEFOLD_WS))
                              : sqzc3d_LABEL_NORM_EXACT;
    impl->label_norm = label_norm;
    return sqzc3d_STATUS_SUCCESS;
  } catch (const std::bad_alloc&) {
    if (out_dec && *out_dec) {
      sqzc3d_close_dec(*out_dec);
      *out_dec = nullptr;
    }
    if (!tmp_path.empty()) {
      std::error_code ec_rm;
      std::filesystem::remove(tmp_path, ec_rm);
    }
    set_error(nullptr, sqzc3d_STATUS_INTERNAL_ERROR, "open_memory: bad_alloc", "sqzc3d_open_memory");
    return sqzc3d_STATUS_INTERNAL_ERROR;
  } catch (const std::exception& e) {
    if (out_dec && *out_dec) {
      sqzc3d_close_dec(*out_dec);
      *out_dec = nullptr;
    }
    if (!tmp_path.empty()) {
      std::error_code ec_rm;
      std::filesystem::remove(tmp_path, ec_rm);
    }
    set_error(nullptr,
              sqzc3d_STATUS_INTERNAL_ERROR,
              std::string("open_memory: exception: ") + e.what(),
              "sqzc3d_open_memory");
    return sqzc3d_STATUS_INTERNAL_ERROR;
  } catch (...) {
    if (out_dec && *out_dec) {
      sqzc3d_close_dec(*out_dec);
      *out_dec = nullptr;
    }
    if (!tmp_path.empty()) {
      std::error_code ec_rm;
      std::filesystem::remove(tmp_path, ec_rm);
    }
    set_error(nullptr, sqzc3d_STATUS_INTERNAL_ERROR, "open_memory: unknown exception", "sqzc3d_open_memory");
    return sqzc3d_STATUS_INTERNAL_ERROR;
  }
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
  if (!dec) return g_last_error.empty() ? kInvalidDecoderError : g_last_error.c_str();
  if (!dec->impl) return kInvalidDecoderError;
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
  if (!dec || !opt || !out_chunk) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (opt->struct_size != static_cast<int>(sizeof(sqzc3d_build_opt_t))) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  const auto* dec_impl = static_cast<const sqzc3dDec*>(dec->impl);
  if (!dec_impl || !dec_impl->reader || !dec_impl->reader->c3d) return sqzc3d_STATUS_INVALID_ARGUMENT;
  const auto& meta = dec_impl->reader->meta;

  int frame_start = opt->frame_range.start;
  int frame_count = opt->frame_range.count;
  if (!normalize_range(meta.n_frames, frame_start, frame_count, &frame_start, &frame_count)) {
    set_error(const_cast<sqzc3d_dec_t*>(dec), sqzc3d_STATUS_INVALID_ARGUMENT, "invalid frame range", "sqzc3d_build_chunks");
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }

  int analog_range_count = opt->analog_range.count;
  int analog_range_start = opt->analog_range.start;
  if (!normalize_range(meta.n_frames, analog_range_start, analog_range_count, &analog_range_start, &analog_range_count)) {
    set_error(const_cast<sqzc3d_dec_t*>(dec), sqzc3d_STATUS_INVALID_ARGUMENT, "invalid analog range", "sqzc3d_build_chunks");
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }

  auto chunk = new (std::nothrow) sqzc3d_chunk_t();
  if (!chunk) return sqzc3d_STATUS_INTERNAL_ERROR;
  std::memset(chunk, 0, sizeof(sqzc3d_chunk_t));
  chunk->struct_size = static_cast<int>(sizeof(*chunk));
  auto chunk_impl = std::make_unique<sqzc3dChunk>();

  std::vector<int> point_indices;
  std::vector<int> analog_indices;

  std::string selection_error;
  const int st_points =
      build_point_selection(opt,
                            dec_impl->reader->point_labels,
                            meta.n_points,
                            point_indices,
                            dec_impl->label_norm,
                            &selection_error);
  if (st_points != sqzc3d_STATUS_SUCCESS) {
    set_error(
        const_cast<sqzc3d_dec_t*>(dec),
        st_points,
        selection_error.empty() ? "invalid point selection" : selection_error,
        "sqzc3d_build_chunks");
    delete chunk;
    return st_points;
  }
  const int st_analog = build_analog_selection(
      opt,
      dec_impl->reader->analog_labels,
      meta.n_analogs,
      analog_indices,
      dec_impl->label_norm,
      &selection_error);
  if (st_analog != sqzc3d_STATUS_SUCCESS) {
    set_error(
        const_cast<sqzc3d_dec_t*>(dec),
        st_analog,
        selection_error.empty() ? "invalid analog selection" : selection_error,
        "sqzc3d_build_chunks");
    delete chunk;
    return st_analog;
  }

  // Store the local->total point selection mapping for downstream remaps/exports.
  chunk_impl->point_indices_total_storage = point_indices;
  const auto point_map = PointSelectionMap::build(chunk_impl->point_indices_total_storage, meta.n_points);

  int analog_enable = opt->analog_enable;
  if (analog_enable == sqzc3d_ANALOG_EN_AUTO) {
    const int effective_analog_frames = (analog_range_count < frame_count) ? analog_range_count : frame_count;
    if (analog_range_count > frame_count) {
      if (!chunk_impl->reason.empty()) chunk_impl->reason += "; ";
      chunk_impl->reason += "analog_range_count clipped to frame_range count";
    } else if (analog_range_count < frame_count) {
      if (!chunk_impl->reason.empty()) chunk_impl->reason += "; ";
      chunk_impl->reason += "analog_range_count shorter than frame_range count";
    }
    const long long projected_bytes =
        static_cast<long long>(effective_analog_frames) *
        static_cast<long long>(meta.n_analog_by_frame) *
        static_cast<long long>(static_cast<int>(analog_indices.size())) *
        static_cast<long long>(sizeof(sqzc3d_num_t));
    if (analog_indices.empty()) {
      analog_enable = sqzc3d_ANALOG_EN_OFF;
      if (!chunk_impl->reason.empty()) chunk_impl->reason += "; ";
      chunk_impl->reason += "analog skipped: empty channel selection";
    } else if (opt->analog_size_soft_limit_bytes > 0 &&
               projected_bytes > opt->analog_size_soft_limit_bytes) {
      const std::string msg =
          "analog projected size " + std::to_string(projected_bytes) +
          " exceeds analog_size_soft_limit_bytes=" + std::to_string(opt->analog_size_soft_limit_bytes) +
          " (set analog_enable=OFF or raise limit)";
      set_error(const_cast<sqzc3d_dec_t*>(dec),
                sqzc3d_STATUS_INVALID_ARGUMENT,
                msg,
                "sqzc3d_build_chunks");
      delete chunk;
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
  }
  if (opt->analog_enable == sqzc3d_ANALOG_EN_OFF) analog_enable = sqzc3d_ANALOG_EN_OFF;
  if (meta.n_analogs <= 0) analog_enable = sqzc3d_ANALOG_EN_OFF;

  chunk_impl->n_frames = frame_count;
  chunk_impl->has_time_axis = true;
  chunk_impl->frame_start = frame_start;
  chunk_impl->analog_frame_start = analog_range_start;
  chunk_impl->source_first_frame = meta.first_frame;
  chunk_impl->source_last_frame = meta.last_frame;
  chunk_impl->point_rate_hz = meta.point_rate_hz;
  chunk_impl->analog_rate_hz = meta.analog_rate_hz;
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

  // POINT:TYPE_GROUPS / group params are indexed in the source total point index space.
  // Remap into the chunk-local [0..n_points) index space for the chunk contract.
  chunk_impl->type_group_name_storage = dec_impl->reader->type_group_names;
  chunk_impl->type_group_starts_storage.clear();
  chunk_impl->type_group_indices_storage.clear();
  if (!chunk_impl->type_group_name_storage.empty()) {
    if (!remap_type_groups_total_to_local(
            dec_impl->reader->type_group_starts,
            dec_impl->reader->type_group_indices,
            point_map,
            chunk_impl->type_group_name_storage.size(),
            chunk_impl->type_group_starts_storage,
            chunk_impl->type_group_indices_storage)) {
      // Malformed source metadata; keep group names but empty their indices.
      chunk_impl->type_group_starts_storage.assign(chunk_impl->type_group_name_storage.size() + 1u, 0);
      chunk_impl->type_group_indices_storage.clear();
    }
  } else {
    // Preserve original metadata for chunks without type group names.
    // If type_group_names is empty, starts may still contain a single "0" sentinel from the reader.
    chunk_impl->type_group_starts_storage = dec_impl->reader->type_group_starts;
    chunk_impl->type_group_indices_storage = dec_impl->reader->type_group_indices;
  }
  if (!chunk_impl->type_group_name_storage.empty()) {
    chunk_impl->type_group_name_ptrs.resize(chunk_impl->type_group_name_storage.size());
    for (std::size_t i = 0; i < chunk_impl->type_group_name_storage.size(); ++i) {
      chunk_impl->type_group_name_ptrs[static_cast<std::size_t>(i)] = chunk_impl->type_group_name_storage[i].c_str();
    }
  }
  if (!chunk_impl->type_group_starts_storage.empty() || !chunk_impl->type_group_indices_storage.empty()) {
    chunk_impl->n_type_groups = static_cast<int>(chunk_impl->type_group_name_storage.size());
  } else {
    chunk_impl->n_type_groups = 0;
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
      set_error(const_cast<sqzc3d_dec_t*>(dec), st, "read frame point data failed", "sqzc3d_build_chunks");
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
        set_error(const_cast<sqzc3d_dec_t*>(dec), st, "read frame residual data failed", "sqzc3d_build_chunks");
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
    const std::size_t total_samples = static_cast<std::size_t>(chunk_impl->n_frames) *
                                      static_cast<std::size_t>(chunk_impl->n_analog_by_frame);
    const std::size_t total_scalar = static_cast<std::size_t>(chunk_impl->n_analogs) * total_samples;
    // Store analogs as channel-major (C, N) with N=frames*samples_per_frame.
    chunk_impl->analog_storage.resize(total_scalar);
    chunk_impl->analog_valid_storage.assign(total_scalar, 0u);
    chunk_impl->n_analog_scalar = static_cast<int>(chunk_impl->analog_storage.size());
    std::vector<sqzc3d_num_t> analog_frame(static_cast<std::size_t>(analog_sample_count), 0.0);
    const int analog_frames = (analog_range_count < chunk_impl->n_frames) ? analog_range_count : chunk_impl->n_frames;
    for (int fi = 0; fi < chunk_impl->n_frames; ++fi) {
      if (fi >= analog_frames) {
        continue;
      }
      const int f = analog_range_start + fi;
      const auto st = sqzc3d::sqzc3d_c3d_stream_read_frame_analogs_sel(
          dec_impl->reader.get(),
          f,
          analog_indices.data(),
          static_cast<int>(analog_indices.size()),
          0,
          meta.n_analog_by_frame,
          analog_frame.data(),
          analog_sample_count);
      if (st != sqzc3d_STATUS_SUCCESS) {
        set_error(const_cast<sqzc3d_dec_t*>(dec), st, "read frame analog data failed", "sqzc3d_build_chunks");
        delete chunk;
        return st;
      }
      const std::size_t sample_base = static_cast<std::size_t>(fi) *
                                      static_cast<std::size_t>(chunk_impl->n_analog_by_frame);
      for (int c = 0; c < chunk_impl->n_analogs; ++c) {
        auto* out_ptr = chunk_impl->analog_storage.data() +
                        static_cast<std::size_t>(c) * total_samples + sample_base;
        auto* valid_ptr = chunk_impl->analog_valid_storage.data() +
                          static_cast<std::size_t>(c) * total_samples + sample_base;
        for (int s = 0; s < chunk_impl->n_analog_by_frame; ++s) {
          out_ptr[static_cast<std::size_t>(s)] =
              analog_frame[static_cast<std::size_t>(s) *
                               static_cast<std::size_t>(chunk_impl->n_analogs) +
                           static_cast<std::size_t>(c)];
          valid_ptr[static_cast<std::size_t>(s)] = 1u;
        }
      }
    }
  }

  // Snapshot the parameter tree (meta_tree) into the chunk so it can survive:
  // - open_memory (temp-file path not exposed to the caller),
  // - bundle export/load roundtrips,
  // - dec lifetime (chunk may outlive the decoder).
#if sqzc3d_WITH_EZC3D
  try {
    if (dec_impl->reader && dec_impl->reader->c3d) {
      chunk_impl->meta_tree_json =
          build_meta_tree_json(dec_impl->reader->c3d->parameters(), static_cast<std::int64_t>(chunk_impl->n_frames));
    }
  } catch (const std::exception& e) {
    if (!chunk_impl->reason.empty()) chunk_impl->reason += "; ";
    chunk_impl->reason += std::string("meta_tree snapshot failed: ") + e.what();
  } catch (...) {
    if (!chunk_impl->reason.empty()) chunk_impl->reason += "; ";
    chunk_impl->reason += "meta_tree snapshot failed: unknown exception";
  }
#endif

  chunk->n_frames = chunk_impl->n_frames;
  chunk->n_points = chunk_impl->n_points;
  chunk->n_points_total = chunk_impl->n_points_total;
  chunk->n_analogs = chunk_impl->n_analogs;
  chunk->n_analog_by_frame = chunk_impl->n_analog_by_frame;
  chunk->n_type_groups = chunk_impl->n_type_groups;
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
  chunk->points_xyz = chunk_impl->points_xyz_storage.empty() ? nullptr : chunk_impl->points_xyz_storage.data();
  chunk->points_valid = chunk_impl->points_valid_storage.empty() ? nullptr : chunk_impl->points_valid_storage.data();
  chunk->analog = chunk_impl->analog_storage.empty() ? nullptr : chunk_impl->analog_storage.data();
  chunk->analog_valid = chunk_impl->analog_valid_storage.empty() ? nullptr : chunk_impl->analog_valid_storage.data();
  chunk->point_labels = chunk_impl->point_label_ptrs.empty() ? nullptr : chunk_impl->point_label_ptrs.data();
  chunk->analog_labels = chunk_impl->analog_label_ptrs.empty() ? nullptr : chunk_impl->analog_label_ptrs.data();
  chunk->type_group_names = chunk_impl->type_group_name_ptrs.empty() ? nullptr : chunk_impl->type_group_name_ptrs.data();
  chunk->type_group_starts = chunk_impl->type_group_starts_storage.empty() ? nullptr : chunk_impl->type_group_starts_storage.data();
  chunk->type_group_indices = chunk_impl->type_group_indices_storage.empty() ? nullptr : chunk_impl->type_group_indices_storage.data();
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
  reset_global_error();
  auto fail = [](int code, const std::string& message) {
    set_global_error(code, message, "sqzc3d_export_bundle");
    return code;
  };
  if (!out_dir || !out_dir[0] || !chunk) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "export_bundle: invalid argument");
  }

  namespace fs = std::filesystem;
  const fs::path out_path = fs::path(out_dir);
  const auto ext = normalize_extension(out_path.extension().string());
  const bool single_file = (ext == ".sqzc3d");

  const bool has_points_xyz = chunk->points_xyz != nullptr && chunk->n_scalar > 0;
  const bool has_points_valid = chunk->points_valid != nullptr && chunk->valid_nscalar > 0;
  const bool has_analogs = chunk->analog != nullptr && chunk->n_analog_scalar > 0;
  const bool has_analog_valid = chunk->analog_valid != nullptr && chunk->n_analog_scalar > 0;

  const std::uint64_t points_xyz_count = has_points_xyz ? static_cast<std::uint64_t>(chunk->n_scalar) : 0u;
  const std::uint64_t points_valid_count =
      has_points_valid ? static_cast<std::uint64_t>(chunk->valid_nscalar) : 0u;
  const std::uint64_t analog_count = has_analogs ? static_cast<std::uint64_t>(chunk->n_analog_scalar) : 0u;
  const std::uint64_t analog_valid_count = has_analog_valid ? static_cast<std::uint64_t>(chunk->n_analog_scalar) : 0u;

  const auto bytes_points_xyz = points_xyz_count * static_cast<std::uint64_t>(sizeof(sqzc3d_num_t));
  const auto bytes_points_valid = points_valid_count * static_cast<std::uint64_t>(sizeof(unsigned char));
  const auto bytes_analogs = analog_count * static_cast<std::uint64_t>(sizeof(sqzc3d_num_t));
  const auto bytes_analog_valid = analog_valid_count * static_cast<std::uint64_t>(sizeof(unsigned char));

  std::uint64_t cursor = 0u;
  const std::uint64_t points_xyz_offset = cursor;
  cursor += has_points_xyz ? bytes_points_xyz : 0u;
  const std::uint64_t points_valid_offset = cursor;
  cursor += has_points_valid ? bytes_points_valid : 0u;
  const std::uint64_t analog_offset = cursor;
  cursor += has_analogs ? bytes_analogs : 0u;
  const std::uint64_t analog_valid_offset = cursor;
  cursor += has_analog_valid ? bytes_analog_valid : 0u;
  (void)cursor;

  std::ostringstream meta_out;
  const auto reason = chunk->reason ? chunk->reason : "";
  const auto n_point_labels = (chunk->point_labels == nullptr ? 0 : chunk->n_points);
  const auto n_analog_labels = (chunk->analog_labels == nullptr ? 0 : chunk->n_analogs);
  const auto* chunk_impl = static_cast<const sqzc3dChunk*>(chunk->impl);
  const bool has_point_indices_total =
      chunk_impl && static_cast<int>(chunk_impl->point_indices_total_storage.size()) == chunk->n_points;
  const int n_type_group_starts =
      chunk->type_group_starts ? (chunk->n_type_groups >= 0 ? chunk->n_type_groups + 1 : 0) : 0;
  const int n_type_group_indices = (chunk->type_group_starts && chunk->n_type_groups >= 0 &&
                                   static_cast<int>(chunk->type_group_starts[static_cast<std::size_t>(chunk->n_type_groups)]) >= 0)
                                      ? chunk->type_group_starts[static_cast<std::size_t>(chunk->n_type_groups)]
                                      : 0;

  meta_out << "{\n";
  meta_out << "  \"format\": \"sqzc3d_bundle_v2\",\n";
  meta_out << "  \"schema_version\": 3,\n";
  meta_out << "  \"sqzc3d_version\": \"" << SQZC3D_VERSION << "\",\n";
  meta_out << "  \"sqzc3d_abi_version\": " << SQZC3D_ABI_VERSION << ",\n";
  meta_out << "  \"endianness\": \"little\",\n";
  meta_out << "  \"n_frames\": " << chunk->n_frames << ",\n";
  meta_out << "  \"n_points\": " << chunk->n_points << ",\n";
  meta_out << "  \"n_points_total\": " << chunk->n_points_total << ",\n";
  meta_out << "  \"n_analogs\": " << chunk->n_analogs << ",\n";
  meta_out << "  \"n_analog_by_frame\": " << chunk->n_analog_by_frame << ",\n";
  meta_out << "  \"analog_layout\": \"CN\",\n";
  meta_out << "  \"n_scalar\": " << chunk->n_scalar << ",\n";
  meta_out << "  \"valid_nscalar\": " << chunk->valid_nscalar << ",\n";
  meta_out << "  \"n_analog_scalar\": " << chunk->n_analog_scalar << ",\n";
  meta_out << "  \"n_type_groups\": " << chunk->n_type_groups << ",\n";
  meta_out << "  \"points_layout\": \"" << points_layout_name(chunk->points_layout) << "\",\n";
  meta_out << "  \"points_pack\": \"" << points_pack_name(chunk->points_pack) << "\",\n";
  meta_out << "  \"read_policy\": \"" << read_policy_name(chunk->read_policy) << "\",\n";
  meta_out << "  \"valid_policy\": " << chunk->valid_policy << ",\n";
  meta_out << std::fixed << std::setprecision(std::numeric_limits<double>::max_digits10);
  if (chunk_impl && chunk_impl->has_time_axis) {
    meta_out << "  \"source_first_frame\": " << chunk_impl->source_first_frame << ",\n";
    meta_out << "  \"source_last_frame\": " << chunk_impl->source_last_frame << ",\n";
    meta_out << "  \"point_rate_hz\": " << chunk_impl->point_rate_hz << ",\n";
    meta_out << "  \"analog_rate_hz\": " << chunk_impl->analog_rate_hz << ",\n";
    meta_out << "  \"frame_start\": " << chunk_impl->frame_start << ",\n";
    meta_out << "  \"analog_frame_start\": " << chunk_impl->analog_frame_start << ",\n";
  }
  meta_out << "  \"residual_gate_mm\": " << chunk->residual_gate_mm << ",\n";
  meta_out << "  \"point_scale\": " << chunk->point_scale << ",\n";
  meta_out << "  \"header_scale\": " << chunk->header_scale << ",\n";
  meta_out << "  \"reason\": \"" << json_escape(reason) << "\",\n";
  if (chunk_impl && !chunk_impl->meta_tree_json.empty()) {
    meta_out << "  \"meta_tree_json\": \"" << json_escape(chunk_impl->meta_tree_json) << "\",\n";
  }

  auto write_string_list = [&](const char** values, int n_values) {
    for (int i = 0; i < n_values; ++i) {
      if (i > 0) meta_out << ", ";
      const char* v = values && values[static_cast<std::size_t>(i)] ? values[static_cast<std::size_t>(i)] : "";
      meta_out << "\"" << json_escape(v) << "\"";
    }
  };
  auto write_int_list = [&](const int* values, int n_values) {
    for (int i = 0; i < n_values; ++i) {
      if (i > 0) meta_out << ", ";
      meta_out << (values ? values[static_cast<std::size_t>(i)] : 0);
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
      false);
  meta_out << "  },\n";
  meta_out << "  \"point_labels\": [";
  write_string_list(chunk->point_labels, static_cast<int>(n_point_labels));
  meta_out << "],\n";
  if (has_point_indices_total) {
    meta_out << "  \"point_indices_total\": [";
    write_int_list(chunk_impl->point_indices_total_storage.data(), chunk->n_points);
    meta_out << "],\n";
  }
  meta_out << "  \"analog_labels\": [";
  write_string_list(chunk->analog_labels, static_cast<int>(n_analog_labels));
  meta_out << "],\n";
  meta_out << "  \"type_group_names\": [";
  write_string_list(chunk->type_group_names, static_cast<int>(chunk->n_type_groups));
  meta_out << "],\n";
  meta_out << "  \"type_group_starts\": [";
  write_int_list(chunk->type_group_starts, n_type_group_starts);
  meta_out << "],\n";
  meta_out << "  \"type_group_indices\": [";
  write_int_list(chunk->type_group_indices, n_type_group_indices);
  meta_out << "]\n";
  meta_out << "}\n";

  const std::string meta_text = meta_out.str();
  if (!meta_out.good()) return fail(sqzc3d_STATUS_INTERNAL_ERROR, "export_bundle: meta json build failed");

  const fs::path meta_path = out_path / "meta.json";
  const fs::path data_path = out_path / "data.bin";
  if (!single_file) {
    std::error_code ec;
    fs::create_directories(out_path, ec);
    if (ec) return fail(sqzc3d_STATUS_INTERNAL_ERROR, "export_bundle: failed to create output directory");

    std::ofstream data_out(data_path, std::ios::binary | std::ios::trunc);
    if (!data_out.good()) return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "export_bundle: failed to open data.bin for write");
    if (has_points_xyz) write_num(data_out, chunk->points_xyz, points_xyz_count * sizeof(sqzc3d_num_t));
    if (has_points_valid) write_num(data_out, chunk->points_valid, points_valid_count * sizeof(unsigned char));
    if (has_analogs) write_num(data_out, chunk->analog, analog_count * sizeof(sqzc3d_num_t));
    if (has_analog_valid) write_num(data_out, chunk->analog_valid, analog_valid_count * sizeof(unsigned char));
    if (!data_out.good()) return fail(sqzc3d_STATUS_INTERNAL_ERROR, "export_bundle: failed to write data.bin");

    std::ofstream meta_file(meta_path, std::ios::binary | std::ios::trunc);
    if (!meta_file.good()) return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "export_bundle: failed to open meta.json for write");
    meta_file.write(meta_text.data(), static_cast<std::streamsize>(meta_text.size()));
    if (!meta_file.good()) return fail(sqzc3d_STATUS_INTERNAL_ERROR, "export_bundle: failed to write meta.json");
    return sqzc3d_STATUS_SUCCESS;
  }

  std::ofstream container_out(out_path, std::ios::binary | std::ios::trunc);
  if (!container_out.good()) return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "export_bundle: failed to open container for write");
  if (!write_sic_bundle_magic(container_out)) return fail(sqzc3d_STATUS_INTERNAL_ERROR, "export_bundle: write container magic failed");
  if (!write_u64_le(container_out, static_cast<std::uint64_t>(meta_text.size()))) return fail(sqzc3d_STATUS_INTERNAL_ERROR, "export_bundle: write meta size failed");
  container_out.write(meta_text.data(), static_cast<std::streamsize>(meta_text.size()));
  if (!container_out.good()) return fail(sqzc3d_STATUS_INTERNAL_ERROR, "export_bundle: write meta failed");
  if (has_points_xyz) write_num(container_out, chunk->points_xyz, points_xyz_count * sizeof(sqzc3d_num_t));
  if (has_points_valid) write_num(container_out, chunk->points_valid, points_valid_count * sizeof(unsigned char));
  if (has_analogs) write_num(container_out, chunk->analog, analog_count * sizeof(sqzc3d_num_t));
  if (has_analog_valid) write_num(container_out, chunk->analog_valid, analog_valid_count * sizeof(unsigned char));
  if (!container_out.good()) return fail(sqzc3d_STATUS_INTERNAL_ERROR, "export_bundle: write payload failed");
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
  reset_global_error();
  auto fail = [](int code, const std::string& message) {
    set_global_error(code, message, "sqzc3d_load_bundle");
    sqzc3d_DEBUG_BUNDLE_LOG(message);
    return code;
  };
  if (opt->struct_size != static_cast<int>(sizeof(sqzc3d_bundle_load_opt_t))) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: invalid bundle load option struct");
  }
  if (!bundle_dir || !out_chunk || bundle_dir[0] == '\0') {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: invalid bundle path");
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
    if (!meta_in.good()) return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: meta.json open failed");
    meta_text.assign((std::istreambuf_iterator<char>(meta_in)), std::istreambuf_iterator<char>());
  } else if (fs::is_regular_file(input_path)) {
    const auto ext = normalize_extension(input_path.extension().string());
    if (ext != ".sqzc3d") {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: unsupported container suffix (expected .sqzc3d)");
    }
    std::ifstream container_in(input_path, std::ios::binary);
    if (!container_in.good()) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: container open failed");
    }
    if (!is_sqzc3d_container_magic(container_in)) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: container magic mismatch");
    }
    std::uint64_t meta_nbytes = 0u;
    if (!read_little_u64(container_in, &meta_nbytes)) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: meta size parse failed");
    }
    std::error_code size_error;
    const auto total_size = fs::file_size(input_path, size_error);
    if (size_error) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: file size stat failed");
    }
    if (total_size < 16u) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: container header exceeds file size");
    }
    if (static_cast<uintmax_t>(meta_nbytes) > total_size - 16u) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: container header exceeds file size");
    }
    meta_text.resize(static_cast<std::size_t>(meta_nbytes));
    container_in.read(meta_text.data(), static_cast<std::streamsize>(meta_nbytes));
    if (!container_in.good()) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: container meta read failed");
    }
    data_base_offset = 16u + meta_nbytes;
    data_size = static_cast<uintmax_t>(total_size);
    data_path = input_path;
  } else {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: unsupported bundle input type");
  }

  std::string format;
  const bool has_format = parse_bundle_name_value(meta_text, "format", &format);
  const bool is_v2 = has_format && format == "sqzc3d_bundle_v2";
  if (has_format && !(format == "sqzc3d_bundle_v1" || is_v2)) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, std::string("load_bundle: unsupported bundle format: ") + format);
  }
  const bool strict_schema = strict && is_v2;
  if (strict_schema && !has_format) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: missing format under strict mode");
  }
  int64_t schema_version = 0;
  bool has_schema_version = false;
  if (!parse_bundle_schema_version(meta_text, strict_schema, &schema_version, &has_schema_version)) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: schema parse failed");
  }
  if (is_v2 && (!has_schema_version || schema_version != 3)) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, std::string("load_bundle: unsupported schema version: ") + std::to_string(schema_version));
  }

  int64_t n_frames = 0;
  int64_t n_points = 0;
  int64_t n_points_total = 0;
  int64_t n_analogs = 0;
  int64_t n_analog_by_frame = 0;
  int64_t n_scalar = 0;
  int64_t valid_nscalar = 0;
  int64_t n_analog_scalar = 0;
  int64_t n_type_groups = 0;
  int type_group_count_v2 = 0;
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
  if (is_v2) {
    if (!parse_bundle_int_value(meta_text, "n_type_groups", &n_type_groups)) {
      return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "meta integer parse failed");
    }
  } else {
    (void)parse_bundle_optional_int_value(meta_text, "n_type_groups", type_group_count_v2, 0);
    n_type_groups = static_cast<int64_t>(type_group_count_v2);
  }
  if (n_type_groups < 0) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "meta integer parse failed");
  }

  std::string points_layout_name;
  std::string points_pack_name;
  std::string read_policy_name;
  std::string analog_layout;
  int64_t valid_policy = 0;
  double residual_gate_mm = 0.0;
  double point_scale = 1.0;
  double header_scale = 1.0;
  bool has_time_axis = false;
  int source_first_frame = 0;
  int source_last_frame = -1;
  int frame_start_v = 0;
  int analog_frame_start_v = 0;
  double point_rate_hz = 0.0;
  double analog_rate_hz = 0.0;
  std::string endianness;
  std::string reason;
  std::string meta_tree_json;
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

  {
    int64_t v = 0;
    if (parse_bundle_int_value(meta_text, "source_first_frame", &v)) {
      if (!cast_metadata_int(v, &source_first_frame)) {
        return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "source_first_frame out of range");
      }
      has_time_axis = true;
    }
    if (parse_bundle_int_value(meta_text, "source_last_frame", &v)) {
      if (!cast_metadata_int(v, &source_last_frame)) {
        return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "source_last_frame out of range");
      }
      has_time_axis = true;
    }
    if (parse_bundle_int_value(meta_text, "frame_start", &v)) {
      if (!cast_metadata_int(v, &frame_start_v)) {
        return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "frame_start out of range");
      }
      has_time_axis = true;
    }
    if (parse_bundle_int_value(meta_text, "analog_frame_start", &v)) {
      if (!cast_metadata_int(v, &analog_frame_start_v)) {
        return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "analog_frame_start out of range");
      }
      has_time_axis = true;
    }

    if (parse_bundle_double_value(meta_text, "point_rate_hz", &point_rate_hz)) {
      has_time_axis = true;
    } else {
      point_rate_hz = 0.0;
    }
    if (parse_bundle_double_value(meta_text, "analog_rate_hz", &analog_rate_hz)) {
      has_time_axis = true;
    } else if (point_rate_hz > 0.0) {
      analog_rate_hz = point_rate_hz * static_cast<double>(n_analog_by_frame);
    } else {
      analog_rate_hz = 0.0;
    }

    if (has_time_axis && source_last_frame < source_first_frame && n_frames > 0) {
      const auto last = static_cast<long long>(source_first_frame) + static_cast<long long>(n_frames) - 1LL;
      if (last >= std::numeric_limits<int>::min() && last <= std::numeric_limits<int>::max()) {
        source_last_frame = static_cast<int>(last);
      }
    }
  }
  (void)parse_bundle_optional_name_value(meta_text, "meta_tree_json", &meta_tree_json);
  if (!parse_bundle_name_value(meta_text, "analog_layout", &analog_layout) || analog_layout != "CN") {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "unsupported analog layout");
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
  std::vector<int> point_indices_total;
  std::vector<std::string> type_group_names;
  std::vector<int> type_group_starts;
  std::vector<int> type_group_indices;
  const bool has_type_group_names = parse_bundle_string_list(meta_text, "type_group_names", type_group_names);
  const bool has_type_group_starts = parse_bundle_int_list(meta_text, "type_group_starts", type_group_starts);
  const bool has_type_group_indices = parse_bundle_int_list(meta_text, "type_group_indices", type_group_indices);
  const bool has_point_indices_total = parse_bundle_int_list(meta_text, "point_indices_total", point_indices_total);
  if (!parse_bundle_string_list(meta_text, "point_labels", point_labels) ||
      !parse_bundle_string_list(meta_text, "analog_labels", analog_labels) ||
      (is_v2 &&
       (!has_type_group_names || !has_type_group_starts || !has_type_group_indices))) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "label list parse failed");
  }
  if ((has_type_group_names || has_type_group_starts || has_type_group_indices) && !is_v2 &&
      (!has_type_group_names || !has_type_group_starts || !has_type_group_indices)) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "label list parse failed");
  }
  if (static_cast<int64_t>(type_group_names.size()) != n_type_groups ||
      (is_v2 && static_cast<int64_t>(type_group_starts.size()) != n_type_groups + 1)) {
    if (is_v2) {
      return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "type group metadata count mismatch");
    }
  }
  if (is_v2 && !type_group_starts.empty()) {
    if (type_group_starts[0] != 0 ||
        type_group_starts[static_cast<std::size_t>(type_group_starts.size()) - 1] !=
            static_cast<int>(type_group_indices.size())) {
      return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "type group starts malformed");
    }
    for (std::size_t i = 1; i < type_group_starts.size(); ++i) {
      if (type_group_starts[i] < type_group_starts[i - 1] ||
          type_group_starts[i] > static_cast<int>(type_group_indices.size())) {
        return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "type group starts malformed");
      }
    }
  }
  for (const int idx : type_group_indices) {
    if (idx < 0 || idx >= n_points) {
      return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "type group index out of bounds");
    }
  }
  if (has_point_indices_total) {
    if (static_cast<int64_t>(point_indices_total.size()) != n_points) {
      return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "point_indices_total count mismatch");
    }
    for (const int idx : point_indices_total) {
      if (idx < 0 || static_cast<int64_t>(idx) >= n_points_total) {
        return fail(sqzc3d_STATUS_DIMENSION_MISMATCH, "point_indices_total out of bounds");
      }
    }
  }

  BundleSection points_xyz{};
  BundleSection points_valid{};
  BundleSection analogs{};
  BundleSection analog_valid{};
  if (!parse_bundle_section(meta_text, "points_xyz", &points_xyz) ||
      !parse_bundle_section(meta_text, "points_valid", &points_valid) ||
      !parse_bundle_section_if_present(meta_text, "analogs", &analogs) ||
      !parse_bundle_section_if_present(meta_text, "analog_valid", &analog_valid)) {
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "sections parse failed");
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
  if (!chunk) return fail(sqzc3d_STATUS_INTERNAL_ERROR, "load_bundle: out of memory");
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
      !cast_metadata_int(n_type_groups, &chunk_impl->n_type_groups) ||
      !cast_metadata_int(valid_policy, &chunk_impl->valid_policy)) {
    delete chunk;
    return fail(sqzc3d_STATUS_INVALID_ARGUMENT, "load_bundle: metadata cast failed");
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
  chunk_impl->has_time_axis = has_time_axis;
  chunk_impl->source_first_frame = source_first_frame;
  chunk_impl->source_last_frame = source_last_frame;
  chunk_impl->point_rate_hz = point_rate_hz;
  chunk_impl->analog_rate_hz = analog_rate_hz;
  chunk_impl->frame_start = frame_start_v;
  chunk_impl->analog_frame_start = analog_frame_start_v;
  chunk_impl->reason = reason;
  chunk_impl->meta_tree_json = meta_tree_json;
  chunk_impl->label_norm = sqzc3d_LABEL_NORM_EXACT;
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

  chunk_impl->type_group_name_storage = std::move(type_group_names);
  chunk_impl->type_group_starts_storage = std::move(type_group_starts);
  chunk_impl->type_group_indices_storage = std::move(type_group_indices);
  chunk_impl->point_labels_storage = std::move(point_labels);
  chunk_impl->analog_labels_storage = std::move(analog_labels);
  if (has_point_indices_total) {
    chunk_impl->point_indices_total_storage = std::move(point_indices_total);
  } else {
    chunk_impl->point_indices_total_storage.clear();
  }
  chunk_impl->point_label_ptrs.resize(chunk_impl->point_labels_storage.size());
  for (std::size_t i = 0; i < chunk_impl->point_labels_storage.size(); ++i) {
    chunk_impl->point_label_ptrs[i] = chunk_impl->point_labels_storage[i].c_str();
  }
  chunk_impl->analog_label_ptrs.resize(chunk_impl->analog_labels_storage.size());
  for (std::size_t i = 0; i < chunk_impl->analog_labels_storage.size(); ++i) {
    chunk_impl->analog_label_ptrs[i] = chunk_impl->analog_labels_storage[i].c_str();
  }
  chunk_impl->type_group_name_ptrs.resize(chunk_impl->type_group_name_storage.size());
  for (std::size_t i = 0; i < chunk_impl->type_group_name_storage.size(); ++i) {
    chunk_impl->type_group_name_ptrs[i] = chunk_impl->type_group_name_storage[i].c_str();
  }
  if (chunk_impl->type_group_starts_storage.empty() || chunk_impl->type_group_indices_storage.empty()) {
    chunk_impl->n_type_groups = 0;
  } else if (chunk_impl->type_group_name_storage.empty()) {
    chunk_impl->n_type_groups = 0;
  }

  chunk->n_type_groups = chunk_impl->n_type_groups;
  chunk->type_group_names = chunk_impl->type_group_name_ptrs.empty() ? nullptr : chunk_impl->type_group_name_ptrs.data();
  chunk->type_group_starts = chunk_impl->type_group_starts_storage.empty()
                                ? nullptr
                                : chunk_impl->type_group_starts_storage.data();
  chunk->type_group_indices = chunk_impl->type_group_indices_storage.empty()
                                 ? nullptr
                                 : chunk_impl->type_group_indices_storage.data();
  chunk->points_xyz = chunk_impl->points_xyz_storage.empty() ? nullptr : chunk_impl->points_xyz_storage.data();
  chunk->points_valid = chunk_impl->points_valid_storage.empty() ? nullptr : chunk_impl->points_valid_storage.data();
  chunk->analog = chunk_impl->analog_storage.empty() ? nullptr : chunk_impl->analog_storage.data();
  chunk->analog_valid = chunk_impl->analog_valid_storage.empty() ? nullptr : chunk_impl->analog_valid_storage.data();
  chunk->point_labels = chunk_impl->point_label_ptrs.empty() ? nullptr : chunk_impl->point_label_ptrs.data();
  chunk->analog_labels = chunk_impl->analog_label_ptrs.empty() ? nullptr : chunk_impl->analog_label_ptrs.data();
  chunk->reason = chunk_impl->reason.empty() ? nullptr : chunk_impl->reason.c_str();
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

sqzc3d_API int sqzc3d_chunk_point_indices_total(
    const sqzc3d_chunk_t* chunk,
    const int** out_point_indices_total,
    int* out_n_points) {
  if (out_point_indices_total) *out_point_indices_total = nullptr;
  if (out_n_points) *out_n_points = 0;
  if (!chunk || !chunk->impl || !out_point_indices_total || !out_n_points) return sqzc3d_STATUS_INVALID_ARGUMENT;
  const auto* impl = static_cast<const sqzc3dChunk*>(chunk->impl);
  if (!impl) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (impl->point_indices_total_storage.empty()) {
    if (chunk->n_points == 0) {
      *out_point_indices_total = nullptr;
      *out_n_points = 0;
      return sqzc3d_STATUS_SUCCESS;
    }
    return sqzc3d_STATUS_NOT_IMPLEMENTED;
  }
  if (static_cast<int>(impl->point_indices_total_storage.size()) != chunk->n_points) {
    return sqzc3d_STATUS_INTERNAL_ERROR;
  }
  *out_point_indices_total = impl->point_indices_total_storage.data();
  *out_n_points = chunk->n_points;
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_chunk_meta_tree_json(
    const sqzc3d_chunk_t* chunk,
    const char** out_json,
    int* out_nbytes) {
  if (out_json) *out_json = nullptr;
  if (out_nbytes) *out_nbytes = 0;
  if (!chunk || !chunk->impl || !out_json || !out_nbytes) return sqzc3d_STATUS_INVALID_ARGUMENT;
  const auto* impl = static_cast<const sqzc3dChunk*>(chunk->impl);
  if (!impl) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (impl->meta_tree_json.empty()) return sqzc3d_STATUS_NOT_IMPLEMENTED;
  if (impl->meta_tree_json.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return sqzc3d_STATUS_INTERNAL_ERROR;
  }
  *out_json = impl->meta_tree_json.c_str();
  *out_nbytes = static_cast<int>(impl->meta_tree_json.size());
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_chunk_time_axis(
    const sqzc3d_chunk_t* chunk,
    sqzc3d_time_axis_t* out_axis) {
  if (!out_axis) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (out_axis->struct_size != static_cast<int>(sizeof(*out_axis))) {
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  if (!chunk || !chunk->impl) return sqzc3d_STATUS_INVALID_ARGUMENT;
  const auto* impl = static_cast<const sqzc3dChunk*>(chunk->impl);
  if (!impl) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (!impl->has_time_axis) return sqzc3d_STATUS_NOT_IMPLEMENTED;

  out_axis->source_first_frame = impl->source_first_frame;
  out_axis->source_last_frame = impl->source_last_frame;
  out_axis->point_rate_hz = impl->point_rate_hz;
  out_axis->analog_rate_hz = impl->analog_rate_hz;
  out_axis->frame_start = impl->frame_start;
  out_axis->analog_frame_start = impl->analog_frame_start;
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_point_indices_for_labels(
    const sqzc3d_chunk_t* chunk,
    const char** labels,
    int n_labels,
    int* out_indices,
    int miss_idx) {
  reset_global_error();
  if (!chunk || !out_indices || n_labels < 0 || (n_labels > 0 && !labels)) {
    set_global_error(sqzc3d_STATUS_INVALID_ARGUMENT, "point_indices_for_labels: invalid argument", "sqzc3d_point_indices_for_labels");
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  const auto* chunk_impl = static_cast<const sqzc3dChunk*>(chunk->impl);
  const int norm_mode = chunk_impl ? chunk_impl->label_norm : sqzc3d_LABEL_NORM_EXACT;
  for (int i = 0; i < n_labels; ++i) {
    out_indices[static_cast<std::size_t>(i)] = miss_idx;
    if (!labels[i]) continue;
    const auto target = normalize_label(labels[i], norm_mode);
    int match_index = -1;
    int match_count = 0;
    for (int j = 0; j < chunk->n_points; ++j) {
      const char* source = (chunk->point_labels && chunk->point_labels[j]) ? chunk->point_labels[j] : "";
      if (normalize_label(source, norm_mode) == target) {
        if (match_count == 0) {
          match_index = j;
        }
        ++match_count;
        if (match_count > 1) break;
      }
    }
    if (match_count == 1) {
      out_indices[static_cast<std::size_t>(i)] = match_index;
    } else if (match_count > 1) {
      std::ostringstream os;
      os << "label selector is ambiguous under label_norm=" << norm_mode << ": '" << labels[i]
         << "' matched multiple point labels";
      set_global_error(sqzc3d_STATUS_INVALID_ARGUMENT, os.str(), "sqzc3d_point_indices_for_labels");
      return sqzc3d_STATUS_INVALID_ARGUMENT;
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
  reset_global_error();
  if (!chunk || !out_indices || n_labels < 0 || (n_labels > 0 && !labels)) {
    set_global_error(sqzc3d_STATUS_INVALID_ARGUMENT, "analog_indices_for_labels: invalid argument", "sqzc3d_analog_indices_for_labels");
    return sqzc3d_STATUS_INVALID_ARGUMENT;
  }
  const auto* chunk_impl = static_cast<const sqzc3dChunk*>(chunk->impl);
  const int norm_mode = chunk_impl ? chunk_impl->label_norm : sqzc3d_LABEL_NORM_EXACT;
  for (int i = 0; i < n_labels; ++i) {
    out_indices[static_cast<std::size_t>(i)] = miss_idx;
    if (!labels[i]) continue;
    const auto target = normalize_label(labels[i], norm_mode);
    int match_index = -1;
    int match_count = 0;
    for (int j = 0; j < chunk->n_analogs; ++j) {
      const char* source = (chunk->analog_labels && chunk->analog_labels[j]) ? chunk->analog_labels[j] : "";
      if (normalize_label(source, norm_mode) == target) {
        if (match_count == 0) {
          match_index = j;
        }
        ++match_count;
        if (match_count > 1) break;
      }
    }
    if (match_count == 1) {
      out_indices[static_cast<std::size_t>(i)] = match_index;
    } else if (match_count > 1) {
      std::ostringstream os;
      os << "label selector is ambiguous under label_norm=" << norm_mode << ": '" << labels[i]
         << "' matched multiple analog labels";
      set_global_error(sqzc3d_STATUS_INVALID_ARGUMENT, os.str(), "sqzc3d_analog_indices_for_labels");
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
  }
  return sqzc3d_STATUS_SUCCESS;
}

sqzc3d_API int sqzc3d_points_view_frames(
    const sqzc3d_chunk_t* chunk,
    int start,
    int count,
    sqzc3d_points_view_t* out_view) {
  if (!chunk || !out_view || start < 0 || count < 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (start > chunk->n_frames || count > chunk->n_frames - start) return sqzc3d_STATUS_INVALID_ARGUMENT;
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
  if (!chunk || !out_view || start_frame < 0 || n_frames < 0) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (start_frame > chunk->n_frames || n_frames > chunk->n_frames - start_frame) return sqzc3d_STATUS_INVALID_ARGUMENT;
  const int source_stride_samples = chunk->n_frames * chunk->n_analog_by_frame;
  const int sample_start = start_frame * chunk->n_analog_by_frame;
  out_view->analog =
      chunk->analog ? chunk->analog + static_cast<std::size_t>(sample_start) : nullptr;
  out_view->analog_valid =
      chunk->analog_valid ? chunk->analog_valid + static_cast<std::size_t>(sample_start) : nullptr;
  out_view->n_frames = n_frames;
  out_view->n_analog_by_frame = chunk->n_analog_by_frame;
  out_view->n_analogs = chunk->n_analogs;
  out_view->n_samples = n_frames * chunk->n_analog_by_frame;
  out_view->source_stride_samples = source_stride_samples;
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
  const int source_stride_samples = chunk->n_frames * chunk->n_analog_by_frame;
  bool contiguous = n > 0;
  for (int i = 1; i < n; ++i) {
    if (channel_indices[i] != channel_indices[0] + i) {
      contiguous = false;
      break;
    }
  }
  out_view->analog =
      (chunk->analog && contiguous && n > 0)
          ? chunk->analog + static_cast<std::size_t>(channel_indices[0]) *
                               static_cast<std::size_t>(source_stride_samples)
          : chunk->analog;
  out_view->analog_valid =
      (chunk->analog_valid && contiguous && n > 0)
          ? chunk->analog_valid + static_cast<std::size_t>(channel_indices[0]) *
                                     static_cast<std::size_t>(source_stride_samples)
          : chunk->analog_valid;
  out_view->n_frames = chunk->n_frames;
  out_view->n_analog_by_frame = chunk->n_analog_by_frame;
  out_view->n_analogs = n;
  out_view->n_samples = chunk->n_frames * chunk->n_analog_by_frame;
  out_view->source_stride_samples = source_stride_samples;
  out_view->channel_indices = contiguous ? nullptr : channel_indices;
  return sqzc3d_STATUS_SUCCESS;
}


