#include "sqzc3d_c3d_stream.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "bench_access_patterns.h"

namespace {

static std::string normalize_extension(std::string ext) {
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return ext;
}

struct MemSnapshot {
  double rss_mb = 0.0;
  double peak_rss_mb = 0.0;
};

static MemSnapshot snapshot_memory() {
  MemSnapshot out{};
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                           sizeof(pmc))) {
    out.rss_mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    out.peak_rss_mb = static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
  }
#else
  struct rusage ru {};
  if (getrusage(RUSAGE_SELF, &ru) == 0) {
    out.peak_rss_mb = static_cast<double>(ru.ru_maxrss) / 1024.0;
  }
  std::ifstream in("/proc/self/statm");
  long pages = 0;
  long resident = 0;
  if (in.good() && (in >> pages >> resident)) {
    const long page_size = sysconf(_SC_PAGESIZE);
    out.rss_mb = static_cast<double>(resident) * static_cast<double>(page_size) / (1024.0 * 1024.0);
  }
#endif
  if (out.peak_rss_mb <= 0.0) out.peak_rss_mb = out.rss_mb;
  return out;
}

struct StreamMetrics {
  double open_ms = 0.0;

  double read_frame_all_ms = 0.0;
  double read_window_T256_kall_ms = 0.0;
  double read_window_T256_k32_ms = 0.0;
  double read_traj_Tfull_k1_ms = 0.0;

  int n_frames = 0;
  int n_points = 0;
  std::int64_t file_bytes = 0;
};

static StreamMetrics compute_stream_metrics(sqzc3d::C3dStreamReader* reader) {
  StreamMetrics m{};
  if (!reader) return m;
  m.n_frames = reader->meta.n_frames;
  m.n_points = reader->meta.n_points;
  if (m.n_frames <= 0 || m.n_points <= 0) return m;

  const int n_frames = m.n_frames;
  const int n_points = m.n_points;
  const int t256 = std::min(256, n_frames);
  const int k32 = std::min(32, n_points);

  std::vector<int> all(static_cast<std::size_t>(n_points));
  for (int i = 0; i < n_points; ++i) all[static_cast<std::size_t>(i)] = i;
  const auto sel32 = sqzc3d_bench::make_strided_selection_indices(n_points, k32, /*stride=*/7);

  std::vector<double> frame_xyz(static_cast<std::size_t>(n_points) * 3u, 0.0);
  std::vector<double> win_all(static_cast<std::size_t>(t256) * static_cast<std::size_t>(n_points) * 3u, 0.0);
  std::vector<double> win32(static_cast<std::size_t>(t256) * static_cast<std::size_t>(k32) * 3u, 0.0);
  std::vector<double> traj1(static_cast<std::size_t>(n_frames) * 3u, 0.0);
  std::vector<unsigned char> valid_all(static_cast<std::size_t>(t256) * static_cast<std::size_t>(n_points), 0u);
  std::vector<unsigned char> valid32(static_cast<std::size_t>(t256) * static_cast<std::size_t>(k32), 0u);
  std::vector<unsigned char> valid1(static_cast<std::size_t>(n_frames), 0u);

  const auto t_frame = sqzc3d_bench::time_ms_adaptive(
      [&](int it) -> double {
        const int f = (n_frames > 1) ? (it % n_frames) : 0;
        const int st = sqzc3d::sqzc3d_c3d_stream_read_frame_all_xyz(
            reader, f, frame_xyz.data(), static_cast<int>(frame_xyz.size()));
        if (st != sqzc3d_STATUS_SUCCESS) return 0.0;
        return frame_xyz[static_cast<std::size_t>((it % n_points) * 3)];
      },
      /*target_total_ms=*/100.0,
      /*max_iters=*/1 << 12);
  m.read_frame_all_ms = t_frame.per_op_ms;

  const auto t_win_all = sqzc3d_bench::time_ms_adaptive(
      [&](int it) -> double {
        const int s0 = (n_frames > t256) ? (it % (n_frames - t256 + 1)) : 0;
        const int st = sqzc3d::sqzc3d_c3d_stream_read_traj_xyz_sel(
            reader,
            all.data(),
            n_points,
            s0,
            s0 + t256,
            win_all.data(),
            static_cast<int>(win_all.size()),
            valid_all.data(),
            static_cast<int>(valid_all.size()));
        if (st != sqzc3d_STATUS_SUCCESS) return 0.0;
        return win_all[static_cast<std::size_t>((it % (t256 * n_points)) * 3u)];
      },
      /*target_total_ms=*/200.0,
      /*max_iters=*/1 << 10);
  m.read_window_T256_kall_ms = t_win_all.per_op_ms;

  const auto t_win32 = sqzc3d_bench::time_ms_adaptive(
      [&](int it) -> double {
        const int s0 = (n_frames > t256) ? (it % (n_frames - t256 + 1)) : 0;
        const int st = sqzc3d::sqzc3d_c3d_stream_read_traj_xyz_sel(
            reader,
            sel32.data(),
            static_cast<int>(sel32.size()),
            s0,
            s0 + t256,
            win32.data(),
            static_cast<int>(win32.size()),
            valid32.data(),
            static_cast<int>(valid32.size()));
        if (st != sqzc3d_STATUS_SUCCESS) return 0.0;
        return win32[static_cast<std::size_t>((it % (t256 * k32)) * 3u)];
      },
      /*target_total_ms=*/200.0,
      /*max_iters=*/1 << 10);
  m.read_window_T256_k32_ms = t_win32.per_op_ms;

  const auto t_traj1 = sqzc3d_bench::time_ms_adaptive(
      [&](int it) -> double {
        const int p = it % n_points;
        const int st = sqzc3d::sqzc3d_c3d_stream_read_traj_xyz_sel(
            reader,
            &p,
            1,
            0,
            n_frames,
            traj1.data(),
            static_cast<int>(traj1.size()),
            valid1.data(),
            static_cast<int>(valid1.size()));
        if (st != sqzc3d_STATUS_SUCCESS) return 0.0;
        return traj1[static_cast<std::size_t>((it % n_frames) * 3u)];
      },
      /*target_total_ms=*/200.0,
      /*max_iters=*/1 << 8);
  m.read_traj_Tfull_k1_ms = t_traj1.per_op_ms;

  return m;
}

static int run_bench(const std::string& input, int repeat) {
  if (repeat < 1) repeat = 1;

  std::error_code ec;
  const std::int64_t file_bytes = static_cast<std::int64_t>(std::filesystem::file_size(input, ec));
  const std::int64_t safe_file_bytes = ec ? 0 : file_bytes;

  const MemSnapshot mem0 = snapshot_memory();
  MemSnapshot mem1 = mem0;

  double sum_open = 0.0;
  StreamMetrics metrics{};
  volatile double checksum = 0.0;

  for (int i = 0; i < repeat; ++i) {
    sqzc3d::C3dStreamReader reader{};
    const auto t0 = sqzc3d_bench::Clock::now();
    const int st = sqzc3d::sqzc3d_c3d_stream_open_file(&reader, input.c_str());
    const auto t1 = sqzc3d_bench::Clock::now();
    if (st != sqzc3d_STATUS_SUCCESS) {
      std::cerr << "open failed: status=" << st << "\n";
      return 2;
    }
    sum_open += sqzc3d_bench::elapsed_ms(t0, t1);

    if (i == 0) {
      metrics = compute_stream_metrics(&reader);
      metrics.open_ms = sum_open;
      metrics.file_bytes = safe_file_bytes;
    }
    if (metrics.n_points > 0) {
      std::vector<double> one(static_cast<std::size_t>(metrics.n_points) * 3u, 0.0);
      (void)sqzc3d::sqzc3d_c3d_stream_read_frame_all_xyz(
          &reader, 0, one.data(), static_cast<int>(one.size()));
      checksum += one[0];
    }
    sqzc3d::sqzc3d_c3d_stream_close(&reader);
    mem1 = snapshot_memory();
  }

  std::cout << std::fixed << std::setprecision(6);
  std::cout << "lib=sqzc3d\n";
  std::cout << "mode=stream\n";
  std::cout << "repeat=" << repeat << "\n";
  std::cout << "input=" << input << "\n";
  std::cout << "frames=" << metrics.n_frames << "\n";
  std::cout << "points=" << metrics.n_points << "\n";
  std::cout << "file_bytes=" << safe_file_bytes << "\n";
  std::cout << "rss_baseline_mb=" << mem0.rss_mb << "\n";
  std::cout << "peak_rss_mb=" << mem1.peak_rss_mb << "\n";
  std::cout << "peak_rss_delta_mb=" << (mem1.peak_rss_mb - mem0.rss_mb) << "\n";
  std::cout << "open_ms=" << (sum_open / static_cast<double>(repeat)) << "\n";
  if (metrics.read_frame_all_ms > 0.0) std::cout << "read_frame_all_ms=" << metrics.read_frame_all_ms << "\n";
  if (metrics.read_window_T256_kall_ms > 0.0) {
    std::cout << "read_window_ms_T256_kall=" << metrics.read_window_T256_kall_ms << "\n";
  }
  if (metrics.read_window_T256_k32_ms > 0.0) {
    std::cout << "read_window_ms_T256_k32=" << metrics.read_window_T256_k32_ms << "\n";
  }
  if (metrics.read_traj_Tfull_k1_ms > 0.0) {
    std::cout << "read_traj_ms_Tfull_k1=" << metrics.read_traj_Tfull_k1_ms << "\n";
  }
  std::cout << "checksum=" << checksum << "\n";
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "usage: bench_sqzc3d_stream <path-to-c3d> [repeat=1]\n";
    return 1;
  }
  const std::string input = argv[1];
  const auto ext = normalize_extension(std::filesystem::path(input).extension().string());
  if (ext != ".c3d") {
    std::cerr << "unsupported input extension. expected .c3d\n";
    return 1;
  }

  int repeat = 1;
  if (argc >= 3) {
    try {
      repeat = std::stoi(argv[2]);
      if (repeat < 1 || repeat > 10) {
        std::cerr << "repeat must be in [1, 10]\n";
        return 1;
      }
    } catch (...) {
      repeat = 1;
    }
  }

  return run_bench(input, repeat);
}
