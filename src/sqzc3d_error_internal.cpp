#include "sqzc3d_error_internal.h"

namespace sqzc3d {
namespace internal {

static thread_local std::string g_last_error;
static thread_local ErrorDetail g_last_error_detail;

void reset_global_error() {
  g_last_error.clear();
  g_last_error_detail = {};
  g_last_error_detail.status = sqzc3d_STATUS_SUCCESS;
}

void set_global_error(
    int status,
    const std::string& message,
    const char* api,
    const char* section,
    int index) {
  g_last_error = message;
  g_last_error_detail.status = status;
  g_last_error_detail.api = api ? api : "";
  g_last_error_detail.section = section ? section : "";
  g_last_error_detail.index = index;
  g_last_error_detail.message = message;
}

const std::string& global_error_message() { return g_last_error; }
const ErrorDetail& global_error_detail() { return g_last_error_detail; }

}  // namespace internal
}  // namespace sqzc3d

