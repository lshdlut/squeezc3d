#pragma once

#include <string>

#include "sqzc3d_types.h"

namespace sqzc3d {
namespace internal {

struct ErrorDetail {
  int status = sqzc3d_STATUS_SUCCESS;
  std::string api;
  std::string section;
  int index = -1;
  std::string message;
};

void reset_global_error();
void set_global_error(
    int status,
    const std::string& message,
    const char* api,
    const char* section,
    int index);

const std::string& global_error_message();
const ErrorDetail& global_error_detail();

}  // namespace internal
}  // namespace sqzc3d

