#include "tetra_viewer/world_script.hpp"

#include "tetra_viewer/first_person_controller.hpp"
#include "tetra_viewer/world_profile.hpp"
#include "tetra_viewer/terrain_runtime.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <chrono>
#include <ostream>
#include <thread>
#include <string_view>
#include <array>
#include <algorithm>
#include <fstream>
#include <limits>
#include <vector>

namespace tetra_viewer {
namespace {

bool parse_size(std::string_view text,std::size_t& value) {
  const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
  return error==std::errc{}&&end==text.data()+text.size()&&value<=1'000'000U;
}

bool parse_double(std::string_view text,double& value) {
  const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
  return error==std::errc{}&&end==text.data()+text.size()&&std::isfinite(value);
}

void hash_value(std::uint64_t& hash,std::uint64_t value) {
  constexpr std::uint64_t prime=1099511628211ULL;
  for(unsigned int byte=0;byte<8U;++byte){
    hash^=(value>>(byte*8U))&0xffU;
    hash*=prime;
  }
}

}  // namespace

void print_world_script_help(std::ostream& output) {
  output<<"tetra_world --script \"idle:N,forward:N,sprint:N,jump:N,look:DX:DY\"\n"
        <<"Each movement step is exactly 1/120 second. Output is one deterministic JSON record.\n";
}

int run_world_script(std::string_view script,std::ostream& output,
                     std::ostream& errors) {
  FirstPersonController controller;
  tetra::Sphere field;
  const auto profile=production_world_profile();
  field.kind=profile.shape;field.terrain=profile.terrain;
  field.secondary=profile.octave_detail_amplitude;
  field.frequency=profile.octave_detail_frequency;
  std::size_t command_count{},step_count{};
  while(!script.empty()){
    const auto separator=script.find(',');
    const auto command=script.substr(0,separator);
    script=separator==std::string_view::npos?std::string_view{}:
        script.substr(separator+1U);
    if(command.empty()){errors<<"empty world-script command\n";return 2;}
    ++command_count;
    const auto colon=command.find(':');
    const auto name=command.substr(0,colon);
    const auto arguments=colon==std::string_view::npos?std::string_view{}:
        command.substr(colon+1U);
    if(name=="look"){
      const auto second=arguments.find(':');
      double x{},y{};
      if(second==std::string_view::npos||
         !parse_double(arguments.substr(0,second),x)||
         !parse_double(arguments.substr(second+1U),y)){
        errors<<"look requires finite DX:DY\n";return 2;
      }
      controller.look(x,y);
      continue;
    }
    std::size_t steps{};
    if(!parse_size(arguments,steps)){
      errors<<name<<" requires a step count in [0,1000000]\n";return 2;
    }
    FirstPersonInput input;
    if(name=="forward")input.forward=1.0;
    else if(name=="back")input.forward=-1.0;
    else if(name=="right")input.right=1.0;
    else if(name=="left")input.right=-1.0;
    else if(name=="sprint"){input.forward=1.0;input.sprint=true;}
    else if(name=="jump")input.jump=true;
    else if(name!="idle"){
      errors<<"unknown world-script command: "<<name<<'\n';return 2;
    }
    for(std::size_t step=0;step<steps;++step){
      controller.advance(1.0/120.0,input,field);
      ++step_count;
    }
  }
  const auto& state=controller.state();
  std::uint64_t hash=1469598103934665603ULL;
  const auto quantize=[](double value){
    return static_cast<std::uint64_t>(std::llround(value*1.0e9));};
  hash_value(hash,quantize(state.feet.x));hash_value(hash,quantize(state.feet.y));
  hash_value(hash,quantize(state.feet.z));hash_value(hash,quantize(state.velocity.x));
  hash_value(hash,quantize(state.velocity.y));hash_value(hash,quantize(state.velocity.z));
  hash_value(hash,quantize(state.yaw));hash_value(hash,quantize(state.pitch));
  hash_value(hash,state.grounded?1U:0U);
  output<<std::setprecision(12)<<"{\"event\":\"world_trace\",\"commands\":"
        <<command_count<<",\"steps\":"<<step_count<<",\"position\":["
        <<state.feet.x<<','<<state.feet.y<<','<<state.feet.z<<"],\"velocity\":["
        <<state.velocity.x<<','<<state.velocity.y<<','<<state.velocity.z
        <<"],\"grounded\":"<<(state.grounded?"true":"false")
        <<",\"hash\":"<<hash<<"}\n";
  return 0;
}

int run_world_runtime_benchmark(std::ostream& output,std::ostream& errors) {
  auto runtime=make_production_terrain_runtime(production_world_profile());
  struct Pose { const char* name;tetra::Vec3 position;tetra::Vec3 target; };
  constexpr std::array poses{
      Pose{"stationary",{0.5,0.72,0.78},{0.5,0.5,0.5}},
      Pose{"walking-speed",{0.5,0.72,0.68},{0.5,0.5,0.4}},
      Pose{"rapid-turn",{0.5,0.72,0.68},{0.5,0.5,0.95}},
      Pose{"near",{0.5,0.61,0.58},{0.5,0.5,0.45}},
      Pose{"far",{0.5,3.0,12.0},{0.5,0.5,0.5}},
      Pose{"reversal",{0.5,0.72,0.78},{0.5,0.5,0.5}},
      Pose{"teleport",{0.12,0.72,0.14},{0.45,0.5,0.45}},
  };
  for(const auto& pose:poses){
    tetra::Camera camera;
    camera.position=pose.position;
    const auto delta=pose.target-pose.position;
    const double magnitude=std::sqrt(
        delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    camera.forward=delta/magnitude;
    camera.viewport_height_pixels=800.0;
    camera.aspect_ratio=1.6;
    const auto start=std::chrono::steady_clock::now();
    runtime->set_camera(camera,false);
    const auto deadline=start+std::chrono::seconds(30);
    TerrainRuntimeDiagnostics diagnostics;
    do{
      static_cast<void>(runtime->update());
      diagnostics=runtime->diagnostics();
      if(diagnostics.converged&&!diagnostics.busy&&
         diagnostics.scene_generation>0U&&
         diagnostics.scene_mesh_revision==diagnostics.mesh_revision)break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }while(std::chrono::steady_clock::now()<deadline);
    if(!(diagnostics.converged&&!diagnostics.busy&&
         diagnostics.scene_mesh_revision==diagnostics.mesh_revision)){
      errors<<"world runtime did not converge for "<<pose.name<<"\n";
      return 1;
    }
    const double milliseconds=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-start).count();
    output<<"{\"event\":\"world_runtime_baseline\",\"path\":\""
          <<pose.name<<"\",\"milliseconds\":"<<std::setprecision(8)
          <<milliseconds<<",\"logical_cells\":"<<diagnostics.logical_cells
          <<",\"active_tetrahedra\":"<<diagnostics.active_tetrahedra
          <<",\"resident_bytes\":"<<diagnostics.resident_bytes
          <<",\"retained_cache_bytes\":"<<diagnostics.retained_cache_bytes
          <<",\"retained_conforming_bytes\":"
          <<diagnostics.retained_conforming_bytes
          <<",\"retained_render_block_bytes\":"
          <<diagnostics.retained_render_block_bytes
          <<",\"retained_host_staging_bytes\":"
          <<diagnostics.retained_host_staging_bytes
          <<",\"cut_selection_ms\":"<<diagnostics.cut_selection_milliseconds
          <<",\"cut_closure_ms\":"<<diagnostics.cut_closure_milliseconds
          <<",\"surface_build_ms\":"<<diagnostics.surface_build_milliseconds
          <<",\"volume_reconstruction_ms\":"
          <<diagnostics.volume_reconstruction_milliseconds
          <<",\"surface_extraction_ms\":"
          <<diagnostics.surface_extraction_milliseconds
          <<",\"surface_assembly_ms\":"
          <<diagnostics.surface_assembly_milliseconds
          <<",\"reused_intersections\":"
          <<diagnostics.reused_surface_intersections
          <<",\"computed_intersections\":"
          <<diagnostics.computed_surface_intersections
          <<",\"reused_hierarchy_blocks\":"
          <<diagnostics.reused_hierarchy_blocks
          <<",\"rebuilt_hierarchy_blocks\":"
          <<diagnostics.rebuilt_hierarchy_blocks
          <<",\"reused_surface_blocks\":"
          <<diagnostics.reused_surface_blocks
          <<",\"rebuilt_surface_blocks\":"
          <<diagnostics.rebuilt_surface_blocks
          <<",\"reused_render_blocks\":"
          <<diagnostics.reused_render_blocks
          <<",\"rebuilt_render_blocks\":"
          <<diagnostics.rebuilt_render_blocks
          <<",\"reused_conforming_blocks\":"
          <<diagnostics.reused_conforming_blocks
          <<",\"rebuilt_conforming_blocks\":"
          <<diagnostics.rebuilt_conforming_blocks
          <<",\"reused_conforming_cells\":"
          <<diagnostics.reused_conforming_cells
          <<",\"rebuilt_conforming_cells\":"
          <<diagnostics.rebuilt_conforming_cells
          <<",\"retained_render_ranges\":"
          <<diagnostics.retained_render_ranges
          <<",\"dirty_render_ranges\":"
          <<diagnostics.dirty_render_ranges
          <<",\"staged_render_bytes\":"
          <<diagnostics.staged_render_bytes
          <<",\"uploaded_render_bytes\":"
          <<diagnostics.uploaded_render_bytes
          <<",\"render_triangles\":"<<diagnostics.render_triangles
          <<",\"work_units\":"<<diagnostics.work_units
          <<",\"cpu_high_water_bytes\":"
          <<diagnostics.cpu_high_water_bytes
          <<",\"triangle_high_water\":"
          <<diagnostics.triangle_high_water
          <<",\"work_high_water\":"<<diagnostics.work_high_water
          <<",\"upload_high_water_bytes\":"
          <<diagnostics.upload_high_water_bytes
          <<",\"submitted_builds\":"<<diagnostics.submitted_builds
          <<",\"superseded_builds\":"<<diagnostics.superseded_builds
          <<",\"canceled_builds\":"<<diagnostics.canceled_builds
          <<",\"budget_rejected_builds\":"
          <<diagnostics.budget_rejected_builds
          <<",\"discarded_work_units\":"
          <<diagnostics.discarded_work_units
          <<",\"maximum_cancellation_latency_ms\":"
          <<diagnostics.maximum_cancellation_latency_milliseconds
          <<",\"budget_exceeded\":"
          <<(diagnostics.budget_exceeded?"true":"false")
          <<",\"hierarchy_hash\":"<<diagnostics.hierarchy_hash
          <<",\"conforming_volume_hash\":"<<diagnostics.conforming_volume_hash
          <<",\"connected_surface_hash\":"<<diagnostics.connected_surface_hash
          <<",\"render_hash\":"<<diagnostics.render_hash
          <<",\"field_sample_hash\":"<<diagnostics.field_sample_hash<<"}\n";
  }
  const auto before=runtime->diagnostics();
  tetra::Camera superseded;
  superseded.position={0.65,0.72,0.72};
  superseded.forward={0.0,-0.2,-1.0};
  runtime->set_camera(superseded,true);
  static_cast<void>(runtime->update());
  for(std::size_t step=0;step<3U;++step){
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    superseded.position={0.7+0.08*static_cast<double>(step),0.72,
                         0.68-0.03*static_cast<double>(step)};
    const auto target=tetra::Vec3{
        superseded.position.x,0.5,superseded.position.z-0.3};
    const auto delta=target-superseded.position;
    const double length=std::sqrt(
        delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    superseded.forward=delta/length;
    runtime->set_camera(superseded,true);
    const auto canceled_target=before.canceled_builds+step+1U;
    const auto cancellation_deadline=
        std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<cancellation_deadline){
      static_cast<void>(runtime->update());
      if(runtime->diagnostics().canceled_builds>=canceled_target&&
         runtime->diagnostics().busy)break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if(runtime->diagnostics().canceled_builds<canceled_target){
      errors<<"world supersession cancellation did not complete\n";return 1;
    }
  }
  const auto supersession_started=std::chrono::steady_clock::now();
  const auto supersession_deadline=
      supersession_started+std::chrono::seconds(30);
  TerrainRuntimeDiagnostics newest;
  do{
    static_cast<void>(runtime->update());newest=runtime->diagnostics();
    if(newest.converged&&!newest.busy)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }while(std::chrono::steady_clock::now()<supersession_deadline);
  if(!newest.converged||newest.busy){
    errors<<"world supersession benchmark did not converge\n";return 1;
  }
  output<<"{\"event\":\"world_resource_budget\",\"path\":\"rapid-supersession\""
        <<",\"milliseconds\":"<<std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-supersession_started).count()
        <<",\"submitted_builds\":"<<newest.submitted_builds-before.submitted_builds
        <<",\"superseded_builds\":"
        <<newest.superseded_builds-before.superseded_builds
        <<",\"canceled_builds\":"<<newest.canceled_builds-before.canceled_builds
        <<",\"discarded_work_units\":"
        <<newest.discarded_work_units-before.discarded_work_units
        <<",\"maximum_cancellation_latency_ms\":"
        <<newest.maximum_cancellation_latency_milliseconds
        <<",\"cpu_high_water_bytes\":"<<newest.cpu_high_water_bytes
        <<",\"triangle_high_water\":"<<newest.triangle_high_water
        <<",\"work_high_water\":"<<newest.work_high_water
        <<",\"upload_high_water_bytes\":"<<newest.upload_high_water_bytes
        <<",\"budget_rejected_builds\":"<<newest.budget_rejected_builds
        <<",\"hierarchy_hash\":"<<newest.hierarchy_hash
        <<",\"render_hash\":"<<newest.render_hash<<"}\n";
  return 0;
}

int capture_world_runtime(std::string_view path,std::ostream& output,
                          std::ostream& errors) {
  return capture_world_runtime_view(
      path,{0.5,0.72,0.78},{0.5,0.5,0.5},output,errors);
}

int capture_world_runtime_view(std::string_view path,
                               tetra::Vec3 camera_position,
                               tetra::Vec3 target,std::ostream& output,
                               std::ostream& errors) {
  auto runtime=make_production_terrain_runtime(production_world_profile());
  tetra::Camera camera;
  camera.position=camera_position;
  auto forward=target-camera.position;
  const auto magnitude=[](tetra::Vec3 value){return std::sqrt(
      value.x*value.x+value.y*value.y+value.z*value.z);};
  const double forward_magnitude=magnitude(forward);
  if(!(forward_magnitude>1.0e-12)||!std::isfinite(forward_magnitude)){
    errors<<"world capture camera and target must be distinct and finite\n";
    return 2;
  }
  forward=forward/forward_magnitude;
  camera.forward=forward;camera.viewport_height_pixels=480.0;
  camera.aspect_ratio=1.6;
  runtime->set_camera(camera,false);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
  TerrainRuntimeDiagnostics diagnostics;
  do{
    static_cast<void>(runtime->update());
    diagnostics=runtime->diagnostics();
    if(diagnostics.converged&&!diagnostics.busy&&diagnostics.scene_generation>0U&&
       diagnostics.scene_mesh_revision==diagnostics.mesh_revision)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }while(std::chrono::steady_clock::now()<deadline);
  if(!(diagnostics.converged&&!diagnostics.busy&&
       diagnostics.scene_mesh_revision==diagnostics.mesh_revision)){
    errors<<"world runtime capture did not converge\n";return 1;
  }

  constexpr int width=768,height=480;
  using Pixel=std::array<unsigned char,3>;
  std::vector<Pixel> pixels(static_cast<std::size_t>(width*height),{15,20,28});
  std::vector<double> depths(static_cast<std::size_t>(width*height),
                             std::numeric_limits<double>::infinity());
  const auto dot=[](tetra::Vec3 first,tetra::Vec3 second){return
      first.x*second.x+first.y*second.y+first.z*second.z;};
  const auto cross=[](tetra::Vec3 first,tetra::Vec3 second){return tetra::Vec3{
      first.y*second.z-first.z*second.y,
      first.z*second.x-first.x*second.z,
      first.x*second.y-first.y*second.x};};
  const auto normalize=[&](tetra::Vec3 value){
    const double size=magnitude(value);return size>1.0e-12?value/size:tetra::Vec3{};};
  const auto right=normalize(cross(forward,camera.up));
  const auto up=cross(right,forward);
  const double tangent=std::tan(camera.vertical_fov_radians*0.5);
  const auto render_camera=camera.position-runtime->scene().render_origin;
  struct Projected { double x{},y{},depth{}; };
  const auto project=[&](const SceneVertex& vertex){
    const tetra::Vec3 point{vertex.position[0],vertex.position[1],vertex.position[2]};
    const auto offset=point-render_camera;
    const double depth=dot(offset,forward);
    return Projected{
        (dot(offset,right)/(depth*tangent*camera.aspect_ratio)*0.5+0.5)*(width-1),
        (0.5-dot(offset,up)/(depth*tangent)*0.5)*(height-1),depth};
  };
  const auto edge=[](Projected first,Projected second,double x,double y){
    return (x-first.x)*(second.y-first.y)-(y-first.y)*(second.x-first.x);};
  const auto& vertices=runtime->scene().triangle_vertices;
  for(std::size_t triangle=0;triangle+2U<vertices.size();triangle+=3U){
    const auto& first=vertices[triangle];
    const auto& second=vertices[triangle+1U];
    const auto& third=vertices[triangle+2U];
    const auto a=project(first),b=project(second),c=project(third);
    if(a.depth<=0.001||b.depth<=0.001||c.depth<=0.001)continue;
    const double area=edge(a,b,c.x,c.y);
    if(std::abs(area)<1.0e-12)continue;
    const int minimum_x=std::max(0,static_cast<int>(std::floor(std::min({a.x,b.x,c.x}))));
    const int maximum_x=std::min(width-1,static_cast<int>(std::ceil(std::max({a.x,b.x,c.x}))));
    const int minimum_y=std::max(0,static_cast<int>(std::floor(std::min({a.y,b.y,c.y}))));
    const int maximum_y=std::min(height-1,static_cast<int>(std::ceil(std::max({a.y,b.y,c.y}))));
    const auto normal=normalize({first.normal[0],first.normal[1],first.normal[2]});
    const double illumination=0.28+0.72*std::max(0.0,dot(normal,normalize(
        render_camera-tetra::Vec3{first.position[0],first.position[1],first.position[2]})));
    for(int y=minimum_y;y<=maximum_y;++y)for(int x=minimum_x;x<=maximum_x;++x){
      const double sample_x=x+0.5,sample_y=y+0.5;
      const double wa=edge(b,c,sample_x,sample_y)/area;
      const double wb=edge(c,a,sample_x,sample_y)/area;
      const double wc=edge(a,b,sample_x,sample_y)/area;
      if(wa<0.0||wb<0.0||wc<0.0)continue;
      const double depth=1.0/(wa/a.depth+wb/b.depth+wc/c.depth);
      const auto index=static_cast<std::size_t>(y*width+x);
      if(depth>=depths[index])continue;
      depths[index]=depth;
      for(std::size_t channel=0;channel<3U;++channel)
        pixels[index][channel]=static_cast<unsigned char>(255.0*std::clamp(
            static_cast<double>(first.colour[channel])*illumination,0.0,1.0));
    }
  }
  // Match the production presentation closely enough for visual inspection:
  // every triangle edge is an explicit depth-tested segment, independent of
  // triangle shape or barycentric interpolation.
  for(std::size_t triangle=0;triangle+2U<vertices.size();triangle+=3U){
    const std::array projected{project(vertices[triangle]),
                               project(vertices[triangle+1U]),
                               project(vertices[triangle+2U])};
    for(const auto edge_indices:std::array{
            std::array<std::size_t,2>{0U,1U},
            std::array<std::size_t,2>{1U,2U},
            std::array<std::size_t,2>{2U,0U}}){
      const auto a=projected[edge_indices[0]],b=projected[edge_indices[1]];
      if(a.depth<=0.001||b.depth<=0.001)continue;
      const int steps=std::max(1,static_cast<int>(std::ceil(
          std::max(std::abs(b.x-a.x),std::abs(b.y-a.y)))));
      for(int step=0;step<=steps;++step){
        const double amount=static_cast<double>(step)/steps;
        const int x=static_cast<int>(std::lround(a.x+(b.x-a.x)*amount));
        const int y=static_cast<int>(std::lround(a.y+(b.y-a.y)*amount));
        if(x<0||x>=width||y<0||y>=height)continue;
        const double depth=1.0/((1.0-amount)/a.depth+amount/b.depth);
        const auto index=static_cast<std::size_t>(y*width+x);
        if(depth>depths[index]+1.0e-3)continue;
        pixels[index]={18,36,28};
      }
    }
  }
  std::ofstream image(std::string(path),std::ios::binary);
  if(!image){errors<<"could not open world capture path\n";return 2;}
  image<<"P6\n"<<width<<' '<<height<<"\n255\n";
  for(const auto& pixel:pixels)
    image.write(reinterpret_cast<const char*>(pixel.data()),
                static_cast<std::streamsize>(pixel.size()));
  if(!image){errors<<"could not write world capture\n";return 2;}
  output<<"{\"event\":\"world_capture\",\"path\":\""<<path
        <<"\",\"hierarchy_hash\":"<<diagnostics.hierarchy_hash
        <<",\"conforming_volume_hash\":"<<diagnostics.conforming_volume_hash
        <<",\"connected_surface_hash\":"<<diagnostics.connected_surface_hash
        <<",\"render_hash\":"<<diagnostics.render_hash<<"}\n";
  return 0;
}

}  // namespace tetra_viewer
