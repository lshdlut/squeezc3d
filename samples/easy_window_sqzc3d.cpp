#include "sqzc3d.h"
#include "sqzc3d_easy.h"

#include <iostream>
#include <vector>

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "usage: easy_window_sqzc3d <path-to-c3d> [start_frame] [frame_count]\n";
    return 1;
  }

  const int start_frame = (argc > 2) ? std::atoi(argv[2]) : 0;
  const int frame_count = (argc > 3) ? std::atoi(argv[3]) : 32;

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

  sqzc3d_build_opt_t preset{};
  sqzc3d_apply_preset_stream_frame_all(&preset);
  sqzc3d_chunk_t* chunk = nullptr;
  const int build_status = sqzc3d::ReadPointsWindow(
      dec, start_frame, frame_count, nullptr, 0, &preset, &chunk);
  if (build_status != sqzc3d_STATUS_SUCCESS || !chunk) {
    std::cerr << "build failed: " << build_status << std::endl;
    sqzc3d_close_dec(dec);
    return 3;
  }

  const sqzc3d::PointWindow points = sqzc3d::FrameMajorPointsView(chunk);
  std::cout << "frames=" << points.n_frames << "\n";
  std::cout << "points=" << points.n_points << "\n";

  if (points.n_frames > 0 && points.n_points > 0 && points.xyz) {
    const int index = 0;
    const sqzc3d_num_t x = points.xyz[index * points.point_stride];
    const sqzc3d_num_t y = points.xyz[index * points.point_stride + 1];
    const sqzc3d_num_t z = points.xyz[index * points.point_stride + 2];
    const unsigned char v = (points.valid && points.valid[index]) ? points.valid[index] : 1u;
    std::cout << "first_point_frame0 = (" << x << ", " << y << ", " << z << ") valid=" << int(v)
              << "\n";
  }

  if (points.n_frames > 0 && points.n_points > 0 && points.xyz) {
    std::vector<sqzc3d_num_t> point_major_xyz(static_cast<std::size_t>(points.n_frames) *
                                              static_cast<std::size_t>(points.n_points) * 3u);
    std::vector<unsigned char> point_major_valid(static_cast<std::size_t>(points.n_frames) *
                                                static_cast<std::size_t>(points.n_points));
    sqzc3d::ReorderFrameMajorToPointMajor(
        points.xyz, points.valid, points.n_frames, points.n_points, point_major_xyz.data(),
        point_major_valid.data());
    std::cout << "point_major_sample_count=" << point_major_xyz.size() << "\n";
  }

  sqzc3d_free_chunk(chunk);
  sqzc3d_close_dec(dec);
  return 0;
}
