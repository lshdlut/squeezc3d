#include "sqzc3d.h"
#include "sqzc3d_easy.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#if sqzc3d_WITH_EZC3D
#include <ezc3d/ezc3d_all.h>
#endif

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace {

struct CaseConfig {
  std::string name;
  sqzc3d_build_opt_t opt{};
  std::vector<int> point_indices;
  std::vector<const char*> point_labels;
  std::vector<int> analog_indices;
  std::vector<const char*> analog_labels;
};

bool check_chunk_index_contract(const sqzc3d_chunk_t* chunk, const std::string& tag) {
  if (!chunk) {
    std::cerr << "  - " << tag << ": null chunk\n";
    return false;
  }
  if (chunk->n_points < 0 || chunk->n_points_total < 0 || chunk->n_points > chunk->n_points_total) {
    std::cerr << "  - " << tag << ": invalid n_points/n_points_total: n_points=" << chunk->n_points
              << " n_points_total=" << chunk->n_points_total << "\n";
    return false;
  }
  const long long expected_residual =
      static_cast<long long>(chunk->n_frames) * static_cast<long long>(chunk->n_points);
  if (expected_residual < 0 || expected_residual > std::numeric_limits<int>::max()) {
    std::cerr << "  - " << tag << ": residual scalar dimension out of int range\n";
    return false;
  }
  if (chunk->residual_nscalar != static_cast<int>(expected_residual)) {
    std::cerr << "  - " << tag << ": residual_nscalar=" << chunk->residual_nscalar
              << " expected=" << expected_residual << "\n";
    return false;
  }
  if (chunk->residual_nscalar > 0 && !chunk->points_residual) {
    std::cerr << "  - " << tag << ": residual storage missing\n";
    return false;
  }
  if (!(chunk->point_units_per_meter > 0.0) || !(chunk->target_units_per_meter > 0.0) ||
      !(chunk->residual_units_per_meter > 0.0)) {
    std::cerr << "  - " << tag << ": invalid unit metadata\n";
    return false;
  }

  if (chunk->n_type_groups > 0) {
    if (!chunk->type_group_names || !chunk->type_group_starts || !chunk->type_group_indices) {
      std::cerr << "  - " << tag << ": type group metadata incomplete: n_type_groups=" << chunk->n_type_groups << "\n";
      return false;
    }
    const int end = chunk->type_group_starts[static_cast<std::size_t>(chunk->n_type_groups)];
    if (end < 0) {
      std::cerr << "  - " << tag << ": type_group_starts end is negative: " << end << "\n";
      return false;
    }
    for (int i = 0; i < end; ++i) {
      const int idx = chunk->type_group_indices[static_cast<std::size_t>(i)];
      if (idx < 0 || idx >= chunk->n_points) {
        std::cerr << "  - " << tag << ": type_group_indices[" << i << "] out of bounds: " << idx
                  << " (n_points=" << chunk->n_points << ")\n";
        return false;
      }
    }
  }

  const int* point_indices_total = nullptr;
  int n_points_map = 0;
  const int map_st = sqzc3d_chunk_point_indices_total(chunk, &point_indices_total, &n_points_map);
  if (map_st == sqzc3d_STATUS_SUCCESS) {
    if (!point_indices_total || n_points_map != chunk->n_points) {
      std::cerr << "  - " << tag << ": point_indices_total invalid: ptr=" << (point_indices_total ? "non-null" : "null")
                << " n=" << n_points_map << " expected=" << chunk->n_points << "\n";
      return false;
    }
    for (int i = 0; i < n_points_map; ++i) {
      const int idx = point_indices_total[static_cast<std::size_t>(i)];
      if (idx < 0 || idx >= chunk->n_points_total) {
        std::cerr << "  - " << tag << ": point_indices_total[" << i << "] out of bounds: " << idx
                  << " (n_points_total=" << chunk->n_points_total << ")\n";
        return false;
      }
    }
  } else if (map_st != sqzc3d_STATUS_NOT_IMPLEMENTED) {
    std::cerr << "  - " << tag << ": sqzc3d_chunk_point_indices_total failed: status=" << map_st << "\n";
    return false;
  }

  const std::vector<std::string> missing_group = {"__missing__"};
  const bool has_type_meta = (chunk->n_type_groups > 0 && chunk->type_group_names && chunk->type_group_starts &&
                              chunk->type_group_indices);
  const auto type_sel = sqzc3d::PointIndicesFromTypeGroups(chunk, missing_group, /*missing_meta_all=*/true);
  if (!has_type_meta) {
    if (static_cast<int>(type_sel.size()) != chunk->n_points) {
      std::cerr << "  - " << tag << ": type-group meta missing should be a no-op: got=" << type_sel.size()
                << " expected=" << chunk->n_points << "\n";
      return false;
    }
  } else {
    if (!type_sel.empty()) {
      std::cerr << "  - " << tag << ": missing type-group name should yield empty set: got=" << type_sel.size() << "\n";
      return false;
    }
    std::vector<int> strict_sel;
    const int strict_st = sqzc3d::PointIndicesFromTypeGroupsStrict(chunk, missing_group, &strict_sel);
    if (strict_st == sqzc3d_STATUS_SUCCESS) {
      std::cerr << "  - " << tag << ": strict missing type-group name should fail\n";
      return false;
    }
  }

  return true;
}

bool check_point_views(const sqzc3d_chunk_t* chunk, const std::string& tag) {
  if (!chunk) return false;
  sqzc3d_points_view_t frame_view{};
  int st = sqzc3d_points_view_frames(chunk, 0, chunk->n_frames, &frame_view);
  if (st != sqzc3d_STATUS_SUCCESS) {
    std::cerr << "  - " << tag << ": frame view failed: " << st << "\n";
    return false;
  }
  if (frame_view.points_residual != chunk->points_residual ||
      frame_view.source_stride_points != chunk->n_points ||
      frame_view.n_frames != chunk->n_frames ||
      frame_view.n_points != chunk->n_points) {
    std::cerr << "  - " << tag << ": frame residual view metadata mismatch\n";
    return false;
  }

  if (chunk->n_points == 0) return true;
  std::vector<int> indices;
  indices.push_back(0);
  if (chunk->n_points > 2) {
    indices.push_back(chunk->n_points - 1);
  } else if (chunk->n_points > 1) {
    indices.push_back(1);
  }

  sqzc3d_points_view_t point_view{};
  st = sqzc3d_points_view_points(chunk, indices.data(), static_cast<int>(indices.size()), &point_view);
  if (st != sqzc3d_STATUS_SUCCESS) {
    std::cerr << "  - " << tag << ": point view failed: " << st << "\n";
    return false;
  }
  if (point_view.points_residual != chunk->points_residual ||
      point_view.source_stride_points != chunk->n_points ||
      point_view.n_points != static_cast<int>(indices.size())) {
    std::cerr << "  - " << tag << ": point residual view metadata mismatch\n";
    return false;
  }

  if (chunk->n_frames == 0) return true;
  sqzc3d_points_view_t frame_point_view{};
  st = sqzc3d_points_view_frame_points(chunk, 0, indices.data(), static_cast<int>(indices.size()), &frame_point_view);
  if (st != sqzc3d_STATUS_SUCCESS) {
    std::cerr << "  - " << tag << ": frame-point view failed: " << st << "\n";
    return false;
  }
  if (frame_point_view.n_frames != 1 ||
      frame_point_view.n_points != static_cast<int>(indices.size()) ||
      frame_point_view.source_stride_points != chunk->n_points) {
    std::cerr << "  - " << tag << ": frame-point residual view metadata mismatch\n";
    return false;
  }
  for (std::size_t i = 0; i < indices.size(); ++i) {
    const int src_p = indices[i];
    const double expected = chunk->points_residual[static_cast<std::size_t>(src_p)];
    const double observed =
        frame_point_view.point_indices ? frame_point_view.points_residual[static_cast<std::size_t>(frame_point_view.point_indices[i])]
                                       : frame_point_view.points_residual[static_cast<std::size_t>(frame_point_view.source_point_offset) + i];
    if (!((std::isnan(expected) && std::isnan(observed)) || expected == observed)) {
      std::cerr << "  - " << tag << ": frame-point residual value mismatch at selected point " << i << "\n";
      return false;
    }
  }
  return true;
}

bool compare_chunks(const sqzc3d_chunk_t* lhs, const sqzc3d_chunk_t* rhs) {
  if (!lhs || !rhs) return false;
  const auto equal_scalar = [](double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    return a == b;
  };
  const auto approx_equal = [](double a, double b) {
    const double scale = std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= 1e-12 * scale;
  };
  if (lhs->n_frames != rhs->n_frames) {
    std::cerr << "  mismatch: n_frames=" << lhs->n_frames << " vs " << rhs->n_frames << "\n";
    return false;
  }
  if (lhs->n_points != rhs->n_points) {
    std::cerr << "  mismatch: n_points=" << lhs->n_points << " vs " << rhs->n_points << "\n";
    return false;
  }
  if (lhs->n_points_total != rhs->n_points_total) {
    std::cerr << "  mismatch: n_points_total=" << lhs->n_points_total << " vs " << rhs->n_points_total << "\n";
    return false;
  }
  if (lhs->n_analogs != rhs->n_analogs) {
    std::cerr << "  mismatch: n_analogs=" << lhs->n_analogs << " vs " << rhs->n_analogs << "\n";
    return false;
  }
  if (lhs->n_analog_by_frame != rhs->n_analog_by_frame) {
    std::cerr << "  mismatch: n_analog_by_frame=" << lhs->n_analog_by_frame << " vs " << rhs->n_analog_by_frame
              << "\n";
    return false;
  }
  if (lhs->n_scalar != rhs->n_scalar) {
    std::cerr << "  mismatch: n_scalar=" << lhs->n_scalar << " vs " << rhs->n_scalar << "\n";
    return false;
  }
  if (lhs->valid_nscalar != rhs->valid_nscalar) {
    std::cerr << "  mismatch: valid_nscalar=" << lhs->valid_nscalar << " vs " << rhs->valid_nscalar << "\n";
    return false;
  }
  if (lhs->residual_nscalar != rhs->residual_nscalar) {
    std::cerr << "  mismatch: residual_nscalar=" << lhs->residual_nscalar << " vs " << rhs->residual_nscalar
              << "\n";
    return false;
  }
  if (lhs->n_analog_scalar != rhs->n_analog_scalar) {
    std::cerr << "  mismatch: n_analog_scalar=" << lhs->n_analog_scalar << " vs " << rhs->n_analog_scalar << "\n";
    return false;
  }
  if (lhs->points_layout != rhs->points_layout) {
    std::cerr << "  mismatch: points_layout=" << lhs->points_layout << " vs " << rhs->points_layout << "\n";
    return false;
  }
  if (lhs->read_policy != rhs->read_policy) {
    std::cerr << "  mismatch: read_policy=" << lhs->read_policy << " vs " << rhs->read_policy << "\n";
    return false;
  }
  if (lhs->points_pack != rhs->points_pack) {
    std::cerr << "  mismatch: points_pack=" << lhs->points_pack << " vs " << rhs->points_pack << "\n";
    return false;
  }
  if (lhs->valid_policy != rhs->valid_policy) {
    std::cerr << "  mismatch: valid_policy=" << lhs->valid_policy << " vs " << rhs->valid_policy << "\n";
    return false;
  }
  if (!approx_equal(lhs->residual_gate_mm, rhs->residual_gate_mm)) {
    std::cerr << std::setprecision(17) << "  mismatch: residual_gate_mm=" << lhs->residual_gate_mm << " vs "
              << rhs->residual_gate_mm << "\n";
    return false;
  }
  if (!approx_equal(lhs->point_scale, rhs->point_scale)) {
    std::cerr << std::setprecision(17) << "  mismatch: point_scale=" << lhs->point_scale << " vs " << rhs->point_scale
              << "\n";
    return false;
  }
  if (!approx_equal(lhs->header_scale, rhs->header_scale)) {
    std::cerr << std::setprecision(17) << "  mismatch: header_scale=" << lhs->header_scale << " vs "
              << rhs->header_scale << "\n";
    return false;
  }
  if (!approx_equal(lhs->point_units_per_meter, rhs->point_units_per_meter) ||
      !approx_equal(lhs->target_units_per_meter, rhs->target_units_per_meter) ||
      !approx_equal(lhs->residual_units_per_meter, rhs->residual_units_per_meter) ||
      lhs->point_units_source != rhs->point_units_source) {
    std::cerr << std::setprecision(17) << "  mismatch: unit metadata lhs=("
              << lhs->point_units_per_meter << ", " << lhs->target_units_per_meter << ", "
              << lhs->residual_units_per_meter << ", " << lhs->point_units_source << ") rhs=("
              << rhs->point_units_per_meter << ", " << rhs->target_units_per_meter << ", "
              << rhs->residual_units_per_meter << ", " << rhs->point_units_source << ")\n";
    return false;
  }

  if ((lhs->reason == nullptr) != (rhs->reason == nullptr)) return false;
  if (lhs->reason && rhs->reason && std::strcmp(lhs->reason, rhs->reason) != 0) {
    std::cerr << "  mismatch: reason lhs=" << lhs->reason << " rhs=" << rhs->reason << "\n";
    return false;
  }
  for (int i = 0; i < lhs->n_scalar; ++i) {
    if (!equal_scalar(lhs->points_xyz[static_cast<std::size_t>(i)], rhs->points_xyz[static_cast<std::size_t>(i)])) {
      std::cerr << "  mismatch: points_xyz[" << i << "] lhs=" << lhs->points_xyz[i] << " rhs=" << rhs->points_xyz[i] << "\n";
      return false;
    }
  }
  for (int i = 0; i < lhs->valid_nscalar; ++i) {
    if (lhs->points_valid[i] != rhs->points_valid[i]) {
      std::cerr << "  mismatch: points_valid[" << i << "] lhs=" << static_cast<int>(lhs->points_valid[i])
                << " rhs=" << static_cast<int>(rhs->points_valid[i]) << "\n";
      return false;
    }
  }
  for (int i = 0; i < lhs->residual_nscalar; ++i) {
    if (!equal_scalar(lhs->points_residual[static_cast<std::size_t>(i)],
                      rhs->points_residual[static_cast<std::size_t>(i)])) {
      std::cerr << "  mismatch: points_residual[" << i << "] lhs=" << lhs->points_residual[i]
                << " rhs=" << rhs->points_residual[i] << "\n";
      return false;
    }
  }
  for (int i = 0; i < lhs->n_analog_scalar; ++i) {
    if (!equal_scalar(lhs->analog[static_cast<std::size_t>(i)], rhs->analog[static_cast<std::size_t>(i)]) ||
        lhs->analog_valid[i] != rhs->analog_valid[i]) {
      std::cerr << "  mismatch: analog[" << i << "] lhs=" << lhs->analog[i] << " rhs=" << rhs->analog[i] << "\n";
      return false;
    }
  }

  if ((lhs->point_labels == nullptr) != (rhs->point_labels == nullptr)) return false;
  if (lhs->point_labels && rhs->point_labels) {
    for (int i = 0; i < lhs->n_points; ++i) {
      const char* left = lhs->point_labels[i] ? lhs->point_labels[i] : "";
      const char* right = rhs->point_labels[i] ? rhs->point_labels[i] : "";
      if (std::strcmp(left, right) != 0) return false;
    }
  }

  if ((lhs->analog_labels == nullptr) != (rhs->analog_labels == nullptr)) return false;
  if (lhs->analog_labels && rhs->analog_labels) {
    for (int i = 0; i < lhs->n_analogs; ++i) {
      const char* left = lhs->analog_labels[i] ? lhs->analog_labels[i] : "";
      const char* right = rhs->analog_labels[i] ? rhs->analog_labels[i] : "";
      if (std::strcmp(left, right) != 0) return false;
    }
  }

  return true;
}

std::vector<unsigned char> read_file_bytes(const std::string& file_path) {
  std::ifstream in(file_path, std::ios::binary);
  if (!in) return {};
  in.seekg(0, std::ios::end);
  const auto end = in.tellg();
  if (end <= 0) return {};
  std::vector<unsigned char> bytes(static_cast<std::size_t>(end));
  in.seekg(0, std::ios::beg);
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!in.good()) return {};
  return bytes;
}

struct EnvSnapshot {
  std::string name;
  bool had_value = false;
  std::string value;
};

bool read_env_value(const char* name, EnvSnapshot* out) {
  if (!name || !out) return false;
  out->name = name;
#ifdef _WIN32
  char* value = nullptr;
  size_t len = 0;
  if (_dupenv_s(&value, &len, name) != 0) return false;
  if (value) {
    out->had_value = true;
    out->value = value;
    std::free(value);
  }
#else
  const char* value = std::getenv(name);
  if (value) {
    out->had_value = true;
    out->value = value;
  }
#endif
  return true;
}

bool set_env_value(const std::string& name, const std::string& value) {
#ifdef _WIN32
  return _putenv_s(name.c_str(), value.c_str()) == 0;
#else
  return ::setenv(name.c_str(), value.c_str(), 1) == 0;
#endif
}

bool unset_env_value(const std::string& name) {
#ifdef _WIN32
  return _putenv_s(name.c_str(), "") == 0;
#else
  return ::unsetenv(name.c_str()) == 0;
#endif
}

struct TempEnvOverride {
  std::vector<EnvSnapshot> snapshots;
  bool ok = true;

  explicit TempEnvOverride(const std::string& missing_dir) {
    const char* names[] = {"TMP", "TEMP", "TMPDIR"};
    snapshots.reserve(3);
    for (const char* name : names) {
      EnvSnapshot snapshot;
      ok = read_env_value(name, &snapshot) && ok;
      snapshots.push_back(snapshot);
      ok = set_env_value(name, missing_dir) && ok;
    }
  }

  ~TempEnvOverride() {
    for (const auto& snapshot : snapshots) {
      if (snapshot.had_value) {
        (void)set_env_value(snapshot.name, snapshot.value);
      } else {
        (void)unset_env_value(snapshot.name);
      }
    }
  }
};

bool compare_time_axis(const sqzc3d_chunk_t* lhs, const sqzc3d_chunk_t* rhs) {
  sqzc3d_time_axis_t left{};
  sqzc3d_time_axis_t right{};
  sqzc3d_default_time_axis(&left);
  sqzc3d_default_time_axis(&right);
  const int st_left = sqzc3d_chunk_time_axis(lhs, &left);
  const int st_right = sqzc3d_chunk_time_axis(rhs, &right);
  if (st_left != st_right) {
    std::cerr << "  mismatch: time axis status lhs=" << st_left << " rhs=" << st_right << "\n";
    return false;
  }
  if (st_left != sqzc3d_STATUS_SUCCESS) return true;
  if (left.source_first_frame != right.source_first_frame ||
      left.source_last_frame != right.source_last_frame ||
      left.frame_start != right.frame_start ||
      left.analog_frame_start != right.analog_frame_start ||
      left.point_rate_hz != right.point_rate_hz ||
      left.analog_rate_hz != right.analog_rate_hz) {
    std::cerr << "  mismatch: time axis metadata\n";
    return false;
  }
  return true;
}

bool compare_meta_tree(const sqzc3d_chunk_t* lhs, const sqzc3d_chunk_t* rhs) {
  const char* left = nullptr;
  const char* right = nullptr;
  int left_n = 0;
  int right_n = 0;
  const int st_left = sqzc3d_chunk_meta_tree_json(lhs, &left, &left_n);
  const int st_right = sqzc3d_chunk_meta_tree_json(rhs, &right, &right_n);
  if (st_left != st_right) {
    std::cerr << "  mismatch: meta_tree status lhs=" << st_left << " rhs=" << st_right << "\n";
    return false;
  }
  if (st_left != sqzc3d_STATUS_SUCCESS) return true;
  if (left_n != right_n || !left || !right || std::memcmp(left, right, static_cast<std::size_t>(left_n)) != 0) {
    std::cerr << "  mismatch: meta_tree payload\n";
    return false;
  }
  return true;
}

#if sqzc3d_WITH_EZC3D
std::string normalize_unit_token(std::string raw) {
  auto begin = raw.begin();
  auto end = raw.end();
  while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
  while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
  std::string out;
  out.reserve(static_cast<std::size_t>(end - begin));
  for (auto it = begin; it != end; ++it) {
    if (!std::isspace(static_cast<unsigned char>(*it))) {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*it))));
    }
  }
  return out;
}

double units_per_meter_from_unit_token(const std::string& raw) {
  const auto u = normalize_unit_token(raw);
  if (u == "mm" || u == "millimeter" || u == "millimeters") return 1000.0;
  if (u == "cm" || u == "centimeter" || u == "centimeters") return 100.0;
  if (u == "m" || u == "meter" || u == "meters") return 1.0;
  if (u == "km" || u == "kilometer" || u == "kilometers") return 1e-3;
  return 0.0;
}

bool scalar_equal_for_oracle(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  return std::fabs(a - b) <= 1e-6 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

bool compare_labels_to_oracle(
    const char* const* actual,
    int n_actual,
    const std::vector<std::string>& expected,
    const char* tag) {
  if (n_actual != static_cast<int>(expected.size())) {
    std::cerr << "  - parser_equivalence: " << tag << " count mismatch actual=" << n_actual
              << " expected=" << expected.size() << "\n";
    return false;
  }
  for (int i = 0; i < n_actual; ++i) {
    const char* label = (actual && actual[i]) ? actual[i] : "";
    if (std::strcmp(label, expected[static_cast<std::size_t>(i)].c_str()) != 0) {
      std::cerr << "  - parser_equivalence: " << tag << "[" << i << "] mismatch actual=" << label
                << " expected=" << expected[static_cast<std::size_t>(i)] << "\n";
      return false;
    }
  }
  return true;
}

struct OracleTypeGroups {
  std::vector<std::string> names;
  std::vector<int> starts;
  std::vector<int> indices;
};

OracleTypeGroups load_oracle_type_groups(const ezc3d::c3d& oracle) {
  OracleTypeGroups out;
  out.starts.push_back(0);
  const auto& params = oracle.parameters();
  if (!params.isGroup("POINT")) return out;
  const auto& group = params.group("POINT");
  if (!group.isParameter("TYPE_GROUPS")) return out;
  const auto& type_groups_param = group.parameter("TYPE_GROUPS");
  if (type_groups_param.type() != ezc3d::DATA_TYPE::CHAR) return out;
  const auto type_groups = type_groups_param.valuesAsString();
  if (type_groups.empty()) return out;

  std::unordered_map<std::string, int> label_to_idx;
  const auto point_labels = oracle.pointNames();
  label_to_idx.reserve(point_labels.size() * 2u + 1u);
  for (int i = 0; i < static_cast<int>(point_labels.size()); ++i) {
    label_to_idx[point_labels[static_cast<std::size_t>(i)]] = i;
  }

  for (const auto& raw_name : type_groups) {
    if (raw_name.empty()) continue;
    out.names.push_back(raw_name);
    if (group.isParameter(raw_name.c_str())) {
      const auto& group_param = group.parameter(raw_name.c_str());
      if (group_param.type() == ezc3d::DATA_TYPE::CHAR) {
        const auto labels = group_param.valuesAsString();
        for (const auto& label : labels) {
          const auto it = label_to_idx.find(label);
          if (it != label_to_idx.end()) out.indices.push_back(it->second);
        }
      }
    }
    out.starts.push_back(static_cast<int>(out.indices.size()));
  }
  return out;
}

bool compare_type_groups_to_oracle(const sqzc3d_chunk_t* actual, const ezc3d::c3d& oracle) {
  const auto expected = load_oracle_type_groups(oracle);
  if (actual->n_type_groups != static_cast<int>(expected.names.size())) {
    std::cerr << "  - parser_equivalence: type group count mismatch actual=" << actual->n_type_groups
              << " expected=" << expected.names.size() << "\n";
    return false;
  }
  if (expected.names.empty()) return true;
  if (!actual->type_group_names || !actual->type_group_starts || !actual->type_group_indices) {
    std::cerr << "  - parser_equivalence: type group metadata missing\n";
    return false;
  }
  for (int i = 0; i < actual->n_type_groups; ++i) {
    const auto idx = static_cast<std::size_t>(i);
    const char* name = actual->type_group_names[i] ? actual->type_group_names[i] : "";
    if (std::strcmp(name, expected.names[idx].c_str()) != 0) {
      std::cerr << "  - parser_equivalence: type_group_names[" << i << "] mismatch actual=" << name
                << " expected=" << expected.names[idx] << "\n";
      return false;
    }
    if (actual->type_group_starts[idx] != expected.starts[idx]) {
      std::cerr << "  - parser_equivalence: type_group_starts[" << i << "] mismatch actual="
                << actual->type_group_starts[idx] << " expected=" << expected.starts[idx] << "\n";
      return false;
    }
  }
  const auto end_idx = static_cast<std::size_t>(actual->n_type_groups);
  if (actual->type_group_starts[end_idx] != expected.starts[end_idx]) {
    std::cerr << "  - parser_equivalence: type_group_starts end mismatch actual="
              << actual->type_group_starts[end_idx] << " expected=" << expected.starts[end_idx] << "\n";
    return false;
  }
  for (std::size_t i = 0; i < expected.indices.size(); ++i) {
    if (actual->type_group_indices[i] != expected.indices[i]) {
      std::cerr << "  - parser_equivalence: type_group_indices[" << i << "] mismatch actual="
                << actual->type_group_indices[i] << " expected=" << expected.indices[i] << "\n";
      return false;
    }
  }
  return true;
}

bool compare_analogs_to_oracle(const sqzc3d_chunk_t* actual, const ezc3d::c3d& oracle) {
  if (actual->n_analogs <= 0) return true;
  const int total_samples = actual->n_frames * actual->n_analog_by_frame;
  if (!actual->analog || actual->n_analog_scalar != actual->n_analogs * total_samples) {
    std::cerr << "  - parser_equivalence: analog payload shape mismatch\n";
    return false;
  }
  const auto& frames = oracle.data().frames();
  if (frames.size() < static_cast<std::size_t>(actual->n_frames)) {
    std::cerr << "  - parser_equivalence: ezc3d frame count smaller than chunk\n";
    return false;
  }
  for (int f = 0; f < actual->n_frames; ++f) {
    const auto& analogs = frames[static_cast<std::size_t>(f)].analogs();
    if (analogs.nbSubframes() < static_cast<std::size_t>(actual->n_analog_by_frame)) {
      std::cerr << "  - parser_equivalence: analog subframe count mismatch at frame " << f << "\n";
      return false;
    }
    for (int s = 0; s < actual->n_analog_by_frame; ++s) {
      const auto& subframe = analogs.subframe(static_cast<std::size_t>(s));
      if (subframe.nbChannels() < static_cast<std::size_t>(actual->n_analogs)) {
        std::cerr << "  - parser_equivalence: analog channel count mismatch at frame " << f
                  << " subframe " << s << "\n";
        return false;
      }
      const int sample = f * actual->n_analog_by_frame + s;
      for (int c = 0; c < actual->n_analogs; ++c) {
        const double expected = subframe.channel(static_cast<std::size_t>(c)).data();
        const double got = actual->analog[static_cast<std::size_t>(c) *
                                          static_cast<std::size_t>(total_samples) +
                                          static_cast<std::size_t>(sample)];
        if (!scalar_equal_for_oracle(got, expected)) {
          std::cerr << "  - parser_equivalence: analog[" << c << "," << sample
                    << "] mismatch actual=" << got << " expected=" << expected << "\n";
          return false;
        }
      }
    }
  }
  return true;
}

bool check_parser_ezc3d_equivalence(const std::string& file_path, const sqzc3d_chunk_t* base) {
  if (!base) return false;
  try {
    ezc3d::c3d oracle(file_path);
    if (base->n_points_total != static_cast<int>(oracle.header().nb3dPoints()) ||
        base->n_points != static_cast<int>(oracle.header().nb3dPoints()) ||
        base->n_analogs != static_cast<int>(oracle.header().nbAnalogs()) ||
        base->n_analog_by_frame != static_cast<int>(oracle.header().nbAnalogByFrame())) {
      std::cerr << "  - parser_equivalence: shape mismatch"
                << " points=" << base->n_points_total << "/" << oracle.header().nb3dPoints()
                << " analogs=" << base->n_analogs << "/" << oracle.header().nbAnalogs()
                << " analog_by_frame=" << base->n_analog_by_frame << "/" << oracle.header().nbAnalogByFrame()
                << "\n";
      return false;
    }

    sqzc3d_time_axis_t axis{};
    sqzc3d_default_time_axis(&axis);
    const int time_st = sqzc3d_chunk_time_axis(base, &axis);
    if (time_st != sqzc3d_STATUS_SUCCESS) {
      std::cerr << "  - parser_equivalence: time axis unavailable status=" << time_st << "\n";
      return false;
    }
    if (axis.source_first_frame != static_cast<int>(oracle.header().firstFrame()) ||
        axis.source_last_frame != static_cast<int>(oracle.header().lastFrame()) ||
        !scalar_equal_for_oracle(axis.point_rate_hz, static_cast<double>(oracle.header().frameRate()))) {
      std::cerr << "  - parser_equivalence: time axis mismatch\n";
      return false;
    }

    if (!compare_labels_to_oracle(base->point_labels, base->n_points, oracle.pointNames(), "point_labels")) {
      return false;
    }
    if (base->n_analogs > 0) {
      auto analog_names = oracle.channelNames();
      if (analog_names.size() > static_cast<std::size_t>(base->n_analogs)) {
        analog_names.resize(static_cast<std::size_t>(base->n_analogs));
      }
      if (!compare_labels_to_oracle(base->analog_labels, base->n_analogs, analog_names, "analog_labels")) {
        return false;
      }
      if (!compare_analogs_to_oracle(base, oracle)) {
        return false;
      }
    }
    if (!compare_type_groups_to_oracle(base, oracle)) {
      return false;
    }

    if (oracle.parameters().isGroup("POINT") && oracle.parameters().group("POINT").isParameter("UNITS")) {
      const auto units = oracle.parameters().group("POINT").parameter("UNITS").valuesAsString();
      if (!units.empty()) {
        const double expected_units_per_meter = units_per_meter_from_unit_token(units[0]);
        if (expected_units_per_meter > 0.0 &&
            !scalar_equal_for_oracle(base->point_units_per_meter, expected_units_per_meter)) {
          std::cerr << "  - parser_equivalence: point unit mismatch actual=" << base->point_units_per_meter
                    << " expected=" << expected_units_per_meter << "\n";
          return false;
        }
      }
    }

    std::cout << "  - parser_equivalence: PASS\n";
    return true;
  } catch (const std::exception& e) {
    std::cerr << "  - parser_equivalence: ezc3d oracle failed: " << e.what() << "\n";
    return false;
  }
}
#endif

bool check_open_memory_no_temp(
    const std::string& file_path,
    const sqzc3d_build_opt_t& base_opt,
    const sqzc3d_chunk_t* base) {
  const auto bytes = read_file_bytes(file_path);
  if (bytes.empty()) {
    std::cerr << "  - open_memory_no_temp: failed to read source bytes\n";
    return false;
  }

  const auto missing_tmp =
      (std::filesystem::path(file_path).parent_path() / "__sqzc3d_missing_tmp_dir__").string();
  TempEnvOverride env_guard(missing_tmp);
  if (!env_guard.ok) {
    std::cerr << "  - open_memory_no_temp: failed to override temp environment\n";
    return false;
  }

  const int modes[] = {sqzc3d_FILE, sqzc3d_MEMORY};
  const char* tags[] = {"default_file_mode", "explicit_memory_mode"};
  for (int i = 0; i < 2; ++i) {
    sqzc3d_open_opt_t open_opt{};
    sqzc3d_default_open_opt(&open_opt);
    open_opt.open_mode = modes[i];
    sqzc3d_dec_t* mem_dec = nullptr;
    const int open_status = sqzc3d_open_memory(
        &mem_dec,
        bytes.data(),
        static_cast<int>(bytes.size()),
        &open_opt);
    if (open_status != sqzc3d_STATUS_SUCCESS || !mem_dec) {
      std::cerr << "  - open_memory_no_temp/" << tags[i] << ": open failed: " << open_status
                << " " << sqzc3d_last_error(mem_dec) << "\n";
      if (mem_dec) sqzc3d_close_dec(mem_dec);
      return false;
    }

    sqzc3d_chunk_t* mem_chunk = nullptr;
    const int build_status = sqzc3d_build_chunks(mem_dec, &base_opt, &mem_chunk);
    if (build_status != sqzc3d_STATUS_SUCCESS || !mem_chunk) {
      std::cerr << "  - open_memory_no_temp/" << tags[i] << ": build failed: " << build_status
                << " " << sqzc3d_last_error(mem_dec) << "\n";
      sqzc3d_close_dec(mem_dec);
      return false;
    }

    const bool same =
        compare_chunks(base, mem_chunk) && compare_time_axis(base, mem_chunk) && compare_meta_tree(base, mem_chunk);
    sqzc3d_free_chunk(mem_chunk);
    sqzc3d_close_dec(mem_dec);
    if (!same) {
      std::cerr << "  - open_memory_no_temp/" << tags[i] << ": memory chunk mismatch\n";
      return false;
    }
  }

  std::cout << "  - open_memory_no_temp: PASS\n";
  return true;
}

std::filesystem::path make_case_path(const std::string& file_path,
                                     const std::string& case_name,
                                     int file_idx,
                                     int case_idx) {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const auto stem = std::filesystem::path(file_path).stem().string();
  const auto base = std::filesystem::temp_directory_path() / "sqzc3d_matrix";
  const auto file = base / (stem + "_" + std::to_string(file_idx) + "_" + std::to_string(case_idx) +
                           "_" + case_name + "_" + std::to_string(now) + ".sqzc3d");
  std::filesystem::create_directories(base);
  return file;
}

bool check_malformed_schema4_export(sqzc3d_chunk_t& chunk, const std::string& file_path, int file_idx) {
  const double saved_target_units_per_meter = chunk.target_units_per_meter;
  chunk.target_units_per_meter = 0.0;
  const auto bundle_path = make_case_path(file_path, "malformed_schema4_export", file_idx, -1);
  const int export_status = sqzc3d_export_bundle(bundle_path.string().c_str(), &chunk);
  chunk.target_units_per_meter = saved_target_units_per_meter;
  std::error_code ec;
  std::filesystem::remove(bundle_path, ec);
  if (export_status == sqzc3d_STATUS_SUCCESS) {
    std::cerr << "  mismatch: malformed schema4 chunk export should fail\n";
    return false;
  }
  return true;
}

bool run_one_case(const sqzc3d_dec_t* dec, const CaseConfig& cfg, const std::string& file_path, int file_idx, int case_idx) {
  sqzc3d_chunk_t* built = nullptr;
  const char* debug = std::getenv("SQZC3D_MATRIX_DEBUG");
  if (debug && debug[0] == '1') {
    std::cerr << "  - " << cfg.name << ": point_sel_mode=" << cfg.opt.point_sel_mode
              << " point_sel_count=" << cfg.opt.point_sel_count << " n_points requested=" << cfg.opt.point_sel_count
              << "\n";
    if (cfg.point_labels.size() > 0) {
      std::cerr << "    point_labels:";
      for (const char* label : cfg.point_labels) {
        std::cerr << " [" << (label ? label : "") << "]";
      }
      std::cerr << "\n";
    }
  }
  const int build_status = sqzc3d_build_chunks(dec, &cfg.opt, &built);
  if (build_status != sqzc3d_STATUS_SUCCESS || !built) {
    std::cerr << "  - " << cfg.name << ": build failed (" << build_status << ") "
              << (dec && sqzc3d_last_error(dec) ? sqzc3d_last_error(dec) : "n/a") << "\n";
    return false;
  }
  if (debug && debug[0] == '1') {
    std::cerr << "    build: n_points=" << built->n_points << " n_points_total=" << built->n_points_total
              << " n_scalar=" << built->n_scalar << " n_frames=" << built->n_frames
              << " valid_nscalar=" << built->valid_nscalar << "\n";
  }
  if ((cfg.opt.read_policy == sqzc3d_READ_POLICY_DENSE || cfg.opt.read_policy == sqzc3d_READ_POLICY_SPARSE) &&
      built->read_policy != cfg.opt.read_policy) {
    std::cerr << "  - " << cfg.name << ": read_policy=" << built->read_policy
              << " expected=" << cfg.opt.read_policy << "\n";
    sqzc3d_free_chunk(built);
    return false;
  }
  if (!check_chunk_index_contract(built, cfg.name + ":built") ||
      !check_point_views(built, cfg.name + ":built")) {
    sqzc3d_free_chunk(built);
    return false;
  }

  const auto bundle_path = make_case_path(file_path, cfg.name, file_idx, case_idx);
  const int export_status = sqzc3d_export_bundle(bundle_path.string().c_str(), built);
  if (export_status != sqzc3d_STATUS_SUCCESS) {
    std::cerr << "  - " << cfg.name << ": export failed (" << export_status << ")\n";
    sqzc3d_free_chunk(built);
    return false;
  }

  sqzc3d_chunk_t* loaded = nullptr;
  const int load_status = sqzc3d_load_bundle(bundle_path.string().c_str(), &loaded);
  sqzc3d_bundle_load_opt_t strict_opt{};
  sqzc3d_default_bundle_load_opt(&strict_opt);
  strict_opt.strict = 1;
  sqzc3d_chunk_t* strict_loaded = nullptr;
  const int strict_status =
      sqzc3d_load_bundle_with_options(bundle_path.string().c_str(), &strict_opt, &strict_loaded);

  const bool load_ok = (load_status == sqzc3d_STATUS_SUCCESS && loaded);
  const bool strict_ok = (strict_status == sqzc3d_STATUS_SUCCESS && strict_loaded);
  if (!load_ok || !strict_ok) {
    std::cerr << "  - " << cfg.name << ": load failed (normal=" << load_status << ", strict=" << strict_status << ")\n";
    const char* keep_debug = std::getenv("SQZC3D_MATRIX_KEEP_BUNDLE");
    if (keep_debug && keep_debug[0] != '\0') {
      std::cerr << "    bundle kept: " << bundle_path << "\n";
    } else {
      std::error_code ec;
      std::filesystem::remove(bundle_path, ec);
    }
    if (loaded) sqzc3d_free_chunk(loaded);
    if (strict_loaded) sqzc3d_free_chunk(strict_loaded);
    return false;
  }
  if (!check_chunk_index_contract(loaded, cfg.name + ":loaded") ||
      !check_point_views(loaded, cfg.name + ":loaded") ||
      !check_chunk_index_contract(strict_loaded, cfg.name + ":strict_loaded") ||
      !check_point_views(strict_loaded, cfg.name + ":strict_loaded")) {
    sqzc3d_free_chunk(built);
    sqzc3d_free_chunk(loaded);
    sqzc3d_free_chunk(strict_loaded);
    std::error_code ec;
    std::filesystem::remove(bundle_path, ec);
    std::cout << "  - " << cfg.name << ": FAIL\n";
    return false;
  }

  const bool same = compare_chunks(built, loaded) && compare_chunks(loaded, strict_loaded);
  sqzc3d_free_chunk(built);
  sqzc3d_free_chunk(loaded);
  sqzc3d_free_chunk(strict_loaded);
  std::error_code ec;
  std::filesystem::remove(bundle_path, ec);
  std::cout << "  - " << cfg.name << ": " << (same ? "PASS" : "FAIL") << "\n";
  return same;
}

void collect_point_labels(const sqzc3d_chunk_t* chunk, std::vector<const char*>& labels_out) {
  labels_out.clear();
  if (!chunk || !chunk->point_labels) return;
  labels_out.reserve(static_cast<size_t>(chunk->n_points));
  for (int i = 0; i < chunk->n_points; ++i) labels_out.push_back(chunk->point_labels[i] ? chunk->point_labels[i] : "");
}

void collect_analog_labels(const sqzc3d_chunk_t* chunk, std::vector<const char*>& labels_out) {
  labels_out.clear();
  if (!chunk || !chunk->analog_labels) return;
  labels_out.reserve(static_cast<size_t>(chunk->n_analogs));
  for (int i = 0; i < chunk->n_analogs; ++i) labels_out.push_back(chunk->analog_labels[i] ? chunk->analog_labels[i] : "");
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "usage: verify_correctness_matrix_sqzc3d <path-to-c3d> [<path-to-c3d> ...]\n";
    return 1;
  }

  bool all_ok = true;
  for (int file_idx = 1; file_idx < argc; ++file_idx) {
    const std::string file_path = argv[file_idx];
    std::cout << "[case] file: " << file_path << "\n";

    sqzc3d_open_opt_t open_opt{};
    sqzc3d_default_open_opt(&open_opt);
    sqzc3d_dec_t* dec = nullptr;
    const int open_status = sqzc3d_open_file(&dec, file_path.c_str(), &open_opt);
    if (open_status != sqzc3d_STATUS_SUCCESS || !dec) {
      std::cerr << "  open failed: " << open_status << " "
                << (sqzc3d_last_error(dec) ? sqzc3d_last_error(dec) : "n/a") << "\n";
      all_ok = false;
      continue;
    }

    sqzc3d_build_opt_t base_opt{};
    sqzc3d_default_build_opt(&base_opt);
    base_opt.residual_gate_mm = 0.0;
    base_opt.read_policy = sqzc3d_READ_POLICY_AUTO;

    sqzc3d_chunk_t* base = nullptr;
    const int base_status = sqzc3d_build_chunks(dec, &base_opt, &base);
    if (base_status != sqzc3d_STATUS_SUCCESS || !base) {
      std::cerr << "  baseline build failed: " << base_status << "\n";
      sqzc3d_close_dec(dec);
      all_ok = false;
      continue;
    }

    std::vector<const char*> base_point_labels;
    std::vector<const char*> base_analog_labels;
    collect_point_labels(base, base_point_labels);
    collect_analog_labels(base, base_analog_labels);

    std::vector<CaseConfig> cases;
    cases.reserve(10);
    cases.push_back({"all_points_auto", base_opt, {}, {}, {}, {}});

    CaseConfig explicit_dense_case;
    explicit_dense_case.name = "read_policy_dense_all";
    explicit_dense_case.opt = base_opt;
    explicit_dense_case.opt.read_policy = sqzc3d_READ_POLICY_DENSE;
    cases.push_back(std::move(explicit_dense_case));

    CaseConfig explicit_sparse_case;
    explicit_sparse_case.name = "read_policy_sparse_all";
    explicit_sparse_case.opt = base_opt;
    explicit_sparse_case.opt.read_policy = sqzc3d_READ_POLICY_SPARSE;
    cases.push_back(std::move(explicit_sparse_case));

    CaseConfig target_unit_case;
    target_unit_case.name = "target_unit_m";
    target_unit_case.opt = base_opt;
    target_unit_case.opt.target_unit = "m";
    cases.push_back(std::move(target_unit_case));

    CaseConfig residual_gate_case;
    residual_gate_case.name = "residual_gate_policy";
    residual_gate_case.opt = base_opt;
    residual_gate_case.opt.valid_policy = sqzc3d_VALID_POLICY_FINITE_XYZ_AND_RESIDUAL_GATE;
    residual_gate_case.opt.residual_gate_mm = 5.0;
    cases.push_back(std::move(residual_gate_case));

    const int half_points = std::max(1, base->n_points / 2);
    if (base->n_points > 0) {
      CaseConfig idx_case;
      idx_case.name = "point_indices_stride2";
      idx_case.opt = base_opt;
      idx_case.opt.point_sel_mode = sqzc3d_POINT_SEL_INDICES;
      idx_case.opt.point_sel_count = half_points;
      idx_case.point_indices.resize(static_cast<size_t>(half_points));
      for (int i = 0; i < half_points; ++i) {
        idx_case.point_indices[static_cast<size_t>(i)] = (i * 2) % std::max(1, base->n_points);
      }
      idx_case.opt.point_sel = idx_case.point_indices.data();
      cases.push_back(std::move(idx_case));

      CaseConfig dense_idx_case;
      dense_idx_case.name = "read_policy_dense_indices_stride2";
      dense_idx_case.opt = base_opt;
      dense_idx_case.opt.read_policy = sqzc3d_READ_POLICY_DENSE;
      dense_idx_case.opt.point_sel_mode = sqzc3d_POINT_SEL_INDICES;
      dense_idx_case.opt.point_sel_count = half_points;
      dense_idx_case.point_indices.resize(static_cast<size_t>(half_points));
      for (int i = 0; i < half_points; ++i) {
        dense_idx_case.point_indices[static_cast<size_t>(i)] = (i * 2) % std::max(1, base->n_points);
      }
      dense_idx_case.opt.point_sel = dense_idx_case.point_indices.data();
      cases.push_back(std::move(dense_idx_case));

      CaseConfig sparse_idx_case;
      sparse_idx_case.name = "read_policy_sparse_indices_stride2";
      sparse_idx_case.opt = base_opt;
      sparse_idx_case.opt.read_policy = sqzc3d_READ_POLICY_SPARSE;
      sparse_idx_case.opt.point_sel_mode = sqzc3d_POINT_SEL_INDICES;
      sparse_idx_case.opt.point_sel_count = half_points;
      sparse_idx_case.point_indices.resize(static_cast<size_t>(half_points));
      for (int i = 0; i < half_points; ++i) {
        sparse_idx_case.point_indices[static_cast<size_t>(i)] = (i * 2) % std::max(1, base->n_points);
      }
      sparse_idx_case.opt.point_sel = sparse_idx_case.point_indices.data();
      cases.push_back(std::move(sparse_idx_case));
    }

    if (!base_point_labels.empty()) {
      CaseConfig label_case;
      label_case.name = "point_labels_head";
      label_case.opt = base_opt;
      label_case.opt.point_sel_mode = sqzc3d_POINT_SEL_LABELS;
      label_case.opt.point_sel_count = std::min(2, base->n_points);
      label_case.point_labels.reserve(static_cast<size_t>(label_case.opt.point_sel_count));
      for (int i = 0; i < label_case.opt.point_sel_count; ++i) {
        label_case.point_labels.push_back(base_point_labels[static_cast<size_t>(i)]);
      }
      label_case.opt.point_labels = label_case.point_labels.data();

      CaseConfig label_case_legacy = label_case;
      label_case_legacy.name = "point_labels_head_legacy_count";
      label_case_legacy.opt.point_sel_count = label_case.opt.point_sel_count;
      label_case_legacy.opt.point_labels_count = 0;
      cases.push_back(std::move(label_case_legacy));
      cases.push_back(std::move(label_case));
    }

    const int half_frames = (base->n_frames > 0) ? std::max(1, base->n_frames / 2) : 0;
    if (base->n_frames > 0) {
      CaseConfig half_frame_case = {};
      half_frame_case.name = "half_frames";
      half_frame_case.opt = base_opt;
      half_frame_case.opt.frame_range = {0, half_frames};
      half_frame_case.opt.analog_range = {0, half_frames};
      cases.push_back(std::move(half_frame_case));
    }

    if (base->n_analogs > 0) {
      const int half_analogs = std::max(1, base->n_analogs / 2);
      CaseConfig analog_idx_case;
      analog_idx_case.name = "analog_indices_head";
      analog_idx_case.opt = base_opt;
      analog_idx_case.opt.analog_enable = sqzc3d_ANALOG_EN_ON;
      analog_idx_case.opt.analog_sel_mode = sqzc3d_ANALOG_SEL_INDICES;
      analog_idx_case.opt.analog_range = {0, base->n_frames};
      analog_idx_case.opt.analog_sel_count = half_analogs;
      analog_idx_case.analog_indices.resize(static_cast<size_t>(half_analogs));
      for (int i = 0; i < half_analogs; ++i) analog_idx_case.analog_indices[static_cast<size_t>(i)] = i;
      analog_idx_case.opt.analog_sel = analog_idx_case.analog_indices.data();
      cases.push_back(std::move(analog_idx_case));

      if (!base_analog_labels.empty()) {
        CaseConfig analog_label_case;
        analog_label_case.name = "analog_labels_head";
        analog_label_case.opt = base_opt;
        analog_label_case.opt.analog_enable = sqzc3d_ANALOG_EN_ON;
        analog_label_case.opt.analog_sel_mode = sqzc3d_ANALOG_SEL_LABELS;
        analog_label_case.opt.analog_range =
            {0, (base->n_frames > 0) ? std::max(1, base->n_frames / 3) : 0};
        analog_label_case.opt.analog_sel_count = std::min(2, base->n_analogs);
        analog_label_case.analog_labels.reserve(static_cast<size_t>(analog_label_case.opt.analog_sel_count));
        for (int i = 0; i < analog_label_case.opt.analog_sel_count; ++i) {
          analog_label_case.analog_labels.push_back(base_analog_labels[static_cast<size_t>(i)]);
        }
        analog_label_case.opt.analog_labels = analog_label_case.analog_labels.data();
        cases.push_back(std::move(analog_label_case));
      }
    }

    bool file_ok = true;

    if (!check_open_memory_no_temp(file_path, base_opt, base)) {
      file_ok = false;
    }

#if sqzc3d_WITH_EZC3D
    if (!check_parser_ezc3d_equivalence(file_path, base)) {
      file_ok = false;
    }
#endif

    if (!check_malformed_schema4_export(*base, file_path, file_idx)) {
      file_ok = false;
    }

    // Verify AUTO analog size gate fails fast (no silent skip).
    if (base->n_analogs > 0 && base->n_analog_by_frame > 0) {
      sqzc3d_build_opt_t gate_opt = base_opt;
      gate_opt.frame_range = {0, 1};
      gate_opt.analog_range = {0, 1};
      gate_opt.analog_enable = sqzc3d_ANALOG_EN_AUTO;
      gate_opt.analog_size_soft_limit_bytes = 1;

      sqzc3d_chunk_t* gate_chunk = nullptr;
      const int st_gate = sqzc3d_build_chunks(dec, &gate_opt, &gate_chunk);
      if (st_gate != sqzc3d_STATUS_INVALID_ARGUMENT || gate_chunk != nullptr) {
        std::cerr << "  mismatch: analog AUTO gate should fail; status=" << st_gate
                  << " chunk=" << (gate_chunk ? "non-null" : "null") << "\n";
        if (gate_chunk) sqzc3d_free_chunk(gate_chunk);
        file_ok = false;
      }
    }

    for (int i = 0; i < static_cast<int>(cases.size()); ++i) {
      if (!run_one_case(dec, cases[i], file_path, file_idx, i)) {
        file_ok = false;
      }
    }
    std::cout << "  file result: " << (file_ok ? "PASSED" : "FAILED") << "\n";
    all_ok = all_ok && file_ok;

    sqzc3d_free_chunk(base);
    sqzc3d_close_dec(dec);
  }

  if (!all_ok) {
    std::cerr << "correctness matrix: FAILED\n";
    return 1;
  }
  std::cout << "correctness matrix: PASSED\n";
  return 0;
}
