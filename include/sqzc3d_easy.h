#ifndef sqzc3d_EASY_H_
#define sqzc3d_EASY_H_

#include "sqzc3d.h"

#include <string>
#include <vector>

namespace sqzc3d {

struct PointWindow {
  const sqzc3d_num_t* xyz = nullptr;
  const unsigned char* valid = nullptr;
  int n_frames = 0;
  int n_points = 0;
  // v0.x easy layer fixed-shape output contract:
  // xyz layout: frame-major AOS [frame][point][xyz], stride=3
  // valid layout: frame-major [frame][point], stride=1
  int point_stride = 3;
  int frame_stride_points = 0;
};

struct AnalogWindow {
  const sqzc3d_num_t* values = nullptr;
  const unsigned char* valid = nullptr;
  int n_analogs = 0;          // channels C
  int n_samples = 0;          // N = n_frames * n_analog_by_frame
  int n_frames = 0;           // for reshaping (optional)
  int n_analog_by_frame = 0;  // samples per frame (subframes)
  // Layout: values[channel][sample] with flattened index:
  // values[channel * channel_stride_samples + sample].
  int channel_stride_samples = 0;  // source samples per channel
  int sample_stride = 1;
};

// Non-contiguous frame-major view into channel-major analog storage.
//
// This presents a (T, C, S) view where:
// - T = n_frames
// - C = n_analogs
// - S = n_analog_by_frame
//
// Element mapping:
//   view(t, c, s) == base[c * source_stride_samples + (t0 + t) * S + s]
// with strides:
//   stride_s = 1
//   stride_c = source_stride_samples
//   stride_t = S
//
// Note: This is a strided view; values are not contiguous in the channel dimension.
struct FrameMajorAnalogView {
  const sqzc3d_num_t* values = nullptr;
  const unsigned char* valid = nullptr;
  int n_frames = 0;           // T
  int n_analogs = 0;          // C
  int n_analog_by_frame = 0;  // S

  int stride_t = 0;
  int stride_c = 0;
  int stride_s = 1;

  // Source samples per channel (N for a full chunk).
  int source_stride_samples = 0;

  // Optional channel gather list (indices into the underlying chunk channels).
  // If non-null, c in [0..n_analogs) maps to source channel channel_indices[c].
  // This breaks the affine stride_c mapping; consumers must apply the gather themselves.
  const int* channel_indices = nullptr;
};

inline int MakeFrameWindowBuildOpt(
    const sqzc3d_build_opt_t* preset_opt,
    sqzc3d_build_opt_t* out_opt) {
  if (!out_opt) return sqzc3d_STATUS_INVALID_ARGUMENT;
  if (preset_opt) {
    *out_opt = *preset_opt;
    if (out_opt->struct_size < static_cast<int>(sizeof(*out_opt))) {
      return sqzc3d_STATUS_INVALID_ARGUMENT;
    }
  } else {
    sqzc3d_default_build_opt(out_opt);
  }
  return sqzc3d_STATUS_SUCCESS;
}

inline int ReadPointsWindow(
    const sqzc3d_dec_t* dec,
    int start_frame,
    int frame_count,
    const int* point_indices,
    int n_point_indices,
    const sqzc3d_build_opt_t* preset_opt,
    sqzc3d_chunk_t** out_chunk) {
  if (!dec || !out_chunk) return sqzc3d_STATUS_INVALID_ARGUMENT;
  sqzc3d_build_opt_t opt;
  const int st = MakeFrameWindowBuildOpt(preset_opt, &opt);
  if (st != sqzc3d_STATUS_SUCCESS) return st;
  opt.frame_range = {start_frame, frame_count};
  if (n_point_indices > 0 && point_indices) {
    opt.point_sel_mode = sqzc3d_POINT_SEL_INDICES;
    opt.point_sel = point_indices;
    opt.point_sel_count = n_point_indices;
  } else {
    opt.point_sel_mode = sqzc3d_POINT_SEL_ALL;
    opt.point_sel = nullptr;
    opt.point_sel_count = 0;
  }
  return sqzc3d_build_chunks(dec, &opt, out_chunk);
}

inline int ReadPointsWindowByLabels(
    const sqzc3d_dec_t* dec,
    int start_frame,
    int frame_count,
    const char* const* point_labels,
    int n_point_labels,
    const sqzc3d_build_opt_t* preset_opt,
    sqzc3d_chunk_t** out_chunk) {
  if (!dec || !out_chunk) return sqzc3d_STATUS_INVALID_ARGUMENT;
  sqzc3d_build_opt_t opt;
  const int st = MakeFrameWindowBuildOpt(preset_opt, &opt);
  if (st != sqzc3d_STATUS_SUCCESS) return st;
  opt.frame_range = {start_frame, frame_count};
  if (n_point_labels > 0 && point_labels) {
    opt.point_sel_mode = sqzc3d_POINT_SEL_LABELS;
    opt.point_labels = point_labels;
    opt.point_labels_count = n_point_labels;
  } else {
    opt.point_sel_mode = sqzc3d_POINT_SEL_ALL;
    opt.point_labels = nullptr;
    opt.point_labels_count = 0;
  }
  return sqzc3d_build_chunks(dec, &opt, out_chunk);
}

inline PointWindow FrameMajorPointsView(const sqzc3d_chunk_t* chunk) {
  PointWindow view;
  if (!chunk) return view;
  view.xyz = chunk->points_xyz;
  view.valid = chunk->points_valid;
  view.n_frames = chunk->n_frames;
  view.n_points = chunk->n_points;
  view.point_stride = 3;
  view.frame_stride_points = chunk->n_points * view.point_stride;
  return view;
}

inline AnalogWindow AnalogSamplesView(const sqzc3d_chunk_t* chunk) {
  AnalogWindow view;
  if (!chunk) return view;
  view.values = chunk->analog;
  view.valid = chunk->analog_valid;
  view.n_analogs = chunk->n_analogs;
  view.n_frames = chunk->n_frames;
  view.n_analog_by_frame = chunk->n_analog_by_frame;
  view.n_samples = view.n_frames * view.n_analog_by_frame;
  view.channel_stride_samples = view.n_samples;
  view.sample_stride = 1;
  return view;
}

inline AnalogWindow AnalogSamplesViewFrames(const sqzc3d_chunk_t* chunk, int start_frame, int frame_count) {
  AnalogWindow view;
  if (!chunk || start_frame < 0 || frame_count < 0) return view;
  if (start_frame > chunk->n_frames || frame_count > chunk->n_frames - start_frame) return view;

  const int source_stride_samples = chunk->n_frames * chunk->n_analog_by_frame;
  const int sample_start = start_frame * chunk->n_analog_by_frame;
  view.values = chunk->analog ? chunk->analog + static_cast<std::size_t>(sample_start) : nullptr;
  view.valid = chunk->analog_valid ? chunk->analog_valid + static_cast<std::size_t>(sample_start) : nullptr;
  view.n_analogs = chunk->n_analogs;
  view.n_frames = frame_count;
  view.n_analog_by_frame = chunk->n_analog_by_frame;
  view.n_samples = frame_count * chunk->n_analog_by_frame;
  view.channel_stride_samples = source_stride_samples;
  view.sample_stride = 1;
  return view;
}

inline FrameMajorAnalogView FrameMajorAnalogViewTCS(const sqzc3d_chunk_t* chunk) {
  FrameMajorAnalogView view;
  if (!chunk) return view;
  view.values = chunk->analog;
  view.valid = chunk->analog_valid;
  view.n_frames = chunk->n_frames;
  view.n_analogs = chunk->n_analogs;
  view.n_analog_by_frame = chunk->n_analog_by_frame;
  view.source_stride_samples = chunk->n_frames * chunk->n_analog_by_frame;
  view.stride_s = 1;
  view.stride_c = view.source_stride_samples;
  view.stride_t = view.n_analog_by_frame;
  view.channel_indices = nullptr;
  return view;
}

inline FrameMajorAnalogView FrameMajorAnalogViewTCSFrames(
    const sqzc3d_chunk_t* chunk, int start_frame, int frame_count) {
  FrameMajorAnalogView view;
  if (!chunk || start_frame < 0 || frame_count < 0) return view;
  if (start_frame > chunk->n_frames || frame_count > chunk->n_frames - start_frame) return view;
  const int sample_start = start_frame * chunk->n_analog_by_frame;
  view.values = chunk->analog ? chunk->analog + static_cast<std::size_t>(sample_start) : nullptr;
  view.valid = chunk->analog_valid ? chunk->analog_valid + static_cast<std::size_t>(sample_start) : nullptr;
  view.n_frames = frame_count;
  view.n_analogs = chunk->n_analogs;
  view.n_analog_by_frame = chunk->n_analog_by_frame;
  view.source_stride_samples = chunk->n_frames * chunk->n_analog_by_frame;
  view.stride_s = 1;
  view.stride_c = view.source_stride_samples;
  view.stride_t = view.n_analog_by_frame;
  view.channel_indices = nullptr;
  return view;
}

inline std::vector<int> PointIndicesFromTypeGroups(
    const sqzc3d_chunk_t* chunk,
    const std::vector<std::string>& group_names,
    bool keep_markers_when_missing) {
  std::vector<int> out_indices;
  if (!chunk || !chunk->point_labels) return out_indices;
  const int n_total = chunk->n_points_total;
  if (n_total <= 0) return out_indices;

  if (!chunk->type_group_names || !chunk->type_group_starts || !chunk->type_group_indices ||
      chunk->n_type_groups <= 0) {
    if (!keep_markers_when_missing) return out_indices;
    out_indices.resize(static_cast<std::size_t>(n_total));
    for (int i = 0; i < n_total; ++i) out_indices[static_cast<std::size_t>(i)] = i;
    return out_indices;
  }

  std::vector<unsigned char> seen(static_cast<std::size_t>(n_total), 0);
  const auto marker = [&](int idx) {
    if (idx < 0 || idx >= n_total) return;
    if (!seen[static_cast<std::size_t>(idx)]) {
      seen[static_cast<std::size_t>(idx)] = 1;
      out_indices.push_back(idx);
    }
  };

  if (group_names.empty()) return out_indices;
  for (int i = 0; i < chunk->n_type_groups; ++i) {
    const char* gname = chunk->type_group_names[static_cast<std::size_t>(i)];
    const std::string group = (gname ? gname : "");
    for (const auto& want : group_names) {
      if (want != group) continue;
      const int start = chunk->type_group_starts[static_cast<std::size_t>(i)];
      const int end = chunk->type_group_starts[static_cast<std::size_t>(i) + 1];
      for (int p = start; p < end; ++p) {
        const int idx = chunk->type_group_indices[static_cast<std::size_t>(p)];
        marker(idx);
      }
      break;
    }
  }
  if (out_indices.empty() && keep_markers_when_missing) {
    for (int i = 0; i < n_total; ++i) {
      if (!seen[static_cast<std::size_t>(i)]) marker(i);
    }
  }
  return out_indices;
}

inline void ReorderFrameMajorToPointMajor(
    const sqzc3d_num_t* frame_major_xyz,
    const unsigned char* frame_major_valid,
    int n_frames,
    int n_points,
    sqzc3d_num_t* out_xyz,
    unsigned char* out_valid) {
  if (!frame_major_xyz || !out_xyz || n_frames < 0 || n_points < 0) return;
  const int point_stride = 3;
  for (int f = 0; f < n_frames; ++f) {
    for (int p = 0; p < n_points; ++p) {
      const int frame_src = (f * n_points + p) * point_stride;
      const int out_idx = p * n_frames * point_stride + f * point_stride;
      out_xyz[out_idx] = frame_major_xyz[frame_src];
      out_xyz[out_idx + 1] = frame_major_xyz[frame_src + 1];
      out_xyz[out_idx + 2] = frame_major_xyz[frame_src + 2];
      if (out_valid && frame_major_valid) {
        out_valid[p * n_frames + f] = frame_major_valid[f * n_points + p];
      } else if (out_valid) {
        out_valid[p * n_frames + f] = 1u;
      }
    }
  }
}

}  // namespace sqzc3d

#endif  // sqzc3d_EASY_H_
