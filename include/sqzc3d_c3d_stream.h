#ifndef sqzc3d_c3d_stream_H_
#define sqzc3d_c3d_stream_H_

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if !defined(sqzc3d_WITH_EZC3D) || (sqzc3d_WITH_EZC3D == 0)
namespace ezc3d {
enum class PROCESSOR_TYPE {
  INTEL = 0,
  DEC = 1,
  MIPS = 2,
  INTEL_BIG_ENDIAN = 3,
  DEC_BIG_ENDIAN = 4,
  MIPS_BIG_ENDIAN = 5,
};
struct c3d {
  virtual ~c3d() = default;
};
}  // namespace ezc3d
#else
#include <ezc3d/ezc3d_all.h>
#endif

#include "sqzc3d_types.h"

namespace sqzc3d {

struct C3dStreamReader;

struct C3dStreamMeta {
  int n_frames = 0;
  int n_points = 0;
  int n_analogs = 0;
  int n_analog_by_frame = 0;
  int point_record_bytes = 0;
  int analog_record_bytes = 0;
  int frame_bytes = 0;
  int point_record_scalar = 0;  ///< 1 => int16+2 bytes, 2 => float.
  double point_scale = 1.0;
  float header_scale = 1.0f;
  double analog_scale = 1.0;
  ezc3d::PROCESSOR_TYPE processor_type = ezc3d::PROCESSOR_TYPE::INTEL;
  std::int64_t data_start_bytes = 0; ///< Absolute byte offset to first frame.
};

struct C3dStreamReader {
  std::fstream file;
  std::unique_ptr<ezc3d::c3d> c3d;
  C3dStreamMeta meta{};
  std::vector<std::string> point_labels;
  std::vector<std::string> analog_labels;
  std::vector<std::string> type_group_names;
  std::vector<int> type_group_starts;
  std::vector<int> type_group_indices;
  std::vector<std::uint8_t> raw_params;
};

// Open C3D and retain header/meta/labels.
sqzc3d_status sqzc3d_c3d_stream_open_file(
    C3dStreamReader* reader,
    const char* file_path,
    bool preserve_raw_params);
void sqzc3d_c3d_stream_close(C3dStreamReader* reader);

// Map point labels to global point indices using the full C3D POINT:LABELS table.
// Best-effort semantics: missing labels map to miss_idx.
// norm_mode is a bitmask aligned with sqzc3d_LABEL_NORM_* (EXACT/TRIM/CASEFOLD_WS).
sqzc3d_status sqzc3d_c3d_stream_point_indices_for_labels(
    const C3dStreamReader* reader,
    const char* const* labels,
    int n_labels,
    int* out_indices,
    int miss_idx,
    int norm_mode);

// Read selected point xyz for one frame: layout [x0,y0,z0,x1,y1,z1,...].
sqzc3d_status sqzc3d_c3d_stream_read_frame_xyz_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* point_indices,
    int n_points_sel,
    sqzc3d_num_t* out_target,
    int out_nscalar,
    unsigned char* out_valid,
    int out_valid_nscalar);

// Read residual for selected point xyz for one frame.
sqzc3d_status sqzc3d_c3d_stream_read_frame_residual_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* point_indices,
    int n_points_sel,
    sqzc3d_num_t* out_residual,
    int out_nscalar);

// Read all point xyz for one frame; no selection.
sqzc3d_status sqzc3d_c3d_stream_read_frame_all_xyz(
    C3dStreamReader* reader,
    int frame_idx,
    sqzc3d_num_t* out_target,
    int out_nscalar);

// Read trajectory for selected points: layout frame-major [T x (n_points_sel*3)].
sqzc3d_status sqzc3d_c3d_stream_read_traj_xyz_sel(
    C3dStreamReader* reader,
    const int* point_indices,
    int n_points_sel,
    int start_frame,
    int end_frame,
    sqzc3d_num_t* out_traj,
    int out_nscalar,
    unsigned char* out_valid,
    int out_valid_nscalar);

// Read all selected analog channels for all subframes in one frame:
// layout [sample0 ch0, sample0 ch1, ... sample1 ch0 ...].
sqzc3d_status sqzc3d_c3d_stream_read_frame_analogs_sel(
    C3dStreamReader* reader,
    int frame_idx,
    const int* analog_indices,
    int n_analog_sel,
    int start_sample,
    int n_samples,
    sqzc3d_num_t* out_analog,
    int out_nscalar);

}  // namespace sqzc3d

#endif  // sqzc3d_c3d_stream_H_

