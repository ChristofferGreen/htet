#include "tetra_viewer/viewer_script.hpp"
#include "tetra_viewer/viewer_scene.hpp"

#include "tetra_core/implicit_surface.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace tetra_viewer {
namespace {

using Clock = std::chrono::steady_clock;

struct ScriptState {
  tetra::TetMesh mesh{tetra::TetMesh::make_unit_cube(default_subdivision_method)};
  tetra::Sphere sphere{};
  tetra::Camera camera{};
  double pixel_threshold{28.0};
  unsigned int maximum_depth{16};
  SurfaceMethod surface_method{default_surface_method};
  VolumeConnectionMethod volume_connection_method{default_volume_connection_method};
  StencilConstruction stencil_construction{StencilConstruction::fixed};
  StencilSelectionObjective stencil_selection_objective{StencilSelectionObjective::balanced};
  ShadingModel shading_model{ShadingModel::studio_flat};
  MaterialRule material_rule{MaterialRule::variational_smooth};
  bool show_faces{true};
  bool show_surface_edges{true};
  bool show_hierarchy_edges{};
  bool show_volume_edges{true};
  bool show_volume_faces{true};
  bool x_cutaway{true};
  double x_cut_position{0.5};
};

void enforce_conforming_smooth_cutaway(ScriptState& state){
  // Method selections are authoritative. Unsupported or disconnected
  // combinations are reported by their own diagnostics; never silently
  // replace the user's selected construction.
  static_cast<void>(state);
}

tetra::AdaptiveResult refine_to_current_surface(ScriptState& state){
  if(state.surface_method==SurfaceMethod::full_tetrahedra&&
     is_variational_material_rule(state.material_rule))
    return tetra::refine_to_whole_cell_surface(state.mesh,state.sphere,state.camera,
                                                state.pixel_threshold,state.maximum_depth,
                                                whole_cell_options(state.material_rule));
  return tetra::refine_to_sphere(state.mesh,state.sphere,state.camera,
                                 state.pixel_threshold,state.maximum_depth);
}

struct InitializedScriptState {
  ScriptState state;
  tetra::AdaptiveResult refinement;
};

const InitializedScriptState& initialized_script_state(){
  static const InitializedScriptState initialized=[] {
    InitializedScriptState value;
    value.refinement=refine_to_current_surface(value.state);
    return value;
  }();
  return initialized;
}

double milliseconds_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string_view trim(std::string_view value) {
  const auto is_space = [](char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
  };
  while (!value.empty() && is_space(value.front())) value.remove_prefix(1);
  while (!value.empty() && is_space(value.back())) value.remove_suffix(1);
  return value;
}

std::vector<std::string_view> split_commands(std::string_view script) {
  std::vector<std::string_view> commands;
  while (true) {
    const auto comma = script.find(',');
    commands.push_back(trim(script.substr(0, comma)));
    if (comma == std::string_view::npos) break;
    script.remove_prefix(comma + 1);
  }
  return commands;
}

bool parse_double(std::string_view text, double& value) {
  text = trim(text);
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end && std::isfinite(value);
}

bool parse_unsigned(std::string_view text, unsigned int& value) {
  text = trim(text);
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end;
}

bool parse_vec3(std::string_view text, tetra::Vec3& value) {
  const auto first_separator = text.find(':');
  if (first_separator == std::string_view::npos) return false;
  const auto second_separator = text.find(':', first_separator + 1);
  if (second_separator == std::string_view::npos || text.find(':', second_separator + 1) != std::string_view::npos)
    return false;
  return parse_double(text.substr(0, first_separator), value.x) &&
      parse_double(text.substr(first_separator + 1, second_separator - first_separator - 1), value.y) &&
      parse_double(text.substr(second_separator + 1), value.z);
}

unsigned int maximum_active_depth(const tetra::TetMesh& mesh) {
  unsigned int depth = 0;
  for (const auto address : mesh.active_leaves()) depth = std::max(depth, mesh.refinement_depth(address));
  return depth;
}

void write_mesh_fields(std::ostream& output, const ScriptState& state) {
  output << "\"subdivision_method\":\"" << tetra::subdivision_method_key(state.mesh.subdivision_method()) << '"'
         << ",\"surface_method\":\"" << surface_method_key(state.surface_method) << '"'
         << ",\"volume_connection\":\"" << volume_connection_method_key(state.volume_connection_method) << '"'
         << ",\"stencil_construction\":\"" << stencil_construction_key(state.stencil_construction) << '"'
         << ",\"stencil_objective\":\"" << stencil_selection_objective_key(state.stencil_selection_objective) << '"'
         << ",\"shading_model\":\"" << shading_model_key(state.shading_model) << '"'
         << ",\"material_rule\":\"" << material_rule_key(state.material_rule) << '"'
         << ",\"solid_faces\":" << (state.show_faces ? "true" : "false")
         << ",\"surface_edges\":" << (state.show_surface_edges ? "true" : "false")
         << ",\"hierarchy_edges\":" << (state.show_hierarchy_edges ? "true" : "false")
         << ",\"volume_edges\":" << (state.show_volume_edges ? "true" : "false")
         << ",\"solid_volume\":" << (state.show_volume_faces ? "true" : "false")
         << ",\"x_cutaway\":" << (state.x_cutaway ? "true" : "false")
         << ",\"x_cut_position\":" << state.x_cut_position
         << ",\"lod_camera\":[" << state.camera.position.x << ','
         << state.camera.position.y << ',' << state.camera.position.z << ']'
         << ",\"lod_direction\":[" << state.camera.forward.x << ','
         << state.camera.forward.y << ',' << state.camera.forward.z << ']'
         << ",\"active_leaves\":" << state.mesh.active_leaves().size()
         << ",\"stored_tetrahedra\":" << state.mesh.tetrahedron_count()
         << ",\"vertices\":" << state.mesh.vertices().size()
         << ",\"layers\":" << state.mesh.layers().size()
         << ",\"maximum_active_depth\":" << maximum_active_depth(state.mesh)
         << ",\"total_active_volume\":" << std::setprecision(17) << state.mesh.total_active_volume();
}

void write_command_event(std::ostream& output, std::string_view command, double duration_ms, const ScriptState& state) {
  output << "{\"event\":\"command\",\"command\":\"" << command
         << "\",\"duration_ms\":" << std::fixed << std::setprecision(3) << duration_ms << ',';
  write_mesh_fields(output, state);
  output << "}\n";
}

void write_stats(std::ostream& output, const ScriptState& state) {
  output << "{\"event\":\"stats\",";
  write_mesh_fields(output, state);
  output << ",\"shape_scale\":" << state.sphere.radius
         << ",\"shape\":\"" << tetra::implicit_shape_key(state.sphere.kind) << '"'
         << ",\"pixel_threshold\":" << state.pixel_threshold
         << ",\"maximum_depth\":" << state.maximum_depth
         << ",\"layer_sizes\":[";
  for (std::size_t index = 0; index < state.mesh.layers().size(); ++index) {
    if (index != 0) output << ',';
    output << state.mesh.layers()[index].tetrahedra.size();
  }
  output << "]}\n";
}

void write_error(std::ostream& errors, std::string_view message, std::string_view command = {}) {
  errors << "{\"event\":\"error\",\"message\":\"" << message << '"';
  if (!command.empty()) errors << ",\"command\":\"" << command << '"';
  errors << "}\n";
}

enum class SetResult { not_recognized, success, error };

SetResult set_double_command(std::string_view command, std::string_view prefix, double minimum, double maximum,
                             double& target, std::ostream& errors) {
  if (!command.starts_with(prefix)) return SetResult::not_recognized;
  double value = 0.0;
  if (!parse_double(command.substr(prefix.size()), value) || value < minimum || value > maximum) {
    write_error(errors, "value outside the supported range", command);
    return SetResult::error;
  }
  target = value;
  return SetResult::success;
}

bool render_image(const ScriptState& state, std::string_view path, std::ostream& errors) {
  constexpr int width = 800;
  constexpr int height = 800;
  constexpr double near_plane = 0.01;
  const auto scene = prepare_scene(
      state.mesh, state.sphere, state.surface_method, state.material_rule,
      state.show_faces, false, state.show_surface_edges, false,
      state.x_cutaway && state.show_volume_edges,
      state.x_cutaway && state.show_volume_faces, state.x_cut_position,
      state.volume_connection_method,state.stencil_construction,
      state.stencil_selection_objective);
  const auto dot = [](tetra::Vec3 a, tetra::Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; };
  const auto cross = [](tetra::Vec3 a, tetra::Vec3 b) {
    return tetra::Vec3{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
  };
  const auto normalize = [&dot](tetra::Vec3 value) {
    const double length = std::sqrt(dot(value, value));
    return length > 0.0 ? value / length : tetra::Vec3{};
  };
  const auto angle_colour = [](double angle) {
    using Colour=std::array<double,3>;
    const Colour blue{{0.08,0.20,0.82}},cyan{{0.05,0.78,0.92}},green{{0.12,0.78,0.30}};
    const Colour yellow{{0.96,0.88,0.12}},orange{{1.00,0.42,0.06}},red{{0.88,0.04,0.04}};
    const Colour magenta{{0.82,0.05,0.70}},white{{1.00,0.96,1.00}};
    const auto mix=[](Colour first,Colour second,double amount){
      return Colour{{first[0]+(second[0]-first[0])*amount,
                     first[1]+(second[1]-first[1])*amount,
                     first[2]+(second[2]-first[2])*amount}};
    };
    if(angle<0.5)return blue;
    if(angle<1.0)return mix(blue,cyan,(angle-0.5)/0.5);
    if(angle<2.0)return mix(cyan,green,angle-1.0);
    if(angle<5.0)return mix(green,yellow,(angle-2.0)/3.0);
    if(angle<10.0)return mix(yellow,orange,(angle-5.0)/5.0);
    if(angle<20.0)return mix(orange,red,(angle-10.0)/10.0);
    if(angle<45.0)return mix(red,magenta,(angle-20.0)/25.0);
    return mix(magenta,white,std::clamp((angle-45.0)/45.0,0.0,1.0));
  };
  const tetra::Vec3 forward=normalize(state.camera.forward);
  const tetra::Vec3 right=normalize(cross(forward,state.camera.up));
  const tetra::Vec3 up = cross(right, forward);
  const double tangent = std::tan(state.camera.vertical_fov_radians * 0.5);

  struct Projected { double x{}; double y{}; double depth{}; };
  const auto project = [&](const SceneVertex& vertex) {
    const tetra::Vec3 point{vertex.position[0], vertex.position[1], vertex.position[2]};
    const tetra::Vec3 offset = point - state.camera.position;
    const double depth = dot(offset, forward);
    return Projected{
        (dot(offset, right) / (depth * tangent) * 0.5 + 0.5) * (width - 1),
        (0.5 - dot(offset, up) / (depth * tangent) * 0.5) * (height - 1),
        depth};
  };
  std::vector<std::array<std::uint8_t, 3>> pixels(
      static_cast<std::size_t>(width * height), {15, 20, 28});
  std::vector<double> depths(static_cast<std::size_t>(width * height), std::numeric_limits<double>::infinity());
  const auto edge = [](Projected a, Projected b, double x, double y) {
    return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
  };
  for (std::size_t triangle = 0; triangle + 2 < scene.triangle_vertices.size(); triangle += 3) {
    const auto& va = scene.triangle_vertices[triangle];
    const auto& vb = scene.triangle_vertices[triangle + 1];
    const auto& vc = scene.triangle_vertices[triangle + 2];
    const Projected a = project(va), b = project(vb), c = project(vc);
    if (a.depth <= near_plane || b.depth <= near_plane || c.depth <= near_plane) continue;
    const double area = edge(a, b, c.x, c.y);
    if (std::abs(area) < 1e-12) continue;
    const int minimum_x = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int maximum_x = std::min(width - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int minimum_y = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int maximum_y = std::min(height - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    tetra::Vec3 normal = normalize({va.normal[0], va.normal[1], va.normal[2]});
    const tetra::Vec3 centre{
        (va.position[0] + vb.position[0] + vc.position[0]) / 3.0,
        (va.position[1] + vb.position[1] + vc.position[1]) / 3.0,
        (va.position[2] + vb.position[2] + vc.position[2]) / 3.0};
    const tetra::Vec3 light = normalize(state.camera.position - centre);
    const bool volume_cut=va.diagnostics[0]<-0.5F;
    const double facing=dot(normal,light);
    const double illumination=volume_cut
        ? 0.48+0.52*std::abs(facing)
        : 0.22+0.78*std::max(0.0,facing);
    const tetra::Vec3 up_seed=std::abs(light.y)<0.9?tetra::Vec3{0.0,1.0,0.0}:tetra::Vec3{1.0,0.0,0.0};
    const tetra::Vec3 diagnostic_right=normalize(cross(up_seed,light));
    const tetra::Vec3 diagnostic_up=cross(light,diagnostic_right);
    const tetra::Vec3 diagnostic_light=normalize({light.x+0.55*diagnostic_right.x+0.35*diagnostic_up.x,
                                                   light.y+0.55*diagnostic_right.y+0.35*diagnostic_up.y,
                                                   light.z+0.55*diagnostic_right.z+0.35*diagnostic_up.z});
    const double diagnostic_relief=0.78+0.22*std::max(0.0,dot(normal,diagnostic_light));
    std::array<double,3> shaded{};
    if(state.shading_model==ShadingModel::dihedral_angle&&!volume_cut){
      shaded=angle_colour(va.diagnostics[0]);
      for(auto& channel:shaded)channel*=diagnostic_relief;
    }else if(state.shading_model==ShadingModel::normal_error&&!volume_cut){
      shaded=angle_colour(va.diagnostics[1]);
      for(auto& channel:shaded)channel*=diagnostic_relief;
    }else if(state.shading_model==ShadingModel::reflection_stripes&&!volume_cut){
      const double reflection_scale=2.0*dot(light,normal);
      const tetra::Vec3 reflected{normal.x*reflection_scale-light.x,
                                  normal.y*reflection_scale-light.y,
                                  normal.z*reflection_scale-light.z};
      const tetra::Vec3 stripe_axis=normalize({0.35,0.82,0.45});
      const double wave=0.5+0.5*std::cos(dot(reflected,stripe_axis)*56.5486678);
      const double stripe=std::clamp((wave-0.18)/(0.82-0.18),0.0,1.0);
      const double smooth=stripe*stripe*(3.0-2.0*stripe);
      shaded={{0.025+(0.88-0.025)*smooth,0.055+(0.96-0.055)*smooth,0.10+(1.0-0.10)*smooth}};
    }else{
      shaded={{va.colour[0]*illumination,va.colour[1]*illumination,va.colour[2]*illumination}};
    }
    for (int y = minimum_y; y <= maximum_y; ++y) for (int x = minimum_x; x <= maximum_x; ++x) {
      const double sample_x = x + 0.5, sample_y = y + 0.5;
      const double wa = edge(b, c, sample_x, sample_y) / area;
      const double wb = edge(c, a, sample_x, sample_y) / area;
      const double wc = edge(a, b, sample_x, sample_y) / area;
      if (wa < 0.0 || wb < 0.0 || wc < 0.0) continue;
      const bool connected_surface=va.diagnostics[0]<-1.5F;
      const int edge_flags=static_cast<int>(va.edge_flags+0.5F);
      const bool wire_only_volume=volume_cut&&!connected_surface&&(edge_flags&8)!=0;
      const double depth = wa * a.depth + wb * b.depth + wc * c.depth;
      const double world_x=wa*va.position[0]+wb*vb.position[0]+wc*vc.position[0];
      if(state.x_cutaway&&!volume_cut&&world_x>state.x_cut_position)continue;
      const std::size_t index = static_cast<std::size_t>(y * width + x);
      if (depth >= depths[index]) continue;
      depths[index] = depth;
      if(!wire_only_volume&&((connected_surface&&state.show_faces)||(volume_cut&&!connected_surface)||
         (!volume_cut&&state.show_faces))){
        for(std::size_t channel=0;channel<3;++channel)
          pixels[index][channel]=static_cast<std::uint8_t>(255.0*shaded[channel]);
      }else{
        const std::array<double,3> background{{0.06,0.08,0.11}};
        for(std::size_t channel=0;channel<3;++channel)
          pixels[index][channel]=static_cast<std::uint8_t>(255.0*background[channel]);
      }
    }
  }

  // Match Vulkan: triangles establish depth, then deduplicated fixed-width
  // screen-space edge geometry is depth tested over it.
  const std::array<std::span<const SceneVertex>,2> line_sets{{
      scene.hierarchy_line_vertices,scene.surface_line_vertices}};
  for(const auto line_vertices:line_sets)
  for (std::size_t line = 0; line + 1 < line_vertices.size(); line += 2) {
    SceneVertex first = line_vertices[line];
    SceneVertex second = line_vertices[line + 1];
    if (state.x_cutaway && first.diagnostics[0] >= -0.5F) {
      const bool first_hidden = first.position[0] > state.x_cut_position;
      const bool second_hidden = second.position[0] > state.x_cut_position;
      if (first_hidden && second_hidden) continue;
      if (first_hidden != second_hidden) {
        SceneVertex& hidden = first_hidden ? first : second;
        const SceneVertex& visible = first_hidden ? second : first;
        const double denominator = hidden.position[0] - visible.position[0];
        if (std::abs(denominator) < 1e-12) continue;
        const double amount = (state.x_cut_position - visible.position[0]) / denominator;
        for (std::size_t axis = 0; axis < 3; ++axis) {
          hidden.position[axis] = static_cast<float>(
              visible.position[axis] + amount * (hidden.position[axis] - visible.position[axis]));
          hidden.colour[axis] = static_cast<float>(
              visible.colour[axis] + amount * (hidden.colour[axis] - visible.colour[axis]));
        }
      }
    }
    const Projected a = project(first), b = project(second);
    if (a.depth <= near_plane || b.depth <= near_plane) continue;
    const double delta_x = b.x - a.x, delta_y = b.y - a.y;
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::abs(delta_x), std::abs(delta_y)))));
    for (int step = 0; step <= steps; ++step) {
      const double amount = static_cast<double>(step) / steps;
      const int x = static_cast<int>(std::lround(a.x + amount * delta_x));
      const int y = static_cast<int>(std::lround(a.y + amount * delta_y));
      if (x < 0 || x >= width || y < 0 || y >= height) continue;
      const double depth = a.depth + amount * (b.depth - a.depth);
      const std::size_t index = static_cast<std::size_t>(y * width + x);
      const double depth_tolerance=first.diagnostics[0]<-1.5F?1.0e-3:1.0e-5;
      if (depth > depths[index]+depth_tolerance) continue;
      depths[index] = depth;
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const double colour = first.colour[channel] + amount * (second.colour[channel] - first.colour[channel]);
        pixels[index][channel] = static_cast<std::uint8_t>(255.0 * std::clamp(colour, 0.0, 1.0));
      }
    }
  }

  std::ofstream image(std::string(path), std::ios::binary);
  if (!image) {
    write_error(errors, "could not open image output path", path);
    return false;
  }
  image << "P6\n" << width << ' ' << height << "\n255\n";
  for (const auto& pixel : pixels)
    image.write(reinterpret_cast<const char*>(pixel.data()), static_cast<std::streamsize>(pixel.size()));
  if (!image) {
    write_error(errors, "could not write image output", path);
    return false;
  }
  return true;
}

}  // namespace

void print_script_help(std::ostream& output) {
  output << "Usage: tetra_viewer --script \"command[,command...]\"\n"
            "\n"
            "The initial state matches the interactive viewer and is adaptively refined\n"
            "before the first command. Commands execute from left to right:\n"
            "  refine-once                 Bisect every active tetrahedron once\n"
            "  refine-to-convergence       Refine sphere intersections to the current limit\n"
            "  validate                    Check volumes, adjacency, and conformity\n"
            "  validate-volume             Validate the selected connected volume\n"
            "  prepare-scene               Build cached CPU geometry and statistics\n"
            "  render-image=<path.ppm>     Write a deterministic headless mesh image\n"
            "  benchmark-refinement=<1..8> Run and time increasing refinement passes\n"
            "  stats                       Print mesh and hierarchy statistics\n"
            "  set-method=<key>            Reset using a registered subdivision method\n"
            "  set-surface-method=<key>    Select a registered surface generation method\n"
            "  set-volume-connection=<key> Select hierarchy cells or adaptive cleaving\n"
            "  set-stencil-construction=<fixed|selected> Select fixed templates or the atlas\n"
            "  set-stencil-objective=<surface|balanced|volume> Select atlas scoring\n"
            "  set-shading-model=<key>     Select a registered diagnostic shading model\n"
            "  set-solid-faces=<on|off>    Show or hide filled surface triangles\n"
            "  set-surface-edges=<on|off>  Show or hide anti-aliased surface edges\n"
            "  set-hierarchy-edges=<on|off> Show or hide the complete hierarchy overlay\n"
            "  set-volume-edges=<on|off>   Show interior and boundary tetrahedron edges in cutaways\n"
            "  set-solid-volume=<on|off>   Fill exposed faces of whole retained tetrahedra\n"
            "  set-x-cut=<off|0..1>        Hide geometry to the right of an X cut plane\n"
            "  set-material-rule=<key>     Select a registered full-tetrahedron material rule\n"
            "  set-shape=<key>             Select sphere, merging-spheres, cube, capped-cylinder, perlin-terrain, torus, cone, gyroid, or rounded-cube\n"
            "  set-camera=<x:y:z>          Set the camera/LOD origin position\n"
            "  set-camera-direction=<x:y:z> Set the LOD camera view direction\n"
            "  set-radius=<0.001..1.0>     Change the implicit shape scale\n"
            "  set-pixel-threshold=<value> Change the projected-size threshold\n"
            "  set-maximum-depth=<0..32>   Change the adaptive iteration limit\n"
            "\n"
            "Every event is emitted as one JSON object. Parsing or validation failures\n"
            "return a nonzero exit code. No window or graphics API is initialized.\n";
}

int run_script(std::string_view script, std::ostream& output, std::ostream& errors) {
  if (trim(script).empty()) {
    write_error(errors, "script is empty");
    return 2;
  }

  const auto commands = split_commands(script);
  if (std::ranges::any_of(commands, [](std::string_view command) { return command.empty(); })) {
    write_error(errors, "script contains an empty command");
    return 2;
  }

  const auto initialization_start = Clock::now();
  const auto& initialized=initialized_script_state();
  ScriptState state=initialized.state;
  const auto initial_refinement=initialized.refinement;
  output << "{\"event\":\"initialized\",\"duration_ms\":" << std::fixed << std::setprecision(3)
         << milliseconds_since(initialization_start) << ",\"adaptive_iterations\":" << initial_refinement.iterations
         << ",\"refined_leaves\":" << initial_refinement.refined_leaves
         << ",\"reached_depth_limit\":" << (initial_refinement.reached_depth_limit ? "true" : "false") << ',';
  write_mesh_fields(output, state);
  output << "}\n";

  for (const auto command : commands) {
    constexpr std::string_view shape_prefix="set-shape=";
    if(command.starts_with(shape_prefix)){
      const auto key=trim(command.substr(shape_prefix.size()));
      const auto found=std::find_if(tetra::implicit_shape_kinds.begin(),
          tetra::implicit_shape_kinds.end(),[&](auto kind){return tetra::implicit_shape_key(kind)==key;});
      if(found==tetra::implicit_shape_kinds.end()){
        write_error(errors,"unknown implicit shape",command);return 2;
      }
      state.sphere.kind=*found;
      state.sphere.secondary=tetra::implicit_shape_default_secondary(*found);
      state.mesh.reset_active_hierarchy();
      const auto start=Clock::now();
      const auto result=refine_to_current_surface(state);
      output<<"{\"event\":\"shape\",\"shape\":\""<<key
            <<"\",\"duration_ms\":"<<std::fixed<<std::setprecision(3)
            <<milliseconds_since(start)<<",\"adaptive_iterations\":"<<result.iterations<<',';
      write_mesh_fields(output,state);output<<"}\n";
      continue;
    }
    constexpr std::string_view camera_prefix = "set-camera=";
    if (command.starts_with(camera_prefix)) {
      tetra::Vec3 position{};
      if (!parse_vec3(command.substr(camera_prefix.size()), position)) {
        write_error(errors, "camera must contain three finite colon-separated values", command);
        return 2;
      }
      state.camera.position = position;
      const auto direction=state.sphere.centre-position;
      const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                    direction.z*direction.z);
      if(length>1.0e-15)state.camera.forward=direction/length;
      write_command_event(output, command, 0.0, state);
      continue;
    }
    constexpr std::string_view camera_direction_prefix="set-camera-direction=";
    if(command.starts_with(camera_direction_prefix)){
      tetra::Vec3 direction{};
      if(!parse_vec3(command.substr(camera_direction_prefix.size()),direction)){
        write_error(errors,"camera direction must contain three finite colon-separated values",command);
        return 2;
      }
      const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                    direction.z*direction.z);
      if(length<=1.0e-15){
        write_error(errors,"camera direction must be nonzero",command);
        return 2;
      }
      state.camera.forward=direction/length;
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view render_prefix = "render-image=";
    if (command.starts_with(render_prefix)) {
      const auto path = trim(command.substr(render_prefix.size()));
      if (path.empty() || !render_image(state, path, errors)) return 2;
      output << "{\"event\":\"image\",\"path\":\"" << path << "\",";
      write_mesh_fields(output, state);
      output << "}\n";
      continue;
    }
    constexpr std::string_view method_prefix = "set-method=";
    if (command.starts_with(method_prefix)) {
      const auto key = command.substr(method_prefix.size());
      const auto method = std::ranges::find_if(tetra::subdivision_methods, [key](tetra::SubdivisionMethod candidate) {
        return tetra::subdivision_method_key(candidate) == key;
      });
      if (method == tetra::subdivision_methods.end()) {
        write_error(errors, "unknown subdivision method", command);
        return 2;
      }
      const auto start = Clock::now();
      state.mesh = tetra::TetMesh::make_unit_cube(*method);
      const auto result = refine_to_current_surface(state);
      const auto duration = milliseconds_since(start);
      output << "{\"event\":\"command\",\"command\":\"" << command
             << "\",\"duration_ms\":" << std::fixed << std::setprecision(3) << duration
             << ",\"adaptive_iterations\":" << result.iterations
             << ",\"refined_leaves\":" << result.refined_leaves
             << ",\"reached_depth_limit\":" << (result.reached_depth_limit ? "true" : "false") << ',';
      write_mesh_fields(output, state);
      output << "}\n";
      continue;
    }
    constexpr std::string_view material_rule_prefix = "set-material-rule=";
    if (command.starts_with(material_rule_prefix)) {
      const auto key = command.substr(material_rule_prefix.size());
      const auto rule = std::ranges::find_if(material_rules, [key](MaterialRule candidate) {
        return material_rule_key(candidate) == key;
      });
      if (rule == material_rules.end()) {
        write_error(errors, "unknown material rule", command);
        return 2;
      }
      state.material_rule = *rule;
      write_command_event(output, command, 0.0, state);
      continue;
    }
    constexpr std::string_view surface_method_prefix = "set-surface-method=";
    if (command.starts_with(surface_method_prefix)) {
      const auto key = command.substr(surface_method_prefix.size());
      const auto method = std::ranges::find_if(surface_methods, [key](SurfaceMethod candidate) {
        return surface_method_key(candidate) == key;
      });
      if (method == surface_methods.end()) {
        write_error(errors, "unknown surface method", command);
        return 2;
      }
      if(state.volume_connection_method==VolumeConnectionMethod::fixed_surface_shell&&
         *method!=SurfaceMethod::surface_optimization){
        write_error(errors,"fixed surface shell requires surface optimization",command);
        return 2;
      }
      state.surface_method = *method;
      enforce_conforming_smooth_cutaway(state);
      write_command_event(output, command, 0.0, state);
      continue;
    }
    constexpr std::string_view volume_connection_prefix = "set-volume-connection=";
    if (command.starts_with(volume_connection_prefix)) {
      const auto key = command.substr(volume_connection_prefix.size());
      const auto method = std::ranges::find_if(
          volume_connection_methods, [key](VolumeConnectionMethod candidate) {
            return volume_connection_method_key(candidate) == key;
          });
      if (method == volume_connection_methods.end()) {
        write_error(errors, "unknown volume connection method", command);
        return 2;
      }
      if(*method==VolumeConnectionMethod::fixed_surface_shell&&
         state.surface_method!=SurfaceMethod::surface_optimization){
        write_error(errors,"fixed surface shell requires surface optimization",command);
        return 2;
      }
      state.volume_connection_method = *method;
      enforce_conforming_smooth_cutaway(state);
      write_command_event(output, command, 0.0, state);
      continue;
    }
    constexpr std::string_view stencil_construction_prefix="set-stencil-construction=";
    if(command.starts_with(stencil_construction_prefix)){
      const auto key=command.substr(stencil_construction_prefix.size());
      const auto construction=std::ranges::find_if(
          stencil_constructions,[key](StencilConstruction candidate){
            return stencil_construction_key(candidate)==key;
          });
      if(construction==stencil_constructions.end()){
        write_error(errors,"unknown stencil construction",command);
        return 2;
      }
      state.stencil_construction=*construction;
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view stencil_objective_prefix="set-stencil-objective=";
    if(command.starts_with(stencil_objective_prefix)){
      const auto key=command.substr(stencil_objective_prefix.size());
      const auto objective=std::ranges::find_if(
          stencil_selection_objectives,[key](StencilSelectionObjective candidate){
            return stencil_selection_objective_key(candidate)==key;
          });
      if(objective==stencil_selection_objectives.end()){
        write_error(errors,"unknown stencil objective",command);
        return 2;
      }
      state.stencil_selection_objective=*objective;
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view shading_model_prefix = "set-shading-model=";
    if (command.starts_with(shading_model_prefix)) {
      const auto key = command.substr(shading_model_prefix.size());
      const auto model = std::ranges::find_if(shading_models, [key](ShadingModel candidate) {
        return shading_model_key(candidate) == key;
      });
      if (model == shading_models.end()) {
        write_error(errors, "unknown shading model", command);
        return 2;
      }
      state.shading_model = *model;
      write_command_event(output, command, 0.0, state);
      continue;
    }
    constexpr std::string_view solid_faces_prefix = "set-solid-faces=";
    if (command.starts_with(solid_faces_prefix)) {
      const auto value=command.substr(solid_faces_prefix.size());
      if(value!="on"&&value!="off"){
        write_error(errors,"solid faces must be on or off",command);
        return 2;
      }
      state.show_faces=value=="on";
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view surface_edges_prefix = "set-surface-edges=";
    if (command.starts_with(surface_edges_prefix)) {
      const auto value=command.substr(surface_edges_prefix.size());
      if(value!="on"&&value!="off"){
        write_error(errors,"surface edges must be on or off",command);
        return 2;
      }
      state.show_surface_edges=value=="on";
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view hierarchy_edges_prefix = "set-hierarchy-edges=";
    if(command.starts_with(hierarchy_edges_prefix)){
      const auto value=command.substr(hierarchy_edges_prefix.size());
      if(value=="on")state.show_hierarchy_edges=true;
      else if(value=="off")state.show_hierarchy_edges=false;
      else{write_error(errors,"hierarchy edges must be on or off",command);return 2;}
      write_command_event(output,command,0.0,state);continue;
    }
    constexpr std::string_view volume_edges_prefix = "set-volume-edges=";
    if (command.starts_with(volume_edges_prefix)) {
      const auto value=command.substr(volume_edges_prefix.size());
      if(value!="on"&&value!="off"){
        write_error(errors,"volume edges must be on or off",command);
        return 2;
      }
      state.show_volume_edges=value=="on";
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view solid_volume_prefix = "set-solid-volume=";
    if (command.starts_with(solid_volume_prefix)) {
      const auto value=command.substr(solid_volume_prefix.size());
      if(value!="on"&&value!="off"){
        write_error(errors,"solid volume must be on or off",command);
        return 2;
      }
      state.show_volume_faces=value=="on";
      enforce_conforming_smooth_cutaway(state);
      write_command_event(output,command,0.0,state);
      continue;
    }
    constexpr std::string_view x_cut_prefix="set-x-cut=";
    if(command.starts_with(x_cut_prefix)){
      const auto value=command.substr(x_cut_prefix.size());
      if(value=="off"){
        state.x_cutaway=false;
      }else if(!parse_double(value,state.x_cut_position)||state.x_cut_position<0.0||state.x_cut_position>1.0){
        write_error(errors,"X cut position outside the supported range",command);
        return 2;
      }else{
        state.x_cutaway=true;
      }
      enforce_conforming_smooth_cutaway(state);
      write_command_event(output,command,0.0,state);
      continue;
    }
    if (command == "refine-once") {
      const auto start = Clock::now();
      state.mesh.refine_all_binary();
      write_command_event(output, command, milliseconds_since(start), state);
      continue;
    }
    if (command == "refine-to-convergence") {
      const auto start = Clock::now();
      const auto result = refine_to_current_surface(state);
      const auto duration = milliseconds_since(start);
      output << "{\"event\":\"command\",\"command\":\"refine-to-convergence\",\"duration_ms\":"
             << std::fixed << std::setprecision(3) << duration
             << ",\"adaptive_iterations\":" << result.iterations
             << ",\"refined_leaves\":" << result.refined_leaves
             << ",\"reached_depth_limit\":" << (result.reached_depth_limit ? "true" : "false") << ',';
      write_mesh_fields(output, state);
      output << "}\n";
      continue;
    }
    if (command == "validate") {
      const auto start = Clock::now();
      const bool positive_volumes = state.mesh.has_positive_active_volumes();
      const bool symmetric_adjacency = state.mesh.has_symmetric_active_adjacency();
      const bool conforming_faces = state.mesh.has_conforming_active_faces();
      const auto duration = milliseconds_since(start);
      const bool valid = positive_volumes && symmetric_adjacency && conforming_faces;
      output << "{\"event\":\"validation\",\"valid\":" << (valid ? "true" : "false")
             << ",\"positive_volumes\":" << (positive_volumes ? "true" : "false")
             << ",\"symmetric_adjacency\":" << (symmetric_adjacency ? "true" : "false")
             << ",\"conforming_faces\":" << (conforming_faces ? "true" : "false")
             << ",\"duration_ms\":" << std::fixed << std::setprecision(3) << duration << ',';
      write_mesh_fields(output, state);
      output << "}\n";
      if (!valid) return 1;
      continue;
    }
    if (command == "prepare-scene") {
      const auto start = Clock::now();
      const auto scene = prepare_scene(
          state.mesh, state.sphere, state.surface_method, state.material_rule,
          state.show_faces, state.show_hierarchy_edges, state.show_surface_edges, true,
          state.x_cutaway && state.show_volume_edges,
          state.x_cutaway && state.show_volume_faces, state.x_cut_position,
          state.volume_connection_method,state.stencil_construction,
          state.stencil_selection_objective);
      const auto projection = prepare_projection_statistics(
          state.mesh, scene, state.camera, state.pixel_threshold);
      const auto duration = milliseconds_since(start);
      output << "{\"event\":\"scene_preparation\",\"duration_ms\":" << std::fixed << std::setprecision(3) << duration
             << ",\"statistics_ms\":" << scene.statistics_milliseconds
             << ",\"upload_preparation_ms\":" << scene.upload_preparation_milliseconds
             << ",\"triangle_vertices\":" << scene.triangle_vertices.size()
             << ",\"hierarchy_line_vertices\":" << scene.hierarchy_line_vertices.size()
             << ",\"surface_line_vertices\":" << scene.surface_line_vertices.size()
             << ",\"line_vertices\":" << (scene.hierarchy_line_vertices.size()+scene.surface_line_vertices.size())
             << ",\"volume_internal_edges\":" << scene.volume_internal_edges
             << ",\"volume_boundary_edges\":" << scene.volume_boundary_edges
             << ",\"visible_volume_face_triangles\":" << scene.visible_volume_face_triangles
             << ",\"connected_surface_edges\":" << scene.connected_surface_edges
             << ",\"connected_volume_vertices\":" << scene.connected_volume_vertices.size()
             << ",\"connected_volume_tetrahedra\":" << scene.connected_volume_tetrahedra.size()
             << ",\"upload_bytes\":" << (scene.triangle_vertices.size()+scene.hierarchy_line_vertices.size()+scene.surface_line_vertices.size())*sizeof(SceneVertex)
             << ",\"inside\":" << scene.inside_count
             << ",\"outside\":" << scene.outside_count
             << ",\"intersecting\":" << scene.intersecting_count
             << ",\"selected\":" << scene.selected_count
             << ",\"whole_cell_boundary_faces\":" << scene.whole_cell_boundary_faces
             << ",\"whole_cell_nonmanifold_edges\":" << scene.whole_cell_nonmanifold_edges
             << ",\"whole_cell_hash\":" << scene.whole_cell_hash
             << ",\"whole_cell_selected_volume\":" << scene.whole_cell_selected_volume
             << ",\"whole_cell_solve_ms\":" << scene.whole_cell_solve_milliseconds
             << ",\"marching_tetrahedra_triangles\":" << scene.marching_tetrahedra_triangles
             << ",\"cleaved_tetrahedra\":" << scene.cleaved_tetrahedra
             << ",\"cleaved_volume\":" << scene.cleaved_volume
             << ",\"surface_layer_tetrahedra\":" << scene.surface_layer_tetrahedra
             << ",\"dual_contour_triangles\":" << scene.dual_contour_triangles
             << ",\"optimized_surface_vertices\":" << scene.optimized_surface_vertices
             << ",\"rejected_surface_moves\":" << scene.rejected_surface_moves
             << ",\"optimized_volume_boundary_vertices\":" << scene.optimized_volume_boundary_vertices
             << ",\"rejected_volume_boundary_moves\":" << scene.rejected_volume_boundary_moves
             << ",\"selected_stencil_cells\":" << scene.selected_stencil_cells
             << ",\"alternate_stencil_cells\":" << scene.alternate_stencil_cells
             << ",\"connected_surface_hash\":" << scene.connected_surface_hash
             << ",\"standalone_surface_hash\":" << scene.standalone_surface_hash
             << ",\"hybrid_shell_vertices\":" << scene.hybrid_shell_vertices
             << ",\"hybrid_shell_tetrahedra\":" << scene.hybrid_shell_tetrahedra
             << ",\"hybrid_recovery_steps\":" << scene.hybrid_recovery_steps
             << ",\"hybrid_failed_prisms\":" << scene.hybrid_failed_prisms
             << ",\"hybrid_missing_provenance\":" << scene.hybrid_missing_provenance
             << ",\"hybrid_inset_failures\":" << scene.hybrid_inset_failures
             << ",\"hybrid_missing_inner_faces\":" << scene.hybrid_missing_inner_faces
             << ",\"hybrid_degenerate_prisms\":" << scene.hybrid_degenerate_prisms
             << ",\"hybrid_unmatched_faces\":" << scene.hybrid_unmatched_faces
             << ",\"hybrid_volume_valid\":" << (scene.hybrid_volume_valid?"true":"false")
             << std::setprecision(9)
             << ",\"minimum_connected_tet_quality_before\":" << scene.minimum_connected_tet_quality_before
             << ",\"minimum_connected_tet_quality_after\":" << scene.minimum_connected_tet_quality_after
             << ",\"minimum_connected_tet_volume_surface_quality_before\":"
             << scene.minimum_connected_tet_volume_surface_quality_before
             << ",\"minimum_connected_tet_volume_surface_quality_after\":"
             << scene.minimum_connected_tet_volume_surface_quality_after
             << ",\"minimum_connected_tet_dihedral_sine_before\":"
             << scene.minimum_connected_tet_dihedral_sine_before
             << ",\"minimum_connected_tet_dihedral_sine_after\":"
             << scene.minimum_connected_tet_dihedral_sine_after
             << ",\"minimum_connected_tet_dihedral_degrees_after\":"
             << scene.minimum_connected_tet_dihedral_degrees_after
             << ",\"maximum_connected_tet_dihedral_degrees_after\":"
             << scene.maximum_connected_tet_dihedral_degrees_after
             << ",\"mean_dihedral_degrees\":" << scene.mean_dihedral_degrees
             << ",\"percentile95_dihedral_degrees\":" << scene.percentile95_dihedral_degrees
             << ",\"percentile99_dihedral_degrees\":" << scene.percentile99_dihedral_degrees
             << ",\"maximum_dihedral_degrees\":" << scene.maximum_dihedral_degrees
             << ",\"mean_normal_error_degrees\":" << scene.mean_normal_error_degrees
             << ",\"percentile95_normal_error_degrees\":" << scene.percentile95_normal_error_degrees
             << ",\"percentile99_normal_error_degrees\":" << scene.percentile99_normal_error_degrees
             << ",\"maximum_normal_error_degrees\":" << scene.maximum_normal_error_degrees
             << ",\"minimum_surface_triangle_angle_degrees\":" << scene.minimum_surface_triangle_angle_degrees
             << ",\"maximum_surface_triangle_edge_ratio\":" << scene.maximum_surface_triangle_edge_ratio
             << ",\"pending\":" << projection.pending_count
             << ",\"accepted\":" << projection.accepted_count << ',';
      write_mesh_fields(output, state);
      output << "}\n";
      continue;
    }
    if(command=="validate-volume"){
      const auto start=Clock::now();
      const auto scene=prepare_scene(
          state.mesh,state.sphere,state.surface_method,state.material_rule,
          false,false,false,false,false,false,1.0,
          state.volume_connection_method,state.stencil_construction,
          state.stencil_selection_objective);
      const bool fixed_shell=state.volume_connection_method==VolumeConnectionMethod::fixed_surface_shell;
      const bool hash_match=scene.connected_surface_hash!=0&&
          scene.connected_surface_hash==scene.standalone_surface_hash;
      const auto topology=validate_connected_complex(scene,&state.mesh);
      const bool valid=!fixed_shell||(state.surface_method==SurfaceMethod::surface_optimization&&
          scene.hybrid_volume_valid&&hash_match&&topology.valid);
      output<<"{\"event\":\"volume_validation\",\"valid\":"<<(valid?"true":"false")
            <<",\"hybrid_volume_valid\":"<<(scene.hybrid_volume_valid?"true":"false")
            <<",\"surface_hash_match\":"<<(hash_match?"true":"false")
            <<",\"authoritative_complex\":"<<(topology.valid?"true":"false")
            <<",\"graded_parent_band\":"<<(topology.graded_parent_band?"true":"false")
            <<",\"exterior_faces\":"<<topology.exterior_faces
            <<",\"unmatched_non_surface_faces\":"<<topology.unmatched_non_surface_faces
            <<",\"nonmanifold_faces\":"<<topology.nonmanifold_faces
            <<",\"maximum_adjacent_edge_ratio\":"<<std::setprecision(9)
            <<topology.maximum_adjacent_edge_ratio
            <<",\"maximum_adjacent_parent_edge_ratio\":"
            <<topology.maximum_adjacent_parent_edge_ratio
            <<",\"maximum_adjacent_parent_depth_difference\":"
            <<topology.maximum_adjacent_parent_depth_difference
            <<",\"maximum_ratio_regions\":["
            <<static_cast<int>(topology.maximum_ratio_first_region)<<','
            <<static_cast<int>(topology.maximum_ratio_second_region)<<']'
            <<",\"failed_prisms\":"<<scene.hybrid_failed_prisms
            <<",\"duration_ms\":"<<std::fixed<<std::setprecision(3)<<milliseconds_since(start)<<',';
      write_mesh_fields(output,state);output<<"}\n";
      if(!valid)return 1;
      continue;
    }
    constexpr std::string_view benchmark_prefix = "benchmark-refinement=";
    if (command.starts_with(benchmark_prefix)) {
      unsigned int passes = 0;
      if (!parse_unsigned(command.substr(benchmark_prefix.size()), passes) || passes == 0 || passes > 8) {
        write_error(errors, "benchmark pass count outside the supported range", command);
        return 2;
      }
      for (unsigned int pass = 1; pass <= passes; ++pass) {
        const auto before = state.mesh.active_leaves().size();
        const auto start = Clock::now();
        state.mesh.refine_all_binary();
        const auto duration = milliseconds_since(start);
        output << "{\"event\":\"refinement_benchmark\",\"pass\":" << pass
               << ",\"before_active_leaves\":" << before
               << ",\"duration_ms\":" << std::fixed << std::setprecision(3) << duration << ',';
        write_mesh_fields(output, state);
        output << "}\n";
      }
      continue;
    }
    if (command == "stats") {
      write_stats(output, state);
      continue;
    }
    const auto radius_result = set_double_command(command, "set-radius=", 0.001, 1.0, state.sphere.radius, errors);
    if (radius_result != SetResult::not_recognized) {
      if (radius_result == SetResult::error) return 2;
      write_command_event(output, command, 0.0, state);
      continue;
    }
    const auto threshold_result = set_double_command(
        command, "set-pixel-threshold=", 0.001, 1000000.0, state.pixel_threshold, errors);
    if (threshold_result != SetResult::not_recognized) {
      if (threshold_result == SetResult::error) return 2;
      write_command_event(output, command, 0.0, state);
      continue;
    }
    constexpr std::string_view depth_prefix = "set-maximum-depth=";
    if (command.starts_with(depth_prefix)) {
      unsigned int value = 0;
      if (!parse_unsigned(command.substr(depth_prefix.size()), value) || value > 32U) {
        write_error(errors, "value outside the supported range", command);
        return 2;
      }
      state.maximum_depth = value;
      write_command_event(output, command, 0.0, state);
      continue;
    }

    write_error(errors, "unknown command", command);
    return 2;
  }
  return 0;
}

}  // namespace tetra_viewer
