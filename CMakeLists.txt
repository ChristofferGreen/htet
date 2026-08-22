cmake_minimum_required(VERSION 3.24)

project(tetra_experiments VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TETRA_BUILD_VIEWER "Build the Vulkan tetrahedral viewer" ON)

include(FetchContent)

FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(doctest)

add_library(tetra_core
  src/tetra_core/tet_mesh.cpp
  src/tetra_core/implicit_surface.cpp
  src/tetra_core/whole_cell_surface.cpp)
add_library(tetra::core ALIAS tetra_core)
target_include_directories(tetra_core PUBLIC src)
target_compile_features(tetra_core PUBLIC cxx_std_23)
target_compile_options(tetra_core PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

# The interactive viewer and tests share this graphics-free command runner.
# Keeping it independent of GLFW and Vulkan makes headless validation usable
# even when the viewer itself cannot be built or launched.
add_library(tetra_viewer_script
  src/tetra_viewer/viewer_script.cpp
  src/tetra_viewer/viewer_scene.cpp)
target_include_directories(tetra_viewer_script PUBLIC src)
target_link_libraries(tetra_viewer_script PUBLIC tetra::core)
target_compile_options(tetra_viewer_script PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

# Reserved integration point for mesh/experiment persistence. It is intentionally
# header-only until the first file format is selected.
add_library(tetra_io INTERFACE)
add_library(tetra::io ALIAS tetra_io)
target_link_libraries(tetra_io INTERFACE tetra::core)

enable_testing()
add_executable(tetra_tests tests/tetra_core/tet_mesh_tests.cpp)
target_link_libraries(tetra_tests PRIVATE tetra_viewer_script doctest::doctest)
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(tetra_tests)

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
