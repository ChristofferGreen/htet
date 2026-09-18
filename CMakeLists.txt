cmake_minimum_required(VERSION 3.24)

project(tetra_experiments VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TETRA_BUILD_VIEWER "Build the Vulkan tetrahedral viewer" ON)
if(APPLE)
  option(TETRA_BUILD_METAL "Build the native macOS Metal world viewer" ON)
else()
  option(TETRA_BUILD_METAL "Build the native macOS Metal world viewer" OFF)
endif()
option(TETRA_ENABLE_SIMD "Enable architecture-specific SIMD kernels" ON)
option(TETRA_BUILD_WANG_AUTHOR_ORACLE
  "Build the pinned Wang author implementation for differential tests" OFF)

include(FetchContent)

if(TETRA_BUILD_WANG_AUTHOR_ORACLE)
  set(BUILD_EXECUTABLE OFF CACHE BOOL "" FORCE)
  set(BUILD_LIBRARY ON CACHE BOOL "" FORCE)
  set(BUILD_BW_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/FHCCT-WYFDT_TEST EXCLUDE_FROM_ALL)
  set_target_properties(dt_core dt PROPERTIES
    CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON)
  if(APPLE)
    target_compile_options(dt_core PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_SOURCE_DIR}/third_party/FHCCT-WYFDT_TEST/build-local/macos_dt_api_compat.h>")
    target_compile_options(dt PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_SOURCE_DIR}/third_party/FHCCT-WYFDT_TEST/build-local/macos_dt_api_compat.h>")
  endif()
endif()

FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(doctest)

add_library(tetra_core
  src/tetra_core/bounded_front_buffer.cpp
  src/tetra_core/adjacency.cpp
  src/tetra_core/four_hexahedra.cpp
  src/tetra_core/geometry_executor.cpp
  src/tetra_core/green_templates.cpp
  src/tetra_core/gpu_hierarchy_snapshot.cpp
  src/tetra_core/layer_storage.cpp
  src/tetra_core/mixed_depth_dual.cpp
  src/tetra_core/parallel_commit.cpp
  src/tetra_core/regular_core_refinement.cpp
  src/tetra_core/regular_core_arbitrary_refinement.cpp
  src/tetra_core/regular_core_refinement_geometry.cpp
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

# Small, isolated research helpers.  They are deliberately not part of
# tetra_core: passing a probe is a precondition for promoting an algorithm.
add_library(tetra_probe_support
  src/tetra_probes/dual_locality_probe.cpp
  src/tetra_probes/dual_embedding_probe.cpp
  src/tetra_probes/plc_chunk_probe.cpp
  src/tetra_probes/sandwich_probe.cpp
  src/tetra_probes/closed_plc_tetrahedralizer.cpp
  src/tetra_probes/dc_free_volume.cpp
  src/tetra_probes/bcc_transition_request.cpp
  src/tetra_probes/refined_core_transaction.cpp
  src/tetra_probes/nonmatching_plc_manifest.cpp
  src/tetra_probes/nonmatching_red_frustum_control.cpp
  src/tetra_probes/canonical_delaunay_seed.cpp
  src/tetra_probes/wang_ordered_tet_mesh.cpp
  src/tetra_probes/wang_local_segment_recovery.cpp
  src/tetra_probes/wang_segment_scheduler.cpp
  src/tetra_probes/wang_constrained_tetrahedralizer.cpp
  src/tetra_probes/exact_binary_predicates.cpp
  src/tetra_probes/terrain_volume_request.cpp
  src/tetra_probes/surface_core_contract.cpp
  src/tetra_probes/advancing_front_fixture.cpp
  src/tetra_probes/advancing_front_step.cpp
  src/tetra_probes/advancing_front_elementary.cpp
  src/tetra_probes/advancing_front_fill.cpp
  src/tetra_probes/advancing_front_pocket.cpp)
target_include_directories(tetra_probe_support PUBLIC src)
target_link_libraries(tetra_probe_support PUBLIC tetra::core)
if(TETRA_BUILD_WANG_AUTHOR_ORACLE)
  target_compile_definitions(tetra_probe_support PRIVATE TETRA_ENABLE_WANG_AUTHOR_ORACLE=1)
  target_include_directories(tetra_probe_support PRIVATE
    third_party/FHCCT-WYFDT_TEST/extern/geom
    third_party/FHCCT-WYFDT_TEST/src
    third_party/FHCCT-WYFDT_TEST/extern/cli11
    third_party/FHCCT-WYFDT_TEST/extern/eigen
    third_party/FHCCT-WYFDT_TEST/extern/spdlog/include
    third_party/FHCCT-WYFDT_TEST/extern/tree)
  target_link_libraries(tetra_probe_support PRIVATE dt)
endif()
target_compile_features(tetra_probe_support PUBLIC cxx_std_23)
target_compile_options(tetra_probe_support PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
if(APPLE AND TETRA_BUILD_WANG_AUTHOR_ORACLE)
  target_compile_options(tetra_probe_support PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_SOURCE_DIR}/third_party/FHCCT-WYFDT_TEST/build-local/macos_dt_api_compat.h>")
endif()

add_executable(dual_locality_probe scripts/dual_locality_probe.cpp)
target_link_libraries(dual_locality_probe PRIVATE tetra_probe_support)
target_compile_options(dual_locality_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(dual_embedding_probe scripts/dual_embedding_probe.cpp)
target_link_libraries(dual_embedding_probe PRIVATE tetra_probe_support)
target_compile_options(dual_embedding_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(strict_dual_placement_probe scripts/strict_dual_placement_probe.cpp)
target_link_libraries(strict_dual_placement_probe PRIVATE tetra_probe_support)
target_compile_options(strict_dual_placement_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(plc_chunk_probe scripts/plc_chunk_probe.cpp)
target_link_libraries(plc_chunk_probe PRIVATE tetra_probe_support)
target_compile_options(plc_chunk_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(tetra_sandwich_probe scripts/tetra_sandwich_probe.cpp)
target_link_libraries(tetra_sandwich_probe PRIVATE tetra_probe_support)
target_compile_options(tetra_sandwich_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(advancing_front_fixture_probe scripts/advancing_front_fixture_probe.cpp)
target_link_libraries(advancing_front_fixture_probe PRIVATE tetra_probe_support)
target_compile_options(advancing_front_fixture_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(advancing_front_planar_probe scripts/advancing_front_planar_probe.cpp)
target_link_libraries(advancing_front_planar_probe PRIVATE tetra_probe_support)
target_compile_options(advancing_front_planar_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(wang_planar_probe scripts/wang_planar_probe.cpp)
target_link_libraries(wang_planar_probe PRIVATE tetra_probe_support)
target_compile_options(wang_planar_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

# Diagnostic-only sweep of real terrain requests through the owned Wang path.
add_executable(terrain_wang_probe scripts/terrain_wang_probe.cpp)
target_link_libraries(terrain_wang_probe PRIVATE tetra_probe_support)
target_compile_options(terrain_wang_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

# Exports the generated DC/core fixture as an interchange surface for the
# pinned-author seed differential.  It is diagnostic-only: production never
# consumes this VTK representation.
add_executable(wang_fixture_surface_export scripts/wang_fixture_surface_export.cpp)
target_link_libraries(wang_fixture_surface_export PRIVATE tetra_probe_support)
target_compile_options(wang_fixture_surface_export PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

# Reproducible layer-by-layer VTK package for the contained-sphere N5 demo.
add_executable(wang_prototype_demo_export scripts/wang_prototype_demo_export.cpp)
target_link_libraries(wang_prototype_demo_export PRIVATE tetra_probe_support)
target_compile_options(wang_prototype_demo_export PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(wang_prototype_reference_probe
  scripts/wang_prototype_reference_probe.cpp)
target_link_libraries(wang_prototype_reference_probe PRIVATE tetra_probe_support)
target_compile_options(wang_prototype_reference_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(arbitrary_buffer_viewer_control scripts/arbitrary_buffer_viewer_control.cpp)
target_link_libraries(arbitrary_buffer_viewer_control PRIVATE tetra::core)
target_compile_options(arbitrary_buffer_viewer_control PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(dc_chunk_interface_probe scripts/dc_chunk_interface_probe.cpp)
target_link_libraries(dc_chunk_interface_probe PRIVATE tetra_probe_support)
target_compile_options(dc_chunk_interface_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(dc_chunk_locality_probe scripts/dc_chunk_locality_probe.cpp)
target_link_libraries(dc_chunk_locality_probe PRIVATE tetra_probe_support)
target_compile_options(dc_chunk_locality_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

# The interactive viewer and tests share this graphics-free command runner.
# Keeping it independent of GLFW and Vulkan makes headless validation usable
# even when the viewer itself cannot be built or launched.
add_library(tetra_viewer_script
  src/tetra_viewer/atmosphere.cpp
  src/tetra_viewer/atmosphere_shadow_front.cpp
  src/tetra_viewer/camera_manipulator.cpp
  src/tetra_viewer/first_person_controller.cpp
  src/tetra_viewer/image_oracle.cpp
  src/tetra_viewer/mesh_update_worker.cpp
  src/tetra_viewer/projection.cpp
  src/tetra_viewer/preview_surface.cpp
  src/tetra_viewer/preview_surface_worker.cpp
  src/tetra_viewer/scene_preparation_worker.cpp
  src/tetra_viewer/shadow_cascades.cpp
  src/tetra_viewer/terrain_display_front.cpp
  src/tetra_viewer/terrain_front_coordinator.cpp
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

add_executable(regular_core_refinement_tests tests/tetra_core/regular_core_refinement_tests.cpp)
target_link_libraries(regular_core_refinement_tests PRIVATE tetra::core doctest::doctest)
target_compile_options(regular_core_refinement_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME regular_core_refinement_tests COMMAND regular_core_refinement_tests)

add_executable(regular_core_arbitrary_refinement_tests tests/tetra_core/regular_core_arbitrary_refinement_tests.cpp)
target_link_libraries(regular_core_arbitrary_refinement_tests PRIVATE tetra::core doctest::doctest)
target_compile_options(regular_core_arbitrary_refinement_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME regular_core_arbitrary_refinement_tests COMMAND regular_core_arbitrary_refinement_tests)

add_executable(bounded_front_buffer_tests tests/tetra_core/bounded_front_buffer_tests.cpp)
target_link_libraries(bounded_front_buffer_tests PRIVATE tetra::core doctest::doctest)
target_compile_options(bounded_front_buffer_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME bounded_front_buffer_tests COMMAND bounded_front_buffer_tests)

add_executable(regular_core_refinement_geometry_tests tests/tetra_core/regular_core_refinement_geometry_tests.cpp)
target_link_libraries(regular_core_refinement_geometry_tests PRIVATE tetra::core doctest::doctest)
target_compile_options(regular_core_refinement_geometry_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME regular_core_refinement_geometry_tests COMMAND regular_core_refinement_geometry_tests)

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
add_executable(tetra_tests
  tests/tetra_core/tet_mesh_tests.cpp
  tests/tetra_core/preview_surface_tests.cpp)
target_link_libraries(tetra_tests PRIVATE tetra_viewer_script doctest::doctest)
add_executable(dual_locality_probe_tests tests/tetra_probes/dual_locality_probe_tests.cpp)
target_link_libraries(dual_locality_probe_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME dual_locality_probe_tests COMMAND dual_locality_probe_tests)
add_executable(dual_embedding_probe_tests tests/tetra_probes/dual_embedding_probe_tests.cpp)
target_link_libraries(dual_embedding_probe_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME dual_embedding_probe_tests COMMAND dual_embedding_probe_tests)
add_executable(plc_chunk_probe_tests tests/tetra_probes/plc_chunk_probe_tests.cpp)
target_link_libraries(plc_chunk_probe_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME plc_chunk_probe_tests COMMAND plc_chunk_probe_tests)
add_executable(sandwich_probe_tests tests/tetra_probes/sandwich_probe_tests.cpp)
target_link_libraries(sandwich_probe_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME sandwich_probe_tests COMMAND sandwich_probe_tests)
add_executable(structured_two_hex_dc_tests
  tests/tetra_probes/structured_two_hex_dc_tests.cpp)
target_link_libraries(structured_two_hex_dc_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME structured_two_hex_dc_tests COMMAND structured_two_hex_dc_tests)
add_executable(closed_plc_tetrahedralizer_tests
  tests/tetra_probes/closed_plc_tetrahedralizer_tests.cpp)
target_link_libraries(closed_plc_tetrahedralizer_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME closed_plc_tetrahedralizer_tests COMMAND closed_plc_tetrahedralizer_tests)
add_executable(dc_free_volume_tests tests/tetra_probes/dc_free_volume_tests.cpp)
target_link_libraries(dc_free_volume_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME dc_free_volume_tests COMMAND dc_free_volume_tests)
add_executable(surface_core_contract_tests tests/tetra_probes/surface_core_contract_tests.cpp)
target_link_libraries(surface_core_contract_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME surface_core_contract_tests COMMAND surface_core_contract_tests)
add_executable(advancing_front_fixture_tests tests/tetra_probes/advancing_front_fixture_tests.cpp)
target_link_libraries(advancing_front_fixture_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME advancing_front_fixture_tests COMMAND advancing_front_fixture_tests)
add_executable(advancing_front_step_tests tests/tetra_probes/advancing_front_step_tests.cpp)
target_link_libraries(advancing_front_step_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME advancing_front_step_tests COMMAND advancing_front_step_tests)
add_executable(advancing_front_elementary_tests tests/tetra_probes/advancing_front_elementary_tests.cpp)
target_link_libraries(advancing_front_elementary_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME advancing_front_elementary_tests COMMAND advancing_front_elementary_tests)
add_executable(advancing_front_planar_tests tests/tetra_probes/advancing_front_planar_tests.cpp)
target_link_libraries(advancing_front_planar_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME advancing_front_planar_tests COMMAND advancing_front_planar_tests)
add_executable(advancing_front_pocket_probe scripts/advancing_front_pocket_probe.cpp)
target_link_libraries(advancing_front_pocket_probe PRIVATE tetra_probe_support)
target_compile_options(advancing_front_pocket_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_executable(advancing_front_pocket_tests tests/tetra_probes/advancing_front_pocket_tests.cpp)
target_link_libraries(advancing_front_pocket_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME advancing_front_pocket_tests COMMAND advancing_front_pocket_tests)
add_executable(nonmatching_plc_manifest_tests tests/tetra_probes/nonmatching_plc_manifest_tests.cpp)
target_link_libraries(nonmatching_plc_manifest_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME nonmatching_plc_manifest_tests COMMAND nonmatching_plc_manifest_tests)
add_executable(nonmatching_surface_core_plc_control_tests tests/tetra_probes/nonmatching_surface_core_plc_control_tests.cpp)
target_link_libraries(nonmatching_surface_core_plc_control_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME nonmatching_surface_core_plc_control_tests COMMAND nonmatching_surface_core_plc_control_tests)
add_executable(nonmatching_red_frustum_control_tests tests/tetra_probes/nonmatching_red_frustum_control_tests.cpp)
target_link_libraries(nonmatching_red_frustum_control_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME nonmatching_red_frustum_control_tests COMMAND nonmatching_red_frustum_control_tests)

add_executable(canonical_delaunay_seed_tests tests/tetra_probes/canonical_delaunay_seed_tests.cpp)
target_link_libraries(canonical_delaunay_seed_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME canonical_delaunay_seed_tests COMMAND canonical_delaunay_seed_tests)
add_executable(wang_owned_scheduler_production_tests
  tests/tetra_probes/wang_owned_scheduler_production_tests.cpp)
target_link_libraries(wang_owned_scheduler_production_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME wang_owned_scheduler_production_tests
  COMMAND wang_owned_scheduler_production_tests)
if(TETRA_BUILD_WANG_AUTHOR_ORACLE)
  add_executable(wang_constrained_tetrahedralizer_tests tests/tetra_probes/wang_constrained_tetrahedralizer_tests.cpp)
  target_link_libraries(wang_constrained_tetrahedralizer_tests PRIVATE tetra_probe_support doctest::doctest)
  add_test(NAME wang_constrained_tetrahedralizer_tests COMMAND wang_constrained_tetrahedralizer_tests)
endif()
add_executable(exact_binary_predicates_tests tests/tetra_probes/exact_binary_predicates_tests.cpp)
target_link_libraries(exact_binary_predicates_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME exact_binary_predicates_tests COMMAND exact_binary_predicates_tests)
add_executable(terrain_volume_request_tests tests/tetra_probes/terrain_volume_request_tests.cpp)
target_link_libraries(terrain_volume_request_tests PRIVATE tetra_probe_support doctest::doctest)
add_test(NAME terrain_volume_request_tests COMMAND terrain_volume_request_tests)

add_executable(implicit_core_refinement_transaction_probe_tests tests/tetra_probes/implicit_core_refinement_transaction_probe_tests.cpp)
target_include_directories(implicit_core_refinement_transaction_probe_tests PRIVATE src)
target_link_libraries(implicit_core_refinement_transaction_probe_tests PRIVATE tetra_probe_support tetra::core doctest::doctest)
target_compile_options(implicit_core_refinement_transaction_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME implicit_core_refinement_transaction_probe_tests COMMAND implicit_core_refinement_transaction_probe_tests)
set_tests_properties(implicit_core_refinement_transaction_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The retained artifact probes include the current hierarchy implementation
# directly. `tetra_probe_support` already exports `tetra_core` as a PUBLIC
# dependency, so adding both here creates duplicate static-library entries on
# every later probe link line.
link_libraries(tetra_probe_support)

# The retained TetGen outputs are evidence for the external CPU oracle, not a
# runtime dependency.  Keep their quality rejection executable so S4 cannot
# be advanced by confusing geometric validity with usable elements.
add_executable(shell_reference_quality_tests tests/tetra_probes/shell_reference_quality_tests.cpp)
target_include_directories(shell_reference_quality_tests PRIVATE src)
target_link_libraries(shell_reference_quality_tests PRIVATE tetra_core doctest::doctest)
target_compile_options(shell_reference_quality_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME shell_reference_quality_tests COMMAND shell_reference_quality_tests)
set_tests_properties(shell_reference_quality_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Focused S4 classifier/control: it must keep the quality failure and reject
# unbounded TetGen refinement as the repair direction.
add_executable(quality_repair_probe_tests tests/tetra_probes/quality_repair_probe_tests.cpp)
target_include_directories(quality_repair_probe_tests PRIVATE src)
target_link_libraries(quality_repair_probe_tests PRIVATE tetra_core doctest::doctest)
target_compile_options(quality_repair_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME quality_repair_probe_tests COMMAND quality_repair_probe_tests)
set_tests_properties(quality_repair_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# A distinct S4 negative-control family: bounded strictly interior Steiner
# insertion with one-tet cavity retetrahedralization, never a global remesher.
add_executable(steiner_cavity_probe_tests tests/tetra_probes/steiner_cavity_probe_tests.cpp)
target_include_directories(steiner_cavity_probe_tests PRIVATE src)
target_link_libraries(steiner_cavity_probe_tests PRIVATE tetra_core doctest::doctest)
target_compile_options(steiner_cavity_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME steiner_cavity_probe_tests COMMAND steiner_cavity_probe_tests)
set_tests_properties(steiner_cavity_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# A more expressive but still bounded S4 negative control: a two-tet cavity
# with finite interface-aware Steiner sites, not a global remesher.
add_executable(multitet_cavity_probe_tests tests/tetra_probes/multitet_cavity_probe_tests.cpp)
target_include_directories(multitet_cavity_probe_tests PRIVATE src)
target_link_libraries(multitet_cavity_probe_tests PRIVATE tetra_core doctest::doctest)
target_compile_options(multitet_cavity_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME multitet_cavity_probe_tests COMMAND multitet_cavity_probe_tests)
set_tests_properties(multitet_cavity_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Structural S4 hypothesis: a bounded, field-normal two-front collar made
# directly from the frozen DC mesh.  This is deliberately collar-only; the
# test prevents it being confused with an already solved grid-core bridge.
add_executable(two_front_transition_probe_tests tests/tetra_probes/two_front_transition_probe_tests.cpp)
target_include_directories(two_front_transition_probe_tests PRIVATE src)
target_link_libraries(two_front_transition_probe_tests PRIVATE tetra_core doctest::doctest)
target_compile_options(two_front_transition_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME two_front_transition_probe_tests COMMAND two_front_transition_probe_tests)
set_tests_properties(two_front_transition_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Reproducible independent-chunk qualification for the fixed-depth two-front
# collar.  This is a collar interface probe; it does not claim a grid bridge.
add_executable(canonical_chunk_collar_probe artifacts/dc-viability-2026-09-09/two_front_transition_probe.cpp)
target_include_directories(canonical_chunk_collar_probe PRIVATE src)
target_link_libraries(canonical_chunk_collar_probe PRIVATE tetra_core)
target_compile_options(canonical_chunk_collar_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

# Deterministic input contract for the external collar -> fill -> retained-core
# quality oracle. TetGen itself remains a caller-supplied research dependency.
add_executable(two_front_complete_oracle_tests tests/tetra_probes/two_front_complete_oracle_tests.cpp)
target_include_directories(two_front_complete_oracle_tests PRIVATE src)
target_link_libraries(two_front_complete_oracle_tests PRIVATE tetra_core doctest::doctest)
target_compile_options(two_front_complete_oracle_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME two_front_complete_oracle_tests COMMAND two_front_complete_oracle_tests)
set_tests_properties(two_front_complete_oracle_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# One complete retained N6 external reference has one authoritative exterior
# and core-interface contract.  It must pass independent geometry checks yet
# remain quality-rejected, with corrupted-domain controls kept executable.
add_executable(complete_n6_domain_harness_tests tests/tetra_probes/complete_n6_domain_harness_tests.cpp)
target_include_directories(complete_n6_domain_harness_tests PRIVATE src)
target_link_libraries(complete_n6_domain_harness_tests PRIVATE tetra_core doctest::doctest)
target_compile_options(complete_n6_domain_harness_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME complete_n6_domain_harness_tests COMMAND complete_n6_domain_harness_tests)
set_tests_properties(complete_n6_domain_harness_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Every bounded N6 collar/buffer candidate is measured by the complete-domain
# oracle and exhaustive S4, beginning with the geometry-valid quality-rejected
# retained reference baseline.
add_executable(bounded_n6_authoritative_joint_probe_tests tests/tetra_probes/bounded_n6_authoritative_joint_probe_tests.cpp)
target_include_directories(bounded_n6_authoritative_joint_probe_tests PRIVATE src)
target_link_libraries(bounded_n6_authoritative_joint_probe_tests PRIVATE doctest::doctest)
target_compile_options(bounded_n6_authoritative_joint_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME bounded_n6_authoritative_joint_probe_tests COMMAND bounded_n6_authoritative_joint_probe_tests)
set_tests_properties(bounded_n6_authoritative_joint_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Exact visible DC triangles allow interior edge fans; a boundary wedge alone
# is diagnostic geometry, not an exhaustive-S4 impossibility proof.
add_executable(n6_boundary_edge_fan_control_tests tests/tetra_probes/n6_frozen_boundary_s4_preflight_probe_tests.cpp)
target_include_directories(n6_boundary_edge_fan_control_tests PRIVATE src)
target_link_libraries(n6_boundary_edge_fan_control_tests PRIVATE doctest::doctest)
target_compile_options(n6_boundary_edge_fan_control_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_boundary_edge_fan_control_tests COMMAND n6_boundary_edge_fan_control_tests)
set_tests_properties(n6_boundary_edge_fan_control_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# First actual bounded N6 edge-star cavity candidate. It may reject on the
# authoritative oracle, but must keep its contract and controls executable.
add_executable(n6_boundary_edge_star_cavity_probe_tests tests/tetra_probes/n6_boundary_edge_star_cavity_probe_tests.cpp)
target_include_directories(n6_boundary_edge_star_cavity_probe_tests PRIVATE src)
target_link_libraries(n6_boundary_edge_star_cavity_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_boundary_edge_star_cavity_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_boundary_edge_star_cavity_probe_tests COMMAND n6_boundary_edge_star_cavity_probe_tests)
set_tests_properties(n6_boundary_edge_star_cavity_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The actual exhaustive-S4 offender is selected by exact tet index and rebuilt
# as the smallest closed cavity, retaining every declared N6 interface.
add_executable(n6_worst_quality_cavity_probe_tests tests/tetra_probes/n6_worst_quality_cavity_probe_tests.cpp)
target_include_directories(n6_worst_quality_cavity_probe_tests PRIVATE src)
target_link_libraries(n6_worst_quality_cavity_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_worst_quality_cavity_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_worst_quality_cavity_probe_tests COMMAND n6_worst_quality_cavity_probe_tests)
set_tests_properties(n6_worst_quality_cavity_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(n6_quality_star_optimization_probe_tests tests/tetra_probes/n6_quality_star_optimization_probe_tests.cpp)
target_include_directories(n6_quality_star_optimization_probe_tests PRIVATE src)
target_link_libraries(n6_quality_star_optimization_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_quality_star_optimization_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_quality_star_optimization_probe_tests COMMAND n6_quality_star_optimization_probe_tests)
set_tests_properties(n6_quality_star_optimization_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(n6_batched_cavity_remesh_probe_tests tests/tetra_probes/n6_batched_cavity_remesh_probe_tests.cpp)
target_include_directories(n6_batched_cavity_remesh_probe_tests PRIVATE src)
target_link_libraries(n6_batched_cavity_remesh_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_batched_cavity_remesh_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_batched_cavity_remesh_probe_tests COMMAND n6_batched_cavity_remesh_probe_tests)
set_tests_properties(n6_batched_cavity_remesh_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(n6_nonstar_cavity_tetrahedralizer_probe_tests tests/tetra_probes/n6_nonstar_cavity_tetrahedralizer_probe_tests.cpp)
target_include_directories(n6_nonstar_cavity_tetrahedralizer_probe_tests PRIVATE src)
target_link_libraries(n6_nonstar_cavity_tetrahedralizer_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_nonstar_cavity_tetrahedralizer_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_nonstar_cavity_tetrahedralizer_probe_tests COMMAND n6_nonstar_cavity_tetrahedralizer_probe_tests)
set_tests_properties(n6_nonstar_cavity_tetrahedralizer_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_executable(n6_final_three_regions_probe_tests tests/tetra_probes/n6_final_three_regions_probe_tests.cpp)
target_include_directories(n6_final_three_regions_probe_tests PRIVATE src)
target_link_libraries(n6_final_three_regions_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_final_three_regions_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_final_three_regions_probe_tests COMMAND n6_final_three_regions_probe_tests)
set_tests_properties(n6_final_three_regions_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Contract controls for the whole-volume audit.  A tetrahedron's exposed
# faces form a valid closed boundary, so a face-count-zero predicate is not a
# closure test.
add_executable(closed_boundary_controls_tests tests/tetra_probes/closed_boundary_controls_tests.cpp)
target_include_directories(closed_boundary_controls_tests PRIVATE src)
target_link_libraries(closed_boundary_controls_tests PRIVATE doctest::doctest)
target_compile_options(closed_boundary_controls_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME closed_boundary_controls_tests COMMAND closed_boundary_controls_tests)
set_tests_properties(closed_boundary_controls_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Bounded structural rejection control: the qualified normal-offset collar is
# carried to a shared, exact regular-core top plane.  A noisy vertical DC step
# must reject rather than be concealed by duplicate core coordinates.
add_executable(two_front_core_bridge_probe_tests tests/tetra_probes/two_front_core_bridge_probe_tests.cpp)
target_include_directories(two_front_core_bridge_probe_tests PRIVATE src)
target_link_libraries(two_front_core_bridge_probe_tests PRIVATE doctest::doctest)
target_compile_options(two_front_core_bridge_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME two_front_core_bridge_probe_tests COMMAND two_front_core_bridge_probe_tests)
set_tests_properties(two_front_core_bridge_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The complete in-process N6 assembly attempts the collar, retained core, and
# smallest direct bridge together.  It is a validated rejection witness until
# a joint internal-front retriangulator replaces the collapsed bridge.
add_executable(bounded_n6_joint_transition_probe_tests tests/tetra_probes/bounded_n6_joint_transition_probe_tests.cpp)
target_include_directories(bounded_n6_joint_transition_probe_tests PRIVATE src)
target_link_libraries(bounded_n6_joint_transition_probe_tests PRIVATE doctest::doctest)
target_compile_options(bounded_n6_joint_transition_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME bounded_n6_joint_transition_probe_tests COMMAND bounded_n6_joint_transition_probe_tests)
set_tests_properties(bounded_n6_joint_transition_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The first actual bounded local retriangulation tries the quotient geometry of
# the four collapsed N6 prisms together.  It must reject shared duplicate
# cavity tets before it can be mistaken for a complete transition.
add_executable(bounded_n6_joint_retriangulator_probe_tests tests/tetra_probes/bounded_n6_joint_retriangulator_probe_tests.cpp)
target_include_directories(bounded_n6_joint_retriangulator_probe_tests PRIVATE src)
target_link_libraries(bounded_n6_joint_retriangulator_probe_tests PRIVATE doctest::doctest)
target_compile_options(bounded_n6_joint_retriangulator_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME bounded_n6_joint_retriangulator_probe_tests COMMAND bounded_n6_joint_retriangulator_probe_tests)
set_tests_properties(bounded_n6_joint_retriangulator_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The four collapsed N6 quotient cells must first become one closed cavity
# boundary.  This shared-owner control rejects if their union is not a valid
# boundary for a single retriangulation.
add_executable(shared_n6_multiprism_cavity_probe_tests tests/tetra_probes/shared_n6_multiprism_cavity_probe_tests.cpp)
target_include_directories(shared_n6_multiprism_cavity_probe_tests PRIVATE src)
target_link_libraries(shared_n6_multiprism_cavity_probe_tests PRIVATE doctest::doctest)
target_compile_options(shared_n6_multiprism_cavity_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME shared_n6_multiprism_cavity_probe_tests COMMAND shared_n6_multiprism_cavity_probe_tests)
set_tests_properties(shared_n6_multiprism_cavity_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# First interface-aware follow-up: place the actual terraced retained-core
# front and the prior shared buffer in one finite request, then reject their
# disconnected exact interfaces before emitting an invalid bridge.
add_executable(terraced_core_shared_buffer_probe_tests tests/tetra_probes/terraced_core_shared_buffer_probe_tests.cpp)
target_include_directories(terraced_core_shared_buffer_probe_tests PRIVATE src)
target_link_libraries(terraced_core_shared_buffer_probe_tests PRIVATE doctest::doctest)
target_compile_options(terraced_core_shared_buffer_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME terraced_core_shared_buffer_probe_tests COMMAND terraced_core_shared_buffer_probe_tests)
set_tests_properties(terraced_core_shared_buffer_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The smallest legal side complex consumes one face from each of the two
# closed N6 fronts and joins them with a single canonical triangular prism.
# It is a rejection control until the resulting joined volume passes audit.
add_executable(n6_connecting_side_complex_probe_tests tests/tetra_probes/n6_connecting_side_complex_probe_tests.cpp)
target_include_directories(n6_connecting_side_complex_probe_tests PRIVATE src)
target_link_libraries(n6_connecting_side_complex_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_connecting_side_complex_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_connecting_side_complex_probe_tests COMMAND n6_connecting_side_complex_probe_tests)
set_tests_properties(n6_connecting_side_complex_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The shared 14-tet quotient fan is not an annular front around the retained
# core.  Keep that whole-front eligibility rejection executable before any
# larger side-complex construction is attempted.
add_executable(n6_disjoint_buffer_core_probe_tests tests/tetra_probes/n6_disjoint_buffer_core_probe_tests.cpp)
target_include_directories(n6_disjoint_buffer_core_probe_tests PRIVATE src)
target_link_libraries(n6_disjoint_buffer_core_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_disjoint_buffer_core_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_disjoint_buffer_core_probe_tests COMMAND n6_disjoint_buffer_core_probe_tests)
set_tests_properties(n6_disjoint_buffer_core_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The artificial collar underside is explicitly removed from the N6 fixed
# boundary.  The remaining visible DC sheet plus fixture curtain is one open
# joint front; this is the input contract for a real collar/core cavity.
add_executable(n6_open_rebuildable_inner_front_probe_tests tests/tetra_probes/n6_open_rebuildable_inner_front_probe_tests.cpp)
target_include_directories(n6_open_rebuildable_inner_front_probe_tests PRIVATE src)
target_link_libraries(n6_open_rebuildable_inner_front_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_open_rebuildable_inner_front_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_open_rebuildable_inner_front_probe_tests COMMAND n6_open_rebuildable_inner_front_probe_tests)
set_tests_properties(n6_open_rebuildable_inner_front_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Smallest real N6 rim-to-core attachment.  It consumes an actual open-rim
# edge and terraced-core face, and keeps the resulting open-rim
# rejection executable until a jointly retriangulated collar neighbourhood is
# available.
add_executable(n6_rim_to_core_cavity_probe_tests tests/tetra_probes/n6_rim_to_core_cavity_probe_tests.cpp)
target_include_directories(n6_rim_to_core_cavity_probe_tests PRIVATE src)
target_link_libraries(n6_rim_to_core_cavity_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_rim_to_core_cavity_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_rim_to_core_cavity_probe_tests COMMAND n6_rim_to_core_cavity_probe_tests)
set_tests_properties(n6_rim_to_core_cavity_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Complete-rim ownership control: the entire rebuildable N6 rim is selected
# before any side faces can be emitted, preventing a return to edge-by-edge
# prism repairs.
add_executable(n6_full_rim_cycle_cavity_probe_tests tests/tetra_probes/n6_full_rim_cycle_cavity_probe_tests.cpp)
target_include_directories(n6_full_rim_cycle_cavity_probe_tests PRIVATE src)
target_link_libraries(n6_full_rim_cycle_cavity_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_full_rim_cycle_cavity_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_full_rim_cycle_cavity_probe_tests COMMAND n6_full_rim_cycle_cavity_probe_tests)
set_tests_properties(n6_full_rim_cycle_cavity_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Bounded exact-core disk-patch policy and the pre-fill common-loop contract.
add_executable(n6_disk_core_patch_refinement_probe_tests tests/tetra_probes/n6_disk_core_patch_refinement_probe_tests.cpp)
target_include_directories(n6_disk_core_patch_refinement_probe_tests PRIVATE src)
target_link_libraries(n6_disk_core_patch_refinement_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_disk_core_patch_refinement_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_disk_core_patch_refinement_probe_tests COMMAND n6_disk_core_patch_refinement_probe_tests)
set_tests_properties(n6_disk_core_patch_refinement_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Explicitly bounded deterministic search for a disk-like exact-core patch
# before a common-loop side complex may be attempted.
add_executable(n6_bounded_disk_patch_search_probe_tests tests/tetra_probes/n6_bounded_disk_patch_search_probe_tests.cpp)
target_include_directories(n6_bounded_disk_patch_search_probe_tests PRIVATE src)
target_link_libraries(n6_bounded_disk_patch_search_probe_tests PRIVATE doctest::doctest)
target_compile_options(n6_bounded_disk_patch_search_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME n6_bounded_disk_patch_search_probe_tests COMMAND n6_bounded_disk_patch_search_probe_tests)
set_tests_properties(n6_bounded_disk_patch_search_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The actual exposed faces of a conservative regular core retain full lattice
# (i,j,k) terraces.  This is intentionally a bounded direct-attachment
# rejection control, not a claim that a buffer bridge is already solved.
add_executable(terraced_core_interface_probe_tests tests/tetra_probes/terraced_core_interface_probe_tests.cpp)
target_include_directories(terraced_core_interface_probe_tests PRIVATE src)
target_link_libraries(terraced_core_interface_probe_tests PRIVATE doctest::doctest)
target_compile_options(terraced_core_interface_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME terraced_core_interface_probe_tests COMMAND terraced_core_interface_probe_tests)
set_tests_properties(terraced_core_interface_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The first common-scaffold buffer attempt is intentionally a qualified
# rejection: zero through two uncut regular-tet rings cannot conform to the
# free normal-offset collar front without a cleaving topology change.
add_executable(scaffolded_interfront_buffer_probe_tests tests/tetra_probes/scaffolded_interfront_buffer_probe_tests.cpp)
target_include_directories(scaffolded_interfront_buffer_probe_tests PRIVATE src)
target_link_libraries(scaffolded_interfront_buffer_probe_tests PRIVATE doctest::doctest)
target_compile_options(scaffolded_interfront_buffer_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME scaffolded_interfront_buffer_probe_tests COMMAND scaffolded_interfront_buffer_probe_tests)
set_tests_properties(scaffolded_interfront_buffer_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# A bounded precondition for a future real cleaver: the documented moated
# core must be disjoint from the free collar, while canonical band cuts remain
# shared and intact regular rings/direct pairing remain rejected.
add_executable(conforming_scaffold_cleaving_probe_tests tests/tetra_probes/conforming_scaffold_cleaving_probe_tests.cpp)
target_include_directories(conforming_scaffold_cleaving_probe_tests PRIVATE src)
target_link_libraries(conforming_scaffold_cleaving_probe_tests PRIVATE doctest::doctest)
target_compile_options(conforming_scaffold_cleaving_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME conforming_scaffold_cleaving_probe_tests COMMAND conforming_scaffold_cleaving_probe_tests)
set_tests_properties(conforming_scaffold_cleaving_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The first real buffer-cleavage primitive: canonical 1:3, 3:1 and 2:2
# plane/tet templates.  The companion corpus scan deliberately retains
# clipped-triangle cases as regression witnesses rather than extending a
# frozen DC triangle across a complete tet plane section.
add_executable(local_buffer_cleaving_witness_tests tests/tetra_probes/local_buffer_cleaving_witness_tests.cpp)
target_include_directories(local_buffer_cleaving_witness_tests PRIVATE src)
target_link_libraries(local_buffer_cleaving_witness_tests PRIVATE doctest::doctest)
target_compile_options(local_buffer_cleaving_witness_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME local_buffer_cleaving_witness_tests COMMAND local_buffer_cleaving_witness_tests)
set_tests_properties(local_buffer_cleaving_witness_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_executable(local_buffer_cleaving_witness_probe artifacts/dc-viability-2026-09-09/local_buffer_cleaving_witness.cpp)
target_include_directories(local_buffer_cleaving_witness_probe PRIVATE src)
target_compile_options(local_buffer_cleaving_witness_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

# The first finite-triangle arrangement stitcher.  It is deliberately a
# two-tet/two-cell reference: the test proves canonical shared-face cut
# identity and a seam-owner rule without claiming a general clipped-patch
# buffer construction.
add_executable(arrangement_aware_cleaving_probe_tests tests/tetra_probes/arrangement_aware_cleaving_probe_tests.cpp)
target_include_directories(arrangement_aware_cleaving_probe_tests PRIVATE src)
target_link_libraries(arrangement_aware_cleaving_probe_tests PRIVATE doctest::doctest)
target_compile_options(arrangement_aware_cleaving_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME arrangement_aware_cleaving_probe_tests COMMAND arrangement_aware_cleaving_probe_tests)
set_tests_properties(arrangement_aware_cleaving_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_executable(arrangement_aware_cleaving_probe artifacts/dc-viability-2026-09-09/arrangement_aware_cleaving_probe.cpp)
target_include_directories(arrangement_aware_cleaving_probe PRIVATE src)
target_compile_options(arrangement_aware_cleaving_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

# Finite clipped-triangle atlas and the first real-DC-patch template.  This
# remains a bounded reference probe, not a general buffer mesher.
add_executable(finite_patch_template_atlas_tests tests/tetra_probes/finite_patch_template_atlas_tests.cpp)
target_include_directories(finite_patch_template_atlas_tests PRIVATE src)
target_link_libraries(finite_patch_template_atlas_tests PRIVATE doctest::doctest)
target_compile_options(finite_patch_template_atlas_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME finite_patch_template_atlas_tests COMMAND finite_patch_template_atlas_tests)
set_tests_properties(finite_patch_template_atlas_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
add_executable(finite_patch_template_atlas artifacts/dc-viability-2026-09-09/finite_patch_template_atlas.cpp)
target_include_directories(finite_patch_template_atlas PRIVATE src)
target_compile_options(finite_patch_template_atlas PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)

add_executable(owner_neighbor_star_template artifacts/dc-viability-2026-09-09/owner_neighbor_star_template.cpp)
target_include_directories(owner_neighbor_star_template PRIVATE src)
target_compile_options(owner_neighbor_star_template PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_executable(owner_neighbor_star_template_tests tests/tetra_probes/owner_neighbor_star_template_tests.cpp)
target_include_directories(owner_neighbor_star_template_tests PRIVATE src)
target_link_libraries(owner_neighbor_star_template_tests PRIVATE doctest::doctest)
target_compile_options(owner_neighbor_star_template_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME owner_neighbor_star_template_tests COMMAND owner_neighbor_star_template_tests)
set_tests_properties(owner_neighbor_star_template_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The transitive frozen-patch closure is a diagnostic gate.  It establishes
# locality/ownership facts before any constrained cavity mesher is allowed.
add_executable(transitive_patch_star_probe artifacts/dc-viability-2026-09-09/transitive_patch_star_probe.cpp)
target_include_directories(transitive_patch_star_probe PRIVATE src)
target_compile_options(transitive_patch_star_probe PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_executable(transitive_patch_star_probe_tests tests/tetra_probes/transitive_patch_star_probe_tests.cpp)
target_include_directories(transitive_patch_star_probe_tests PRIVATE src)
target_link_libraries(transitive_patch_star_probe_tests PRIVATE doctest::doctest)
target_compile_options(transitive_patch_star_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME transitive_patch_star_probe_tests COMMAND transitive_patch_star_probe_tests)
set_tests_properties(transitive_patch_star_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# First S4 refinement step: canonical one-level 2:1 selection around the
# worst DC triangle and an explicitly non-adaptive fine-grid oracle.
add_executable(local_hex_refinement_probe_tests tests/tetra_probes/local_hex_refinement_probe_tests.cpp)
target_include_directories(local_hex_refinement_probe_tests PRIVATE src)
target_link_libraries(local_hex_refinement_probe_tests PRIVATE doctest::doctest)
target_compile_options(local_hex_refinement_probe_tests PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wconversion>)
add_test(NAME local_hex_refinement_probe_tests COMMAND local_hex_refinement_probe_tests)
set_tests_properties(local_hex_refinement_probe_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Stop the retained-probe default link scope before configuring viewer
# dependencies.  Without this reset, GLFW and SPIRV-Cross inherit the private
# research library and their install/export sets become invalid.
set_property(DIRECTORY PROPERTY LINK_LIBRARIES "")

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(tetra_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

set(TETRA_VULKAN_VIEWER_AVAILABLE OFF)
if(TETRA_BUILD_VIEWER)
  if(APPLE)
    find_path(Vulkan_INCLUDE_DIR vulkan/vulkan.h HINTS /opt/homebrew/opt/vulkan-headers/include /opt/homebrew/opt/molten-vk/libexec/include)
    find_library(Vulkan_LIBRARY vulkan HINTS /opt/homebrew/opt/vulkan-loader/lib)
  endif()
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    set(TETRA_VULKAN_VIEWER_AVAILABLE ON)
  else()
    message(STATUS "Vulkan SDK not found; tetra_viewer is disabled. Install a Vulkan SDK and reconfigure to enable it.")
  endif()
endif()

if(APPLE AND TETRA_BUILD_METAL)
  # Homebrew GCC can be selected for the C++ project, but it cannot compile
  # ARC Objective-C++ or Apple's modern Metal headers. Keep ordinary C++ on
  # the user's selected compiler and use the platform compiler for .mm files.
  set(CMAKE_OBJCXX_COMPILER /usr/bin/clang++ CACHE FILEPATH
      "Objective-C++ compiler used by the native Metal target" FORCE)
  enable_language(OBJCXX)
endif()

if(TETRA_VULKAN_VIEWER_AVAILABLE OR (APPLE AND TETRA_BUILD_METAL))
  add_subdirectory(src/tetra_viewer)
endif()
