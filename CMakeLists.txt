cmake_minimum_required(VERSION 3.24)

project(tetra_experiments VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TETRA_BUILD_VIEWER "Build the Vulkan tetrahedral viewer" ON)
option(TETRA_ENABLE_SIMD "Enable architecture-specific SIMD kernels" ON)

include(FetchContent)

FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(doctest)

add_library(tetra_core
  src/tetra_core/adjacency.cpp
  src/tetra_core/four_hexahedra.cpp
  src/tetra_core/geometry_executor.cpp
  src/tetra_core/green_templates.cpp
  src/tetra_core/layer_storage.cpp
  src/tetra_core/mixed_depth_dual.cpp
  src/tetra_core/parallel_commit.cpp
  src/tetra_core/tet_mesh.cpp
  src/tetra_core/implicit_surface.cpp
  src/tetra_core/world_hierarchy.cpp
  src/tetra_core/world_cut_directory.cpp
  src/tetra_core/whole_cell_surface.cpp)
add_library(tetra::core ALIAS tetra_core)
target_include_directories(tetra_core PUBLIC src)
target_compile_features(tetra_core PUBLIC cxx_std_23)
if(TETRA_ENABLE_SIMD)
  target_compile_definitions(tetra_core PRIVATE TETRA_ENABLE_SIMD=1)
endif()
target_compile_options(tetra_core PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

# The interactive viewer and tests share this graphics-free command runner.
# Keeping it independent of GLFW and Vulkan makes headless validation usable
# even when the viewer itself cannot be built or launched.
add_library(tetra_viewer_script
  src/tetra_viewer/atmosphere.cpp
  src/tetra_viewer/camera_manipulator.cpp
  src/tetra_viewer/first_person_controller.cpp
  src/tetra_viewer/mesh_update_worker.cpp
  src/tetra_viewer/projection.cpp
  src/tetra_viewer/scene_preparation_worker.cpp
  src/tetra_viewer/terrain_runtime.cpp
  src/tetra_viewer/viewer_script.cpp
  src/tetra_viewer/viewer_scene.cpp
  src/tetra_viewer/world_script.cpp)
target_include_directories(tetra_viewer_script PUBLIC src)
target_link_libraries(tetra_viewer_script PUBLIC tetra::core)
target_compile_options(tetra_viewer_script PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

# Reserved integration point for mesh/experiment persistence. It is intentionally
# header-only until the first file format is selected.
add_library(tetra_io INTERFACE)
add_library(tetra::io ALIAS tetra_io)
target_link_libraries(tetra_io INTERFACE tetra::core)

add_executable(tetra_simd_benchmark benchmarks/simd_benchmark.cpp)
target_link_libraries(tetra_simd_benchmark PRIVATE tetra::core)
target_compile_options(tetra_simd_benchmark PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(tetra_world_transaction_benchmark
  benchmarks/world_transaction_benchmark.cpp)
target_link_libraries(tetra_world_transaction_benchmark PRIVATE tetra::core)
target_compile_options(tetra_world_transaction_benchmark PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(tetra_derived_surface_benchmark
  benchmarks/derived_surface_benchmark.cpp)
target_link_libraries(tetra_derived_surface_benchmark PRIVATE tetra_viewer_script)
target_compile_options(tetra_derived_surface_benchmark PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

enable_testing()
add_executable(tetra_tests tests/tetra_core/tet_mesh_tests.cpp)
target_link_libraries(tetra_tests PRIVATE tetra_viewer_script doctest::doctest)
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(tetra_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

if(TETRA_BUILD_VIEWER)
  if(APPLE)
    find_path(Vulkan_INCLUDE_DIR vulkan/vulkan.h HINTS /opt/homebrew/opt/vulkan-headers/include /opt/homebrew/opt/molten-vk/libexec/include)
    find_library(Vulkan_LIBRARY vulkan HINTS /opt/homebrew/opt/vulkan-loader/lib)
  endif()
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    add_subdirectory(src/tetra_viewer)
  else()
    message(STATUS "Vulkan SDK not found; tetra_viewer is disabled. Install a Vulkan SDK and reconfigure to enable it.")
  endif()
endif()
