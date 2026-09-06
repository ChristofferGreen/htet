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
         ${TETRA_SHADER_DIR}/gpu_terrain_extract.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_classify.comp.spv
         ${TETRA_SHADER_DIR}/gpu_terrain_triangles.comp.spv
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
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_extract.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_extract.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_classify.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_classify.comp.spv
  COMMAND ${GLSLC} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_terrain_triangles.comp -o ${TETRA_SHADER_DIR}/gpu_terrain_triangles.comp.spv
    DEPENDS scene.vert scene.frag wire.frag edge.vert edge.frag shadow.vert sky.vert sky.frag fullscreen.vert tone_map.frag atmosphere.comp atmosphere_reference_hillaire.comp gpu_lod.comp gpu_terrain_extract.comp gpu_terrain_classify.comp gpu_terrain_triangles.comp)
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
  ${TETRA_SHADER_DIR}/gpu_terrain_extract.comp.spv
  ${TETRA_SHADER_DIR}/gpu_terrain_classify.comp.spv
  ${TETRA_SHADER_DIR}/gpu_terrain_triangles.comp.spv)
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
    DEPENDS gpu_terrain_classify.comp spirv-cross
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
            ${TETRA_METAL_GPU_TERRAIN_EXTRACT_MSL}
            ${TETRA_METAL_GPU_TERRAIN_CLASSIFY_MSL}
            ${TETRA_METAL_GPU_TERRAIN_TRIANGLES_MSL}
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
  add_test(NAME "metal GPU terrain runtime parity"
    COMMAND $<TARGET_FILE:tetra_world_metal> --metal-gpu-terrain-runtime-smoke-test)
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
