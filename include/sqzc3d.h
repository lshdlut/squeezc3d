// Minimal C3D module API (chunk-first, no object graph retention).
#ifndef sqzc3d_H_
#define sqzc3d_H_

#include <stddef.h>
#include <stdint.h>

#include "sqzc3d_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#if defined(sqzc3d_BUILD)
#define sqzc3d_API __declspec(dllexport)
#elif defined(sqzc3d_IMPORT)
#define sqzc3d_API __declspec(dllimport)
#else
#define sqzc3d_API
#endif
#else
#define sqzc3d_API __attribute__((visibility("default")))
#endif

#define SQZC3D_FEATURE_OPEN_FILE 0x1
#define SQZC3D_FEATURE_OPEN_MEMORY 0x2
#define SQZC3D_FEATURE_BUILD_CHUNKS 0x4
#define SQZC3D_FEATURE_BUNDLE 0x8
#define SQZC3D_FEATURE_ANALOG 0x10

// Public semantic version and ABI version.
#define SQZC3D_VERSION_MAJOR 0
#define SQZC3D_VERSION_MINOR 3
#define SQZC3D_VERSION_PATCH 4
#define SQZC3D_VERSION "0.3.4"
#define SQZC3D_ABI_VERSION 3

typedef struct sqzc3d_range_t_ {
  int start;
  int count;
} sqzc3d_range_t;

enum {
  sqzc3d_FILE = 0,
  sqzc3d_MEMORY = 1,
};

enum {
  sqzc3d_POINT_SEL_ALL = 0,
  sqzc3d_POINT_SEL_INDICES = 1,
  sqzc3d_POINT_SEL_LABELS = 2,
};

enum {
  sqzc3d_ANALOG_EN_AUTO = 0,
  sqzc3d_ANALOG_EN_OFF = 1,
  sqzc3d_ANALOG_EN_ON = 2,
};

enum {
  sqzc3d_ANALOG_SEL_ALL = 0,
  sqzc3d_ANALOG_SEL_INDICES = 1,
  sqzc3d_ANALOG_SEL_LABELS = 2,
};

enum {
  sqzc3d_LABEL_NORM_EXACT = 1,
  sqzc3d_LABEL_NORM_TRIM = 2,
  sqzc3d_LABEL_NORM_CASEFOLD_WS = 4,
};

enum {
  sqzc3d_POINTS_LAYOUT_FRAME_MAJOR = 0,
};

enum {
  sqzc3d_POINTS_PACK_AOS_XYZ_VALID = 0,
};

enum {
  sqzc3d_VALID_POLICY_FINITE_XYZ = 0,
};

enum {
  sqzc3d_READ_POLICY_AUTO = 0,
  sqzc3d_READ_POLICY_DENSE = 1,
  sqzc3d_READ_POLICY_SPARSE = 2,
};

typedef struct sqzc3d_open_opt_t_ {
  int struct_size;
  int open_mode;              // sqzc3d_FILE or sqzc3d_MEMORY
  int cache_labels;
  int label_norm;             // bitmask: sqzc3d_LABEL_NORM_*
} sqzc3d_open_opt_t;

typedef struct sqzc3d_build_opt_t_ {
  int struct_size;
  sqzc3d_range_t frame_range;

  // Point selection.
  //
  // Important: when selecting by indices, indices are in the SOURCE-TOTAL point index space
  // (the original C3D POINT:LABELS order) before chunk materialization.
  int point_sel_mode;
  // When point_sel_mode == sqzc3d_POINT_SEL_INDICES, point_sel[i] is a SOURCE-TOTAL point index.
  const int* point_sel;
  int point_sel_count;
  // When point_sel_mode == sqzc3d_POINT_SEL_LABELS, point_labels are matched against SOURCE labels.
  const char* const* point_labels;
  int point_labels_count;

  int analog_enable;          // sqzc3d_ANALOG_EN_*
  sqzc3d_range_t analog_range;  // same index space as frame_range for current implementation
  int analog_sel_mode;
  // When analog_sel_mode == sqzc3d_ANALOG_SEL_INDICES, analog_sel[i] is a SOURCE-TOTAL analog channel index.
  const int* analog_sel;
  int analog_sel_count;
  // When analog_sel_mode == sqzc3d_ANALOG_SEL_LABELS, analog_labels are matched against SOURCE labels.
  const char* const* analog_labels;
  int analog_labels_count;

  int points_layout;
  int points_pack;
  int valid_policy;
  double residual_gate_mm;
  int read_policy;
  double dense_threshold_ratio;
  size_t io_buffer_bytes;
  int64_t analog_size_soft_limit_bytes;
} sqzc3d_build_opt_t;

typedef struct sqzc3d_chunk_t_ {
  int struct_size;
  int n_frames;
  // Number of points materialized into this chunk.
  // Point indices exposed from this chunk use the CHUNK-LOCAL index space [0..n_points).
  int n_points;
  // Number of points in the source C3D ("total points").
  int n_points_total;
  int n_analogs;
  int n_analog_by_frame;
  // Number of point type groups attached to this chunk.
  int n_type_groups;

  int n_scalar;
  int valid_nscalar;
  int n_analog_scalar;

  int points_layout;
  int read_policy;
  int points_pack;
  int valid_policy;
  double residual_gate_mm;
  double point_scale;
  double header_scale;

  sqzc3d_num_t* points_xyz;
  unsigned char* points_valid;
  // Analog layout: channel-major [channel][sample] (C, N), where:
  // - N = n_frames * n_analog_by_frame
  // - sample = frame * n_analog_by_frame + subframe
  // Flattened index: analog[channel * N + sample].
  sqzc3d_num_t* analog;
  // Same layout as analog.
  unsigned char* analog_valid;
  // Point labels for the chunk, length n_points (CHUNK-LOCAL order).
  const char** point_labels;
  // Analog labels for the chunk, length n_analogs (CHUNK-LOCAL order).
  const char** analog_labels;
  // Type group metadata (optional):
  // - names: length n_type_groups
  // - starts: length n_type_groups + 1, prefix offsets
  // - indices: flattened CHUNK-LOCAL point indices addressed by starts (indices into point_labels)
  const char** type_group_names;
  const int* type_group_starts;
  const int* type_group_indices;
  const char* reason;  // optional human-readable status/mismatch info owned by chunk

  void* impl;
} sqzc3d_chunk_t;

typedef struct sqzc3d_dec_t_ {
  void* impl;
  const char* last_error;
} sqzc3d_dec_t;

typedef struct sqzc3d_error_detail_t_ {
  int status;
  const char* api;
  const char* section;
  int index;
  const char* message;
} sqzc3d_error_detail_t;

typedef struct sqzc3d_bundle_load_opt_t_ {
  int struct_size;
  int strict;
  int reserved;
} sqzc3d_bundle_load_opt_t;

typedef struct sqzc3d_points_view_t_ {
  const sqzc3d_num_t* points_xyz;
  const unsigned char* points_valid;
  int n_frames;
  int n_points;
  // Number of points per frame in the underlying chunk (stride for gather views).
  int source_stride_points;
  // Contiguous slice origin in the underlying chunk point index space.
  int source_point_offset;
  // Non-null for gather view; values are indices into the underlying chunk point index space.
  const int* point_indices;
} sqzc3d_points_view_t;

typedef struct sqzc3d_analogs_view_t_ {
  const sqzc3d_num_t* analog;
  const unsigned char* analog_valid;
  int n_frames;
  int n_analog_by_frame;
  int n_analogs;
  // n_samples = n_frames * n_analog_by_frame
  int n_samples;
  // Source samples per channel (stride in samples between channels).
  // For views derived from a chunk: source_stride_samples = chunk->n_frames * chunk->n_analog_by_frame.
  int source_stride_samples;
  // Non-null for gather view; values are indices into the underlying chunk channel index space.
  const int* channel_indices;
} sqzc3d_analogs_view_t;

sqzc3d_API void sqzc3d_default_open_opt(sqzc3d_open_opt_t* out_opt);
sqzc3d_API void sqzc3d_default_build_opt(sqzc3d_build_opt_t* out_opt);
sqzc3d_API void sqzc3d_apply_preset_stream_frame_all(sqzc3d_build_opt_t* out_opt);
sqzc3d_API void sqzc3d_apply_preset_stream_frame_sel(sqzc3d_build_opt_t* out_opt);
sqzc3d_API void sqzc3d_apply_preset_window_analysis(sqzc3d_build_opt_t* out_opt);
sqzc3d_API void sqzc3d_apply_preset_interpolation_ready(sqzc3d_build_opt_t* out_opt);

sqzc3d_API int sqzc3d_open_file(
    sqzc3d_dec_t** out_dec,
    const char* file_path,
    const sqzc3d_open_opt_t* opt);
sqzc3d_API int sqzc3d_open_memory(
    sqzc3d_dec_t** out_dec,
    const void* data,
    int n_bytes,
    const sqzc3d_open_opt_t* opt);
sqzc3d_API int sqzc3d_close_dec(sqzc3d_dec_t* dec);

// Get the last error message.
// If `dec` is NULL, returns the last error for the current thread (useful for open failures with no handle).
sqzc3d_API const char* sqzc3d_last_error(const sqzc3d_dec_t* dec);

sqzc3d_API int sqzc3d_build_chunks(
    const sqzc3d_dec_t* dec,
    const sqzc3d_build_opt_t* opt,
    sqzc3d_chunk_t** out_chunk);
sqzc3d_API int sqzc3d_free_chunk(sqzc3d_chunk_t* chunk);

sqzc3d_API int sqzc3d_chunk_num_frames(const sqzc3d_chunk_t* chunk);
sqzc3d_API int sqzc3d_chunk_num_points(const sqzc3d_chunk_t* chunk);
sqzc3d_API int sqzc3d_chunk_num_scalar(const sqzc3d_chunk_t* chunk);

// Optional mapping from chunk-local point indices -> source total point indices.
// If unavailable (e.g. bundles without this metadata), returns sqzc3d_STATUS_NOT_IMPLEMENTED.
sqzc3d_API int sqzc3d_chunk_point_indices_total(
    const sqzc3d_chunk_t* chunk,
    const int** out_point_indices_total,
    int* out_n_points);

sqzc3d_API int sqzc3d_point_indices_for_labels(
    const sqzc3d_chunk_t* chunk,
    const char** labels,
    int n_labels,
    int* out_indices,
    int miss_idx);

sqzc3d_API int sqzc3d_analog_indices_for_labels(
    const sqzc3d_chunk_t* chunk,
    const char** labels,
    int n_labels,
    int* out_indices,
    int miss_idx);

sqzc3d_API int sqzc3d_points_view_frames(
    const sqzc3d_chunk_t* chunk,
    int start,
    int count,
    sqzc3d_points_view_t* out_view);

sqzc3d_API int sqzc3d_points_view_points(
    const sqzc3d_chunk_t* chunk,
    const int* point_indices,
    int n,
    sqzc3d_points_view_t* out_view);

sqzc3d_API int sqzc3d_analogs_view_samples(
    const sqzc3d_chunk_t* chunk,
    int start_frame,
    int n_frames,
    sqzc3d_analogs_view_t* out_view);

sqzc3d_API int sqzc3d_analogs_view_channels(
    const sqzc3d_chunk_t* chunk,
    const int* channel_indices,
    int n,
    sqzc3d_analogs_view_t* out_view);

// Export chunk payload to a simple bundle layout:
// <out_dir>/meta.json and <out_dir>/data.bin.
sqzc3d_API int sqzc3d_export_bundle(
    const char* out_dir,
    const sqzc3d_chunk_t* chunk);

// Feature bits indicating available capabilities in current build.
sqzc3d_API int sqzc3d_get_features(void);
sqzc3d_API const char* sqzc3d_version(void);
sqzc3d_API int sqzc3d_abi_version(void);
// Optional diagnostics for last API error.
sqzc3d_API void sqzc3d_default_error_detail(sqzc3d_error_detail_t* out_detail);
// If `dec` is NULL, returns the last error detail for the current thread (useful for open failures with no handle).
sqzc3d_API int sqzc3d_last_error_detail(
    const sqzc3d_dec_t* dec,
    sqzc3d_error_detail_t* out_detail);

// Load a bundle exported by sqzc3d_export_bundle.
// `bundle_dir` may be either:
// - a directory that contains meta.json + data.bin (legacy/dir format), or
// - a single-file `.sqzc3d` / `.sqzc3D` container (v2 single-file format).
// out_chunk may be used with existing view/query/free APIs.
sqzc3d_API int sqzc3d_load_bundle(
    const char* bundle_dir,
    sqzc3d_chunk_t** out_chunk);
sqzc3d_API void sqzc3d_default_bundle_load_opt(sqzc3d_bundle_load_opt_t* out_opt);
sqzc3d_API int sqzc3d_load_bundle_with_options(
    const char* bundle_dir,
    const sqzc3d_bundle_load_opt_t* opt,
    sqzc3d_chunk_t** out_chunk);

#ifdef __cplusplus
}
#endif

#endif  // sqzc3d_H_

