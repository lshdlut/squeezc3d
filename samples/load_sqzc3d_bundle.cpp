#include "sqzc3d.h"

#include <iostream>

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "usage: load_sqzc3d_bundle <bundle-dir-or-.sqzc3d>" << std::endl;
    return 1;
  }

  sqzc3d_chunk_t* chunk = nullptr;
  const int load_status = sqzc3d_load_bundle(argv[1], &chunk);
  if (load_status != sqzc3d_STATUS_SUCCESS || !chunk) {
    std::cerr << "load failed: " << load_status << std::endl;
    return 2;
  }

  std::cout << "frames=" << chunk->n_frames << std::endl;
  std::cout << "read_policy=" << chunk->read_policy << std::endl;
  std::cout << "points=" << chunk->n_points << std::endl;
  std::cout << "points_total=" << chunk->n_points_total << std::endl;
  std::cout << "analogs=" << chunk->n_analogs << std::endl;
  std::cout << "analog_by_frame=" << chunk->n_analog_by_frame << std::endl;
  std::cout << "n_scalar=" << chunk->n_scalar << std::endl;
  std::cout << "valid_nscalar=" << chunk->valid_nscalar << std::endl;
  std::cout << "n_analog_scalar=" << chunk->n_analog_scalar << std::endl;
  if (chunk->reason) {
    std::cout << "reason=" << chunk->reason << std::endl;
  }
  if (chunk->n_points > 0 && chunk->point_labels && chunk->point_labels[0]) {
    std::cout << "first_point_label=" << chunk->point_labels[0] << std::endl;
  }
  if (chunk->n_analogs > 0 && chunk->analog_labels && chunk->analog_labels[0]) {
    std::cout << "first_analog_label=" << chunk->analog_labels[0] << std::endl;
  }

  sqzc3d_free_chunk(chunk);
  return 0;
}

