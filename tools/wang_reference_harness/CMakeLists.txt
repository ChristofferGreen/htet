cmake_minimum_required(VERSION 3.15)
project(wang_reference_harness LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(BUILD_LIBRARY ON CACHE BOOL "" FORCE)
set(BUILD_BW_TESTS OFF CACHE BOOL "" FORCE)

if(APPLE)
  add_compile_options(
    "$<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_LIST_DIR}/macos_dt_api_compat.h>")
endif()

get_filename_component(TETRA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(WANG_REFERENCE_ROOT "${TETRA_ROOT}/third_party/FHCCT-WYFDT_TEST")
if(NOT EXISTS "${WANG_REFERENCE_ROOT}/src/dt.cpp")
  message(FATAL_ERROR "Pinned FHCCT-WYFDT_TEST checkout is missing")
endif()

add_subdirectory("${WANG_REFERENCE_ROOT}" author_reference)

add_executable(wang_author_reference_probe wang_author_reference_probe.cpp)
target_include_directories(wang_author_reference_probe PRIVATE
  "${WANG_REFERENCE_ROOT}/src"
  "${WANG_REFERENCE_ROOT}/extern/geom"
  "${WANG_REFERENCE_ROOT}/extern/cli11"
  "${WANG_REFERENCE_ROOT}/extern/eigen"
  "${WANG_REFERENCE_ROOT}/extern/spdlog/include"
  "${WANG_REFERENCE_ROOT}/extern/tree")
target_link_libraries(wang_author_reference_probe PRIVATE dt)
