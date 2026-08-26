#pragma once

#include "tetra_core/tet_mesh.hpp"

#include <iosfwd>
#include <string_view>

namespace tetra_viewer {

// Deterministic graphics-free tetra_world controller trace. Commands are
// comma-separated: idle:N, forward:N, sprint:N, jump:N, look:DX:DY.
int run_world_script(std::string_view script,std::ostream& output,
                     std::ostream& errors);
void print_world_script_help(std::ostream& output);
int run_world_runtime_benchmark(std::ostream& output,std::ostream& errors);
int capture_world_runtime(std::string_view path,std::ostream& output,
                          std::ostream& errors);
int capture_world_runtime_view(std::string_view path,
                               tetra::Vec3 camera_position,
                               tetra::Vec3 target,std::ostream& output,
                               std::ostream& errors);

}  // namespace tetra_viewer
