cmake_minimum_required(VERSION 3.16)
project(sqzc3d VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

function(_sqzc3d_set_pic_if_possible target_name)
  if (NOT TARGET "${target_name}")
    return()
  endif()

  get_target_property(_sqzc3d_aliased "${target_name}" ALIASED_TARGET)
  if (_sqzc3d_aliased)
    set(target_name "${_sqzc3d_aliased}")
  endif()

  get_target_property(_sqzc3d_imported "${target_name}" IMPORTED)
  if (_sqzc3d_imported)
    return()
  endif()

  set_target_properties("${target_name}" PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

set(_SQZC3D_WITH_EZC3D_PREDEFINED OFF)
if (DEFINED SQZC3D_WITH_EZC3D)
  set(_SQZC3D_WITH_EZC3D_PREDEFINED ON)
endif()

option(SQZC3D_WITH_EZC3D "Enable ezc3d-backed parser implementation" ON)
option(SQZC3D_FETCH_EZC3D "Fetch ezc3d from GitHub when not provided externally" ON)
option(SQZC3D_BUILD_PYTHON "Build Python extension module (pybind11)" OFF)

if (SQZC3D_BUILD_PYTHON AND NOT WIN32)
  # The Python extension is a shared module. When ezc3d is fetched/built as a static library,
  # it must be compiled with -fPIC on ELF platforms (e.g. manylinux) to link successfully.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

set(_SQZC3D_DEFAULT_APPLY_EZC3D_PATCHES OFF)
if (CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
  # Emscripten/WASM builds may disable/limit exception catching; ezc3d uses try/catch as control flow
  # in some query helpers. Enable local patches by default for WASM to match native behavior.
  set(_SQZC3D_DEFAULT_APPLY_EZC3D_PATCHES ON)
endif()
option(SQZC3D_APPLY_EZC3D_PATCHES "Apply local patches to ezc3d when fetched" ${_SQZC3D_DEFAULT_APPLY_EZC3D_PATCHES})
set(SQZC3D_EZC3D_GIT_REPOSITORY "https://github.com/pyomeca/ezc3d.git" CACHE STRING "ezc3d git repository")
set(SQZC3D_EZC3D_GIT_TAG "Release_1.6.3" CACHE STRING "ezc3d git tag/branch/commit")

function(_sqzc3d_apply_ezc3d_patches ezc3d_source_dir)
  if (NOT SQZC3D_APPLY_EZC3D_PATCHES)
    return()
  endif()

  set(_SQZC3D_PATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches")
  set(_SQZC3D_APPLY_SCRIPT "${_SQZC3D_PATCH_DIR}/apply_git_patch.cmake")
  set(_SQZC3D_EZC3D_PATCH_FILES
    "${_SQZC3D_PATCH_DIR}/ezc3d_noexcept_isgroup_isparameter.patch"
  )

  if (NOT EXISTS "${_SQZC3D_APPLY_SCRIPT}")
    message(FATAL_ERROR "Missing patch script: ${_SQZC3D_APPLY_SCRIPT}")
  endif()

  foreach (_sqzc3d_patch_file IN LISTS _SQZC3D_EZC3D_PATCH_FILES)
    if (NOT EXISTS "${_sqzc3d_patch_file}")
      message(FATAL_ERROR "Missing ezc3d patch file: ${_sqzc3d_patch_file}")
    endif()

    execute_process(
      COMMAND "${CMAKE_COMMAND}" "-DPATCH_FILE=${_sqzc3d_patch_file}" -P "${_SQZC3D_APPLY_SCRIPT}"
      WORKING_DIRECTORY "${ezc3d_source_dir}"
      RESULT_VARIABLE _sqzc3d_patch_res
    )

    if (NOT _sqzc3d_patch_res EQUAL 0)
      message(FATAL_ERROR "Failed to apply ezc3d patch: ${_sqzc3d_patch_file}")
    endif()
  endforeach()
endfunction()

if (DEFINED sqzc3d_WITH_EZC3D)
  if (_SQZC3D_WITH_EZC3D_PREDEFINED)
    if (NOT sqzc3d_WITH_EZC3D STREQUAL SQZC3D_WITH_EZC3D)
      message(WARNING
        "Both sqzc3d_WITH_EZC3D and SQZC3D_WITH_EZC3D are set with different values; using SQZC3D_WITH_EZC3D")
    endif()
  else()
    # Backward compatibility: map legacy option to canonical name.
    set(SQZC3D_WITH_EZC3D ${sqzc3d_WITH_EZC3D})
  endif()
endif()
set(sqzc3d_WITH_EZC3D ${SQZC3D_WITH_EZC3D})

set(SQZC3D_SOURCE
  src/sqzc3d_c3d_stream.cpp
  src/sqzc3d_error_internal.cpp
  src/sqzc3d.cpp
)

add_library(sqzc3d STATIC ${SQZC3D_SOURCE})
set_target_properties(sqzc3d PROPERTIES LINKER_LANGUAGE CXX)
set_target_properties(sqzc3d PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(
  sqzc3d
  PUBLIC
    SQZC3D_WITH_EZC3D=$<BOOL:${SQZC3D_WITH_EZC3D}>
    sqzc3d_WITH_EZC3D=$<BOOL:${SQZC3D_WITH_EZC3D}>
)
target_include_directories(sqzc3d
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (SQZC3D_WITH_EZC3D)
  if (TARGET ezc3d)
    target_link_libraries(sqzc3d PUBLIC ezc3d)
  elseif (TARGET ezc3d::ezc3d)
    target_link_libraries(sqzc3d PUBLIC ezc3d::ezc3d)
  elseif (DEFINED EZC3D_INCLUDE_DIR)
    target_include_directories(sqzc3d PUBLIC ${EZC3D_INCLUDE_DIR})
    if (DEFINED EZC3D_LIBRARY)
      target_link_libraries(sqzc3d PUBLIC ${EZC3D_LIBRARY})
    elseif (DEFINED EZC3D_LIBRARIES)
      target_link_libraries(sqzc3d PUBLIC ${EZC3D_LIBRARIES})
    elseif (SQZC3D_FETCH_EZC3D)
      message(STATUS "SQZC3D_WITH_EZC3D=ON and no local ezc3d, fetching from ${SQZC3D_EZC3D_GIT_REPOSITORY}")
      include(FetchContent)
      # Keep dependency build minimal (no ezc3d examples; prefer static lib to avoid runtime DLL wiring).
      set(BUILD_EXAMPLE OFF CACHE BOOL "Disable ezc3d examples" FORCE)
      set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build ezc3d as a static library" FORCE)
      FetchContent_Declare(
        ezc3d
        GIT_REPOSITORY ${SQZC3D_EZC3D_GIT_REPOSITORY}
        GIT_TAG        ${SQZC3D_EZC3D_GIT_TAG}
      )
      FetchContent_MakeAvailable(ezc3d)
      _sqzc3d_apply_ezc3d_patches("${ezc3d_SOURCE_DIR}")
      _sqzc3d_set_pic_if_possible(ezc3d)
      _sqzc3d_set_pic_if_possible(ezc3d::ezc3d)
      if (TARGET ezc3d)
        target_link_libraries(sqzc3d PUBLIC ezc3d)
      elseif (TARGET ezc3d::ezc3d)
        target_link_libraries(sqzc3d PUBLIC ezc3d::ezc3d)
      else()
        message(FATAL_ERROR "Fetched ezc3d, but expected CMake target was not found")
      endif()
    else()
      message(WARNING "SQZC3D_WITH_EZC3D=ON but no EZC3D_LIBRARY provided; link target still needs manual wiring")
    endif()
  else()
    if (SQZC3D_FETCH_EZC3D)
      message(STATUS "SQZC3D_WITH_EZC3D=ON and no local ezc3d, fetching from ${SQZC3D_EZC3D_GIT_REPOSITORY}")
      include(FetchContent)
      # Keep dependency build minimal (no ezc3d examples; prefer static lib to avoid runtime DLL wiring).
      set(BUILD_EXAMPLE OFF CACHE BOOL "Disable ezc3d examples" FORCE)
      set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build ezc3d as a static library" FORCE)
      FetchContent_Declare(
        ezc3d
        GIT_REPOSITORY ${SQZC3D_EZC3D_GIT_REPOSITORY}
        GIT_TAG        ${SQZC3D_EZC3D_GIT_TAG}
      )
      FetchContent_MakeAvailable(ezc3d)
      _sqzc3d_apply_ezc3d_patches("${ezc3d_SOURCE_DIR}")
      _sqzc3d_set_pic_if_possible(ezc3d)
      _sqzc3d_set_pic_if_possible(ezc3d::ezc3d)
      if (TARGET ezc3d)
        target_link_libraries(sqzc3d PUBLIC ezc3d)
      elseif (TARGET ezc3d::ezc3d)
        target_link_libraries(sqzc3d PUBLIC ezc3d::ezc3d)
      else()
        message(FATAL_ERROR "Fetched ezc3d, but expected CMake target was not found")
      endif()
    else()
      message(FATAL_ERROR "SQZC3D_WITH_EZC3D=ON but ezc3d target/include path not found")
    endif()
  endif()
endif()

if (SQZC3D_BUILD_PYTHON)
  # For extension modules we only need Python "Development.Module"; requiring
  # full "Development" also pulls in the embedding/library components
  # (e.g. Python_LIBRARIES / Development.Embed) which are not guaranteed to
  # exist in manylinux images.
  find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
  find_package(pybind11 CONFIG REQUIRED)

  pybind11_add_module(_core MODULE python/bindings/sqzc3d_pybind.cpp)
  set_property(TARGET _core PROPERTY CXX_STANDARD 17)
  target_link_libraries(_core PRIVATE sqzc3d)

  if (DEFINED SKBUILD_PLATLIB_DIR)
    set(_SQZC3D_PY_INSTALL_DIR "${SKBUILD_PLATLIB_DIR}/sqzc3d")
  else()
    set(_SQZC3D_PY_INSTALL_DIR "sqzc3d")
  endif()
  install(
    TARGETS _core
    COMPONENT python
    LIBRARY DESTINATION "${_SQZC3D_PY_INSTALL_DIR}"
    RUNTIME DESTINATION "${_SQZC3D_PY_INSTALL_DIR}"
    ARCHIVE DESTINATION "${_SQZC3D_PY_INSTALL_DIR}"
  )
endif()

option(SQZC3D_BUILD_EXAMPLES "Build sample CLI tools" OFF)
if (NOT CMAKE_CROSSCOMPILING AND SQZC3D_BUILD_EXAMPLES)
  add_executable(bench_sqzc3d samples/bench/bench_sqzc3d.cpp)
  target_link_libraries(bench_sqzc3d PRIVATE sqzc3d)
  set_property(TARGET bench_sqzc3d PROPERTY CXX_STANDARD 17)

  if (SQZC3D_WITH_EZC3D)
    add_executable(bench_sqzc3d_stream samples/bench/bench_sqzc3d_stream.cpp)
    target_link_libraries(bench_sqzc3d_stream PRIVATE sqzc3d)
    set_property(TARGET bench_sqzc3d_stream PROPERTY CXX_STANDARD 17)

    add_executable(bench_ezc3d samples/bench/bench_ezc3d.cpp)
    target_link_libraries(bench_ezc3d PRIVATE sqzc3d)
    set_property(TARGET bench_ezc3d PROPERTY CXX_STANDARD 17)
  endif()

  add_executable(c3dinfo_sqzc3d samples/tools/c3dinfo_sqzc3d.cpp)
  target_link_libraries(c3dinfo_sqzc3d PRIVATE sqzc3d)
  set_property(TARGET c3dinfo_sqzc3d PROPERTY CXX_STANDARD 17)

  add_executable(export_sqzc3d_bundle samples/tools/export_sqzc3d_bundle.cpp)
  target_link_libraries(export_sqzc3d_bundle PRIVATE sqzc3d)
  set_property(TARGET export_sqzc3d_bundle PROPERTY CXX_STANDARD 17)

  add_executable(verify_correctness_matrix_sqzc3d samples/verify/cpp/verify_correctness_matrix_sqzc3d.cpp)
  target_link_libraries(verify_correctness_matrix_sqzc3d PRIVATE sqzc3d)
  set_property(TARGET verify_correctness_matrix_sqzc3d PROPERTY CXX_STANDARD 17)

  add_executable(easy_window_sqzc3d samples/tools/easy_window_sqzc3d.cpp)
  target_link_libraries(easy_window_sqzc3d PRIVATE sqzc3d)
  set_property(TARGET easy_window_sqzc3d PROPERTY CXX_STANDARD 17)

endif()
