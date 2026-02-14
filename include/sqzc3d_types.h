#ifndef sqzc3d_TYPES_H_
#define sqzc3d_TYPES_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef double sqzc3d_num_t;
typedef unsigned char sqzc3d_byte_t;
typedef int sqzc3d_status;

// keep return-code semantics aligned with existing Scale-IK status set.
enum {
  sqzc3d_STATUS_SUCCESS = 0,
  sqzc3d_STATUS_INVALID_ARGUMENT = 1,
  sqzc3d_STATUS_DIMENSION_MISMATCH = 2,
  sqzc3d_STATUS_NOT_IMPLEMENTED = 3,
  sqzc3d_STATUS_INTERNAL_ERROR = 255,
};

#ifdef __cplusplus
}
#endif

#endif  // sqzc3d_TYPES_H_


