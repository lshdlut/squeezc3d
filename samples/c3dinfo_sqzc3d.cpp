#include "sqzc3d.h"

#include <iostream>

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "usage: c3dinfo_sqzc3d <path-to-c3d>" << std::endl;
    return 1;
  }

  sqzc3d_open_opt_t open_opt{};
  sqzc3d_default_open_opt(&open_opt);
  open_opt.open_mode = sqzc3d_FILE;

  sqzc3d_dec_t* dec = nullptr;
  const int open_status = sqzc3d_open_file(&dec, argv[1], &open_opt);
  if (open_status != sqzc3d_STATUS_SUCCESS || !dec) {
    std::cerr << "open failed: " << (dec ? sqzc3d_last_error(dec) : "invalid handle") << std::endl;
    if (dec) sqzc3d_close_dec(dec);
    return 2;
  }

  sqzc3d_build_opt_t build_opt{};
  sqzc3d_default_build_opt(&build_opt);
  build_opt.analog_enable = sqzc3d_ANALOG_EN_OFF;
  build_opt.residual_gate_mm = 0.0;
  build_opt.frame_range = {0, -1};
  build_opt.point_sel_mode = sqzc3d_POINT_SEL_ALL;
  build_opt.point_sel_count = 0;

  sqzc3d_chunk_t* chunk = nullptr;
  const int build_status = sqzc3d_build_chunks(dec, &build_opt, &chunk);
  if (build_status != sqzc3d_STATUS_SUCCESS || !chunk) {
    std::cerr << "build failed: " << build_status << std::endl;
    sqzc3d_close_dec(dec);
    return 3;
  }

  const int nFrames = sqzc3d_chunk_num_frames(chunk);
  const int nPoints = sqzc3d_chunk_num_points(chunk);
  const int nScalars = sqzc3d_chunk_num_scalar(chunk);
  const int nValid = chunk->valid_nscalar;

  std::cout << "c3d=" << argv[1] << std::endl;
  std::cout << "frames=" << nFrames << std::endl;
  std::cout << "points=" << nPoints << std::endl;
  std::cout << "point_scalar=" << nScalars << std::endl;
  std::cout << "valid_scalar=" << nValid << std::endl;
  std::cout << "analogs=" << chunk->n_analogs << std::endl;
  std::cout << "analog_by_frame=" << chunk->n_analog_by_frame << std::endl;
  std::cout << "point_scale=" << chunk->point_scale << std::endl;
  std::cout << "header_scale=" << chunk->header_scale << std::endl;
  std::cout << "residual_gate_mm=" << chunk->residual_gate_mm << std::endl;
  if (chunk->reason) {
    std::cout << "reason=" << chunk->reason << std::endl;
  }
  if (nPoints > 0 && chunk->point_labels) {
    std::cout << "first_point_label="
              << (chunk->point_labels[0] ? chunk->point_labels[0] : "(none)") << std::endl;
  }

  sqzc3d_free_chunk(chunk);
  sqzc3d_close_dec(dec);
  return 0;
}

