#ifndef SQZC3D_BENCH_ACCESS_PATTERNS_H_
#define SQZC3D_BENCH_ACCESS_PATTERNS_H_

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace sqzc3d_bench {

using Clock = std::chrono::high_resolution_clock;

inline double elapsed_ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
}

inline double elapsed_ns(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(end - start).count();
}

struct PointsFrameMajorView {
  const double* xyz = nullptr;               // [frame][point][3]
  const unsigned char* valid = nullptr;      // [frame][point]
  int n_frames = 0;
  int n_points = 0;                          // points per frame in source (stride)
};

struct TimedResult {
  double total_ms = 0.0;
  double per_op_ms = 0.0;
  int iters = 0;
  volatile double checksum = 0.0;
};

template <class Fn>
inline TimedResult time_ms_adaptive(Fn fn, double target_total_ms, int max_iters) {
  TimedResult out{};
  if (target_total_ms <= 0.0) target_total_ms = 50.0;
  if (max_iters < 1) max_iters = 1;

  int iters = 1;
  while (true) {
    volatile double checksum = 0.0;
    const auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
      checksum += fn(i);
    }
    const auto t1 = Clock::now();
    const double total_ms = elapsed_ms(t0, t1);
    if (total_ms >= target_total_ms || iters >= max_iters) {
      out.total_ms = total_ms;
      out.per_op_ms = (iters > 0) ? (total_ms / static_cast<double>(iters)) : 0.0;
      out.iters = iters;
      out.checksum = checksum;
      return out;
    }
    if (iters > (std::numeric_limits<int>::max() / 2)) {
      out.total_ms = total_ms;
      out.per_op_ms = (iters > 0) ? (total_ms / static_cast<double>(iters)) : 0.0;
      out.iters = iters;
      out.checksum = checksum;
      return out;
    }
    iters *= 2;
    if (iters > max_iters) iters = max_iters;
  }
}

inline bool is_finite_xyz(double x, double y, double z) {
  const auto inf = std::numeric_limits<double>::infinity();
  return x == x && x != inf && x != -inf &&
         y == y && y != inf && y != -inf &&
         z == z && z != inf && z != -inf;
}

inline double bench_frame_view_ns(const PointsFrameMajorView& v, int k_points) {
  if (!v.xyz || v.n_frames <= 0 || v.n_points <= 0) return 0.0;
  const int k = std::max(1, std::min(k_points, v.n_points));
  int iters = 1;
  double total_ns = 0.0;
  volatile double checksum = 0.0;
  while (true) {
    checksum = 0.0;
    const auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
      const int f = (v.n_frames > 1) ? ((i * 1315423911u) % static_cast<unsigned>(v.n_frames)) : 0;
      const double* frame = v.xyz + static_cast<std::size_t>(f) * static_cast<std::size_t>(v.n_points) * 3u;
      checksum += frame[static_cast<std::size_t>((i % k) * 3)];
    }
    const auto t1 = Clock::now();
    total_ns = elapsed_ns(t0, t1);
    if (total_ns >= 1e8 || iters >= (1 << 27)) break;  // target ~100ms
    iters *= 2;
  }
  (void)checksum;
  return (iters > 0) ? (total_ns / static_cast<double>(iters)) : 0.0;
}

inline TimedResult bench_frame_copy_ms(
    const PointsFrameMajorView& v,
    int frame_idx,
    int k_points,
    std::vector<double>* out_buf) {
  TimedResult out{};
  if (!v.xyz || v.n_frames <= 0 || v.n_points <= 0 || !out_buf) return out;
  const int k = std::max(0, std::min(k_points, v.n_points));
  if (k <= 0) return out;

  out_buf->assign(static_cast<std::size_t>(k) * 3u, 0.0);
  const std::size_t bytes = static_cast<std::size_t>(k) * 3u * sizeof(double);
  const int f0 = std::max(0, std::min(frame_idx, v.n_frames - 1));

  out = time_ms_adaptive(
      [&](int i) -> double {
        const int f = (v.n_frames > 1) ? ((f0 + i) % v.n_frames) : f0;
        const double* src = v.xyz + static_cast<std::size_t>(f) * static_cast<std::size_t>(v.n_points) * 3u;
        std::memcpy(out_buf->data(), src, bytes);
        return (*out_buf)[static_cast<std::size_t>((i % k) * 3)];
      },
      /*target_total_ms=*/50.0,
      /*max_iters=*/1 << 20);
  return out;
}

inline TimedResult bench_window_copy_ms(
    const PointsFrameMajorView& v,
    int start_frame,
    int window_frames,
    int k_points,
    std::vector<double>* out_buf) {
  TimedResult out{};
  if (!v.xyz || v.n_frames <= 0 || v.n_points <= 0 || !out_buf) return out;
  const int k = std::max(0, std::min(k_points, v.n_points));
  const int t = std::max(0, std::min(window_frames, v.n_frames));
  if (k <= 0 || t <= 0) return out;

  out_buf->assign(static_cast<std::size_t>(t) * static_cast<std::size_t>(k) * 3u, 0.0);
  const std::size_t row_bytes = static_cast<std::size_t>(k) * 3u * sizeof(double);
  const int s0 = std::max(0, std::min(start_frame, v.n_frames - t));

  out = time_ms_adaptive(
      [&](int i) -> double {
        const int s = (v.n_frames > t) ? ((s0 + i) % (v.n_frames - t + 1)) : s0;
        if (k == v.n_points) {
          const std::size_t win_bytes = static_cast<std::size_t>(t) * row_bytes;
          const double* src = v.xyz + static_cast<std::size_t>(s) * static_cast<std::size_t>(v.n_points) * 3u;
          std::memcpy(out_buf->data(), src, win_bytes);
          return (*out_buf)[static_cast<std::size_t>(i % (t * k)) * 3u];
        }
        for (int f = 0; f < t; ++f) {
          const double* src = v.xyz + static_cast<std::size_t>(s + f) * static_cast<std::size_t>(v.n_points) * 3u;
          double* dst = out_buf->data() + static_cast<std::size_t>(f) * static_cast<std::size_t>(k) * 3u;
          std::memcpy(dst, src, row_bytes);
        }
        return (*out_buf)[static_cast<std::size_t>((i % k) * 3)];
      },
      /*target_total_ms=*/100.0,
      /*max_iters=*/1 << 16);
  return out;
}

inline TimedResult bench_traj_strided_ms(
    const PointsFrameMajorView& v,
    int start_frame,
    int window_frames,
    int k_points) {
  TimedResult out{};
  if (!v.xyz || v.n_frames <= 0 || v.n_points <= 0) return out;
  const int k = std::max(0, std::min(k_points, v.n_points));
  const int t = std::max(0, std::min(window_frames, v.n_frames));
  if (k <= 0 || t <= 0) return out;

  const int s0 = std::max(0, std::min(start_frame, v.n_frames - t));
  out = time_ms_adaptive(
      [&](int i) -> double {
        const int s = (v.n_frames > t) ? ((s0 + i) % (v.n_frames - t + 1)) : s0;
        volatile double checksum = 0.0;
        for (int p = 0; p < k; ++p) {
          for (int f = 0; f < t; ++f) {
            const double* src = v.xyz + static_cast<std::size_t>(s + f) * static_cast<std::size_t>(v.n_points) * 3u +
                                static_cast<std::size_t>(p) * 3u;
            checksum += src[0];
          }
        }
        return checksum;
      },
      /*target_total_ms=*/100.0,
      /*max_iters=*/1 << 10);
  return out;
}

inline TimedResult bench_reorder_frame_to_point_major_ms(
    const PointsFrameMajorView& v,
    int start_frame,
    int window_frames,
    int k_points,
    std::vector<double>* out_point_major) {
  TimedResult out{};
  if (!v.xyz || v.n_frames <= 0 || v.n_points <= 0 || !out_point_major) return out;
  const int k = std::max(0, std::min(k_points, v.n_points));
  const int t = std::max(0, std::min(window_frames, v.n_frames));
  if (k <= 0 || t <= 0) return out;
  const int s0 = std::max(0, std::min(start_frame, v.n_frames - t));

  out_point_major->assign(static_cast<std::size_t>(k) * static_cast<std::size_t>(t) * 3u, 0.0);
  out = time_ms_adaptive(
      [&](int i) -> double {
        const int s = (v.n_frames > t) ? ((s0 + i) % (v.n_frames - t + 1)) : s0;
        for (int f = 0; f < t; ++f) {
          const double* frame = v.xyz + static_cast<std::size_t>(s + f) * static_cast<std::size_t>(v.n_points) * 3u;
          for (int p = 0; p < k; ++p) {
            const std::size_t dst = (static_cast<std::size_t>(p) * static_cast<std::size_t>(t) +
                                     static_cast<std::size_t>(f)) *
                                    3u;
            const std::size_t src = static_cast<std::size_t>(p) * 3u;
            (*out_point_major)[dst] = frame[src];
            (*out_point_major)[dst + 1u] = frame[src + 1u];
            (*out_point_major)[dst + 2u] = frame[src + 2u];
          }
        }
        return (*out_point_major)[static_cast<std::size_t>(i % (k * t)) * 3u];
      },
      /*target_total_ms=*/100.0,
      /*max_iters=*/1 << 10);
  return out;
}

inline TimedResult bench_traj_contig_point_major_ms(
    const double* point_major_xyz,
    int window_frames,
    int k_points) {
  TimedResult out{};
  if (!point_major_xyz || window_frames <= 0 || k_points <= 0) return out;
  const int t = window_frames;
  const int k = k_points;
  out = time_ms_adaptive(
      [&](int i) -> double {
        volatile double checksum = 0.0;
        const int p0 = (k > 0) ? (i % k) : 0;
        for (int p = 0; p < k; ++p) {
          const double* traj = point_major_xyz + static_cast<std::size_t>(p) * static_cast<std::size_t>(t) * 3u;
          for (int f = 0; f < t; ++f) {
            checksum += traj[static_cast<std::size_t>(f) * 3u];
          }
        }
        return checksum + static_cast<double>(p0) * 1e-9;
      },
      /*target_total_ms=*/100.0,
      /*max_iters=*/1 << 10);
  return out;
}

inline TimedResult bench_sel_apply_ms(
    const PointsFrameMajorView& v,
    int start_frame,
    int window_frames,
    const int* sel_indices,
    int n_sel,
    std::vector<double>* out_buf) {
  TimedResult out{};
  if (!v.xyz || v.n_frames <= 0 || v.n_points <= 0 || !sel_indices || n_sel <= 0 || !out_buf) return out;
  const int t = std::max(0, std::min(window_frames, v.n_frames));
  if (t <= 0) return out;

  out_buf->assign(static_cast<std::size_t>(t) * static_cast<std::size_t>(n_sel) * 3u, 0.0);
  const int s0 = std::max(0, std::min(start_frame, v.n_frames - t));
  out = time_ms_adaptive(
      [&](int i) -> double {
        const int s = (v.n_frames > t) ? ((s0 + i) % (v.n_frames - t + 1)) : s0;
        for (int f = 0; f < t; ++f) {
          const double* frame = v.xyz + static_cast<std::size_t>(s + f) * static_cast<std::size_t>(v.n_points) * 3u;
          double* dst = out_buf->data() + static_cast<std::size_t>(f) * static_cast<std::size_t>(n_sel) * 3u;
          for (int j = 0; j < n_sel; ++j) {
            const int p = sel_indices[j];
            const std::size_t src = static_cast<std::size_t>(p) * 3u;
            const std::size_t out_o = static_cast<std::size_t>(j) * 3u;
            dst[out_o] = frame[src];
            dst[out_o + 1u] = frame[src + 1u];
            dst[out_o + 2u] = frame[src + 2u];
          }
        }
        return (*out_buf)[static_cast<std::size_t>(i % (t * n_sel)) * 3u];
      },
      /*target_total_ms=*/100.0,
      /*max_iters=*/1 << 10);
  return out;
}

inline std::vector<int> make_strided_selection_indices(int n_points, int k, int stride) {
  std::vector<int> out;
  if (n_points <= 0 || k <= 0) return out;
  const int k_clamped = std::max(1, std::min(k, n_points));
  out.reserve(static_cast<std::size_t>(k_clamped));
  int step = stride;
  if (step <= 0) step = std::max(1, n_points / k_clamped);
  int cur = 0;
  for (int i = 0; i < k_clamped; ++i) {
    out.push_back(cur);
    cur += step;
    cur %= n_points;
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  while (static_cast<int>(out.size()) < k_clamped) {
    const int next = static_cast<int>(out.size());
    if (next >= n_points) break;
    out.push_back(next);
  }
  return out;
}

inline std::vector<int> filter_indices_by_mask(
    const std::vector<int>& indices,
    const unsigned char* allow_mask,
    int allow_mask_n) {
  std::vector<int> out;
  if (!allow_mask || allow_mask_n <= 0) return out;
  out.reserve(indices.size());
  for (const int idx : indices) {
    if (idx >= 0 && idx < allow_mask_n && allow_mask[static_cast<std::size_t>(idx)]) out.push_back(idx);
  }
  return out;
}

}  // namespace sqzc3d_bench

#endif  // SQZC3D_BENCH_ACCESS_PATTERNS_H_

