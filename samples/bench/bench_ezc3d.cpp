#include <ezc3d/ezc3d_all.h>

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
#include <unordered_map>
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

struct TypeGroups {
  std::vector<std::string> names;
  std::vector<int> starts;
  std::vector<int> indices;
};

static std::vector<std::string> load_point_labels(const ezc3d::c3d& c3d) {
  std::vector<std::string> labels;
  const auto& params = c3d.parameters();
  if (!params.isGroup("POINT")) return labels;
  const auto& g = params.group("POINT");
  if (!g.isParameter("LABELS")) return labels;
  const auto& p = g.parameter("LABELS");
  if (p.type() != ezc3d::DATA_TYPE::CHAR) return labels;
  const auto v = p.valuesAsString();
  labels.assign(v.begin(), v.end());
  return labels;
}

static TypeGroups load_point_type_groups(const ezc3d::c3d& c3d, const std::vector<std::string>& point_labels) {
  TypeGroups out;
  out.starts.push_back(0);
  const auto& params = c3d.parameters();
  if (!params.isGroup("POINT")) return out;
  const auto& g = params.group("POINT");
  if (!g.isParameter("TYPE_GROUPS")) return out;
  const auto& p = g.parameter("TYPE_GROUPS");
  if (p.type() != ezc3d::DATA_TYPE::CHAR) return out;
  const auto groups = p.valuesAsString();
  if (groups.empty()) return out;

  std::unordered_map<std::string, int> label_to_idx;
  label_to_idx.reserve(point_labels.size() * 2u + 1u);
  for (int i = 0; i < static_cast<int>(point_labels.size()); ++i) {
    label_to_idx[point_labels[static_cast<std::size_t>(i)]] = i;
  }

  for (const auto& raw_name : groups) {
    const std::string gname = raw_name;
    if (gname.empty()) continue;
    out.names.push_back(gname);
    if (!g.isParameter(gname.c_str())) {
      out.starts.push_back(static_cast<int>(out.indices.size()));
      continue;
    }
    const auto& gp = g.parameter(gname.c_str());
    if (gp.type() == ezc3d::DATA_TYPE::CHAR) {
      const auto labels = gp.valuesAsString();
      for (const auto& label : labels) {
        const auto it = label_to_idx.find(label);
        if (it != label_to_idx.end()) out.indices.push_back(it->second);
      }
    }
    out.starts.push_back(static_cast<int>(out.indices.size()));
  }
  return out;
}

static int largest_type_group_index(const TypeGroups& tg) {
  if (tg.names.empty() || tg.starts.size() != tg.names.size() + 1u) return -1;
  int best = -1;
  int best_n = -1;
  for (int i = 0; i < static_cast<int>(tg.names.size()); ++i) {
    const int s = tg.starts[static_cast<std::size_t>(i)];
    const int e = tg.starts[static_cast<std::size_t>(i) + 1u];
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

static double bench_frame_view_ns_ezc3d(const ezc3d::DataNS::Data& data, int n_frames, int n_points, int k_points) {
  if (n_frames <= 0 || n_points <= 0) return 0.0;
  const int k = std::max(1, std::min(k_points, n_points));
  const auto& frames = data.frames();

  int iters = 1;
  double total_ns = 0.0;
  volatile double checksum = 0.0;
  while (true) {
    checksum = 0.0;
    const auto t0 = sqzc3d_bench::Clock::now();
    for (int i = 0; i < iters; ++i) {
      const int f = (n_frames > 1) ? ((i * 1315423911u) % static_cast<unsigned>(n_frames)) : 0;
      const auto& pts = frames[static_cast<std::size_t>(f)].points().points();
      const int p = i % k;
      checksum += pts[static_cast<std::size_t>(p)].x();
    }
    const auto t1 = sqzc3d_bench::Clock::now();
    total_ns = sqzc3d_bench::elapsed_ns(t0, t1);
    if (total_ns >= 1e8 || iters >= (1 << 27)) break;  // target ~100ms
    iters *= 2;
  }
  (void)checksum;
  return (iters > 0) ? (total_ns / static_cast<double>(iters)) : 0.0;
}

static sqzc3d_bench::TimedResult bench_frame_copy_ms_ezc3d(
    const ezc3d::DataNS::Data& data,
    int n_frames,
    int n_points,
    int frame_idx,
    int k_points,
    std::vector<double>* out_buf) {
  sqzc3d_bench::TimedResult out{};
  if (n_frames <= 0 || n_points <= 0 || !out_buf) return out;
  const int k = std::max(0, std::min(k_points, n_points));
  if (k <= 0) return out;
  const auto& frames = data.frames();

  out_buf->assign(static_cast<std::size_t>(k) * 3u, 0.0);
  const int f0 = std::max(0, std::min(frame_idx, n_frames - 1));

  out = sqzc3d_bench::time_ms_adaptive(
      [&](int i) -> double {
        const int f = (n_frames > 1) ? ((f0 + i) % n_frames) : f0;
        const auto& pts = frames[static_cast<std::size_t>(f)].points().points();
        for (int p = 0; p < k; ++p) {
          const auto& pt = pts[static_cast<std::size_t>(p)];
          const std::size_t o = static_cast<std::size_t>(p) * 3u;
          (*out_buf)[o] = pt.x();
          (*out_buf)[o + 1u] = pt.y();
          (*out_buf)[o + 2u] = pt.z();
        }
        return (*out_buf)[static_cast<std::size_t>((i % k) * 3)];
      },
      /*target_total_ms=*/50.0,
      /*max_iters=*/1 << 16);
  return out;
}

static sqzc3d_bench::TimedResult bench_window_copy_ms_ezc3d(
    const ezc3d::DataNS::Data& data,
    int n_frames,
    int n_points,
    int start_frame,
    int window_frames,
    int k_points,
    std::vector<double>* out_buf) {
  sqzc3d_bench::TimedResult out{};
  if (n_frames <= 0 || n_points <= 0 || !out_buf) return out;
  const int k = std::max(0, std::min(k_points, n_points));
  const int t = std::max(0, std::min(window_frames, n_frames));
  if (k <= 0 || t <= 0) return out;
  const auto& frames = data.frames();

  out_buf->assign(static_cast<std::size_t>(t) * static_cast<std::size_t>(k) * 3u, 0.0);
  const int s0 = std::max(0, std::min(start_frame, n_frames - t));

  out = sqzc3d_bench::time_ms_adaptive(
      [&](int i) -> double {
        const int s = (n_frames > t) ? ((s0 + i) % (n_frames - t + 1)) : s0;
        for (int f = 0; f < t; ++f) {
          const auto& pts = frames[static_cast<std::size_t>(s + f)].points().points();
          double* dst = out_buf->data() + static_cast<std::size_t>(f) * static_cast<std::size_t>(k) * 3u;
          for (int p = 0; p < k; ++p) {
            const auto& pt = pts[static_cast<std::size_t>(p)];
            const std::size_t o = static_cast<std::size_t>(p) * 3u;
            dst[o] = pt.x();
            dst[o + 1u] = pt.y();
            dst[o + 2u] = pt.z();
          }
        }
        return (*out_buf)[static_cast<std::size_t>((i % (t * k)) * 3u)];
      },
      /*target_total_ms=*/100.0,
      /*max_iters=*/1 << 12);
  return out;
}

static sqzc3d_bench::TimedResult bench_traj_strided_ms_ezc3d(
    const ezc3d::DataNS::Data& data,
    int n_frames,
    int n_points,
    int start_frame,
    int window_frames,
    int k_points) {
  sqzc3d_bench::TimedResult out{};
  if (n_frames <= 0 || n_points <= 0) return out;
  const int k = std::max(0, std::min(k_points, n_points));
  const int t = std::max(0, std::min(window_frames, n_frames));
  if (k <= 0 || t <= 0) return out;
  const auto& frames = data.frames();

  const int s0 = std::max(0, std::min(start_frame, n_frames - t));
  out = sqzc3d_bench::time_ms_adaptive(
      [&](int i) -> double {
        const int s = (n_frames > t) ? ((s0 + i) % (n_frames - t + 1)) : s0;
        volatile double checksum = 0.0;
        for (int p = 0; p < k; ++p) {
          for (int f = 0; f < t; ++f) {
            const auto& pts = frames[static_cast<std::size_t>(s + f)].points().points();
            checksum += pts[static_cast<std::size_t>(p)].x();
          }
        }
        return checksum;
      },
      /*target_total_ms=*/100.0,
      /*max_iters=*/1 << 10);
  return out;
}

static sqzc3d_bench::TimedResult bench_reorder_frame_to_point_major_ms_ezc3d(
    const ezc3d::DataNS::Data& data,
    int n_frames,
    int n_points,
    int start_frame,
    int window_frames,
    int k_points,
    std::vector<double>* out_point_major) {
  sqzc3d_bench::TimedResult out{};
  if (n_frames <= 0 || n_points <= 0 || !out_point_major) return out;
  const int k = std::max(0, std::min(k_points, n_points));
  const int t = std::max(0, std::min(window_frames, n_frames));
  if (k <= 0 || t <= 0) return out;
  const auto& frames = data.frames();

  const int s0 = std::max(0, std::min(start_frame, n_frames - t));
  out_point_major->assign(static_cast<std::size_t>(k) * static_cast<std::size_t>(t) * 3u, 0.0);
  out = sqzc3d_bench::time_ms_adaptive(
      [&](int i) -> double {
        const int s = (n_frames > t) ? ((s0 + i) % (n_frames - t + 1)) : s0;
        for (int f = 0; f < t; ++f) {
          const auto& pts = frames[static_cast<std::size_t>(s + f)].points().points();
          for (int p = 0; p < k; ++p) {
            const auto& pt = pts[static_cast<std::size_t>(p)];
            const std::size_t dst = (static_cast<std::size_t>(p) * static_cast<std::size_t>(t) +
                                     static_cast<std::size_t>(f)) *
                                    3u;
            (*out_point_major)[dst] = pt.x();
            (*out_point_major)[dst + 1u] = pt.y();
            (*out_point_major)[dst + 2u] = pt.z();
          }
        }
        return (*out_point_major)[static_cast<std::size_t>(i % (k * t)) * 3u];
      },
      /*target_total_ms=*/100.0,
      /*max_iters=*/1 << 10);
  return out;
}

static sqzc3d_bench::TimedResult bench_sel_apply_ms_ezc3d(
    const ezc3d::DataNS::Data& data,
    int n_frames,
    int n_points,
    int start_frame,
    int window_frames,
    const int* sel_indices,
    int n_sel,
    std::vector<double>* out_buf) {
  sqzc3d_bench::TimedResult out{};
  if (n_frames <= 0 || n_points <= 0 || !sel_indices || n_sel <= 0 || !out_buf) return out;
  const int t = std::max(0, std::min(window_frames, n_frames));
  if (t <= 0) return out;
  const auto& frames = data.frames();

  out_buf->assign(static_cast<std::size_t>(t) * static_cast<std::size_t>(n_sel) * 3u, 0.0);
  const int s0 = std::max(0, std::min(start_frame, n_frames - t));
  out = sqzc3d_bench::time_ms_adaptive(
      [&](int i) -> double {
        const int s = (n_frames > t) ? ((s0 + i) % (n_frames - t + 1)) : s0;
        for (int f = 0; f < t; ++f) {
          const auto& pts = frames[static_cast<std::size_t>(s + f)].points().points();
          double* dst = out_buf->data() + static_cast<std::size_t>(f) * static_cast<std::size_t>(n_sel) * 3u;
          for (int j = 0; j < n_sel; ++j) {
            const int p = sel_indices[j];
            const auto& pt = pts[static_cast<std::size_t>(p)];
            const std::size_t o = static_cast<std::size_t>(j) * 3u;
            dst[o] = pt.x();
            dst[o + 1u] = pt.y();
            dst[o + 2u] = pt.z();
          }
        }
        return (*out_buf)[static_cast<std::size_t>(i % (t * n_sel)) * 3u];
      },
      /*target_total_ms=*/100.0,
      /*max_iters=*/1 << 10);
  return out;
}

static AccessMetrics compute_access_metrics_native(
    const ezc3d::c3d& c3d,
    const std::vector<std::string>& point_labels,
    const TypeGroups& type_groups) {
  AccessMetrics m{};
  const int n_frames = static_cast<int>(c3d.data().nbFrames());
  const int n_points = static_cast<int>(c3d.header().nb3dPoints());
  if (n_frames <= 0 || n_points <= 0) return m;

  m.k_small = 1;
  m.k_mid = std::min(32, n_points);
  m.k_all = n_points;
  m.t32 = std::min(32, n_frames);
  m.t256 = std::min(256, n_frames);
  const int start = 0;

  const auto& data = c3d.data();

  std::vector<double> tmp;
  std::vector<double> win;
  std::vector<double> point_major;
  std::vector<double> sel_buf;

  m.frame_view_ns_k1 = bench_frame_view_ns_ezc3d(data, n_frames, n_points, m.k_small);
  m.frame_view_ns_k32 = bench_frame_view_ns_ezc3d(data, n_frames, n_points, m.k_mid);
  m.frame_view_ns_kall = bench_frame_view_ns_ezc3d(data, n_frames, n_points, m.k_all);

  m.frame_copy_ms_k1 = bench_frame_copy_ms_ezc3d(data, n_frames, n_points, 0, m.k_small, &tmp).per_op_ms;
  m.frame_copy_ms_k32 = bench_frame_copy_ms_ezc3d(data, n_frames, n_points, 0, m.k_mid, &tmp).per_op_ms;
  m.frame_copy_ms_kall = bench_frame_copy_ms_ezc3d(data, n_frames, n_points, 0, m.k_all, &tmp).per_op_ms;

  m.window_read_ms_T32_k32 =
      bench_window_copy_ms_ezc3d(data, n_frames, n_points, start, m.t32, m.k_mid, &win).per_op_ms;
  m.window_read_ms_T32_kall =
      bench_window_copy_ms_ezc3d(data, n_frames, n_points, start, m.t32, m.k_all, &win).per_op_ms;
  m.window_read_ms_T256_k32 =
      bench_window_copy_ms_ezc3d(data, n_frames, n_points, start, m.t256, m.k_mid, &win).per_op_ms;
  m.window_read_ms_T256_kall =
      bench_window_copy_ms_ezc3d(data, n_frames, n_points, start, m.t256, m.k_all, &win).per_op_ms;

  m.traj_strided_ms_T32_k32 =
      bench_traj_strided_ms_ezc3d(data, n_frames, n_points, start, m.t32, m.k_mid).per_op_ms;
  m.traj_strided_ms_T32_kall =
      bench_traj_strided_ms_ezc3d(data, n_frames, n_points, start, m.t32, m.k_all).per_op_ms;
  m.traj_strided_ms_T256_k32 =
      bench_traj_strided_ms_ezc3d(data, n_frames, n_points, start, m.t256, m.k_mid).per_op_ms;
  m.traj_strided_ms_T256_kall =
      bench_traj_strided_ms_ezc3d(data, n_frames, n_points, start, m.t256, m.k_all).per_op_ms;
  m.traj_strided_ms_Tfull_k1 =
      bench_traj_strided_ms_ezc3d(data, n_frames, n_points, start, n_frames, m.k_small).per_op_ms;

  m.reorder_ms_T32_k32 =
      bench_reorder_frame_to_point_major_ms_ezc3d(data, n_frames, n_points, start, m.t32, m.k_mid, &point_major)
          .per_op_ms;
  m.traj_contig_ms_T32_k32 =
      sqzc3d_bench::bench_traj_contig_point_major_ms(point_major.data(), m.t32, m.k_mid).per_op_ms;
  m.reorder_ms_T32_kall =
      bench_reorder_frame_to_point_major_ms_ezc3d(data, n_frames, n_points, start, m.t32, m.k_all, &point_major)
          .per_op_ms;
  m.traj_contig_ms_T32_kall =
      sqzc3d_bench::bench_traj_contig_point_major_ms(point_major.data(), m.t32, m.k_all).per_op_ms;
  m.reorder_ms_T256_k32 =
      bench_reorder_frame_to_point_major_ms_ezc3d(data, n_frames, n_points, start, m.t256, m.k_mid, &point_major)
          .per_op_ms;
  m.traj_contig_ms_T256_k32 =
      sqzc3d_bench::bench_traj_contig_point_major_ms(point_major.data(), m.t256, m.k_mid).per_op_ms;
  m.reorder_ms_T256_kall =
      bench_reorder_frame_to_point_major_ms_ezc3d(data, n_frames, n_points, start, m.t256, m.k_all, &point_major)
          .per_op_ms;
  m.traj_contig_ms_T256_kall =
      sqzc3d_bench::bench_traj_contig_point_major_ms(point_major.data(), m.t256, m.k_all).per_op_ms;

  if (!point_labels.empty() && m.k_mid > 0) {
    std::vector<std::string_view> query(static_cast<std::size_t>(m.k_mid));
    for (int i = 0; i < m.k_mid; ++i) query[static_cast<std::size_t>(i)] = point_labels[static_cast<std::size_t>(i)];
    if (m.k_mid >= 3) {
      query[static_cast<std::size_t>(m.k_mid - 1)] = "__missing__";
      query[static_cast<std::size_t>(m.k_mid - 2)] = query[0];
    }
    std::vector<int> out_idx(static_cast<std::size_t>(m.k_mid), -1);
    std::unordered_map<std::string_view, int> label_to_idx;
    label_to_idx.reserve(point_labels.size() * 2u + 1u);
    for (int i = 0; i < static_cast<int>(point_labels.size()); ++i) {
      label_to_idx[point_labels[static_cast<std::size_t>(i)]] = i;
    }
    const auto map_res = sqzc3d_bench::time_ms_adaptive(
        [&](int) -> double {
          for (int i = 0; i < m.k_mid; ++i) {
            const auto it = label_to_idx.find(query[static_cast<std::size_t>(i)]);
            out_idx[static_cast<std::size_t>(i)] = (it == label_to_idx.end()) ? -1 : it->second;
          }
          return static_cast<double>(out_idx[0]);
        },
        /*target_total_ms=*/50.0,
        /*max_iters=*/1 << 14);
    int miss = 0;
    for (const int idx : out_idx) miss += (idx < 0) ? 1 : 0;
    m.label_map_ms_k32 = map_res.per_op_ms;
    m.label_map_miss_k32 = miss;
  }

  const int group_i = largest_type_group_index(type_groups);
  std::vector<unsigned char> allow_mask(static_cast<std::size_t>(n_points), 0u);
  if (group_i >= 0) {
    m.sel_group_name = type_groups.names[static_cast<std::size_t>(group_i)];
    const int s = type_groups.starts[static_cast<std::size_t>(group_i)];
    const int e = type_groups.starts[static_cast<std::size_t>(group_i) + 1u];
    for (int p = s; p < e; ++p) {
      const int idx = type_groups.indices[static_cast<std::size_t>(p)];
      if (idx >= 0 && idx < n_points && !allow_mask[static_cast<std::size_t>(idx)]) {
        allow_mask[static_cast<std::size_t>(idx)] = 1u;
        ++m.sel_group_n;
      }
    }
  }
  const std::vector<int> base_sel = sqzc3d_bench::make_strided_selection_indices(n_points, m.k_mid, /*stride=*/7);
  m.sel_base_k = static_cast<int>(base_sel.size());
  const std::vector<int> filt_sel =
      (m.sel_group_n > 0) ? sqzc3d_bench::filter_indices_by_mask(base_sel, allow_mask.data(), n_points) : base_sel;
  m.sel_filtered_k = static_cast<int>(filt_sel.size());
  if (!filt_sel.empty()) {
    m.sel_apply_ms_T256_k32 =
        bench_sel_apply_ms_ezc3d(data, n_frames, n_points, start, m.t256, filt_sel.data(),
                                 static_cast<int>(filt_sel.size()), &sel_buf)
            .per_op_ms;
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

  std::error_code ec;
  const std::int64_t file_bytes = static_cast<std::int64_t>(std::filesystem::file_size(input, ec));
  const std::int64_t safe_file_bytes = ec ? 0 : file_bytes;

  const MemSnapshot mem0 = snapshot_memory();
  MemSnapshot mem1 = mem0;

  double sum_load = 0.0;
  int last_frames = 0;
  int last_points = 0;
  AccessMetrics metrics{};
  volatile double checksum = 0.0;

  for (int i = 0; i < repeat; ++i) {
    try {
      const auto t0 = sqzc3d_bench::Clock::now();
      ezc3d::c3d c3d(input);
      const auto point_labels = load_point_labels(c3d);
      const auto type_groups = load_point_type_groups(c3d, point_labels);
      const auto t1 = sqzc3d_bench::Clock::now();
      sum_load += sqzc3d_bench::elapsed_ms(t0, t1);

      last_frames = static_cast<int>(c3d.data().nbFrames());
      last_points = static_cast<int>(c3d.header().nb3dPoints());
      if (i == 0) {
        metrics = compute_access_metrics_native(c3d, point_labels, type_groups);
      }

      if (last_frames > 0 && last_points > 0) {
        const auto& frames = c3d.data().frames();
        const auto& pts = frames[0].points().points();
        if (!pts.empty()) checksum += pts[0].x();
      }
    } catch (const std::exception& e) {
      std::cerr << "load failed: " << e.what() << "\n";
      return 2;
    }
    mem1 = snapshot_memory();
  }

  std::cout << std::fixed << std::setprecision(6);
  std::cout << "lib=ezc3d\n";
  std::cout << "mode=materialize\n";
  std::cout << "repeat=" << repeat << "\n";
  std::cout << "input=" << input << "\n";
  std::cout << "frames=" << last_frames << "\n";
  std::cout << "points=" << last_points << "\n";
  std::cout << "file_bytes=" << safe_file_bytes << "\n";
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
    std::cerr << "usage: bench_ezc3d <path-to-c3d> [repeat=1]\n";
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
