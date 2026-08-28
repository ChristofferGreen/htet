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

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
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
  DEPENDS scene.vert scene.frag wire.frag edge.vert edge.frag shadow.vert sky.vert sky.frag fullscreen.vert tone_map.frag)
add_custom_target(tetra_viewer_shaders DEPENDS
  ${TETRA_SHADER_DIR}/scene.vert.spv ${TETRA_SHADER_DIR}/scene.frag.spv ${TETRA_SHADER_DIR}/wire.frag.spv
  ${TETRA_SHADER_DIR}/edge.vert.spv ${TETRA_SHADER_DIR}/edge.frag.spv
  ${TETRA_SHADER_DIR}/shadow.vert.spv
  ${TETRA_SHADER_DIR}/sky.vert.spv ${TETRA_SHADER_DIR}/sky.frag.spv
  ${TETRA_SHADER_DIR}/fullscreen.vert.spv ${TETRA_SHADER_DIR}/tone_map.frag.spv)
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
