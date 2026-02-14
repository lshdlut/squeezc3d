cmake_minimum_required(VERSION 3.16)
project(sqzc3d VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SQZC3D_SOURCE
  src/sqzc3d_c3d_stream.cpp
  src/sqzc3d.cpp
)

add_library(sqzc3d STATIC ${SQZC3D_SOURCE})
set_target_properties(sqzc3d PROPERTIES LINKER_LANGUAGE CXX)
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

option(SQZC3D_WITH_EZC3D "Enable ezc3d-backed parser implementation" ON)
option(SQZC3D_FETCH_EZC3D "Fetch ezc3d from GitHub when not provided externally" ON)
set(SQZC3D_EZC3D_GIT_REPOSITORY "https://github.com/pyomeca/ezc3d.git" CACHE STRING "ezc3d git repository")
set(SQZC3D_EZC3D_GIT_TAG "Stable" CACHE STRING "ezc3d git tag/branch/commit")

if (DEFINED sqzc3d_WITH_EZC3D)
  if (NOT sqzc3d_WITH_EZC3D STREQUAL SQZC3D_WITH_EZC3D)
    message(WARNING
      "Both sqzc3d_WITH_EZC3D and SQZC3D_WITH_EZC3D are set with different values; using SQZC3D_WITH_EZC3D")
  endif()
  set(SQZC3D_WITH_EZC3D ${sqzc3d_WITH_EZC3D})
endif()
set(sqzc3d_WITH_EZC3D ${SQZC3D_WITH_EZC3D})

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
      FetchContent_Declare(
        ezc3d
        GIT_REPOSITORY ${SQZC3D_EZC3D_GIT_REPOSITORY}
        GIT_TAG        ${SQZC3D_EZC3D_GIT_TAG}
      )
      FetchContent_MakeAvailable(ezc3d)
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
      FetchContent_Declare(
        ezc3d
        GIT_REPOSITORY ${SQZC3D_EZC3D_GIT_REPOSITORY}
        GIT_TAG        ${SQZC3D_EZC3D_GIT_TAG}
      )
      FetchContent_MakeAvailable(ezc3d)
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

option(SQZC3D_BUILD_EXAMPLES "Build sample CLI tools" ON)
if (NOT CMAKE_CROSSCOMPILING AND SQZC3D_BUILD_EXAMPLES)
  add_executable(bench_sqzc3d samples/bench_sqzc3d.cpp)
  target_link_libraries(bench_sqzc3d PRIVATE sqzc3d)
  set_property(TARGET bench_sqzc3d PROPERTY CXX_STANDARD 17)

  add_executable(c3dinfo_sqzc3d samples/c3dinfo_sqzc3d.cpp)
  target_link_libraries(c3dinfo_sqzc3d PRIVATE sqzc3d)
  set_property(TARGET c3dinfo_sqzc3d PROPERTY CXX_STANDARD 17)

  add_executable(export_sqzc3d_bundle samples/export_sqzc3d_bundle.cpp)
  target_link_libraries(export_sqzc3d_bundle PRIVATE sqzc3d)
  set_property(TARGET export_sqzc3d_bundle PROPERTY CXX_STANDARD 17)

  add_executable(load_sqzc3d_bundle samples/load_sqzc3d_bundle.cpp)
  target_link_libraries(load_sqzc3d_bundle PRIVATE sqzc3d)
  set_property(TARGET load_sqzc3d_bundle PROPERTY CXX_STANDARD 17)

  add_executable(verify_correctness_matrix_sqzc3d samples/verify_correctness_matrix_sqzc3d.cpp)
  target_link_libraries(verify_correctness_matrix_sqzc3d PRIVATE sqzc3d)
  set_property(TARGET verify_correctness_matrix_sqzc3d PROPERTY CXX_STANDARD 17)

  add_executable(easy_window_sqzc3d samples/easy_window_sqzc3d.cpp)
  target_link_libraries(easy_window_sqzc3d PRIVATE sqzc3d)
  set_property(TARGET easy_window_sqzc3d PROPERTY CXX_STANDARD 17)

  if (SQZC3D_WITH_EZC3D)
    add_executable(bundle_roundtrip_sqzc3d samples/bundle_roundtrip_sqzc3d.cpp)
    target_link_libraries(bundle_roundtrip_sqzc3d PRIVATE sqzc3d)
    set_property(TARGET bundle_roundtrip_sqzc3d PROPERTY CXX_STANDARD 17)
  endif()
endif()


