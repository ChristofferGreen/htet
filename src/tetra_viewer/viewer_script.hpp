#pragma once

#include <iosfwd>
#include <string_view>

namespace tetra_viewer {

// Runs a comma-separated, headless viewer workflow.  This entry point has no
// GLFW, Dear ImGui, or Vulkan dependency and is therefore safe in CI and on
// machines without a graphics runtime.
int run_script(std::string_view script, std::ostream& output, std::ostream& errors);

void print_script_help(std::ostream& output);

}  // namespace tetra_viewer
