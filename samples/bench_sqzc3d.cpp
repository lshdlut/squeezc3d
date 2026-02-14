#include "sqzc3d.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

namespace {

using Clock = std::chrono::high_resolution_clock;

std::string normalize_extension(std::string ext) {
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return ext;
}

bool is_bundle_path(std::string_view path) {
  const std::filesystem::path p(path);
  const auto ext = normalize_extension(p.extension().string());
  return ext == ".sqzc3d" || ext == ".sqzc3d";
}

bool is_c3d_path(std::string_view path) {
  const std::filesystem::path p(path);
  return normalize_extension(p.extension().string()) == ".c3d";
}

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
}

int run_bundle_bench(const std::string& input, int repeat) {
  if (repeat < 1) repeat = 1;
  double sum_load = 0.0;
  double sum_scan = 0.0;
  int last_frames = 0;
  int last_points = 0;
  int last_scalar = 0;
  int64_t last_valid_count = 0;
  volatile double checksum = 0.0;
  for (int i = 0; i < repeat; ++i) {
    const auto t0 = Clock::now();
    sqzc3d_chunk_t* chunk = nullptr;
    const int load_status = sqzc3d_load_bundle(input.c_str(), &chunk);
    const auto t1 = Clock::now();
    if (load_status != sqzc3d_STATUS_SUCCESS || !chunk) {
      std::cerr << "load_bundle failed: " << load_status << "\n";
      if (chunk) sqzc3d_free_chunk(chunk);
      return 2;
    }
    const auto load_ms = elapsed_ms(t0, t1);

    const int frames = sqzc3d_chunk_num_frames(chunk);
    const int points = sqzc3d_chunk_num_points(chunk);
    const int scalar = sqzc3d_chunk_num_scalar(chunk);
    int64_t valid_count = 0;
    if (chunk->points_valid) {
      const int scalar_valid = chunk->valid_nscalar;
      for (int j = 0; j < scalar_valid; ++j) {
        valid_count += static_cast<int64_t>(chunk->points_valid[j]);
      }
    }

    const auto t2 = Clock::now();
    if (chunk->points_xyz) {
      for (int j = 0; j < scalar; ++j) {
        checksum += chunk->points_xyz[j];
      }
    }
    const auto t3 = Clock::now();
    const auto scan_ms = elapsed_ms(t2, t3);
    sum_load += load_ms;
    sum_scan += scan_ms;

    last_frames = frames;
    last_points = points;
    last_scalar = scalar;
    last_valid_count = valid_count;
    sqzc3d_free_chunk(chunk);
  }

  std::cout << std::fixed << std::setprecision(3);
  std::cout << "mode=bundle\n";
  std::cout << "repeat=" << repeat << "\n";
  std::cout << "input=" << input << "\n";
  std::cout << "frames=" << last_frames << "\n";
  std::cout << "points=" << last_points << "\n";
  std::cout << "scalar=" << last_scalar << "\n";
  std::cout << "valid_count=" << last_valid_count << "\n";
  std::cout << "load_ms=" << (sum_load / static_cast<double>(repeat)) << "\n";
  std::cout << "scan_ms=" << (sum_scan / static_cast<double>(repeat)) << "\n";
  std::cout << "checksum=" << checksum << "\n";
  return 0;
}

int run_c3d_bench(const std::string& input, int repeat) {
#if sqzc3d_WITH_EZC3D == 0
  (void)input;
  (void)repeat;
  std::cerr << "c3d benchmark requires sqzc3d_WITH_EZC3D=ON\n";
  return 1;
#else
  sqzc3d_open_opt_t open_opt{};
  sqzc3d_default_open_opt(&open_opt);

  double sum_open = 0.0;
  double sum_build = 0.0;
  double sum_scan = 0.0;
  int last_frames = 0;
  int last_points = 0;
  int last_scalar = 0;
  int64_t last_valid_count = 0;
  volatile double checksum = 0.0;
  for (int i = 0; i < repeat; ++i) {
    const auto t0 = Clock::now();
    sqzc3d_dec_t* dec = nullptr;
    const int open_status = sqzc3d_open_file(&dec, input.c_str(), &open_opt);
    const auto t1 = Clock::now();
    if (open_status != sqzc3d_STATUS_SUCCESS || !dec) {
      std::cerr << "open failed: " << (dec ? sqzc3d_last_error(dec) : "invalid handle") << "\n";
      if (dec) sqzc3d_close_dec(dec);
      return 2;
    }

    sqzc3d_build_opt_t build_opt{};
    sqzc3d_default_build_opt(&build_opt);
    build_opt.residual_gate_mm = 0.0;
    build_opt.read_policy = sqzc3d_READ_POLICY_AUTO;

    sqzc3d_chunk_t* chunk = nullptr;
    const auto t2 = Clock::now();
    const int build_status = sqzc3d_build_chunks(dec, &build_opt, &chunk);
    const auto t3 = Clock::now();
    if (build_status != sqzc3d_STATUS_SUCCESS || !chunk) {
      std::cerr << "build failed: " << build_status << "\n";
      sqzc3d_close_dec(dec);
      return 3;
    }

    const int frames = sqzc3d_chunk_num_frames(chunk);
    const int points = sqzc3d_chunk_num_points(chunk);
    const int scalar = sqzc3d_chunk_num_scalar(chunk);
    int64_t valid_count = 0;
    if (chunk->points_valid) {
      for (int j = 0; j < chunk->valid_nscalar; ++j) {
        valid_count += static_cast<int64_t>(chunk->points_valid[j]);
      }
    }

    const auto t4 = Clock::now();
    if (chunk->points_xyz) {
      for (int j = 0; j < scalar; ++j) {
        checksum += chunk->points_xyz[j];
      }
    }
    const auto t5 = Clock::now();

    sum_open += elapsed_ms(t0, t1);
    sum_build += elapsed_ms(t2, t3);
    sum_scan += elapsed_ms(t4, t5);

    last_frames = frames;
    last_points = points;
    last_scalar = scalar;
    last_valid_count = valid_count;

    sqzc3d_free_chunk(chunk);
    sqzc3d_close_dec(dec);
  }

  std::cout << std::fixed << std::setprecision(3);
  std::cout << "mode=c3d\n";
  std::cout << "repeat=" << repeat << "\n";
  std::cout << "input=" << input << "\n";
  std::cout << "frames=" << last_frames << "\n";
  std::cout << "points=" << last_points << "\n";
  std::cout << "scalar=" << last_scalar << "\n";
  std::cout << "valid_count=" << last_valid_count << "\n";
  std::cout << "open_ms=" << (sum_open / static_cast<double>(repeat)) << "\n";
  std::cout << "build_ms=" << (sum_build / static_cast<double>(repeat)) << "\n";
  std::cout << "scan_ms=" << (sum_scan / static_cast<double>(repeat)) << "\n";
  std::cout << "checksum=" << checksum << "\n";
  return 0;
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "usage: bench_sqzc3d <path-to-c3d-or-bundle> [repeat=5]\n";
    return 1;
  }

  const std::string input = argv[1];
  int repeat = 5;
  if (argc >= 3) {
    try {
      repeat = std::stoi(argv[2]);
      if (repeat < 1 || repeat > 200) {
        std::cerr << "repeat must be in [1, 200]" << std::endl;
        return 1;
      }
    } catch (...) {
      std::cerr << "invalid repeat value" << std::endl;
      return 1;
    }
  }

  if (is_c3d_path(input)) {
    return run_c3d_bench(input, repeat);
  }
  if (is_bundle_path(input) || std::filesystem::is_directory(input)) {
    return run_bundle_bench(input, repeat);
  }

  std::cerr << "unsupported input extension. expected .c3d, .sqzc3d, .sqzc3d or bundle directory\n";
  return 1;
}

