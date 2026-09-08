FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.6
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw imgui)

set(TETRA_IMGUI_SOURCES
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp)

if(TETRA_VULKAN_VIEWER_AVAILABLE)
  add_library(imgui STATIC
    ${TETRA_IMGUI_SOURCES}
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)

  add_library(tetra_viewer_application STATIC main.cpp scene_renderer.cpp)
  target_link_libraries(tetra_viewer_application PUBLIC tetra_viewer_script glfw imgui Vulkan::Vulkan)

  add_executable(tetra_viewer viewer_main.cpp)
  target_link_libraries(tetra_viewer PRIVATE tetra_viewer_application)

  add_executable(tetra_world ../tetra_world/main.cpp)
  target_link_libraries(tetra_world PRIVATE tetra_viewer_application)

  find_program(GLSLC glslc REQUIRED)
  set(TETRA_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
  file(MAKE_DIRECTORY ${TETRA_SHADER_DIR})
  add_custom_command(
  OUTPUT ${TETRA_SHADER_DIR}/scene.vert.spv ${TETRA_SHADER_DIR}/scene.frag.spv
         ${TETRA_SHADER_DIR}/wire.frag.spv
         ${TETRA_SHADER_DIR}/edge.vert.spv ${TETRA_SHADER_DIR}/edge.frag.spv
         ${TETRA_SHADER_DIR}/shadow.vert.spv
         ${TETRA_SHADER_DIR}/sky.vert.spv ${TETRA_SHADER_DIR}/sky.frag.spv
         ${TETRA_SHADER_DIR}/fullscreen.vert.spv
         ${TETRA_SHADER_DIR}/tone_map.frag.spv
         ${TETRA_SHADER_DIR}/tone_map_faithful.frag.spv
         ${TETRA_SHADER_DIR}/atmosphere.comp.spv
         ${TETRA_SHADER_DIR}/atmosphere_faithful.comp.spv
         ${TETRA_SHADER_DIR}/atmosphere_reference_hillaire.comp.spv
         ${TETRA_SHADER_DIR}/gpu_lod.comp.spv
         ${TETRA_SHADER_DIR}/gpu_hierarchy_frontier.comp.spv
         ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_worklist.comp.spv
         ${TETRA_SHADER_DIR}/gpu_hierarchy_canonicalize.comp.spv
         ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_green_closure.comp.spv
         ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_red_repair.comp.spv
         ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_red_scan.comp.spv
         ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_owner_materialize.comp.spv
         ${TETRA_SHADER_DIR}/gpu_hierarchy_closure.comp.spv
         ${TETRA_SHADER_DIR}/gpu_hierarchy_closure_control.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_extract.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_classify.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_triangles.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_triangle_counts.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_exclusive_scan.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_triangle_finalize.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_triangle_scatter.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_owner_counts.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_owner_finalize.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_owner_emit.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_project.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_draw.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_commit_validate.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_commit_copy.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_commit_publish.comp.spv
         ${TETRA_SHADER_DIR}/gpu_volume_split_closure.comp.spv
         ${TETRA_SHADER_DIR}/gpu_volume_split_marks.comp.spv
         ${TETRA_SHADER_DIR}/gpu_volume_mutation_journal.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/scene.vert -o ${TETRA_SHADER_DIR}/scene.vert.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/scene.frag -o ${TETRA_SHADER_DIR}/scene.frag.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/wire.frag -o ${TETRA_SHADER_DIR}/wire.frag.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/edge.vert -o ${TETRA_SHADER_DIR}/edge.vert.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/edge.frag -o ${TETRA_SHADER_DIR}/edge.frag.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/shadow.vert -o ${TETRA_SHADER_DIR}/shadow.vert.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/sky.vert -o ${TETRA_SHADER_DIR}/sky.vert.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/sky.frag -o ${TETRA_SHADER_DIR}/sky.frag.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/fullscreen.vert -o ${TETRA_SHADER_DIR}/fullscreen.vert.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/tone_map.frag -o ${TETRA_SHADER_DIR}/tone_map.frag.spv
  COMMAND ${GLSLC} -DFAITHFUL_SHADOW_SPLIT=1 ${CMAKE_CURRENT_SOURCE_DIR}/tone_map.frag -o ${TETRA_SHADER_DIR}/tone_map_faithful.frag.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/atmosphere.comp -o ${TETRA_SHADER_DIR}/atmosphere.comp.spv
  COMMAND ${GLSLC} -DFAITHFUL_SHADOW_SPLIT=1 ${CMAKE_CURRENT_SOURCE_DIR}/atmosphere.comp -o ${TETRA_SHADER_DIR}/atmosphere_faithful.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/atmosphere_reference_hillaire.comp -o ${TETRA_SHADER_DIR}/atmosphere_reference_hillaire.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_lod.comp -o ${TETRA_SHADER_DIR}/gpu_lod.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_frontier.comp -o ${TETRA_SHADER_DIR}/gpu_hierarchy_frontier.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_compact_worklist.comp -o ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_worklist.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_canonicalize.comp -o ${TETRA_SHADER_DIR}/gpu_hierarchy_canonicalize.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_compact_green_closure.comp -o ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_green_closure.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_compact_red_repair.comp -o ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_red_repair.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_compact_red_scan.comp -o ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_red_scan.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_compact_owner_materialize.comp -o ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_owner_materialize.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_closure.comp -o ${TETRA_SHADER_DIR}/gpu_hierarchy_closure.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_closure_control.comp -o ${TETRA_SHADER_DIR}/gpu_hierarchy_closure_control.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_extract.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_extract.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_classify.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_classify.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_triangles.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_triangles.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_triangle_counts.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_triangle_counts.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_exclusive_scan.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_exclusive_scan.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_triangle_finalize.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_triangle_finalize.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_triangle_scatter.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_triangle_scatter.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_owner_counts.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_owner_counts.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_owner_finalize.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_owner_finalize.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_owner_emit.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_owner_emit.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_project.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_project.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_draw.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_draw.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_commit_validate.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_commit_validate.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_commit_copy.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_commit_copy.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_commit_publish.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_commit_publish.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_volume_split_closure.comp -o ${TETRA_SHADER_DIR}/gpu_volume_split_closure.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_volume_split_marks.comp -o ${TETRA_SHADER_DIR}/gpu_volume_split_marks.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_volume_mutation_journal.comp -o ${TETRA_SHADER_DIR}/gpu_volume_mutation_journal.comp.spv
    DEPENDS scene.vert scene.frag wire.frag edge.vert edge.frag shadow.vert sky.vert sky.frag fullscreen.vert tone_map.frag atmosphere.comp atmosphere_reference_hillaire.comp gpu_lod.comp gpu_hierarchy_frontier.comp gpu_hierarchy_compact_worklist.comp gpu_hierarchy_canonicalize.comp gpu_hierarchy_compact_green_closure.comp gpu_hierarchy_compact_red_repair.comp gpu_hierarchy_compact_red_scan.comp gpu_hierarchy_compact_owner_materialize.comp gpu_hierarchy_closure.comp gpu_hierarchy_closure_control.comp gpu_terrain_extract.comp gpu_terrain_classify.comp gpu_terrain_triangles.comp gpu_terrain_triangle_counts.comp gpu_terrain_triangle_finalize.comp gpu_terrain_owner_counts.comp gpu_terrain_owner_finalize.comp gpu_terrain_owner_emit.comp gpu_terrain_project.comp gpu_terrain_draw.comp gpu_terrain_commit_validate.comp gpu_terrain_commit_copy.comp gpu_terrain_commit_publish.comp gpu_volume_split_closure.comp gpu_volume_split_marks.comp gpu_volume_mutation_journal.comp gpu_terrain_field.glsl)
  add_custom_target(tetra_viewer_shaders DEPENDS
  ${TETRA_SHADER_DIR}/scene.vert.spv ${TETRA_SHADER_DIR}/scene.frag.spv ${TETRA_SHADER_DIR}/wire.frag.spv
  ${TETRA_SHADER_DIR}/edge.vert.spv ${TETRA_SHADER_DIR}/edge.frag.spv
  ${TETRA_SHADER_DIR}/shadow.vert.spv
  ${TETRA_SHADER_DIR}/sky.vert.spv ${TETRA_SHADER_DIR}/sky.frag.spv
  ${TETRA_SHADER_DIR}/fullscreen.vert.spv ${TETRA_SHADER_DIR}/tone_map.frag.spv
  ${TETRA_SHADER_DIR}/tone_map_faithful.frag.spv
  ${TETRA_SHADER_DIR}/atmosphere.comp.spv
  ${TETRA_SHADER_DIR}/atmosphere_faithful.comp.spv
  ${TETRA_SHADER_DIR}/atmosphere_reference_hillaire.comp.spv
  ${TETRA_SHADER_DIR}/gpu_lod.comp.spv
  ${TETRA_SHADER_DIR}/gpu_hierarchy_frontier.comp.spv
  ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_worklist.comp.spv
  ${TETRA_SHADER_DIR}/gpu_hierarchy_canonicalize.comp.spv
  ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_green_closure.comp.spv
  ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_red_repair.comp.spv
  ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_red_scan.comp.spv
  ${TETRA_SHADER_DIR}/gpu_hierarchy_compact_owner_materialize.comp.spv
  ${TETRA_SHADER_DIR}/gpu_hierarchy_closure.comp.spv
  ${TETRA_SHADER_DIR}/gpu_hierarchy_closure_control.comp.spv
  ${TETRA_SHADER_DIR}/gpu_terrain_extract.comp.spv
  ${TETRA_SHADER_DIR}/gpu_terrain_classify.comp.spv
  ${TETRA_SHADER_DIR}/gpu_terrain_triangles.comp.spv ${TETRA_SHADER_DIR}/gpu_terrain_triangle_counts.comp.spv ${TETRA_SHADER_DIR}/gpu_terrain_exclusive_scan.comp.spv ${TETRA_SHADER_DIR}/gpu_terrain_triangle_finalize.comp.spv ${TETRA_SHADER_DIR}/gpu_terrain_triangle_scatter.comp.spv ${TETRA_SHADER_DIR}/gpu_terrain_owner_counts.comp.spv ${TETRA_SHADER_DIR}/gpu_terrain_owner_finalize.comp.spv ${TETRA_SHADER_DIR}/gpu_terrain_owner_emit.comp.spv ${TETRA_SHADER_DIR}/gpu_terrain_project.comp.spv ${TETRA_SHADER_DIR}/gpu_terrain_draw.comp.spv ${TETRA_SHADER_DIR}/gpu_terrain_commit_validate.comp.spv ${TETRA_SHADER_DIR}/gpu_terrain_commit_copy.comp.spv ${TETRA_SHADER_DIR}/gpu_terrain_commit_publish.comp.spv ${TETRA_SHADER_DIR}/gpu_volume_split_closure.comp.spv ${TETRA_SHADER_DIR}/gpu_volume_mutation_journal.comp.spv)
  add_dependencies(tetra_viewer tetra_viewer_shaders)
  add_dependencies(tetra_world tetra_viewer_shaders)
  target_compile_definitions(tetra_viewer_application PRIVATE TETRA_VIEWER_SHADER_DIR="${TETRA_SHADER_DIR}")

  if(APPLE)
  # GLFW loads the Vulkan loader by a short dylib name.  Keep the public
  # executable path as a launcher that supplies Homebrew's MoltenVK runtime;
  # the compiled application remains next to it as tetra_viewer_bin.
  set_target_properties(tetra_viewer PROPERTIES OUTPUT_NAME tetra_viewer_bin)
  configure_file(tetra_viewer_macos.sh.in ${CMAKE_CURRENT_BINARY_DIR}/tetra_viewer @ONLY)
  file(CHMOD ${CMAKE_CURRENT_BINARY_DIR}/tetra_viewer
    PERMISSIONS
      OWNER_READ OWNER_WRITE OWNER_EXECUTE
      GROUP_READ GROUP_EXECUTE
      WORLD_READ WORLD_EXECUTE)
  add_custom_command(TARGET tetra_viewer POST_BUILD
    COMMAND chmod +x ${CMAKE_CURRENT_BINARY_DIR}/tetra_viewer)
  set_target_properties(tetra_world PROPERTIES OUTPUT_NAME tetra_world_bin)
  configure_file(tetra_world_macos.sh.in ${CMAKE_CURRENT_BINARY_DIR}/tetra_world @ONLY)
  file(CHMOD ${CMAKE_CURRENT_BINARY_DIR}/tetra_world
    PERMISSIONS
      OWNER_READ OWNER_WRITE OWNER_EXECUTE
      GROUP_READ GROUP_EXECUTE
      WORLD_READ WORLD_EXECUTE)
  add_custom_command(TARGET tetra_world POST_BUILD
    COMMAND chmod +x ${CMAKE_CURRENT_BINARY_DIR}/tetra_world)
  endif()
endif()

if(APPLE AND TETRA_BUILD_METAL)
  # Keep the atmosphere's physical transport model single-source.  The build
  # translates its SPIR-V to native MSL; the application never links or loads
  # Vulkan or MoltenVK.
  set(SPIRV_CROSS_CLI ON CACHE BOOL "Build the SPIRV-Cross translator" FORCE)
  set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "Disable SPIRV-Cross tests" FORCE)
  set(SPIRV_CROSS_SHARED OFF CACHE BOOL "Build static SPIRV-Cross libraries" FORCE)
  FetchContent_Declare(
    spirv_cross
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG vulkan-sdk-1.4.357.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(spirv_cross)

  add_library(imgui_metal STATIC
    ${TETRA_IMGUI_SOURCES}
    ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm)
  target_include_directories(imgui_metal PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui_metal PUBLIC glfw
    "-framework Metal" "-framework QuartzCore")
  target_compile_options(imgui_metal PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

  add_executable(tetra_world_metal metal_main.mm)
  target_link_libraries(tetra_world_metal PRIVATE
    tetra_viewer_script imgui_metal glfw
    "-framework Metal" "-framework MetalFX" "-framework QuartzCore" "-framework Cocoa"
    "-framework IOKit" "-framework CoreVideo")
  target_compile_options(tetra_world_metal PRIVATE -fobjc-arc)
  find_program(METAL_GLSLC glslc REQUIRED)
  set(TETRA_METAL_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/metal_shaders)
  file(MAKE_DIRECTORY ${TETRA_METAL_SHADER_DIR})
  set(TETRA_METAL_ATMOSPHERE_SHADERS)
  foreach(ATMOSPHERE_MODE RANGE 0 16)
    set(ATMOSPHERE_SPV
      ${TETRA_METAL_SHADER_DIR}/atmosphere_mode_${ATMOSPHERE_MODE}.spv)
    set(ATMOSPHERE_MSL
      ${TETRA_METAL_SHADER_DIR}/atmosphere_mode_${ATMOSPHERE_MODE}.metal)
    add_custom_command(
      OUTPUT ${ATMOSPHERE_MSL}
      COMMAND ${METAL_GLSLC}
              -DFAITHFUL_SHADOW_SPLIT=1
              -DTETRA_METAL_RAY_TRACED_VISIBILITY=1
              -DTETRA_ATMOSPHERE_SPECIALIZED_MODE=${ATMOSPHERE_MODE}
              ${CMAKE_CURRENT_SOURCE_DIR}/atmosphere.comp
              -o ${ATMOSPHERE_SPV}
      COMMAND $<TARGET_FILE:spirv-cross> ${ATMOSPHERE_SPV}
              --msl --msl-version 20400 --output ${ATMOSPHERE_MSL}
      DEPENDS atmosphere.comp spirv-cross
      VERBATIM)
    list(APPEND TETRA_METAL_ATMOSPHERE_SHADERS ${ATMOSPHERE_MSL})
  endforeach()
  set(TETRA_METAL_GRAPHICS_SHADER_NAMES
    scene.vert scene.frag wire.frag edge.vert edge.frag shadow.vert
    sky.vert sky.frag fullscreen.vert tone_map.frag)
  set(TETRA_METAL_GRAPHICS_SHADERS)
  foreach(GRAPHICS_SHADER IN LISTS TETRA_METAL_GRAPHICS_SHADER_NAMES)
    set(GRAPHICS_SPV ${TETRA_METAL_SHADER_DIR}/${GRAPHICS_SHADER}.spv)
    set(GRAPHICS_MSL ${TETRA_METAL_SHADER_DIR}/${GRAPHICS_SHADER}.metal)
    set(GRAPHICS_MSL_OPTIONS --msl --msl-version 20400)
    # Vulkan's positive-height viewport and Metal's viewport map clip-space Y
    # in opposite directions.  Scene and shadow geometry perform that backend
    # conversion before rasterization; the translated full-screen triangle
    # needs the equivalent conversion here or it presents every render target
    # upside down while leaving the underlying depth oracle apparently valid.
    if(GRAPHICS_SHADER STREQUAL "fullscreen.vert")
      list(APPEND GRAPHICS_MSL_OPTIONS --flip-vert-y)
    endif()
    add_custom_command(
      OUTPUT ${GRAPHICS_MSL}
      COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/${GRAPHICS_SHADER}
              -o ${GRAPHICS_SPV}
      COMMAND $<TARGET_FILE:spirv-cross> ${GRAPHICS_SPV}
              ${GRAPHICS_MSL_OPTIONS} --output ${GRAPHICS_MSL}
      DEPENDS ${GRAPHICS_SHADER} spirv-cross
      VERBATIM)
    list(APPEND TETRA_METAL_GRAPHICS_SHADERS ${GRAPHICS_MSL})
  endforeach()
  set(TETRA_METAL_FAITHFUL_TONE_SPV
    ${TETRA_METAL_SHADER_DIR}/tone_map_faithful.frag.spv)
  set(TETRA_METAL_FAITHFUL_TONE_MSL
    ${TETRA_METAL_SHADER_DIR}/tone_map_faithful.frag.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_FAITHFUL_TONE_MSL}
    COMMAND ${METAL_GLSLC} -DFAITHFUL_SHADOW_SPLIT=1
            ${CMAKE_CURRENT_SOURCE_DIR}/tone_map.frag
            -o ${TETRA_METAL_FAITHFUL_TONE_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_FAITHFUL_TONE_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_FAITHFUL_TONE_MSL}
    DEPENDS tone_map.frag spirv-cross
    VERBATIM)
  list(APPEND TETRA_METAL_GRAPHICS_SHADERS
       ${TETRA_METAL_FAITHFUL_TONE_MSL})
  # P4c2 keeps hierarchy selection single-source: Metal consumes the exact
  # SPIR-V translated compute kernel dispatched by the Vulkan diagnostic.
  set(TETRA_METAL_GPU_LOD_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_lod.comp.spv)
  set(TETRA_METAL_GPU_LOD_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_lod.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_LOD_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_lod.comp
            -o ${TETRA_METAL_GPU_LOD_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_LOD_SPV}
            --msl --msl-version 20400 --output ${TETRA_METAL_GPU_LOD_MSL}
    DEPENDS gpu_lod.comp spirv-cross
    VERBATIM)
  set(TETRA_METAL_GPU_HIERARCHY_FRONTIER_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_frontier.comp.spv)
  set(TETRA_METAL_GPU_HIERARCHY_FRONTIER_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_frontier.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_HIERARCHY_FRONTIER_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_frontier.comp
            -o ${TETRA_METAL_GPU_HIERARCHY_FRONTIER_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_HIERARCHY_FRONTIER_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_GPU_HIERARCHY_FRONTIER_MSL}
    DEPENDS gpu_hierarchy_frontier.comp spirv-cross
    VERBATIM)
  set(TETRA_METAL_GPU_HIERARCHY_COMPACT_WORKLIST_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_compact_worklist.comp.spv)
  set(TETRA_METAL_GPU_HIERARCHY_COMPACT_WORKLIST_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_compact_worklist.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_HIERARCHY_COMPACT_WORKLIST_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_compact_worklist.comp
            -o ${TETRA_METAL_GPU_HIERARCHY_COMPACT_WORKLIST_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_HIERARCHY_COMPACT_WORKLIST_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_GPU_HIERARCHY_COMPACT_WORKLIST_MSL}
    DEPENDS gpu_hierarchy_compact_worklist.comp spirv-cross
    VERBATIM)
  set(TETRA_METAL_GPU_HIERARCHY_CANONICALIZE_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_canonicalize.comp.spv)
  set(TETRA_METAL_GPU_HIERARCHY_CANONICALIZE_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_canonicalize.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_HIERARCHY_CANONICALIZE_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_canonicalize.comp
            -o ${TETRA_METAL_GPU_HIERARCHY_CANONICALIZE_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_HIERARCHY_CANONICALIZE_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_GPU_HIERARCHY_CANONICALIZE_MSL}
    DEPENDS gpu_hierarchy_canonicalize.comp spirv-cross
    VERBATIM)
  set(TETRA_METAL_GPU_HIERARCHY_COMPACT_GREEN_CLOSURE_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_compact_green_closure.comp.spv)
  set(TETRA_METAL_GPU_HIERARCHY_COMPACT_GREEN_CLOSURE_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_compact_green_closure.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_HIERARCHY_COMPACT_GREEN_CLOSURE_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_compact_green_closure.comp
            -o ${TETRA_METAL_GPU_HIERARCHY_COMPACT_GREEN_CLOSURE_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_HIERARCHY_COMPACT_GREEN_CLOSURE_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_GPU_HIERARCHY_COMPACT_GREEN_CLOSURE_MSL}
    DEPENDS gpu_hierarchy_compact_green_closure.comp spirv-cross
    VERBATIM)
  set(TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_REPAIR_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_compact_red_repair.comp.spv)
  set(TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_REPAIR_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_compact_red_repair.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_REPAIR_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_compact_red_repair.comp
            -o ${TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_REPAIR_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_REPAIR_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_REPAIR_MSL}
    DEPENDS gpu_hierarchy_compact_red_repair.comp spirv-cross
    VERBATIM)
  set(TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_SCAN_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_compact_red_scan.comp.spv)
  set(TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_SCAN_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_compact_red_scan.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_SCAN_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_compact_red_scan.comp
            -o ${TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_SCAN_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_SCAN_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_SCAN_MSL}
    DEPENDS gpu_hierarchy_compact_red_scan.comp spirv-cross
    VERBATIM)
  set(TETRA_METAL_GPU_HIERARCHY_COMPACT_OWNER_MATERIALIZE_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_compact_owner_materialize.comp.spv)
  set(TETRA_METAL_GPU_HIERARCHY_COMPACT_OWNER_MATERIALIZE_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_compact_owner_materialize.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_HIERARCHY_COMPACT_OWNER_MATERIALIZE_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_compact_owner_materialize.comp
            -o ${TETRA_METAL_GPU_HIERARCHY_COMPACT_OWNER_MATERIALIZE_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_HIERARCHY_COMPACT_OWNER_MATERIALIZE_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_GPU_HIERARCHY_COMPACT_OWNER_MATERIALIZE_MSL}
    DEPENDS gpu_hierarchy_compact_owner_materialize.comp spirv-cross
    VERBATIM)
  set(TETRA_METAL_GPU_HIERARCHY_CLOSURE_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_closure.comp.spv)
  set(TETRA_METAL_GPU_HIERARCHY_CLOSURE_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_closure.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_HIERARCHY_CLOSURE_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_closure.comp
            -o ${TETRA_METAL_GPU_HIERARCHY_CLOSURE_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_HIERARCHY_CLOSURE_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_GPU_HIERARCHY_CLOSURE_MSL}
    DEPENDS gpu_hierarchy_closure.comp spirv-cross
    VERBATIM)
  set(TETRA_METAL_GPU_HIERARCHY_CLOSURE_CONTROL_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_closure_control.comp.spv)
  set(TETRA_METAL_GPU_HIERARCHY_CLOSURE_CONTROL_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_hierarchy_closure_control.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_HIERARCHY_CLOSURE_CONTROL_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_hierarchy_closure_control.comp
            -o ${TETRA_METAL_GPU_HIERARCHY_CLOSURE_CONTROL_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_HIERARCHY_CLOSURE_CONTROL_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_GPU_HIERARCHY_CLOSURE_CONTROL_MSL}
    DEPENDS gpu_hierarchy_closure_control.comp spirv-cross
    VERBATIM)
  # P5a establishes only the extractor's shared shader ABI. Dispatch and
  # publication remain separately qualified P5b/P5c work.
  set(TETRA_METAL_GPU_TERRAIN_EXTRACT_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_terrain_extract.comp.spv)
  set(TETRA_METAL_GPU_TERRAIN_EXTRACT_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_terrain_extract.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_TERRAIN_EXTRACT_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_extract.comp
            -o ${TETRA_METAL_GPU_TERRAIN_EXTRACT_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_TERRAIN_EXTRACT_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_GPU_TERRAIN_EXTRACT_MSL}
    DEPENDS gpu_terrain_extract.comp spirv-cross
    VERBATIM)
  set(TETRA_METAL_GPU_TERRAIN_CLASSIFY_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_terrain_classify.comp.spv)
  set(TETRA_METAL_GPU_TERRAIN_CLASSIFY_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_terrain_classify.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_TERRAIN_CLASSIFY_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_classify.comp
            -o ${TETRA_METAL_GPU_TERRAIN_CLASSIFY_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_TERRAIN_CLASSIFY_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_GPU_TERRAIN_CLASSIFY_MSL}
    DEPENDS gpu_terrain_classify.comp gpu_terrain_field.glsl spirv-cross
    VERBATIM)
  set(TETRA_METAL_GPU_TERRAIN_TRIANGLES_SPV
    ${TETRA_METAL_SHADER_DIR}/gpu_terrain_triangles.comp.spv)
  set(TETRA_METAL_GPU_TERRAIN_TRIANGLES_MSL
    ${TETRA_METAL_SHADER_DIR}/gpu_terrain_triangles.comp.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_GPU_TERRAIN_TRIANGLES_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_triangles.comp
            -o ${TETRA_METAL_GPU_TERRAIN_TRIANGLES_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_TERRAIN_TRIANGLES_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_GPU_TERRAIN_TRIANGLES_MSL}
    DEPENDS gpu_terrain_triangles.comp spirv-cross
    VERBATIM)
  foreach(TETRA_PARALLEL_TRIANGLE_SHADER IN ITEMS
          gpu_terrain_triangle_counts.comp
          gpu_terrain_exclusive_scan.comp
          gpu_terrain_triangle_finalize.comp
          gpu_terrain_triangle_scatter.comp
          gpu_terrain_owner_counts.comp
          gpu_terrain_owner_finalize.comp
          gpu_terrain_owner_emit.comp
          gpu_terrain_compact_owner_control.comp
          gpu_terrain_compact_owner_count.comp
          gpu_terrain_compact_owner_scan.comp
          gpu_terrain_compact_owner_emit.comp
          gpu_terrain_compact_owner_triangle_emit.comp
          gpu_terrain_compact_owner_microbatch.comp
          gpu_terrain_compact_owner_microbatch_validate.comp
          gpu_terrain_compact_owner_hybrid_scan.comp
          gpu_terrain_compact_owner_hybrid_finalize.comp
          gpu_terrain_compact_owner_trace.comp
          gpu_terrain_compact_owner_validate.comp
          gpu_terrain_compact_owner_copy.comp
          gpu_terrain_compact_owner_publish.comp
          gpu_volume_split_closure.comp
          gpu_volume_split_marks.comp
          gpu_volume_mutation_journal.comp)
    set(TETRA_PARALLEL_TRIANGLE_SPV
      ${TETRA_METAL_SHADER_DIR}/${TETRA_PARALLEL_TRIANGLE_SHADER}.spv)
    set(TETRA_PARALLEL_TRIANGLE_MSL
      ${TETRA_METAL_SHADER_DIR}/${TETRA_PARALLEL_TRIANGLE_SHADER}.metal)
    add_custom_command(
      OUTPUT ${TETRA_PARALLEL_TRIANGLE_MSL}
      COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/${TETRA_PARALLEL_TRIANGLE_SHADER}
              -o ${TETRA_PARALLEL_TRIANGLE_SPV}
      COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_PARALLEL_TRIANGLE_SPV}
              --msl --msl-version 20400 --output ${TETRA_PARALLEL_TRIANGLE_MSL}
      DEPENDS ${TETRA_PARALLEL_TRIANGLE_SHADER} spirv-cross
      VERBATIM)
    list(APPEND TETRA_METAL_PARALLEL_TRIANGLE_MSL
         ${TETRA_PARALLEL_TRIANGLE_MSL})
  endforeach()
  foreach(TETRA_GPU_TERRAIN_COMMIT_SHADER IN ITEMS
          gpu_terrain_commit_validate.comp
          gpu_terrain_commit_copy.comp
          gpu_terrain_commit_publish.comp)
    set(TETRA_GPU_TERRAIN_COMMIT_SPV
      ${TETRA_METAL_SHADER_DIR}/${TETRA_GPU_TERRAIN_COMMIT_SHADER}.spv)
    set(TETRA_GPU_TERRAIN_COMMIT_MSL
      ${TETRA_METAL_SHADER_DIR}/${TETRA_GPU_TERRAIN_COMMIT_SHADER}.metal)
    add_custom_command(
      OUTPUT ${TETRA_GPU_TERRAIN_COMMIT_MSL}
      COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/${TETRA_GPU_TERRAIN_COMMIT_SHADER}
              -o ${TETRA_GPU_TERRAIN_COMMIT_SPV}
      COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_GPU_TERRAIN_COMMIT_SPV}
              --msl --msl-version 20400 --output ${TETRA_GPU_TERRAIN_COMMIT_MSL}
      DEPENDS ${TETRA_GPU_TERRAIN_COMMIT_SHADER} spirv-cross
      VERBATIM)
    list(APPEND TETRA_METAL_GPU_TERRAIN_COMMIT_MSL
         ${TETRA_GPU_TERRAIN_COMMIT_MSL})
  endforeach()
  set(TETRA_METAL_GPU_TERRAIN_PROJECT_SPV ${TETRA_METAL_SHADER_DIR}/gpu_terrain_project.comp.spv)
  set(TETRA_METAL_GPU_TERRAIN_PROJECT_MSL ${TETRA_METAL_SHADER_DIR}/gpu_terrain_project.comp.metal)
  add_custom_command(OUTPUT ${TETRA_METAL_GPU_TERRAIN_PROJECT_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_project.comp -o ${TETRA_METAL_GPU_TERRAIN_PROJECT_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_TERRAIN_PROJECT_SPV} --msl --msl-version 20400 --output ${TETRA_METAL_GPU_TERRAIN_PROJECT_MSL}
    DEPENDS gpu_terrain_project.comp gpu_terrain_field.glsl spirv-cross VERBATIM)
  set(TETRA_METAL_GPU_TERRAIN_DRAW_SPV ${TETRA_METAL_SHADER_DIR}/gpu_terrain_draw.comp.spv)
  set(TETRA_METAL_GPU_TERRAIN_DRAW_MSL ${TETRA_METAL_SHADER_DIR}/gpu_terrain_draw.comp.metal)
  add_custom_command(OUTPUT ${TETRA_METAL_GPU_TERRAIN_DRAW_MSL}
    COMMAND ${METAL_GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_draw.comp -o ${TETRA_METAL_GPU_TERRAIN_DRAW_SPV}
    COMMAND $<TARGET_FILE:spirv-cross> ${TETRA_METAL_GPU_TERRAIN_DRAW_SPV} --msl --msl-version 20400 --output ${TETRA_METAL_GPU_TERRAIN_DRAW_MSL}
    DEPENDS gpu_terrain_draw.comp gpu_terrain_field.glsl spirv-cross VERBATIM)
  set(TETRA_METAL_REFERENCE_ATMOSPHERE_SPV
    ${TETRA_METAL_SHADER_DIR}/atmosphere_reference_hillaire.spv)
  set(TETRA_METAL_REFERENCE_ATMOSPHERE_MSL
    ${TETRA_METAL_SHADER_DIR}/atmosphere_reference_hillaire.metal)
  add_custom_command(
    OUTPUT ${TETRA_METAL_REFERENCE_ATMOSPHERE_MSL}
    COMMAND ${METAL_GLSLC}
            ${CMAKE_CURRENT_SOURCE_DIR}/atmosphere_reference_hillaire.comp
            -o ${TETRA_METAL_REFERENCE_ATMOSPHERE_SPV}
    COMMAND $<TARGET_FILE:spirv-cross>
            ${TETRA_METAL_REFERENCE_ATMOSPHERE_SPV}
            --msl --msl-version 20400
            --output ${TETRA_METAL_REFERENCE_ATMOSPHERE_MSL}
    DEPENDS atmosphere_reference_hillaire.comp spirv-cross
    VERBATIM)
  add_custom_target(tetra_world_metal_shaders
    DEPENDS ${TETRA_METAL_ATMOSPHERE_SHADERS}
            ${TETRA_METAL_GRAPHICS_SHADERS}
            ${TETRA_METAL_GPU_LOD_MSL}
            ${TETRA_METAL_GPU_HIERARCHY_FRONTIER_MSL}
            ${TETRA_METAL_GPU_HIERARCHY_COMPACT_WORKLIST_MSL}
            ${TETRA_METAL_GPU_HIERARCHY_CANONICALIZE_MSL}
            ${TETRA_METAL_GPU_HIERARCHY_COMPACT_GREEN_CLOSURE_MSL}
            ${TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_REPAIR_MSL}
            ${TETRA_METAL_GPU_HIERARCHY_COMPACT_RED_SCAN_MSL}
            ${TETRA_METAL_GPU_HIERARCHY_COMPACT_OWNER_MATERIALIZE_MSL}
            ${TETRA_METAL_GPU_HIERARCHY_CLOSURE_MSL}
            ${TETRA_METAL_GPU_HIERARCHY_CLOSURE_CONTROL_MSL}
            ${TETRA_METAL_GPU_TERRAIN_EXTRACT_MSL}
            ${TETRA_METAL_GPU_TERRAIN_CLASSIFY_MSL}
            ${TETRA_METAL_GPU_TERRAIN_TRIANGLES_MSL}
            ${TETRA_METAL_PARALLEL_TRIANGLE_MSL}
            ${TETRA_METAL_GPU_TERRAIN_COMMIT_MSL}
            ${TETRA_METAL_GPU_TERRAIN_PROJECT_MSL}
            ${TETRA_METAL_GPU_TERRAIN_DRAW_MSL}
            ${TETRA_METAL_REFERENCE_ATMOSPHERE_MSL})
  add_dependencies(tetra_world_metal tetra_world_metal_shaders)
  target_compile_definitions(tetra_world_metal PRIVATE
    TETRA_METAL_ATMOSPHERE_SHADER_DIR="${TETRA_METAL_SHADER_DIR}")
  # A macOS CI worker can validate that every translated SPIR-V shader still
  # compiles through Metal's runtime compiler without opening the renderer.
  add_test(NAME "metal atmosphere translated shaders compile"
    COMMAND $<TARGET_FILE:tetra_world_metal> --metal-atmosphere-compiler-check)
  add_test(NAME "metal GPU hierarchy selector parity"
    COMMAND $<TARGET_FILE:tetra_world_metal> --metal-gpu-lod-selector-smoke-test)
  add_test(NAME "metal GPU terrain extraction parity"
    COMMAND $<TARGET_FILE:tetra_world_metal> --metal-gpu-terrain-extract-smoke-test)
  add_test(NAME "metal GPU terrain classification parity"
    COMMAND $<TARGET_FILE:tetra_world_metal> --metal-gpu-terrain-classify-smoke-test)
  add_test(NAME "metal GPU terrain base triangle parity"
    COMMAND ${CMAKE_COMMAND} -E env TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-gpu-terrain-triangle-smoke-test)
  add_test(NAME "metal GPU terrain parallel compaction parity"
    COMMAND ${CMAKE_COMMAND} -E env TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-gpu-terrain-parallel-triangle-smoke-test)
  add_test(NAME "metal GPU terrain projected geometry parity"
    COMMAND ${CMAKE_COMMAND} -E env TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-gpu-terrain-project-smoke-test)
  add_test(NAME "metal GPU terrain draw candidate parity"
    COMMAND ${CMAKE_COMMAND} -E env TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-gpu-terrain-draw-smoke-test)
  add_test(NAME "metal GPU terrain native private chain parity"
    COMMAND ${CMAKE_COMMAND} -E env TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-gpu-terrain-native-chain-smoke-test)
  add_test(NAME "metal GPU compact owner P8 private-front parity"
    COMMAND ${CMAKE_COMMAND} -E env TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-gpu-compact-owner-p8-smoke-test)
  add_test(NAME "metal GPU terrain live private slots"
    COMMAND ${CMAKE_COMMAND} -E env TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-gpu-terrain-live-slots-smoke-test)
  add_test(NAME "metal GPU terrain runtime parity"
    COMMAND $<TARGET_FILE:tetra_world_metal> --metal-gpu-terrain-runtime-smoke-test)
  # This is the production front contract: an actual BlockedTerrainRuntime
  # publication is regenerated through selector -> closure -> owners -> P8
  # and compared with its CPU SceneVertex front, including camera coverage and
  # depth. It intentionally fails while any incomplete device front remains.
  add_test(NAME "metal GPU production device-front parity"
    COMMAND ${CMAKE_COMMAND} -E env TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-gpu-production-front-parity-smoke-test)
  set_tests_properties("metal GPU production device-front parity" PROPERTIES TIMEOUT 360)
  # P7d composes the hardware compact-stream, native-slot, SceneVertex, and
  # retained-front oracles into the final diagnostic-only parity gate before
  # P8 may remove any readback.
  add_test(NAME "metal GPU terrain full surface parity qualification"
    COMMAND ${CMAKE_COMMAND} -E env TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-gpu-terrain-surface-parity-smoke-test)
  # P5c2 drives the real hidden render loop with the diagnostic packets on;
  # the test asserts completed/accepted/stale slots while CPU terrain remains
  # the only drawable front.
  add_test(NAME "metal GPU terrain interactive slots retain CPU front"
    COMMAND ${CMAKE_COMMAND} -E env
      TETWORLD_METAL_GPU_TERRAIN_DIAGNOSTIC=1
      TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-motion-smoke-test)
  add_test(NAME "metal GPU terrain static slots retain CPU front"
    COMMAND ${CMAKE_COMMAND} -E env
      TETWORLD_METAL_GPU_TERRAIN_DIAGNOSTIC=1
      TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-smoke-test)
  # The selector must retain the complete CPU front while a requested native
  # candidate is unavailable; it must never expose a partial GPU buffer.
  add_test(NAME "metal GPU terrain renderer selector retains complete front"
    COMMAND ${CMAKE_COMMAND} -E env
      TETWORLD_METAL_GPU_TERRAIN_RENDERER=1
      TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-smoke-test)
  # GPU mesh emission is an opt-in comparison route until the live P7e path
  # owns camera selection and closure. A normal launch must not report it as
  # selected merely because the CPU front can be copied into private storage.
  add_test(NAME "metal GPU mesh emission defaults to CPU terrain generation"
    COMMAND ${CMAKE_COMMAND} -E env
      TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-smoke-test)
  set_tests_properties("metal GPU mesh emission defaults to CPU terrain generation"
    PROPERTIES PASS_REGULAR_EXPRESSION "\\\"gpu_renderer_requested\\\":false")
  add_test(NAME "metal GPU live hierarchy selection freezes CPU generation"
    COMMAND ${CMAKE_COMMAND} -E env
      TETWORLD_METAL_BACKGROUND=1
      TETWORLD_METAL_GPU_TERRAIN_LIVE_SELECTION=1
      $<TARGET_FILE:tetra_world_metal> --metal-smoke-test)
  set_tests_properties("metal GPU live hierarchy selection freezes CPU generation"
    PROPERTIES PASS_REGULAR_EXPRESSION
      "\\\"gpu_live_selection\\\":\\{\\\"enabled\\\":true,\\\"submitted\\\":[1-9][0-9]*,\\\"completed\\\":[1-9][0-9]*,\\\"accepted\\\":[1-9][0-9]*,\\\"failed\\\":0,\\\"cpu_generation_violations\\\":0\\}")
  add_test(NAME "metal GPU live hierarchy selection follows camera without CPU terrain"
    COMMAND ${CMAKE_COMMAND} -E env
      TETWORLD_METAL_BACKGROUND=1
      TETWORLD_METAL_GPU_TERRAIN_LIVE_SELECTION=1
      $<TARGET_FILE:tetra_world_metal> --metal-motion-smoke-test)
  set_tests_properties("metal GPU live hierarchy selection follows camera without CPU terrain"
    PROPERTIES PASS_REGULAR_EXPRESSION
      "\\\"gpu_live_selection\\\":\\{\\\"enabled\\\":true,\\\"submitted\\\":[2-9][0-9]*,\\\"completed\\\":[2-9][0-9]*,\\\"accepted\\\":[2-9][0-9]*,\\\"failed\\\":0,\\\"cpu_generation_violations\\\":0\\}")
  # P7e4a's fixed full-snapshot candidate has no passing CTest because a
  # normal live camera flight is nonviable. P7e4a1 adds its own compact-list
  # contract test before it replaces this fenced prototype.
  add_test(NAME "metal GPU live hierarchy selection state contract"
    COMMAND $<TARGET_FILE:tetra_world_metal>
      --metal-gpu-live-selection-state-smoke-test)
  set_tests_properties("metal GPU live hierarchy selection state contract"
    PROPERTIES PASS_REGULAR_EXPRESSION
      "\\\"revision_replaced\\\":true,\\\"persistent\\\":true,\\\"accepted\\\":1,\\\"failed\\\":0,\\\"passed\\\":true")
  add_test(NAME "metal GPU hierarchy frontier is canonical and bounded"
    COMMAND $<TARGET_FILE:tetra_world_metal>
      --metal-gpu-hierarchy-frontier-smoke-test)
  set_tests_properties("metal GPU hierarchy frontier is canonical and bounded"
    PROPERTIES PASS_REGULAR_EXPRESSION
      "\\\"canonical\\\":true,\\\"closure\\\":true,\\\"green_mask\\\":true,\\\"overflow\\\":true,\\\"passed\\\":true")
  add_test(NAME "metal GPU compact green closure parity"
    COMMAND $<TARGET_FILE:tetra_world_metal>
      --metal-gpu-compact-green-closure-smoke-test)
  set_tests_properties("metal GPU compact green closure parity"
    PROPERTIES PASS_REGULAR_EXPRESSION
      "\\\"green\\\":true,\\\"malformed_retained\\\":true,\\\"budget_retained\\\":true,\\\"red_terminal_latched\\\":true,\\\"device_quiesced\\\":true,\\\"passed\\\":true")
  add_test(NAME "metal GPU compact red scan large-list parity"
    COMMAND $<TARGET_FILE:tetra_world_metal>
      --metal-gpu-compact-red-scan-large-smoke-test)
  set_tests_properties("metal GPU compact red scan large-list parity"
    PROPERTIES PASS_REGULAR_EXPRESSION
      "\\\"count\\\":66952,\\\"blocks\\\":262,\\\"levels\\\":2,.*\\\"passed\\\":true")
  add_test(NAME "metal GPU compact live closure encoder"
    COMMAND $<TARGET_FILE:tetra_world_metal>
      --metal-gpu-compact-live-closure-smoke-test)
  set_tests_properties("metal GPU compact live closure encoder"
    PROPERTIES PASS_REGULAR_EXPRESSION
      "\\\"selector_submitted\\\":1,\\\"compact_encoded\\\":1,\\\"compact_completed\\\":1,\\\"direct_red_rounds\\\":8,\\\"final_latched\\\":1,\\\"p8_owner_materialization\\\":true,\\\"p8_encoded\\\":1,\\\"p8_completed\\\":1,\\\"p8_rejected\\\":0,\\\"p8_private_commit\\\":true,\\\"passed\\\":true")
  # Qualification-only payload readback: the fixture reconstructs the CPU
  # closed cut from the same immutable snapshot/camera tuple, then compares
  # it to the completed compact private front.  The normal device route is
  # deliberately still scalar-audit-only.
  add_test(NAME "metal GPU compact live private-front exact parity"
    COMMAND $<TARGET_FILE:tetra_world_metal>
      --metal-gpu-compact-live-parity-smoke-test)
  set_tests_properties("metal GPU compact live private-front exact parity"
    PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION
      "\\\"root_seam\\\":true,\\\"mixed_depth\\\":true,\\\"moving_camera\\\":true,\\\"field_change\\\":true,\\\"image_parity\\\":true,\\\"failure_retains\\\":true,\\\"stale_revision\\\":true,\\\"p8_hybrid\\\":false,\\\"passed\\\":true")
  add_test(NAME "metal GPU compact P8 hybrid exact parity"
    COMMAND $<TARGET_FILE:tetra_world_metal>
      --metal-gpu-compact-hybrid-parity-smoke-test)
  set_tests_properties("metal GPU compact P8 hybrid exact parity"
    PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION
      "\\\"root_seam\\\":true,\\\"mixed_depth\\\":true,\\\"moving_camera\\\":true,\\\"field_change\\\":true,\\\"image_parity\\\":true,\\\"failure_retains\\\":true,\\\"stale_revision\\\":true,\\\"p8_hybrid\\\":true,\\\"passed\\\":true")
  # P7e4c measures the real compact selector→closure→P8 private-front route
  # against the independently executed CPU reference on the same moving
  # camera tuples. It records a result even when GPU p95 is not faster; CPU
  # remains the default until a separate promotion decision is justified.
  add_test(NAME "metal GPU compact camera-to-private-front p95 benchmark"
    COMMAND $<TARGET_FILE:tetra_world_metal>
      --metal-gpu-compact-live-performance-smoke-test)
  set_tests_properties("metal GPU compact camera-to-private-front p95 benchmark"
    PROPERTIES TIMEOUT 90 PASS_REGULAR_EXPRESSION
      "\\\"profiles\\\":2,\\\"warmup\\\":4,\\\"samples\\\":30,.*\\\"material_p95_ratio\\\":0.90,.*\\\"benchmark_payload_readback\\\":false,.*\\\"passed\\\":true")
  # P7e4a1's device-front camera proof stays opt-in.  These are deliberately
  # bounded: terminal output includes the last reached phase and scalar P8
  # audit, so timeout output cannot be mistaken for a shader failure.
  add_test(NAME "metal GPU device-front motion commits private P8 front"
    COMMAND ${CMAKE_COMMAND} -E env
      TETWORLD_METAL_BACKGROUND=1
      TETWORLD_METAL_GPU_TERRAIN_DEVICE_FRONT=1
      TETWORLD_METAL_DEVICE_FRONT_SMOKE_TIMEOUT_SECONDS=60
      $<TARGET_FILE:tetra_world_metal> --metal-motion-smoke-test)
  set_tests_properties("metal GPU device-front motion commits private P8 front"
    PROPERTIES TIMEOUT 90 PASS_REGULAR_EXPRESSION
      "\\\"p8_private_commit\\\":true,.*\\\"display_front\\\":{\\\"promotions\\\":[1-9][0-9]*,\\\"private_frames\\\":[1-9][0-9]*,\\\"bootstrap_fallback_frames\\\":0,\\\"binding_violations\\\":0,\\\"private_vertices_bound\\\":true,\\\"private_indirect_bound\\\":true,\\\"indexed_exact_selection\\\":false,.*\\\"passed\\\":true")
  # This follows the ordinary interactive selection path, but ends under the
  # usual bounded test harness. Full-front correctness is separately checked
  # by the device-front parity fixture.
  add_test(NAME "metal GPU device-front is ordinary-launch default"
    COMMAND ${CMAKE_COMMAND} -E env
      TETWORLD_METAL_BACKGROUND=1
      TETWORLD_METAL_DEVICE_FRONT_SMOKE_TIMEOUT_SECONDS=60
      $<TARGET_FILE:tetra_world_metal> --metal-device-front-default-smoke-test)
  set_tests_properties("metal GPU device-front is ordinary-launch default"
    PROPERTIES TIMEOUT 90 PASS_REGULAR_EXPRESSION
      "\\\"p8_private_commit\\\":true,\\\"device_front_default_selected\\\":true,\\\"complete_front_vertices\\\":{\\\"cpu_bootstrap\\\":[1-9][0-9]*,\\\"gpu_p8\\\":[1-9][0-9]*,\\\"exact\\\":true},.*\\\"display_front\\\":{\\\"promotions\\\":[1-9][0-9]*,\\\"private_frames\\\":[1-9][0-9]*,\\\"bootstrap_fallback_frames\\\":0,\\\"binding_violations\\\":0,\\\"private_vertices_bound\\\":true,\\\"private_indirect_bound\\\":true,\\\"indexed_exact_selection\\\":false,.*\\\"p6_requests\\\":0,\\\"cpu_surface_build_requests\\\":0,.*\\\"passed\\\":true")
  # Operators can explicitly return to the established CPU route.  Keep this
  # independent of automated-test defaults so it proves an explicit CPU
  # override takes precedence over the interactive device-front default.
  add_test(NAME "metal GPU device-front CPU override selects fallback"
    COMMAND ${CMAKE_COMMAND} -E env
      TETWORLD_METAL_BACKGROUND=1
      TETWORLD_METAL_GPU_TERRAIN_DEVICE_FRONT=0
      $<TARGET_FILE:tetra_world_metal> --metal-smoke-test)
  set_tests_properties("metal GPU device-front CPU override selects fallback"
    PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION
      "\\\"gpu_renderer_requested\\\":false,\\\"gpu_renderer_available\\\":false,\\\"gpu_live_selection\\\":{\\\"enabled\\\":false,\\\"submitted\\\":0,\\\"completed\\\":0,\\\"accepted\\\":0,\\\"failed\\\":0,\\\"cpu_generation_violations\\\":0}")
  add_test(NAME "metal GPU device-front motion rejects failed P8 input"
    COMMAND ${CMAKE_COMMAND} -E env
      TETWORLD_METAL_BACKGROUND=1
      TETWORLD_METAL_GPU_TERRAIN_DEVICE_FRONT=1
      TETWORLD_METAL_GPU_TERRAIN_DEVICE_FRONT_INJECT_GREEN_BUDGET_FAILURE=1
      TETWORLD_METAL_DEVICE_FRONT_SMOKE_TIMEOUT_SECONDS=60
      $<TARGET_FILE:tetra_world_metal> --metal-motion-smoke-test)
  set_tests_properties("metal GPU device-front motion rejects failed P8 input"
    PROPERTIES TIMEOUT 90 PASS_REGULAR_EXPRESSION
      "\\\"p8_rejected\\\":[1-9][0-9]*,\\\"p8_private_commit\\\":false,.*\\\"display_front\\\":{\\\"promotions\\\":0,\\\"private_frames\\\":0,\\\"bootstrap_fallback_frames\\\":[1-9][0-9]*,\\\"binding_violations\\\":0,\\\"private_vertices_bound\\\":false,\\\"private_indirect_bound\\\":false,\\\"indexed_exact_selection\\\":false,\\\"bootstrap_vertices_retained\\\":true,\\\"bootstrap_indirect_retained\\\":true},.*\\\"passed\\\":true")
  add_test(NAME "metal GPU compact owner materializer parity"
    COMMAND $<TARGET_FILE:tetra_world_metal>
      --metal-gpu-compact-owner-materialize-smoke-test)
  set_tests_properties("metal GPU compact owner materializer parity"
    PROPERTIES PASS_REGULAR_EXPRESSION
      "\\\"word_parity\\\":true,\\\"malformed_retained\\\":true,\\\"capacity_retained\\\":true,\\\"record_retained\\\":true,\\\"mask_failed\\\":true,\\\"passed\\\":true")
  # P8c2 collects GPU counter evidence for the owner-direct generator and
  # completed command-buffer frame interval on the selected private front.
  # Keep this independent performance qualification ahead of the long P8b
  # private-front motion stress test: the latter intentionally leaves a large
  # graphics workload immediately before process exit and distorted this
  # test's device p95 despite a clean isolated run.
  add_test(NAME "metal GPU terrain owner-direct performance qualification"
    COMMAND ${CMAKE_COMMAND} -E env
      TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-gpu-terrain-performance-smoke-test)
  set_tests_properties("metal GPU terrain owner-direct performance qualification"
    PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION
      "\\\"generation_samples\\\":30,.*\\\"generation_limit_ms\\\":60\\.0000,.*\\\"frame_limit_ms\\\":33\\.3333,.*\\\"passed\\\":true")
  # P8b drives the selected renderer through the production moving-camera
  # loop.  It verifies the selected, complete private display front across
  # camera motion without enabling legacy candidate-payload readback. Native
  # root expansion remains a separate P8c performance route.
  add_test(NAME "metal GPU terrain selected private front moves readback-free"
    COMMAND ${CMAKE_COMMAND} -E env
      TETWORLD_METAL_GPU_TERRAIN_RENDERER=1
      TETWORLD_METAL_GPU_TERRAIN_PRIVATE_FRONT_QUALIFICATION=1
      TETWORLD_METAL_BACKGROUND=1
      $<TARGET_FILE:tetra_world_metal> --metal-motion-smoke-test)
  add_test(NAME "metal GPU conforming volume split closure parity"
    COMMAND $<TARGET_FILE:tetra_world_metal>
      --metal-gpu-volume-split-closure-smoke-test)
  set_target_properties(tetra_world_metal PROPERTIES
    OUTPUT_NAME "TetWorldMetal"
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "TetWorldMetal"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.tetrahedra.tetworldmetal")
  # Preserve the historical command-line launch path after the app bundle was
  # renamed. Without this compatibility link, an old lowercase bundle can
  # survive indefinitely and launch a stale renderer even after a successful
  # rebuild of the canonical target.
  set(TETRA_METAL_LEGACY_EXECUTABLE
    "${CMAKE_CURRENT_BINARY_DIR}/tetra_world_metal.app/Contents/MacOS/tetra_world_metal")
  add_custom_command(TARGET tetra_world_metal POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/tetra_world_metal.app/Contents/MacOS"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${TETRA_METAL_LEGACY_EXECUTABLE}"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            "$<TARGET_FILE:tetra_world_metal>"
            "${TETRA_METAL_LEGACY_EXECUTABLE}"
    VERBATIM)
endif()
