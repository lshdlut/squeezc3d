#include "sqzc3d.h"

#include <iostream>

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "usage: export_sqzc3d_bundle <path-to-c3d> <out-dir>" << std::endl;
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
  build_opt.residual_gate_mm = 0.0;
  build_opt.analog_enable = sqzc3d_ANALOG_EN_AUTO;

  sqzc3d_chunk_t* chunk = nullptr;
  const int build_status = sqzc3d_build_chunks(dec, &build_opt, &chunk);
  if (build_status != sqzc3d_STATUS_SUCCESS || !chunk) {
    std::cerr << "build failed: " << build_status << std::endl;
    sqzc3d_close_dec(dec);
    return 3;
  }

  const int export_status = sqzc3d_export_bundle(argv[2], chunk);
  if (export_status != sqzc3d_STATUS_SUCCESS) {
    std::cerr << "export failed: " << export_status << std::endl;
    sqzc3d_free_chunk(chunk);
    sqzc3d_close_dec(dec);
    return 4;
  }

  std::cout << "exported: " << argv[2] << "/meta.json, " << argv[2] << "/data.bin" << std::endl;
  sqzc3d_free_chunk(chunk);
  sqzc3d_close_dec(dec);
  return 0;
}

