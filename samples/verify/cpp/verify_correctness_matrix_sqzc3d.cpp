#include "sqzc3d.h"
#include "sqzc3d_easy.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <string>
#include <vector>

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
  if (lhs->n_analog_scalar != rhs->n_analog_scalar) {
    std::cerr << "  mismatch: n_analog_scalar=" << lhs->n_analog_scalar << " vs " << rhs->n_analog_scalar << "\n";
    return false;
  }
  if (lhs->raw_params_nbytes != rhs->raw_params_nbytes) {
    std::cerr << "  mismatch: raw_params_nbytes=" << lhs->raw_params_nbytes << " vs " << rhs->raw_params_nbytes
              << "\n";
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

  for (int i = 0; i < lhs->raw_params_nbytes; ++i) {
    if (lhs->raw_params[i] != rhs->raw_params[i]) return false;
  }
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
  if (!check_chunk_index_contract(built, cfg.name + ":built")) {
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
      !check_chunk_index_contract(strict_loaded, cfg.name + ":strict_loaded")) {
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
      std::cerr << "  open failed: " << open_status << "\n";
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
    cases.reserve(6);
    cases.push_back({"all_points_auto", base_opt, {}, {}, {}, {}});

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
