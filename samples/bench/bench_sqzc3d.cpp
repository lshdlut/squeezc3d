#include "sqzc3d.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cctype>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
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

static std::string normalize_extension(std::string ext) {
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return ext;
}

static bool is_bundle_path(std::string_view path) {
  const std::filesystem::path p(path);
  const auto ext = normalize_extension(p.extension().string());
  return ext == ".sqzc3d";
}

static bool is_c3d_path(std::string_view path) {
  const std::filesystem::path p(path);
  return normalize_extension(p.extension().string()) == ".c3d";
}

struct LoadResult {
  int status = sqzc3d_STATUS_SUCCESS;
  double load_ms = 0.0;
  std::int64_t file_bytes = 0;
  sqzc3d_chunk_t* chunk = nullptr;
};

static LoadResult load_sqzc3d_c3d_materialize(const std::string& input) {
  LoadResult out{};
#if sqzc3d_WITH_EZC3D == 0
  (void)input;
  out.status = sqzc3d_STATUS_NOT_IMPLEMENTED;
  return out;
#else
  std::error_code ec;
  out.file_bytes = static_cast<std::int64_t>(std::filesystem::file_size(input, ec));
  if (ec) out.file_bytes = 0;

  sqzc3d_open_opt_t open_opt;
  sqzc3d_default_open_opt(&open_opt);

  sqzc3d_build_opt_t build_opt;
  sqzc3d_default_build_opt(&build_opt);
  build_opt.frame_range = {0, -1};
  build_opt.point_sel_mode = sqzc3d_POINT_SEL_ALL;
  build_opt.point_sel = nullptr;
  build_opt.point_sel_count = 0;

  sqzc3d_dec_t* dec = nullptr;
  const auto t0 = sqzc3d_bench::Clock::now();
  out.status = sqzc3d_open_file(&dec, input.c_str(), &open_opt);
  if (out.status != sqzc3d_STATUS_SUCCESS || !dec) {
    if (dec) sqzc3d_close_dec(dec);
    return out;
  }
  out.status = sqzc3d_build_chunks(dec, &build_opt, &out.chunk);
  (void)sqzc3d_close_dec(dec);
  const auto t1 = sqzc3d_bench::Clock::now();
  out.load_ms = sqzc3d_bench::elapsed_ms(t0, t1);
  if (out.status != sqzc3d_STATUS_SUCCESS || !out.chunk) {
    if (out.chunk) sqzc3d_free_chunk(out.chunk);
    out.chunk = nullptr;
  }
  return out;
#endif
}

static LoadResult load_sqzc3d_bundle(const std::string& input) {
  LoadResult out{};
  const auto t0 = sqzc3d_bench::Clock::now();
  out.status = sqzc3d_load_bundle(input.c_str(), &out.chunk);
  const auto t1 = sqzc3d_bench::Clock::now();
  out.load_ms = sqzc3d_bench::elapsed_ms(t0, t1);
  if (out.status != sqzc3d_STATUS_SUCCESS || !out.chunk) {
    if (out.chunk) sqzc3d_free_chunk(out.chunk);
    out.chunk = nullptr;
  }
  return out;
}

static int largest_type_group_index(const sqzc3d_chunk_t* chunk) {
  if (!chunk || chunk->n_type_groups <= 0 || !chunk->type_group_starts || !chunk->type_group_names) return -1;
  int best = -1;
  int best_n = -1;
  for (int i = 0; i < chunk->n_type_groups; ++i) {
    const int s = chunk->type_group_starts[static_cast<std::size_t>(i)];
    const int e = chunk->type_group_starts[static_cast<std::size_t>(i) + 1u];
    const int n = e - s;
    if (n > best_n) {
      best_n = n;
      best = i;
    }
  }
  return best;
}

struct AccessMetrics {
  int k_small = 0;
  int k_mid = 0;
  int k_all = 0;
  int t32 = 0;
  int t256 = 0;

  double frame_view_ns_k1 = 0.0;
  double frame_view_ns_k32 = 0.0;
  double frame_view_ns_kall = 0.0;

  double frame_copy_ms_k1 = 0.0;
  double frame_copy_ms_k32 = 0.0;
  double frame_copy_ms_kall = 0.0;

  double window_read_ms_T32_k32 = 0.0;
  double window_read_ms_T32_kall = 0.0;
  double window_read_ms_T256_k32 = 0.0;
  double window_read_ms_T256_kall = 0.0;

  double traj_strided_ms_T32_k32 = 0.0;
  double traj_strided_ms_T32_kall = 0.0;
  double traj_strided_ms_T256_k32 = 0.0;
  double traj_strided_ms_T256_kall = 0.0;
  double traj_strided_ms_Tfull_k1 = 0.0;

  double reorder_ms_T32_k32 = 0.0;
  double traj_contig_ms_T32_k32 = 0.0;
  double reorder_ms_T32_kall = 0.0;
  double traj_contig_ms_T32_kall = 0.0;
  double reorder_ms_T256_k32 = 0.0;
  double traj_contig_ms_T256_k32 = 0.0;
  double reorder_ms_T256_kall = 0.0;
  double traj_contig_ms_T256_kall = 0.0;

  double label_map_ms_k32 = 0.0;
  int label_map_miss_k32 = 0;

  std::string sel_group_name = "-";
  int sel_group_n = 0;
  int sel_base_k = 0;
  int sel_filtered_k = 0;
  double sel_apply_ms_T256_k32 = 0.0;
};

static AccessMetrics compute_access_metrics(const sqzc3d_chunk_t* chunk) {
  AccessMetrics m{};
  if (!chunk || !chunk->points_xyz || chunk->n_frames <= 0 || chunk->n_points <= 0) return m;

  const int n_frames = chunk->n_frames;
  const int n_points = chunk->n_points;

  m.k_small = 1;
  m.k_mid = std::min(32, n_points);
  m.k_all = n_points;
  m.t32 = std::min(32, n_frames);
  m.t256 = std::min(256, n_frames);
  const int start = 0;

  const sqzc3d_bench::PointsFrameMajorView v{
      /*xyz=*/chunk->points_xyz,
      /*valid=*/chunk->points_valid,
      /*n_frames=*/n_frames,
      /*n_points=*/n_points,
  };

  std::vector<double> tmp;
  std::vector<double> win;
  std::vector<double> point_major;
  std::vector<double> sel_buf;
  std::vector<int> sel_scratch;

  m.frame_view_ns_k1 = sqzc3d_bench::bench_frame_view_ns(v, m.k_small);
  m.frame_view_ns_k32 = sqzc3d_bench::bench_frame_view_ns(v, m.k_mid);
  m.frame_view_ns_kall = sqzc3d_bench::bench_frame_view_ns(v, m.k_all);

  m.frame_copy_ms_k1 = sqzc3d_bench::bench_frame_copy_ms(v, 0, m.k_small, &tmp).per_op_ms;
  m.frame_copy_ms_k32 = sqzc3d_bench::bench_frame_copy_ms(v, 0, m.k_mid, &tmp).per_op_ms;
  m.frame_copy_ms_kall = sqzc3d_bench::bench_frame_copy_ms(v, 0, m.k_all, &tmp).per_op_ms;

  m.window_read_ms_T32_k32 = sqzc3d_bench::bench_window_copy_ms(v, start, m.t32, m.k_mid, &win).per_op_ms;
  m.window_read_ms_T32_kall = sqzc3d_bench::bench_window_copy_ms(v, start, m.t32, m.k_all, &win).per_op_ms;
  m.window_read_ms_T256_k32 = sqzc3d_bench::bench_window_copy_ms(v, start, m.t256, m.k_mid, &win).per_op_ms;
  m.window_read_ms_T256_kall = sqzc3d_bench::bench_window_copy_ms(v, start, m.t256, m.k_all, &win).per_op_ms;

  m.traj_strided_ms_T32_k32 = sqzc3d_bench::bench_traj_strided_ms(v, start, m.t32, m.k_mid).per_op_ms;
  m.traj_strided_ms_T32_kall = sqzc3d_bench::bench_traj_strided_ms(v, start, m.t32, m.k_all).per_op_ms;
  m.traj_strided_ms_T256_k32 = sqzc3d_bench::bench_traj_strided_ms(v, start, m.t256, m.k_mid).per_op_ms;
  m.traj_strided_ms_T256_kall = sqzc3d_bench::bench_traj_strided_ms(v, start, m.t256, m.k_all).per_op_ms;
  m.traj_strided_ms_Tfull_k1 = sqzc3d_bench::bench_traj_strided_ms(v, start, n_frames, m.k_small).per_op_ms;

  m.reorder_ms_T32_k32 =
      sqzc3d_bench::bench_reorder_frame_to_point_major_ms(v, start, m.t32, m.k_mid, &point_major).per_op_ms;
  m.traj_contig_ms_T32_k32 = sqzc3d_bench::bench_traj_contig_point_major_ms(point_major.data(), m.t32, m.k_mid).per_op_ms;
  m.reorder_ms_T32_kall =
      sqzc3d_bench::bench_reorder_frame_to_point_major_ms(v, start, m.t32, m.k_all, &point_major).per_op_ms;
  m.traj_contig_ms_T32_kall = sqzc3d_bench::bench_traj_contig_point_major_ms(point_major.data(), m.t32, m.k_all).per_op_ms;
  m.reorder_ms_T256_k32 =
      sqzc3d_bench::bench_reorder_frame_to_point_major_ms(v, start, m.t256, m.k_mid, &point_major).per_op_ms;
  m.traj_contig_ms_T256_k32 =
      sqzc3d_bench::bench_traj_contig_point_major_ms(point_major.data(), m.t256, m.k_mid).per_op_ms;
  m.reorder_ms_T256_kall =
      sqzc3d_bench::bench_reorder_frame_to_point_major_ms(v, start, m.t256, m.k_all, &point_major).per_op_ms;
  m.traj_contig_ms_T256_kall =
      sqzc3d_bench::bench_traj_contig_point_major_ms(point_major.data(), m.t256, m.k_all).per_op_ms;

  if (chunk->point_labels && m.k_mid > 0) {
    std::vector<const char*> query(static_cast<std::size_t>(m.k_mid));
    for (int i = 0; i < m.k_mid; ++i) {
      query[static_cast<std::size_t>(i)] = chunk->point_labels[static_cast<std::size_t>(i)];
    }
    if (m.k_mid >= 3) {
      query[static_cast<std::size_t>(m.k_mid - 1)] = "__missing__";
      query[static_cast<std::size_t>(m.k_mid - 2)] = query[0];
    }
    std::vector<int> out_idx(static_cast<std::size_t>(m.k_mid), -1);
    const auto map_res = sqzc3d_bench::time_ms_adaptive(
        [&](int) -> double {
          (void)sqzc3d_point_indices_for_labels(
              chunk, query.data(), m.k_mid, out_idx.data(), /*miss_idx=*/-1);
          return static_cast<double>(out_idx[0]);
        },
        /*target_total_ms=*/50.0,
        /*max_iters=*/1 << 16);
    int miss = 0;
    for (const int idx : out_idx) miss += (idx < 0) ? 1 : 0;
    m.label_map_ms_k32 = map_res.per_op_ms;
    m.label_map_miss_k32 = miss;
  }

  const int group_i = largest_type_group_index(chunk);
  std::vector<unsigned char> allow_mask(static_cast<std::size_t>(n_points), 0u);
  if (group_i >= 0 && chunk->type_group_indices && chunk->type_group_starts) {
    m.sel_group_name = chunk->type_group_names && chunk->type_group_names[group_i] ? chunk->type_group_names[group_i]
                                                                                  : "-";
    const int s = chunk->type_group_starts[static_cast<std::size_t>(group_i)];
    const int e = chunk->type_group_starts[static_cast<std::size_t>(group_i) + 1u];
    for (int p = s; p < e; ++p) {
      const int idx = chunk->type_group_indices[static_cast<std::size_t>(p)];
      if (idx >= 0 && idx < n_points && !allow_mask[static_cast<std::size_t>(idx)]) {
        allow_mask[static_cast<std::size_t>(idx)] = 1u;
        ++m.sel_group_n;
      }
    }
  }
  const std::vector<int> base_sel = sqzc3d_bench::make_strided_selection_indices(n_points, m.k_mid, /*stride=*/7);
  m.sel_base_k = static_cast<int>(base_sel.size());
  const std::vector<int> filt_sel =
      (m.sel_group_n > 0) ? sqzc3d_bench::filter_indices_by_mask(base_sel, allow_mask.data(), n_points)
                          : std::vector<int>();
  m.sel_filtered_k = (m.sel_group_n > 0 && !filt_sel.empty()) ? static_cast<int>(filt_sel.size())
                                                              : static_cast<int>(base_sel.size());
  if (m.sel_filtered_k > 0 && m.t256 > 0) {
    sel_scratch.reserve(base_sel.size());
    sel_buf.assign(static_cast<std::size_t>(m.t256) * static_cast<std::size_t>(m.sel_filtered_k) * 3u, 0.0);
    const auto sel_res = sqzc3d_bench::time_ms_adaptive(
        [&](int it) -> double {
          const int t = m.t256;
          const int s0 = (n_frames > t) ? (it % (n_frames - t + 1)) : 0;
          const int* sel = base_sel.data();
          int n_sel = static_cast<int>(base_sel.size());

          sel_scratch.clear();
          if (m.sel_group_n > 0) {
            for (const int idx : base_sel) {
              if (idx >= 0 && idx < n_points && allow_mask[static_cast<std::size_t>(idx)]) sel_scratch.push_back(idx);
            }
            if (!sel_scratch.empty()) {
              sel = sel_scratch.data();
              n_sel = static_cast<int>(sel_scratch.size());
            }
          }

          for (int f = 0; f < t; ++f) {
            const double* frame = v.xyz + static_cast<std::size_t>(s0 + f) * static_cast<std::size_t>(n_points) * 3u;
            double* dst = sel_buf.data() + static_cast<std::size_t>(f) * static_cast<std::size_t>(n_sel) * 3u;
            for (int j = 0; j < n_sel; ++j) {
              const int p = sel[j];
              const std::size_t src = static_cast<std::size_t>(p) * 3u;
              const std::size_t out_o = static_cast<std::size_t>(j) * 3u;
              dst[out_o] = frame[src];
              dst[out_o + 1u] = frame[src + 1u];
              dst[out_o + 2u] = frame[src + 2u];
            }
          }
          return sel_buf[static_cast<std::size_t>(it % (t * n_sel)) * 3u];
        },
        /*target_total_ms=*/100.0,
        /*max_iters=*/1 << 10);
    m.sel_apply_ms_T256_k32 = sel_res.per_op_ms;
  }
  return m;
}

static void print_access_metrics(const AccessMetrics& m) {
  std::cout << "k_small=" << m.k_small << "\n";
  std::cout << "k_mid=" << m.k_mid << "\n";
  std::cout << "k_all=" << m.k_all << "\n";
  std::cout << "T32=" << m.t32 << "\n";
  std::cout << "T256=" << m.t256 << "\n";

  std::cout << "frame_view_ns_k1=" << m.frame_view_ns_k1 << "\n";
  std::cout << "frame_view_ns_k32=" << m.frame_view_ns_k32 << "\n";
  std::cout << "frame_view_ns_kall=" << m.frame_view_ns_kall << "\n";

  std::cout << "frame_copy_ms_k1=" << m.frame_copy_ms_k1 << "\n";
  std::cout << "frame_copy_ms_k32=" << m.frame_copy_ms_k32 << "\n";
  std::cout << "frame_copy_ms_kall=" << m.frame_copy_ms_kall << "\n";

  std::cout << "window_read_ms_T32_k32=" << m.window_read_ms_T32_k32 << "\n";
  std::cout << "window_read_ms_T32_kall=" << m.window_read_ms_T32_kall << "\n";
  std::cout << "window_read_ms_T256_k32=" << m.window_read_ms_T256_k32 << "\n";
  std::cout << "window_read_ms_T256_kall=" << m.window_read_ms_T256_kall << "\n";

  std::cout << "traj_strided_ms_T32_k32=" << m.traj_strided_ms_T32_k32 << "\n";
  std::cout << "traj_strided_ms_T32_kall=" << m.traj_strided_ms_T32_kall << "\n";
  std::cout << "traj_strided_ms_T256_k32=" << m.traj_strided_ms_T256_k32 << "\n";
  std::cout << "traj_strided_ms_T256_kall=" << m.traj_strided_ms_T256_kall << "\n";
  std::cout << "traj_strided_ms_Tfull_k1=" << m.traj_strided_ms_Tfull_k1 << "\n";

  std::cout << "reorder_ms_T32_k32=" << m.reorder_ms_T32_k32 << "\n";
  std::cout << "traj_contig_ms_T32_k32=" << m.traj_contig_ms_T32_k32 << "\n";
  std::cout << "reorder_ms_T32_kall=" << m.reorder_ms_T32_kall << "\n";
  std::cout << "traj_contig_ms_T32_kall=" << m.traj_contig_ms_T32_kall << "\n";
  std::cout << "reorder_ms_T256_k32=" << m.reorder_ms_T256_k32 << "\n";
  std::cout << "traj_contig_ms_T256_k32=" << m.traj_contig_ms_T256_k32 << "\n";
  std::cout << "reorder_ms_T256_kall=" << m.reorder_ms_T256_kall << "\n";
  std::cout << "traj_contig_ms_T256_kall=" << m.traj_contig_ms_T256_kall << "\n";

  if (m.label_map_ms_k32 > 0.0 || m.label_map_miss_k32 > 0) {
    std::cout << "label_map_ms_k32=" << m.label_map_ms_k32 << "\n";
    std::cout << "label_map_miss_k32=" << m.label_map_miss_k32 << "\n";
  }

  std::cout << "sel_group_name=" << m.sel_group_name << "\n";
  std::cout << "sel_group_n=" << m.sel_group_n << "\n";
  std::cout << "sel_base_k=" << m.sel_base_k << "\n";
  std::cout << "sel_filtered_k=" << m.sel_filtered_k << "\n";
  if (m.sel_apply_ms_T256_k32 > 0.0) {
    std::cout << "sel_apply_ms_T256_k32=" << m.sel_apply_ms_T256_k32 << "\n";
  }
}

static int run_bench(const std::string& input, int repeat) {
  if (repeat < 1) repeat = 1;
  const MemSnapshot mem0 = snapshot_memory();
  MemSnapshot mem1 = mem0;

  double sum_load = 0.0;
  int last_frames = 0;
  int last_points = 0;
  std::int64_t last_file_bytes = 0;
  AccessMetrics metrics{};
  volatile double checksum = 0.0;

  for (int i = 0; i < repeat; ++i) {
    LoadResult r{};
    if (is_c3d_path(input)) {
      r = load_sqzc3d_c3d_materialize(input);
    } else {
      r = load_sqzc3d_bundle(input);
    }
    if (r.status != sqzc3d_STATUS_SUCCESS || !r.chunk) {
      std::cerr << "load failed: status=" << r.status << "\n";
      if (r.chunk) sqzc3d_free_chunk(r.chunk);
      return 2;
    }
    sum_load += r.load_ms;
    last_file_bytes = r.file_bytes;
    last_frames = r.chunk->n_frames;
    last_points = r.chunk->n_points;
    if (i == 0) {
      metrics = compute_access_metrics(r.chunk);
    }

    if (r.chunk->points_xyz) {
      const int scalar = r.chunk->n_scalar;
      const int n = std::min(scalar, 256);
      for (int j = 0; j < n; ++j) {
        const double x = r.chunk->points_xyz[j];
        checksum += (x == x) ? x : 0.0;
      }
    }
    sqzc3d_free_chunk(r.chunk);
    mem1 = snapshot_memory();
  }

  std::cout << std::fixed << std::setprecision(6);
  std::cout << "lib=sqzc3d\n";
  std::cout << "mode=materialize\n";
  std::cout << "repeat=" << repeat << "\n";
  std::cout << "input=" << input << "\n";
  std::cout << "frames=" << last_frames << "\n";
  std::cout << "points=" << last_points << "\n";
  std::cout << "file_bytes=" << last_file_bytes << "\n";
  std::cout << "rss_baseline_mb=" << mem0.rss_mb << "\n";
  std::cout << "peak_rss_mb=" << mem1.peak_rss_mb << "\n";
  std::cout << "peak_rss_delta_mb=" << (mem1.peak_rss_mb - mem0.rss_mb) << "\n";
  std::cout << "load_ms=" << (sum_load / static_cast<double>(repeat)) << "\n";
  print_access_metrics(metrics);
  std::cout << "checksum=" << checksum << "\n";
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "usage: bench_sqzc3d <path-to-c3d-or-bundle> [repeat=1]\n";
    return 1;
  }

  const std::string input = argv[1];
  int repeat = 1;
  if (argc >= 3) {
    try {
      repeat = std::stoi(argv[2]);
      if (repeat < 1 || repeat > 50) {
        std::cerr << "repeat must be in [1, 50]\n";
        return 1;
      }
    } catch (...) {
      repeat = 1;
    }
  }

  if (!is_c3d_path(input) && !(is_bundle_path(input) || std::filesystem::is_directory(input))) {
    std::cerr << "unsupported input extension. expected .c3d or .sqzc3d/.sqzc3D / bundle directory\n";
    return 1;
  }

  return run_bench(input, repeat);
}
