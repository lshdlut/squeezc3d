if (NOT DEFINED PATCH_FILE)
  message(FATAL_ERROR "PATCH_FILE is required")
endif()

find_program(_SQZC3D_GIT_EXE git)
if (NOT _SQZC3D_GIT_EXE)
  message(FATAL_ERROR "git not found; cannot apply patch: ${PATCH_FILE}")
endif()

execute_process(
  COMMAND "${_SQZC3D_GIT_EXE}" apply --reverse --check "${PATCH_FILE}"
  WORKING_DIRECTORY "."
  RESULT_VARIABLE _sqzc3d_reverse_check
  OUTPUT_QUIET
  ERROR_QUIET
)

if (_sqzc3d_reverse_check EQUAL 0)
  message(STATUS "Patch already applied: ${PATCH_FILE}")
  return()
endif()

execute_process(
  COMMAND "${_SQZC3D_GIT_EXE}" apply --whitespace=nowarn "${PATCH_FILE}"
  WORKING_DIRECTORY "."
  RESULT_VARIABLE _sqzc3d_apply
)

if (NOT _sqzc3d_apply EQUAL 0)
  message(FATAL_ERROR "Failed to apply patch: ${PATCH_FILE}")
endif()

message(STATUS "Patch applied: ${PATCH_FILE}")
