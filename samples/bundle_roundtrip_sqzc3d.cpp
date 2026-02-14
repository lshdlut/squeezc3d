#include "sqzc3d.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "usage: bundle_roundtrip_sqzc3d <path-to-c3d>" << std::endl;
    return 1;
  }

  sqzc3d_open_opt_t open_opt{};
  sqzc3d_default_open_opt(&open_opt);

  sqzc3d_dec_t* dec = nullptr;
  const int open_status = sqzc3d_open_file(&dec, argv[1], &open_opt);
  if (open_status != sqzc3d_STATUS_SUCCESS || !dec) {
    std::cerr << "open failed: " << (dec ? sqzc3d_last_error(dec) : "invalid handle") << std::endl;
    if (dec) sqzc3d_close_dec(dec);
    return 2;
  }

  sqzc3d_build_opt_t build_opt{};
  sqzc3d_default_build_opt(&build_opt);
  build_opt.residual_gate_mm = 0.0;

  sqzc3d_chunk_t* chunk = nullptr;
  const int build_status = sqzc3d_build_chunks(dec, &build_opt, &chunk);
  if (build_status != sqzc3d_STATUS_SUCCESS || !chunk) {
    std::cerr << "build failed: " << build_status << std::endl;
    sqzc3d_close_dec(dec);
    return 3;
  }

  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const std::string work_dir =
      std::string("sqzc3d_rt_") + std::to_string(now) + std::string("_bundle");
  const int export_status = sqzc3d_export_bundle(work_dir.c_str(), chunk);
  if (export_status != sqzc3d_STATUS_SUCCESS) {
    std::cerr << "export failed: " << export_status << std::endl;
    sqzc3d_free_chunk(chunk);
    sqzc3d_close_dec(dec);
    return 4;
  }

  sqzc3d_chunk_t* loaded = nullptr;
  const int load_status = sqzc3d_load_bundle(work_dir.c_str(), &loaded);
  if (load_status != sqzc3d_STATUS_SUCCESS || !loaded) {
    std::cerr << "load failed: " << load_status << std::endl;
    sqzc3d_free_chunk(chunk);
    sqzc3d_close_dec(dec);
    return 5;
  }

  bool ok = true;
  auto fail_reason = "unknown";
  if (chunk->n_frames != loaded->n_frames) {
    fail_reason = "n_frames mismatch";
    ok = false;
  } else if (chunk->n_points != loaded->n_points) {
    fail_reason = "n_points mismatch";
    ok = false;
  } else if (chunk->n_points_total != loaded->n_points_total) {
    fail_reason = "n_points_total mismatch";
    ok = false;
  } else if (chunk->n_analogs != loaded->n_analogs) {
    fail_reason = "n_analogs mismatch";
    ok = false;
  } else if (chunk->n_analog_by_frame != loaded->n_analog_by_frame) {
    fail_reason = "n_analog_by_frame mismatch";
    ok = false;
  } else if (chunk->n_scalar != loaded->n_scalar) {
    fail_reason = "n_scalar mismatch";
    ok = false;
  } else if (chunk->valid_nscalar != loaded->valid_nscalar) {
    fail_reason = "valid_nscalar mismatch";
    ok = false;
  } else if (chunk->n_analog_scalar != loaded->n_analog_scalar) {
    fail_reason = "n_analog_scalar mismatch";
    ok = false;
  } else if (chunk->points_layout != loaded->points_layout) {
    fail_reason = "points_layout mismatch";
    ok = false;
  } else if (chunk->points_pack != loaded->points_pack) {
    fail_reason = "points_pack mismatch";
    ok = false;
  } else if (chunk->read_policy != loaded->read_policy) {
    fail_reason = "read_policy mismatch";
    ok = false;
  } else if (chunk->valid_policy != loaded->valid_policy) {
    fail_reason = "valid_policy mismatch";
    ok = false;
  } else if (chunk->raw_params_nbytes != loaded->raw_params_nbytes) {
    fail_reason = "raw_params_nbytes mismatch";
    ok = false;
  }
  if (!ok) {
    std::cerr << "base read_policy=" << chunk->read_policy << ", loaded read_policy=" << loaded->read_policy
              << ", base reason=" << (chunk->reason ? chunk->reason : "") << ", loaded reason="
              << (loaded->reason ? loaded->reason : "") << std::endl;
  }
  if (ok && chunk->n_scalar > 0 && chunk->points_xyz && loaded->points_xyz) {
    for (int i = 0; i < chunk->n_scalar; ++i) {
      if (std::fabs(chunk->points_xyz[i] - loaded->points_xyz[i]) > 0.0) {
        ok = false;
        fail_reason = "points_xyz mismatch";
        break;
      }
    }
  }
  if (ok && chunk->valid_nscalar > 0 && chunk->points_valid && loaded->points_valid) {
    for (int i = 0; i < chunk->valid_nscalar; ++i) {
      if (chunk->points_valid[i] != loaded->points_valid[i]) {
        ok = false;
        fail_reason = "points_valid mismatch";
        break;
      }
    }
  }
  if (ok && chunk->n_analog_scalar > 0 && chunk->analog && loaded->analog) {
    for (int i = 0; i < chunk->n_analog_scalar; ++i) {
      if (std::fabs(chunk->analog[i] - loaded->analog[i]) > 0.0) {
        ok = false;
        fail_reason = "analog mismatch";
        break;
      }
    }
  }
  if (ok && chunk->n_analog_scalar > 0 && chunk->analog_valid && loaded->analog_valid) {
    for (int i = 0; i < chunk->n_analog_scalar; ++i) {
      if (chunk->analog_valid[i] != loaded->analog_valid[i]) {
        ok = false;
        fail_reason = "analog_valid mismatch";
        break;
      }
    }
  }
  if (ok && chunk->raw_params_nbytes > 0 && loaded->raw_params_nbytes > 0 &&
      chunk->raw_params && loaded->raw_params) {
    const auto raw = std::equal(
        chunk->raw_params, chunk->raw_params + chunk->raw_params_nbytes, loaded->raw_params);
    if (!raw) {
      ok = false;
      fail_reason = "raw_params mismatch";
    }
  }

  if (!ok) {
    std::cerr << "bundle roundtrip mismatch: " << fail_reason << std::endl;
    sqzc3d_free_chunk(loaded);
    sqzc3d_free_chunk(chunk);
    sqzc3d_close_dec(dec);
    return 6;
  }

  std::cout << "bundle roundtrip ok: " << work_dir << std::endl;

  sqzc3d_free_chunk(loaded);
  sqzc3d_free_chunk(chunk);
  sqzc3d_close_dec(dec);
  return 0;
}


