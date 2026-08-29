#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "tetra_core/implicit_surface.hpp"
#include "tetra_core/adjacency.hpp"
#include "tetra_core/four_hexahedra.hpp"
#include "tetra_core/geometry_executor.hpp"
#include "tetra_core/green_templates.hpp"
#include "tetra_core/layer_storage.hpp"
#include "tetra_core/mixed_depth_dual.hpp"
#include "tetra_core/parallel_commit.hpp"
#include "tetra_core/whole_cell_surface.hpp"
#include "tetra_core/world_hierarchy.hpp"
#include "tetra_core/world_cut_directory.hpp"
#include "tetra_viewer/viewer_scene.hpp"
#include "tetra_viewer/atmosphere.hpp"
#include "tetra_viewer/atmosphere_shadow_front.hpp"
#include "tetra_viewer/camera_manipulator.hpp"
#include "tetra_viewer/first_person_controller.hpp"
#include "tetra_viewer/image_oracle.hpp"
#include "tetra_viewer/mesh_update_worker.hpp"
#include "tetra_viewer/projection.hpp"
#include "tetra_viewer/scene_preparation_worker.hpp"
#include "tetra_viewer/shadow_cascades.hpp"
#include "tetra_viewer/terrain_runtime.hpp"
#include "tetra_viewer/viewer_script.hpp"
#include "tetra_viewer/world_script.hpp"

#include <cmath>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <numbers>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>

TEST_CASE("shared geometry executor partitions ranges and propagates failures") {
  tetra::GeometryExecutor executor({
      .worker_count=4U,.blocks_per_worker=4U,
      .external_callers_may_participate=false});
  auto group=executor.make_group(17U,tetra::GeometryTaskPriority::interactive);
  std::array<std::atomic_uint,257> visits{};
  executor.parallel_for(group,0U,visits.size(),13U,
      [&](std::size_t begin,std::size_t end,std::stop_token stop){
        CHECK_FALSE(stop.stop_requested());
        for(std::size_t index=begin;index<end;++index)
          visits[index].fetch_add(1U,std::memory_order_relaxed);
      });
  executor.wait(group);
  CHECK(group.generation()==17U);
  CHECK(group.pending_tasks()==0U);
  for(const auto& visit:visits)
    CHECK(visit.load(std::memory_order_relaxed)==1U);
  const auto metrics=executor.metrics();
  CHECK(metrics.submitted_tasks==16U);
  CHECK(metrics.completed_tasks==16U);
  CHECK(metrics.maximum_active_workers<=4U);
  CHECK(metrics.maximum_queued_tasks<=16U);

  auto failing=executor.make_group();
  executor.submit(failing,[](std::stop_token){
    throw std::runtime_error("expected executor failure");
  });
  CHECK_THROWS_WITH_AS(executor.wait(failing),"expected executor failure",
                       std::runtime_error);
  CHECK(executor.metrics().task_exceptions==1U);
}

TEST_CASE("shared geometry executor completes nested work without oversubscription") {
  tetra::GeometryExecutor executor({
      .worker_count=1U,.blocks_per_worker=3U,
      .external_callers_may_participate=false});
  auto outer=executor.make_group();
  std::atomic_uint visits{};
  executor.submit(outer,[&](std::stop_token){
    auto inner=executor.make_group();
    executor.parallel_for(inner,0U,100U,1U,
        [&](std::size_t begin,std::size_t end,std::stop_token){
          visits.fetch_add(static_cast<unsigned int>(end-begin));
        });
    executor.wait_and_help(inner);
  });
  executor.wait(outer);
  CHECK(visits.load()==100U);
  CHECK(executor.metrics().maximum_queued_tasks<=3U);
  CHECK(executor.metrics().nested_executor_entries>=1U);
  CHECK(executor.metrics().maximum_active_workers==1U);
}

TEST_CASE("shared geometry executor cancels active groups during shutdown") {
  auto executor=std::make_unique<tetra::GeometryExecutor>(
      tetra::GeometryExecutorConfiguration{.worker_count=2U});
  auto active=executor->make_group();
  std::atomic_bool started{},observed_stop{};
  executor->submit(active,[&](std::stop_token stop){
    started.store(true,std::memory_order_release);
    while(!stop.stop_requested())std::this_thread::yield();
    observed_stop.store(true,std::memory_order_release);
  });
  while(!started.load(std::memory_order_acquire))std::this_thread::yield();
  executor.reset();
  CHECK(active.stop_requested());
  CHECK(observed_stop.load(std::memory_order_acquire));
  CHECK(active.pending_tasks()==0U);
}

TEST_CASE("shared geometry executor cancellation skips queued work") {
  tetra::GeometryExecutor executor({.worker_count=1U});
  auto blocker=executor.make_group();
  std::mutex mutex;
  std::condition_variable started_condition;
  bool started{};
  bool release{};
  executor.submit(blocker,[&](std::stop_token){
    std::unique_lock lock(mutex);
    started=true;
    started_condition.notify_all();
    started_condition.wait(lock,[&]{return release;});
  });
  {
    std::unique_lock lock(mutex);
    started_condition.wait(lock,[&]{return started;});
  }

  auto canceled=executor.make_group(2U,tetra::GeometryTaskPriority::speculative);
  std::atomic_uint executions{};
  for(unsigned int index=0;index<8U;++index)
    executor.submit(canceled,[&](std::stop_token){++executions;});
  canceled.request_stop();
  {
    std::lock_guard lock(mutex);
    release=true;
  }
  started_condition.notify_all();
  executor.wait(blocker);
  executor.wait(canceled);
  CHECK(executions.load()==0U);
  CHECK(executor.metrics().canceled_tasks==8U);
}

TEST_CASE("shared geometry executor gives bounded service to lower priorities") {
  tetra::GeometryExecutor executor({.worker_count=1U});
  auto blocker=executor.make_group();
  std::mutex mutex;
  std::condition_variable condition;
  bool started{};
  bool release{};
  executor.submit(blocker,[&](std::stop_token){
    std::unique_lock lock(mutex);
    started=true;
    condition.notify_all();
    condition.wait(lock,[&]{return release;});
  });
  {
    std::unique_lock lock(mutex);
    condition.wait(lock,[&]{return started;});
  }
  auto publication=executor.make_group(
      1U,tetra::GeometryTaskPriority::publication_critical);
  auto speculative=executor.make_group(
      2U,tetra::GeometryTaskPriority::speculative);
  std::atomic_uint publications_before_speculative{};
  std::atomic_uint publication_count{};
  std::atomic_uint observed_before_speculative{100U};
  for(unsigned int index=0;index<24U;++index)
    executor.submit(publication,[&](std::stop_token){++publication_count;});
  executor.submit(speculative,[&](std::stop_token){
    observed_before_speculative=publication_count.load();
    ++publications_before_speculative;
  });
  {
    std::lock_guard lock(mutex);
    release=true;
  }
  condition.notify_all();
  executor.wait(blocker);
  executor.wait(publication);
  executor.wait(speculative);
  CHECK(publications_before_speculative.load()==1U);
  CHECK(observed_before_speculative.load()<24U);
  CHECK(executor.metrics().bounded_fairness_yields>=1U);
}

TEST_CASE("geometry executor default leaves capacity for rendering") {
  const auto hardware=std::max(1U,std::thread::hardware_concurrency());
  const auto workers=tetra::default_geometry_worker_count();
  CHECK(workers>=1U);
  CHECK(workers<=8U);
  if(hardware>2U)CHECK(workers<=hardware-2U);
}

TEST_CASE("parallel active-cut planning matches the serial command stream") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int generation=0;generation<3U;++generation)
    mesh.refine_all_binary();
  REQUIRE(mesh.logical_red_owners().size()>=1024U);
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=4096U;
  const tetra::Sphere surface{};
  tetra::Camera camera;
  camera.position={0.5,0.5,1.8};
  const auto serial=tetra::plan_adaptation(
      mesh,surface,camera,18.0,12U,configuration,5U);
  tetra::GeometryExecutor executor({.worker_count=4U,.blocks_per_worker=4U});
  const auto parallel=tetra::plan_adaptation(
      mesh,surface,camera,18.0,12U,configuration,5U,nullptr,{},&executor);
  CHECK(parallel.commands==serial.commands);
  CHECK(parallel.requested_splits==serial.requested_splits);
  CHECK(parallel.requested_merges==serial.requested_merges);
  CHECK(parallel.planned_splits==serial.planned_splits);
  CHECK(parallel.planned_merges==serial.planned_merges);
  CHECK(parallel.logical_candidates==serial.logical_candidates);
  CHECK(parallel.field_classifications==serial.field_classifications);
  CHECK(parallel.exact_field_evaluations==serial.exact_field_evaluations);
  CHECK(parallel.projection_evaluations==serial.projection_evaluations);
  CHECK(parallel.depth_rejections==serial.depth_rejections);
  CHECK(parallel.camera_demand_evaluations==serial.camera_demand_evaluations);
  CHECK(parallel.camera_recent_updates==serial.camera_recent_updates);
  CHECK(parallel.parallel_workers==4U);
  CHECK(parallel.parallel_tasks>=4U);
  CHECK(parallel.parallel_candidates==mesh.logical_red_owners().size());

  camera.position={0.5,0.5,80.0};
  const auto serial_merge=tetra::plan_adaptation(
      mesh,surface,camera,18.0,12U,configuration,6U);
  const auto parallel_merge=tetra::plan_adaptation(
      mesh,surface,camera,18.0,12U,configuration,6U,nullptr,{},&executor);
  REQUIRE(serial_merge.requested_splits==0U);
  CHECK(parallel_merge.commands==serial_merge.commands);
  CHECK(parallel_merge.requested_merges==serial_merge.requested_merges);
  CHECK(parallel_merge.planned_merges==serial_merge.planned_merges);
  CHECK(parallel_merge.projection_evaluations==
        serial_merge.projection_evaluations);
  CHECK(parallel_merge.camera_demand_evaluations==
        serial_merge.camera_demand_evaluations);
  CHECK(parallel_merge.parallel_tasks>parallel.parallel_tasks);
  CHECK(parallel_merge.parallel_candidates>
        parallel.parallel_candidates);
}

TEST_CASE("headless multithreaded geometry benchmark validates worker hashes") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "benchmark-multithreaded-geometry=4",output,errors)==0);
  CHECK(errors.str().empty());
  CHECK(output.str().find("\"event\":\"multithreaded_geometry_benchmark\"")!=
        std::string::npos);
  CHECK(output.str().find("\"workers\":4")!=std::string::npos);
  CHECK(output.str().find("\"hashes_match\":true")!=std::string::npos);
  std::ostringstream invalid_output,invalid_errors;
  CHECK(tetra_viewer::run_script(
      "benchmark-multithreaded-geometry=0",invalid_output,invalid_errors)==2);
}

TEST_CASE("parallel derived green construction matches serial split and merge") {
  auto base=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int generation=0;generation<3U;++generation)
    base.refine_all_binary();
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=4096U;
  tetra::Sphere surface{};
  tetra::Camera camera;
  camera.position={0.5,0.5,1.35};
  const auto plan=tetra::plan_adaptation(
      base,surface,camera,18.0,15U,configuration,0U);
  REQUIRE_FALSE(plan.commands.empty());
  auto serial=base;
  REQUIRE(tetra::commit_adaptation(serial,plan,configuration).status==
          tetra::AdaptationCommitStatus::committed);
  const auto verify_equal=[&](const tetra::TetMesh& parallel){
    CHECK(std::ranges::equal(
        parallel.logical_red_owners(),serial.logical_red_owners()));
    CHECK(std::ranges::equal(
        parallel.logical_midpoint_masks(),serial.logical_midpoint_masks()));
    CHECK(std::ranges::equal(
        parallel.logical_stencil_choices(),serial.logical_stencil_choices()));
    CHECK(std::ranges::equal(
        parallel.logical_derived_hashes(),serial.logical_derived_hashes()));
    CHECK(std::ranges::equal(
        parallel.logical_derived_offsets(),serial.logical_derived_offsets()));
    CHECK(std::ranges::equal(
        parallel.logical_derived_addresses(),
        serial.logical_derived_addresses()));
    CHECK(std::ranges::equal(
        parallel.conforming_volume().addresses(),
        serial.conforming_volume().addresses()));
    CHECK(parallel.has_positive_active_volumes());
    CHECK(parallel.has_symmetric_active_adjacency());
    CHECK(parallel.has_conforming_active_faces());
    REQUIRE_FALSE(parallel.logical_derived_offsets().empty());
    CHECK(parallel.logical_derived_offsets().back()==
          parallel.logical_derived_addresses().size());
  };
  for(const std::size_t workers:{2U,4U,8U}){
    auto parallel=base;
    tetra::GeometryExecutor executor({
        .worker_count=workers,.blocks_per_worker=4U});
    REQUIRE(tetra::commit_adaptation(
        parallel,plan,configuration,0U,nullptr,&executor).status==
        tetra::AdaptationCommitStatus::committed);
    CAPTURE(workers);
    verify_equal(parallel);
    CHECK(parallel.last_bcc_update_metrics().parallel_green_tasks>0U);
    CHECK(parallel.last_bcc_update_metrics().parallel_green_workers==workers);

    tetra::Camera far_camera=camera;
    far_camera.position={0.5,0.5,80.0};
    const auto serial_merge_plan=tetra::plan_adaptation(
        serial,surface,far_camera,18.0,15U,configuration,1U);
    const auto parallel_merge_plan=tetra::plan_adaptation(
        parallel,surface,far_camera,18.0,15U,configuration,1U,
        nullptr,{},&executor);
    REQUIRE(parallel_merge_plan.commands==serial_merge_plan.commands);
    auto serial_merged=serial;
    auto parallel_merged=parallel;
    REQUIRE(tetra::commit_adaptation(
        serial_merged,serial_merge_plan,configuration,1U).status==
        tetra::AdaptationCommitStatus::committed);
    REQUIRE(tetra::commit_adaptation(
        parallel_merged,parallel_merge_plan,configuration,1U,nullptr,
        &executor).status==tetra::AdaptationCommitStatus::committed);
    CHECK(std::ranges::equal(
        parallel_merged.logical_red_owners(),
        serial_merged.logical_red_owners()));
    CHECK(std::ranges::equal(
        parallel_merged.logical_derived_hashes(),
        serial_merged.logical_derived_hashes()));
    CHECK(std::ranges::equal(
        parallel_merged.logical_derived_offsets(),
        serial_merged.logical_derived_offsets()));
    CHECK(std::ranges::equal(
        parallel_merged.logical_derived_addresses(),
        serial_merged.logical_derived_addresses()));
    CHECK(std::ranges::equal(
        parallel_merged.conforming_volume().addresses(),
        serial_merged.conforming_volume().addresses()));
  }
}

TEST_CASE("parallel scene classification preserves complete geometry") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int generation=0;generation<3U;++generation)
    mesh.refine_all_binary();
  const tetra::Sphere surface{};
  const auto prepare=[&](tetra::GeometryExecutor* executor){
    return tetra_viewer::prepare_scene(
        mesh,surface,tetra_viewer::SurfaceMethod::marching_tetrahedra,
        tetra_viewer::MaterialRule::variational_smooth,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells,
        tetra_viewer::StencilConstruction::fixed,
        tetra_viewer::StencilSelectionObjective::balanced,
        {.surface_diagnostics=false,.summary_statistics=true},{},false,
        executor);
  };
  const auto serial=prepare(nullptr);
  tetra::GeometryExecutor executor({.worker_count=4U,.blocks_per_worker=4U});
  const auto parallel=prepare(&executor);
  CHECK(parallel.relations==serial.relations);
  CHECK(parallel.depth_counts==serial.depth_counts);
  CHECK(parallel.inside_count==serial.inside_count);
  CHECK(parallel.outside_count==serial.outside_count);
  CHECK(parallel.intersecting_count==serial.intersecting_count);
  CHECK(parallel.total_volume==serial.total_volume);
  CHECK(tetra_viewer::surface_geometry_hashes(parallel)==
        tetra_viewer::surface_geometry_hashes(serial));
  CHECK(parallel.parallel_classification_workers==4U);
  CHECK(parallel.parallel_classification_tasks>=4U);
}

TEST_CASE("parallel owner-local surface patches match serial retained output") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int generation=0;generation<3U;++generation)
    mesh.refine_all_binary();
  const tetra::Sphere surface{};
  const auto prepare=[&](tetra_viewer::SceneCache& cache,
                         tetra::GeometryExecutor* executor){
    return cache.update_scene(
        mesh,surface,1U,tetra_viewer::SurfaceMethod::marching_tetrahedra,
        tetra_viewer::MaterialRule::variational_smooth,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells,
        tetra_viewer::StencilConstruction::fixed,
        tetra_viewer::StencilSelectionObjective::balanced,
        {.surface_diagnostics=false,.summary_statistics=true},{},0U,
        executor);
  };
  tetra_viewer::SceneCache serial,parallel;
  REQUIRE(prepare(serial,nullptr));
  tetra::GeometryExecutor executor({
      .worker_count=4U,.blocks_per_worker=4U});
  REQUIRE(prepare(parallel,&executor));
  CHECK(tetra_viewer::surface_geometry_hashes(parallel.scene())==
        tetra_viewer::surface_geometry_hashes(serial.scene()));
  REQUIRE(parallel.surface_patch_records().size()==
          serial.surface_patch_records().size());
  for(std::size_t index=0;index<serial.surface_patch_records().size();++index){
    const auto& left=parallel.surface_patch_records()[index];
    const auto& right=serial.surface_patch_records()[index];
    CHECK(left.logical_owner==right.logical_owner);
    CHECK(left.topology_hash==right.topology_hash);
    CHECK(left.triangle_count==right.triangle_count);
    const auto left_triangles=parallel.surface_patch_arena().subspan(
        left.triangle_begin,left.triangle_count);
    const auto right_triangles=serial.surface_patch_arena().subspan(
        right.triangle_begin,right.triangle_count);
    REQUIRE(left_triangles.size()==right_triangles.size());
    const bool bytes_equal=left_triangles.empty()||std::memcmp(
        left_triangles.data(),right_triangles.data(),
        left_triangles.size_bytes())==0;
    CHECK(bytes_equal);
  }
  CHECK(parallel.surface_patch_metrics().parallel_generation_tasks>1U);
  CHECK(parallel.surface_patch_metrics().parallel_generation_workers==4U);
}

TEST_CASE("complete green placing templates cover all sixty-four edge masks") {
  struct Point { double x{},y{},z{}; };
  constexpr std::array<Point,10> points{{
      {0.0,0.0,1.0},{0.0,1.0,0.0},{1.0,0.0,0.0},{0.0,0.0,0.0},
      {1.0,0.0,1.0},{1.0,1.0,0.0},{2.0,0.0,0.0},{0.0,1.0,1.0},
      {0.0,2.0,0.0},{0.0,0.0,2.0}}};
  const auto determinant=[](Point a,Point b,Point c,Point d){
    const Point ab{b.x-a.x,b.y-a.y,b.z-a.z};
    const Point ac{c.x-a.x,c.y-a.y,c.z-a.z};
    const Point ad{d.x-a.x,d.y-a.y,d.z-a.z};
    return ab.x*(ac.y*ad.z-ac.z*ad.y)-ab.y*(ac.x*ad.z-ac.z*ad.x)+
           ab.z*(ac.x*ad.y-ac.y*ad.x);
  };
  using Triangle=std::array<std::uint8_t,3>;
  std::map<std::pair<unsigned int,unsigned int>,std::vector<Triangle>> boundaries;
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  constexpr std::array<unsigned int,4> face_edge_masks{{
      (1U<<0U)|(1U<<1U)|(1U<<3U),(1U<<0U)|(1U<<2U)|(1U<<4U),
      (1U<<1U)|(1U<<2U)|(1U<<5U),(1U<<3U)|(1U<<4U)|(1U<<5U)}};

  for(unsigned int mask=0;mask<64U;++mask){
    CAPTURE(mask);
    const auto& stencil=tetra::complete_green_template(static_cast<std::uint8_t>(mask));
    REQUIRE(stencil.count>0U);
    REQUIRE(stencil.count<=16U);
    double volume{};
    std::array<bool,10> used{};
    std::map<Triangle,unsigned int> triangle_counts;
    for(std::size_t index=0;index<stencil.count;++index){
      const auto& tet=stencil.tetrahedra[index];
      for(const auto point:tet){
        REQUIRE(point<points.size());
        used[point]=true;
        const auto edge=tetra::grande_point_edge[point];
        CHECK((edge==0xffU||(mask&(1U<<edge))!=0U));
      }
      const double six_volume=determinant(
          points[tet[0]],points[tet[1]],points[tet[2]],points[tet[3]]);
      CHECK(six_volume>0.0);
      volume+=six_volume/6.0;
      for(const auto face:faces){
        Triangle triangle{{tet[face[0]],tet[face[1]],tet[face[2]]}};
        std::sort(triangle.begin(),triangle.end());
        ++triangle_counts[triangle];
      }
    }
    CHECK(volume==doctest::Approx(8.0/6.0).epsilon(1.0e-12));
    for(std::size_t point=0;point<used.size();++point){
      const auto edge=tetra::grande_point_edge[point];
      if(edge==0xffU||(mask&(1U<<edge))!=0U)CHECK(used[point]);
    }
    for(unsigned int face=0;face<4U;++face){
      std::vector<Triangle> boundary;
      for(const auto& [triangle,count]:triangle_counts){
        if(count!=1U)continue;
        const auto on_face=[&](std::uint8_t point){
          const auto& value=points[point];
          if(face==0U)return value.z==0.0;
          if(face==1U)return value.y==0.0;
          if(face==2U)return value.x==0.0;
          return value.x+value.y+value.z==2.0;
        };
        if(std::ranges::all_of(triangle,on_face))boundary.push_back(triangle);
      }
      std::sort(boundary.begin(),boundary.end());
      const auto key=std::pair{face,mask&face_edge_masks[face]};
      if(const auto found=boundaries.find(key);found!=boundaries.end())
        CHECK(found->second==boundary);
      else boundaries.emplace(key,std::move(boundary));
    }
  }
}

TEST_CASE("green masks canonicalize under every orientation preserving vertex permutation") {
  std::vector<std::array<std::uint8_t,4>> orientations;
  std::array<std::uint8_t,4> order{{0U,1U,2U,3U}};
  do{
    unsigned int inversions{};
    for(std::size_t first=0;first<order.size();++first)
      for(std::size_t second=first+1;second<order.size();++second)
        inversions+=order[first]>order[second]?1U:0U;
    if((inversions&1U)==0U)orientations.push_back(order);
  }while(std::next_permutation(order.begin(),order.end()));
  REQUIRE(orientations.size()==12U);

  for(std::uint8_t mask=0;mask<64U;++mask){
    const auto canonical=tetra::canonical_green_mask(mask);
    CHECK(canonical.mask<=mask);
    CHECK(tetra::permute_green_mask(mask,canonical.vertex_order)==canonical.mask);
    for(const auto& orientation:orientations){
      CAPTURE(mask);
      const auto permuted=tetra::permute_green_mask(mask,orientation);
      CHECK(std::popcount(permuted)==std::popcount(mask));
      CHECK(tetra::canonical_green_mask(permuted).mask==canonical.mask);
      std::array<std::uint8_t,4> inverse{};
      for(std::uint8_t index=0;index<orientation.size();++index)
        inverse[orientation[index]]=index;
      CHECK(tetra::permute_green_mask(permuted,inverse)==mask);
      // Every oriented lookup remains a direct Grande restriction-compatible
      // stencil; the preceding exhaustive test proves its volume and faces.
      CHECK(tetra::complete_green_template(permuted).count>0U);
    }
  }

  const std::array<std::uint8_t,4> duplicate{{0U,0U,2U,3U}};
  const std::array<std::uint8_t,4> reflected{{1U,0U,2U,3U}};
  CHECK_THROWS_AS(static_cast<void>(tetra::permute_green_mask(1U,duplicate)),
                  std::invalid_argument);
  CHECK_THROWS_AS(static_cast<void>(tetra::permute_green_mask(1U,reflected)),
                  std::invalid_argument);
}

TEST_CASE("Scholz construction defines four exact barycentric hexahedra") {
  const auto construction=tetra::make_four_hexahedra();
  REQUIRE(construction.cells.size()==4U);
  std::set<tetra::BarycentricTwelfths> distinct_points;
  for(std::size_t owner=0;owner<construction.cells.size();++owner){
    const auto& cell=construction.cells[owner];
    CHECK(cell[0].weights[owner]==12U);
    CHECK(cell[7].weights==std::array<std::uint8_t,4>{{3U,3U,3U,3U}});
    for(const auto& point:cell){
      CHECK(std::accumulate(point.weights.begin(),point.weights.end(),0U)==12U);
      distinct_points.insert(point);
    }
  }
  // Four vertices, six edge midpoints, four face centroids, and one cell
  // centroid are shared by the four fixed-size hexahedra.
  CHECK(distinct_points.size()==15U);

  auto canonical_cells=construction.cells;
  for(auto& cell:canonical_cells)std::sort(cell.begin(),cell.end());
  std::sort(canonical_cells.begin(),canonical_cells.end());
  std::array<std::uint8_t,4> permutation{{0U,1U,2U,3U}};
  std::size_t permutations{};
  std::array<bool,2> parity_seen{};
  do{
    unsigned int inversions{};
    for(std::size_t first=0;first<permutation.size();++first)
      for(std::size_t second=first+1;second<permutation.size();++second)
        inversions+=permutation[first]>permutation[second]?1U:0U;
    parity_seen[inversions&1U]=true;
    auto permuted=tetra::make_four_hexahedra(permutation).cells;
    for(auto& cell:permuted)std::sort(cell.begin(),cell.end());
    std::sort(permuted.begin(),permuted.end());
    CHECK(permuted==canonical_cells);
    ++permutations;
  }while(std::next_permutation(permutation.begin(),permutation.end()));
  CHECK(permutations==24U);
  CHECK(parity_seen==std::array<bool,2>{{true,true}});

  CHECK_THROWS_AS(static_cast<void>(tetra::make_four_hexahedra({{0U,1U,1U,3U}})),
                  std::invalid_argument);
  CHECK_THROWS_AS(static_cast<void>(tetra::make_four_hexahedra({{0U,1U,2U,4U}})),
                  std::invalid_argument);
}

TEST_CASE("four-hexahedra regular boundary lattices match for all vertex orders") {
  using SampleSet=std::set<std::array<std::uint64_t,4>>;
  const auto samples=[](const tetra::FourHexahedra& construction,
                        std::uint8_t opposite,std::uint32_t resolution){
    SampleSet result;
    for(std::uint8_t owner=0;owner<4U;++owner){
      if(owner==opposite)continue;
      const auto quad=tetra::four_hexahedra_boundary_quad(
          construction,owner,opposite);
      for(std::uint32_t u=0;u<=resolution;++u)
        for(std::uint32_t v=0;v<=resolution;++v){
          const auto sample=tetra::sample_boundary_quad(quad,resolution,u,v);
          CHECK(sample.numerators[opposite]==0U);
          CHECK(std::accumulate(sample.numerators.begin(),
                                sample.numerators.end(),std::uint64_t{})==
                sample.denominator);
          result.insert(sample.numerators);
        }
    }
    return result;
  };

  constexpr std::array<std::uint32_t,5> resolutions{{1U,2U,3U,4U,8U}};
  const auto identity=tetra::make_four_hexahedra();
  std::array<std::uint8_t,4> permutation{{0U,1U,2U,3U}};
  do{
    const auto construction=tetra::make_four_hexahedra(permutation);
    for(std::uint8_t opposite=0;opposite<4U;++opposite)
      for(const auto resolution:resolutions){
        CAPTURE(permutation);
        CAPTURE(opposite);
        CAPTURE(resolution);
        const auto actual=samples(construction,opposite,resolution);
        CHECK(actual==samples(identity,opposite,resolution));
        CHECK(actual.size()==3U*resolution*resolution+3U*resolution+1U);
      }
  }while(std::next_permutation(permutation.begin(),permutation.end()));

  const auto quad=tetra::four_hexahedra_boundary_quad(identity,0U,3U);
  CHECK_THROWS_AS(static_cast<void>(tetra::four_hexahedra_boundary_quad(identity,0U,0U)),
                  std::invalid_argument);
  CHECK_THROWS_AS(static_cast<void>(tetra::four_hexahedra_boundary_quad(identity,4U,0U)),
                  std::invalid_argument);
  CHECK_THROWS_AS(static_cast<void>(tetra::sample_boundary_quad(quad,0U,0U,0U)),
                  std::invalid_argument);
  CHECK_THROWS_AS(static_cast<void>(tetra::sample_boundary_quad(quad,2U,3U,0U)),
                  std::invalid_argument);
  CHECK_THROWS_AS(static_cast<void>(tetra::sample_boundary_quad(
                      quad,std::numeric_limits<std::uint32_t>::max(),0U,0U)),
                  std::invalid_argument);
}

TEST_CASE("red and green conforming cells produce identical four-hexahedra face samples") {
  using Face=std::array<tetra::VertexId,3>;
  struct Incident { tetra::TetId cell{}; std::uint8_t opposite{}; };
  using GlobalSample=std::vector<std::pair<tetra::VertexId,std::uint64_t>>;
  using GlobalSamples=std::set<GlobalSample>;
  const auto face_samples=[](const tetra::TetMesh& mesh,const Incident incident,
                             std::uint32_t resolution){
    GlobalSamples result;
    const auto& tet=mesh.tetrahedron(incident.cell);
    const auto construction=tetra::make_four_hexahedra();
    for(std::uint8_t owner=0;owner<4U;++owner){
      if(owner==incident.opposite)continue;
      const auto quad=tetra::four_hexahedra_boundary_quad(
          construction,owner,incident.opposite);
      for(std::uint32_t u=0;u<=resolution;++u)
        for(std::uint32_t v=0;v<=resolution;++v){
          const auto local=tetra::sample_boundary_quad(quad,resolution,u,v);
          GlobalSample global;
          for(std::size_t vertex=0;vertex<4U;++vertex)
            if(local.numerators[vertex]!=0U)
              global.emplace_back(tet.vertices[vertex],local.numerators[vertex]);
          std::sort(global.begin(),global.end());
          result.insert(std::move(global));
        }
    }
    return result;
  };

  std::array<bool,2> local_parity_seen{};
  for(const auto strategy:{tetra::BccTransitionStrategy::crystalline_restricted,
                           tetra::BccTransitionStrategy::complete_minimal}){
    auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
    if(strategy==tetra::BccTransitionStrategy::complete_minimal)
      REQUIRE(mesh.set_transition_strategy(strategy));
    REQUIRE(mesh.refine_selected_binary({mesh.logical_red_owners().front()}));
    REQUIRE(mesh.has_conforming_active_faces());

    std::map<Face,std::vector<Incident>> incidence;
    std::size_t transition_cells{};
    for(const auto address:mesh.conforming_volume().addresses()){
      const auto& tet=mesh.tetrahedron(address);
      transition_cells+=tet.transition_parent!=tetra::invalid_tet?1U:0U;
      auto sorted_vertices=tet.vertices;
      std::sort(sorted_vertices.begin(),sorted_vertices.end());
      std::array<std::uint8_t,4> order{};
      for(std::size_t local=0;local<4U;++local)
        order[local]=static_cast<std::uint8_t>(
            std::ranges::find(sorted_vertices,tet.vertices[local])-
            sorted_vertices.begin());
      unsigned int inversions{};
      for(std::size_t first=0;first<4U;++first)
        for(std::size_t second=first+1;second<4U;++second)
          inversions+=order[first]>order[second]?1U:0U;
      local_parity_seen[inversions&1U]=true;

      for(std::uint8_t opposite=0;opposite<4U;++opposite){
        Face face{};
        std::size_t output{};
        for(std::uint8_t vertex=0;vertex<4U;++vertex)
          if(vertex!=opposite)face[output++]=tet.vertices[vertex];
        std::sort(face.begin(),face.end());
        incidence[face].push_back({address,opposite});
      }
    }
    REQUIRE(transition_cells>0U);
    std::size_t paired_faces{};
    for(const auto& [face,incidents]:incidence){
      CAPTURE(strategy);
      CAPTURE(face);
      REQUIRE(incidents.size()<=2U);
      if(incidents.size()!=2U)continue;
      for(const auto resolution:{1U,2U,4U})
        CHECK(face_samples(mesh,incidents[0],resolution)==
              face_samples(mesh,incidents[1],resolution));
      ++paired_faces;
    }
    CHECK(paired_faces>0U);
  }
  CHECK(local_parity_seen==std::array<bool,2>{{true,true}});
}

TEST_CASE("complete minimal BCC transitions remain conforming without mask enlargement") {
  auto complete=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  REQUIRE(complete.set_transition_strategy(tetra::BccTransitionStrategy::complete_minimal));
  const auto request=complete.logical_red_owners().front();
  REQUIRE(complete.refine_selected_binary({request}));
  CHECK(complete.has_positive_active_volumes());
  CHECK(complete.has_conforming_active_faces());
  CHECK(complete.logical_midpoint_masks().size()==complete.logical_red_owners().size());
  CHECK(std::ranges::any_of(complete.logical_midpoint_masks(),[](std::uint8_t mask){
    return mask!=0U;
  }));

  auto restricted=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  REQUIRE(restricted.refine_selected_binary({restricted.logical_red_owners().front()}));
  CHECK(complete.logical_red_owners().size()<=restricted.logical_red_owners().size());
}

TEST_CASE("tetrahedron quality metrics distinguish regular and degenerate elements") {
  const double height=std::sqrt(2.0/3.0);
  const std::array<tetra::Vec3,4> regular{{
      {0.0,0.0,0.0},{1.0,0.0,0.0},{0.5,std::sqrt(3.0)/2.0,0.0},
      {0.5,std::sqrt(3.0)/6.0,height}}};
  const auto regular_quality=tetra_viewer::evaluate_tetrahedron_quality(regular);
  CHECK(regular_quality.signed_six_volume>0.0);
  CHECK(regular_quality.mean_ratio==doctest::Approx(1.0));
  CHECK(regular_quality.volume_surface_longest_edge==doctest::Approx(1.0));
  CHECK(regular_quality.minimum_dihedral_sine==doctest::Approx(std::sin(std::acos(1.0/3.0))));
  CHECK(regular_quality.minimum_dihedral_degrees==
        doctest::Approx(std::acos(1.0/3.0)*180.0/std::acos(-1.0)));
  CHECK(regular_quality.maximum_dihedral_degrees==
        doctest::Approx(regular_quality.minimum_dihedral_degrees));

  auto flat=regular;
  flat[3].z=1.0e-9;
  const auto flat_quality=tetra_viewer::evaluate_tetrahedron_quality(flat);
  CHECK(flat_quality.mean_ratio<1.0e-5);
  CHECK(flat_quality.volume_surface_longest_edge<1.0e-8);
  CHECK(flat_quality.minimum_dihedral_sine<1.0e-8);
  CHECK(flat_quality.maximum_dihedral_degrees>179.0);

  std::swap(flat[0],flat[1]);
  const auto inverted=tetra_viewer::evaluate_tetrahedron_quality(flat);
  CHECK(inverted.signed_six_volume<0.0);
  CHECK(inverted.mean_ratio==0.0);
  CHECK(inverted.volume_surface_longest_edge==0.0);
}

TEST_CASE("infinite reversed projection maps near to one and has no far clip") {
  const tetra::Vec3 camera_world{1.0e9+3.0,-2.0e9+4.0,3.0e9+5.0};
  const tetra::Vec3 render_origin{1.0e9,-2.0e9,3.0e9};
  const auto projection=tetra_viewer::make_infinite_reversed_projection(
      camera_world,render_origin,{0.0,0.0,-1.0},{0.0,1.0,0.0},
      std::acos(-1.0)/2.0,16.0/9.0);
  CHECK(projection.depth_convention==
        tetra_viewer::DepthConvention::reversed_infinite);
  CHECK(projection.depth_at(projection.near_plane)==doctest::Approx(1.0));
  CHECK(projection.depth_at(1.0)==doctest::Approx(0.001));
  CHECK(projection.depth_at(1.0e12)>0.0);
  CHECK(projection.depth_at(1.0e12)<projection.depth_at(1.0e6));

  const auto point=projection.camera_relative+
      projection.forward*1.0e12;
  const auto projected=projection.project(point);
  CHECK(projected.visible);
  CHECK(projected.ndc_x==doctest::Approx(0.0).epsilon(1.0e-12));
  CHECK(projected.ndc_y==doctest::Approx(0.0).epsilon(1.0e-12));
  CHECK(projected.depth==doctest::Approx(1.0e-15).epsilon(1.0e-12));

  const auto before_near=projection.project(
      projection.camera_relative+projection.forward*(projection.near_plane*0.5));
  CHECK_FALSE(before_near.visible);
  CHECK(before_near.depth>1.0);
}

TEST_CASE("reversed projection matrix matches the shared CPU projection") {
  const auto projection=tetra_viewer::make_infinite_reversed_projection(
      {12.0,7.0,-3.0},{10.0,5.0,-5.0},{0.2,-0.1,-1.0},
      {0.0,1.0,0.0},0.9,1.7);
  const tetra::Vec3 point{2.25,1.8,-8.0};
  const auto expected=projection.project(point);
  const std::array<float,4> homogeneous{
      static_cast<float>(point.x),static_cast<float>(point.y),
      static_cast<float>(point.z),1.0F};
  std::array<double,4> clip{};
  for(std::size_t row=0;row<4U;++row)
    for(std::size_t column=0;column<4U;++column)
      clip[row]+=projection.matrix[column*4U+row]*homogeneous[column];
  REQUIRE(clip[3]>0.0);
  CHECK(clip[0]/clip[3]==doctest::Approx(expected.ndc_x).epsilon(1.0e-6));
  CHECK(clip[1]/clip[3]==doctest::Approx(expected.ndc_y).epsilon(1.0e-6));
  CHECK(clip[2]/clip[3]==doctest::Approx(expected.depth).epsilon(1.0e-6));
}

TEST_CASE("reversed projection preserves the positive-height Vulkan screen orientation") {
  const auto projection=tetra_viewer::make_infinite_reversed_projection(
      {},{},{0.0,0.0,-1.0},{0.0,1.0,0.0},std::acos(-1.0)/2.0,1.0);
  const auto screen_right=projection.project({-0.25,0.0,-1.0});
  const auto screen_down=projection.project({0.0,-0.25,-1.0});
  REQUIRE(screen_right.visible);
  REQUIRE(screen_down.visible);
  CHECK(screen_right.ndc_x>0.0);
  CHECK(screen_down.ndc_y>0.0);
  CHECK(projection.right.x==doctest::Approx(-1.0));
  CHECK(projection.up.y==doctest::Approx(-1.0));
}

TEST_CASE("planet visibility remains separate from infinite projection") {
  constexpr double earth_radius=6371000.0;
  CHECK(tetra_viewer::main_scene_depth_convention==
        tetra_viewer::DepthConvention::reversed_infinite);
  CHECK(tetra_viewer::shadow_map_depth_convention==
        tetra_viewer::DepthConvention::standard_finite);
  CHECK(tetra_viewer::planetary_horizon_distance(earth_radius,0.0)==0.0);
  CHECK(tetra_viewer::planetary_horizon_distance(earth_radius,1000.0)==
        doctest::Approx(std::sqrt(1000.0*(2.0*earth_radius+1000.0))));
  CHECK(tetra_viewer::planetary_horizon_distance(earth_radius,1000.0,4000.0)==
        doctest::Approx(
            std::sqrt(1000.0*(2.0*earth_radius+1000.0))+
            std::sqrt(4000.0*(2.0*earth_radius+4000.0))));
  const tetra::Vec3 camera{0.0,0.0,earth_radius+1000.0};
  CHECK(tetra_viewer::sphere_fully_occluded_by_planet(
      camera,{},earth_radius,{0.0,0.0,-earth_radius-100.0},10.0));
  CHECK_FALSE(tetra_viewer::sphere_fully_occluded_by_planet(
      camera,{},earth_radius,{2.0*earth_radius,0.0,earth_radius+1000.0},10.0));
  CHECK_FALSE(tetra_viewer::sphere_fully_occluded_by_planet(
      camera,{},earth_radius,{0.0,0.0,earth_radius+500.0},10.0));
}

TEST_CASE("reversed depth retains useful precision from ground to Earth orbit") {
  const auto projection=tetra_viewer::make_infinite_reversed_projection(
      {},{},{0.0,0.0,-1.0},{0.0,1.0,0.0},1.0,16.0/9.0);
  for(const double distance:{1.0,100000.0,6371000.0,42000000.0}){
    const float depth=static_cast<float>(projection.depth_at(distance));
    const float next=std::nextafter(depth,0.0F);
    REQUIRE(depth>0.0F);
    REQUIRE(next>0.0F);
    const double next_distance=projection.near_plane/static_cast<double>(next);
    CHECK(next_distance>distance);
    CHECK((next_distance-distance)/distance<2.0e-7);
  }
}

TEST_CASE("block-local camera-relative conversion preserves ground detail at planet coordinates") {
  const tetra::Vec3 block_origin{6371000.0,-4213377.0,982451653.0};
  const tetra::Vec3 render_origin{
      block_origin.x+0.125,block_origin.y-0.25,block_origin.z+0.5};
  const tetra::Vec3 local{0.001,0.002,0.003};
  const auto relative=tetra_viewer::camera_relative_block_position(
      block_origin,render_origin,local);
  CHECK(relative.x==doctest::Approx(-0.124).epsilon(1.0e-9));
  CHECK(relative.y==doctest::Approx(0.252).epsilon(1.0e-9));
  CHECK(relative.z==doctest::Approx(-0.497).epsilon(1.0e-9));
  const std::array<float,3> gpu{
      static_cast<float>(relative.x),static_cast<float>(relative.y),
      static_cast<float>(relative.z)};
  CHECK(gpu[0]==doctest::Approx(-0.124F).epsilon(1.0e-6));
}

TEST_CASE("editor orbit camera rotates pans and dollies independently") {
  tetra_viewer::OrbitCamera camera;
  const auto length=[](tetra::Vec3 value){
    return std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);
  };
  CHECK(camera.position().x==doctest::Approx(0.5));
  CHECK(camera.position().y==doctest::Approx(0.5));
  CHECK(camera.position().z==doctest::Approx(3.0));

  const auto original_target=camera.target;
  camera.orbit(100.0,-50.0);
  CHECK(camera.target.x==doctest::Approx(original_target.x));
  CHECK(camera.target.y==doctest::Approx(original_target.y));
  CHECK(camera.target.z==doctest::Approx(original_target.z));
  CHECK(length(camera.position()-camera.target)==doctest::Approx(camera.distance));

  const auto position_before_pan=camera.position();
  const auto target_before_pan=camera.target;
  const auto forward_before_pan=camera.forward();
  camera.pan(40.0,-20.0,800.0,0.7853981633974483);
  const auto position_delta=camera.position()-position_before_pan;
  const auto target_delta=camera.target-target_before_pan;
  CHECK(length(target_delta)>0.0);
  CHECK(position_delta.x==doctest::Approx(target_delta.x));
  CHECK(position_delta.y==doctest::Approx(target_delta.y));
  CHECK(position_delta.z==doctest::Approx(target_delta.z));
  CHECK(camera.forward().x==doctest::Approx(forward_before_pan.x));
  CHECK(camera.forward().y==doctest::Approx(forward_before_pan.y));
  CHECK(camera.forward().z==doctest::Approx(forward_before_pan.z));

  const double distance_before_dolly=camera.distance;
  camera.dolly(1.0,0.15);
  CHECK(camera.distance<distance_before_dolly);
  CHECK(camera.distance==doctest::Approx(distance_before_dolly-distance_before_dolly*0.22*0.15));
}

TEST_CASE("production world profile pins the playable rendering contract") {
  const auto profile=tetra_viewer::production_world_profile();
  CHECK(profile.subdivision==tetra::SubdivisionMethod::bcc_red_green);
  CHECK(profile.shape==tetra::ImplicitShapeKind::perlin_terrain);
  CHECK(profile.surface==tetra_viewer::SurfaceMethod::surface_optimization);
  CHECK(profile.volume_connection==
        tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  CHECK(profile.material==tetra_viewer::MaterialRule::variational_smooth);
  CHECK(profile.shading==tetra_viewer::ShadingModel::stone_pbr);
  CHECK(profile.adaptation==tetra::AdaptationConfiguration{});
  CHECK(profile.terrain.height_offset==
        doctest::Approx(-0.56685212800775142));
  CHECK(profile.terrain.landform_amplitude==doctest::Approx(1.5));
  CHECK(profile.terrain.mountain_amplitude==doctest::Approx(6.0));
  CHECK(profile.terrain.planetary_mountain_amplitude_scale==
        doctest::Approx(24.0));
  CHECK(profile.terrain.planetary_mountain_frequency_scale==
        doctest::Approx(0.0625));
  CHECK(profile.terrain.planetary_mountain_fade_start==doctest::Approx(24.0));
  CHECK(profile.terrain.planetary_mountain_fade_end==doctest::Approx(96.0));
  CHECK(profile.terrain.gameplay_hill_amplitude==doctest::Approx(0.7));
  CHECK(profile.terrain.gameplay_feature_amplitude==doctest::Approx(0.18));
  CHECK(profile.terrain.gameplay_corridor_depth==doctest::Approx(0.1));
  CHECK(profile.terrain.ground_roughness_amplitude==doctest::Approx(0.025));
  CHECK(profile.terrain.spawn_flat_radius==doctest::Approx(0.8));
  CHECK(profile.terrain.spawn_blend_radius==doctest::Approx(3.0));
  CHECK(profile.terrain.planet_radius==doctest::Approx(20'000.0));
  CHECK(profile.octave_detail_amplitude==doctest::Approx(0.0));
  CHECK(profile.draw_chunks==tetra_viewer::default_surface_draw_chunk_strategy);
  CHECK(profile.domain.world_extent==doctest::Approx(65'536.0));
  CHECK(profile.background_red_depth==3U);
  CHECK(profile.near_red_depth==20U);
  CHECK(profile.near_volume_radius==doctest::Approx(0.6));
  CHECK(profile.maximum_volume_blocks==4096U);
  CHECK(profile.maximum_hierarchy_blocks==65536U);
  CHECK(profile.hierarchy_guard_frustum_scale==doctest::Approx(1.35));
  CHECK(profile.hierarchy_prediction_factor==doctest::Approx(1.0));
  CHECK(profile.hierarchy_recent_retention_epochs==8U);
  CHECK(profile.view_distance==doctest::Approx(5.0));
  CHECK(profile.pixel_threshold==doctest::Approx(128.0));
  CHECK(profile.maximum_depth==20U);
  CHECK(profile.budgets.maximum_cpu_bytes==512U*1024U*1024U);
  CHECK(profile.budgets.maximum_triangles==500000U);
  CHECK(profile.budgets.maximum_work_units==20000000U);
  CHECK(profile.budgets.maximum_upload_bytes==32U*1024U*1024U);
  CHECK(profile.show_faces);
  CHECK(profile.show_surface_edges);
  CHECK_FALSE(profile.show_hierarchy_edges);
  CHECK_FALSE(profile.x_cutaway);
}

TEST_CASE("production terrain is a closed radial field inside one sparse root") {
  const auto profile=tetra_viewer::production_world_profile();
  tetra::Sphere field;
  field.kind=profile.shape;
  field.terrain=profile.terrain;
  field.secondary=profile.octave_detail_amplitude;
  field.frequency=profile.octave_detail_frequency;
  const double radius=field.terrain.planet_radius;
  const tetra::Vec3 centre{
      field.centre.x,field.centre.y-radius,field.centre.z};
  CHECK(field.signed_distance(field.centre)==doctest::Approx(0.0).epsilon(1e-9));
  CHECK(field.signed_distance(centre)<-radius*0.99);
  CHECK(field.signed_distance(centre+tetra::Vec3{0.0,radius+20.0,0.0})>0.0);
  const auto north_normal=field.normal(field.centre);
  CHECK(std::abs(north_normal.x)<1e-5);
  CHECK(north_normal.y>0.99999);
  CHECK(std::abs(north_normal.z)<1e-5);
  const auto projected=field.project_to_surface(
      centre+tetra::Vec3{radius*0.8,radius*0.7,-radius*0.2});
  CHECK(std::abs(field.signed_distance(projected))<1e-7);
  const auto maximum=profile.domain.world_origin+
      tetra::Vec3{profile.domain.world_extent,profile.domain.world_extent,
                  profile.domain.world_extent};
  for(std::size_t axis=0;axis<3U;++axis){
    const double coordinate=axis==0U?centre.x:(axis==1U?centre.y:centre.z);
    const double minimum=axis==0U?profile.domain.world_origin.x:
        (axis==1U?profile.domain.world_origin.y:profile.domain.world_origin.z);
    const double upper=axis==0U?maximum.x:(axis==1U?maximum.y:maximum.z);
    CHECK(coordinate-radius>minimum);
    CHECK(coordinate+radius<upper);
  }
}

TEST_CASE("world resource budgets independently gate every publication cost") {
  const tetra_viewer::WorldResourceBudgets budgets{
      .maximum_cpu_bytes=100U,.maximum_triangles=200U,
      .maximum_work_units=300U,.maximum_upload_bytes=400U};
  CHECK(tetra_viewer::evaluate_world_resource_budgets(
      budgets,{100U,200U,300U,400U}).admitted());
  const auto cpu=tetra_viewer::evaluate_world_resource_budgets(
      budgets,{101U,200U,300U,400U});
  CHECK_FALSE(cpu.cpu);CHECK(cpu.triangles);CHECK(cpu.work);CHECK(cpu.upload);
  const auto triangles=tetra_viewer::evaluate_world_resource_budgets(
      budgets,{100U,201U,300U,400U});
  CHECK(triangles.cpu);CHECK_FALSE(triangles.triangles);
  const auto work=tetra_viewer::evaluate_world_resource_budgets(
      budgets,{100U,200U,301U,400U});
  CHECK(work.triangles);CHECK_FALSE(work.work);
  const auto upload=tetra_viewer::evaluate_world_resource_budgets(
      budgets,{100U,200U,300U,401U});
  CHECK(upload.work);CHECK_FALSE(upload.upload);
  auto invalid=tetra_viewer::production_world_profile();
  invalid.budgets.maximum_cpu_bytes=0U;
  CHECK_THROWS_AS(([&]{
      tetra_viewer::BlockedTerrainRuntime runtime{invalid};
    }()),std::invalid_argument);

  invalid=tetra_viewer::production_world_profile();
  invalid.maximum_volume_blocks=0U;
  CHECK_THROWS_AS(([&]{
      tetra_viewer::BlockedTerrainRuntime runtime{invalid};
    }()),std::invalid_argument);

  invalid=tetra_viewer::production_world_profile();
  invalid.maximum_hierarchy_blocks=tetra::bcc_root_tetrahedron_count-1U;
  CHECK_THROWS_AS(([&]{
      tetra_viewer::BlockedTerrainRuntime runtime{invalid};
    }()),std::invalid_argument);

  invalid=tetra_viewer::production_world_profile();
  invalid.hierarchy_recent_retention_epochs=0U;
  CHECK_THROWS_AS(([&]{
      tetra_viewer::BlockedTerrainRuntime runtime{invalid};
    }()),std::invalid_argument);

  auto undersized=tetra_viewer::production_world_profile();
  undersized.budgets.maximum_triangles=1U;
  CHECK_THROWS_AS(([&]{
      tetra_viewer::BlockedTerrainRuntime runtime{undersized};
    }()),std::length_error);
}

TEST_CASE("world residency plans deduplicate overlapping hard volume pins") {
  const auto target=tetra::WorldTetAddress::root(0U)
      .child(0U).child(0U).child(0U);
  std::array targets{target};
  auto checkpoint=tetra::make_sparse_world_cut_checkpoint(
      targets,3U,1U,tetra::HierarchyResidencyTier::surface);
  tetra::WorldCutDirectory source(checkpoint);
  std::vector<tetra::WorldTetAddress> owners;
  source.for_each_logical_owner(
      [&](tetra::WorldTetAddress owner){owners.push_back(owner);});
  std::ranges::sort(owners);
  const auto geometry=tetra::world_tetrahedron_geometry(target);
  tetra::Vec3 centre{};
  for(const auto point:geometry)centre=centre+point;
  centre=centre/4.0;
  const tetra::WorldStreamingDemand::Domain domain{
      .world_origin={0.0,0.0,0.0},.world_extent=1.0};
  const std::array pins{
      tetra_viewer::WorldVolumePin{
          centre,0.01,tetra_viewer::WorldVolumePinKind::player_collision},
      tetra_viewer::WorldVolumePin{
          centre,0.01,tetra_viewer::WorldVolumePinKind::terrain_edit}};
  const auto plan=tetra_viewer::plan_world_residency(
      owners,3U,domain,pins,64U);
  REQUIRE_FALSE(plan.volume_blocks.empty());
  CHECK(plan.volume_blocks.size()<plan.surface_blocks.size());
  CHECK(plan.metrics.player_collision_blocks==plan.volume_blocks.size());
  CHECK(plan.metrics.terrain_edit_blocks==plan.volume_blocks.size());
  CHECK(plan.metrics.physics_blocks==0U);
  tetra_viewer::apply_world_residency_plan(checkpoint,plan);
  tetra::WorldCutDirectory tiered(std::move(checkpoint));
  CHECK(tiered.metrics().volume_blocks==plan.volume_blocks.size());
  CHECK(tiered.metrics().surface_blocks==
        plan.surface_blocks.size()-plan.volume_blocks.size());
  CHECK(tiered.metrics().summary_blocks+plan.surface_blocks.size()==
        tiered.metrics().blocks);

  REQUIRE(plan.volume_blocks.size()>0U);
  CHECK_THROWS_AS(([&]{static_cast<void>(tetra_viewer::plan_world_residency(
      owners,3U,domain,pins,plan.volume_blocks.size()-1U));}()),std::length_error);
  const std::array invalid_pins{tetra_viewer::WorldVolumePin{
      centre,-1.0,tetra_viewer::WorldVolumePinKind::physics}};
  CHECK_THROWS_AS(([&]{static_cast<void>(tetra_viewer::plan_world_residency(
      owners,3U,domain,invalid_pins,64U));}()),std::invalid_argument);
  const std::array invalid_kind{tetra_viewer::WorldVolumePin{
      centre,0.1,static_cast<tetra_viewer::WorldVolumePinKind>(255U)}};
  CHECK_THROWS_AS(([&]{static_cast<void>(tetra_viewer::plan_world_residency(
      owners,3U,domain,invalid_kind,64U));}()),std::invalid_argument);
}

TEST_CASE("world hierarchy demand is revisioned predictive recent and bounded") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int pass=0;pass<6U;++pass)mesh.refine_all_binary();
  auto checkpoint=tetra::make_world_cut_checkpoint(mesh,3U,17U);
  const tetra::WorldStreamingDemand::Domain domain{};
  tetra::Camera camera;
  camera.position={0.5,0.5,0.95};camera.forward={0.0,0.0,-1.0};
  camera.vertical_fov_radians=0.45;camera.aspect_ratio=1.4;
  const std::array pins{
      tetra_viewer::WorldVolumePin{
          {0.5,0.5,0.5},0.18,
          tetra_viewer::WorldVolumePinKind::terrain_edit},
      tetra_viewer::WorldVolumePin{
          {0.5,0.5,0.5},0.18,
          tetra_viewer::WorldVolumePinKind::physics}};
  tetra_viewer::WorldHierarchyDemandConfiguration configuration;
  configuration.player_radius=0.05;
  configuration.guard_frustum_scale=1.6;
  configuration.prediction_factor=3.0;
  configuration.recent_retention_epochs=2U;
  configuration.maximum_blocks=checkpoint.blocks.size();
  const auto first=tetra_viewer::plan_world_hierarchy_demand(
      checkpoint,domain,camera,pins,configuration);
  REQUIRE(first.state.records.size()==checkpoint.blocks.size());
  CHECK(first.state.epoch==1U);
  CHECK(first.metrics.canonical_hash!=0U);
  CHECK(first.metrics.loaded_blocks==checkpoint.blocks.size());
  CHECK(first.metrics.blocks_by_kind[0]>0U);
  CHECK(first.metrics.blocks_by_kind[1]>0U);
  CHECK(first.metrics.blocks_by_kind[4]>0U);
  CHECK(first.metrics.blocks_by_kind[5]>0U);
  CHECK(first.metrics.blocks_by_kind[6]>0U);
  CHECK(first.metrics.blocks_by_kind[7]>0U);
  CHECK(std::ranges::is_sorted(
      first.state.records,{},&tetra_viewer::WorldHierarchyDemandRecord::id));
  CHECK(std::ranges::all_of(first.state.records,[&](const auto& record){
    return record.revision==checkpoint.revision;
  }));
  const auto visual_demand_has_surface_authority=[](const auto& record){
    const auto visual=tetra_viewer::world_hierarchy_demand_mask(
        tetra_viewer::WorldHierarchyDemandKind::visible)|
        tetra_viewer::world_hierarchy_demand_mask(
            tetra_viewer::WorldHierarchyDemandKind::guard)|
        tetra_viewer::world_hierarchy_demand_mask(
            tetra_viewer::WorldHierarchyDemandKind::predicted)|
        tetra_viewer::world_hierarchy_demand_mask(
            tetra_viewer::WorldHierarchyDemandKind::recent);
    return (record.kinds&visual)==0U||
        record.residency!=tetra::HierarchyResidencyTier::summary;
  };
  CHECK(std::ranges::all_of(
      first.state.records,visual_demand_has_surface_authority));

  camera.position.x+=0.12;
  camera.forward={0.35,0.0,-0.9367496997597597};
  const auto moved=tetra_viewer::plan_world_hierarchy_demand(
      checkpoint,domain,camera,pins,configuration,&first.state);
  CHECK(moved.state.epoch==2U);
  CHECK(moved.metrics.blocks_by_kind[2]>0U);
  CHECK(moved.metrics.promoted_blocks>0U);
  CHECK(moved.metrics.demoted_blocks>0U);
  CHECK(std::ranges::all_of(
      moved.state.records,visual_demand_has_surface_authority));

  camera.forward={0.0,0.0,1.0};
  const auto turned=tetra_viewer::plan_world_hierarchy_demand(
      checkpoint,domain,camera,pins,configuration,&moved.state);
  CHECK(turned.state.epoch==3U);
  CHECK(turned.metrics.blocks_by_kind[3]>0U);
  CHECK(std::ranges::all_of(
      turned.state.records,visual_demand_has_surface_authority));
  const auto repeated=tetra_viewer::plan_world_hierarchy_demand(
      checkpoint,domain,camera,pins,configuration,&turned.state);
  const auto repeated_again=tetra_viewer::plan_world_hierarchy_demand(
      checkpoint,domain,camera,pins,configuration,&turned.state);
  CHECK(repeated.state.epoch==turned.state.epoch);
  CHECK(repeated.state.records==repeated_again.state.records);
  CHECK(repeated.metrics.canonical_hash==repeated_again.metrics.canonical_hash);

  camera.position.x+=0.01;
  const auto retained_once=tetra_viewer::plan_world_hierarchy_demand(
      checkpoint,domain,camera,pins,configuration,&turned.state);
  camera.position.x+=0.01;
  const auto expired=tetra_viewer::plan_world_hierarchy_demand(
      checkpoint,domain,camera,pins,configuration,&retained_once.state);
  CHECK(expired.state.epoch==turned.state.epoch+2U);
  CHECK(expired.metrics.expired_records>0U);

  configuration.maximum_blocks=checkpoint.blocks.size()-1U;
  CHECK_THROWS_AS(static_cast<void>(
      tetra_viewer::plan_world_hierarchy_demand(
          checkpoint,domain,camera,pins,configuration,&turned.state)),
      std::length_error);
  configuration.maximum_blocks=checkpoint.blocks.size();
  auto invalid_camera=camera;invalid_camera.forward={};
  CHECK_THROWS_AS(static_cast<void>(
      tetra_viewer::plan_world_hierarchy_demand(
          checkpoint,domain,invalid_camera,pins,configuration,&turned.state)),
      std::invalid_argument);
  auto invalid_domain=domain;invalid_domain.world_extent=0.0;
  CHECK_THROWS_AS(static_cast<void>(
      tetra_viewer::plan_world_hierarchy_demand(
          checkpoint,invalid_domain,camera,pins,configuration,&turned.state)),
      std::invalid_argument);
}

TEST_CASE("atmosphere shadow front selects guarded offscreen casters surface only") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int pass=0;pass<6U;++pass)mesh.refine_all_binary();
  auto checkpoint=tetra::make_world_cut_checkpoint(mesh,3U,31U);
  const tetra::WorldStreamingDemand::Domain domain{};
  tetra_viewer::AtmosphereShadowFrontRequest local{
      .receiver_bounds={{0.05,0.35,0.35},{0.15,0.65,0.65}},
      .sun_direction={1.0,0.0,0.0},.caster_reach=0.0,
      .render_origin={0.5,0.5,0.5},.generation=7U};
  const auto local_front=tetra_viewer::plan_atmosphere_shadow_front(
      checkpoint,domain,local);
  local.caster_reach=0.8;
  const auto guarded=tetra_viewer::plan_atmosphere_shadow_front(
      checkpoint,domain,local);
  CHECK(guarded.caster_bounds.minimum.x==doctest::Approx(0.05));
  CHECK(guarded.caster_bounds.maximum.x==doctest::Approx(0.95));
  CHECK(guarded.caster_blocks.size()>local_front.caster_blocks.size());
  CHECK(guarded.complete());
  CHECK(std::ranges::is_sorted(guarded.caster_blocks));
  tetra::Camera demand_camera;
  demand_camera.position={0.1,0.5,0.5};
  demand_camera.forward={-1.0,0.0,0.0};
  demand_camera.up={0.0,1.0,0.0};
  demand_camera.vertical_fov_radians=0.4;
  tetra_viewer::WorldHierarchyDemandConfiguration demand_configuration;
  demand_configuration.maximum_blocks=checkpoint.blocks.size();
  const auto demand=tetra_viewer::plan_world_hierarchy_demand(
      checkpoint,domain,demand_camera,{},demand_configuration,nullptr,
      guarded.caster_blocks);
  CHECK(demand.metrics.blocks_by_kind[8]==guarded.caster_blocks.size());
  std::size_t offscreen_casters{};
  for(const auto& record:demand.state.records)
    if(tetra_viewer::has_world_hierarchy_demand(
           record,tetra_viewer::WorldHierarchyDemandKind::atmosphere_shadow)){
      CHECK(record.residency!=tetra::HierarchyResidencyTier::summary);
      offscreen_casters+=!tetra_viewer::has_world_hierarchy_demand(
          record,tetra_viewer::WorldHierarchyDemandKind::visible)?1U:0U;
    }
  CHECK(offscreen_casters>0U);
  CHECK_FALSE(tetra_viewer::atmosphere_shadow_front_compatible(
      guarded,checkpoint.revision,7U));
  const auto published=tetra_viewer::publish_atmosphere_shadow_depth(guarded,7U);
  CHECK(tetra_viewer::atmosphere_shadow_front_compatible(
      published,checkpoint.revision,7U));
  CHECK_FALSE(tetra_viewer::atmosphere_shadow_front_compatible(
      published,checkpoint.revision+1U,7U));
  CHECK_FALSE(tetra_viewer::atmosphere_shadow_front_compatible(
      published,checkpoint.revision,8U));

  REQUIRE_FALSE(guarded.caster_blocks.empty());
  const auto selected=guarded.caster_blocks.front();
  const auto found=std::ranges::lower_bound(
      checkpoint.blocks,selected,{},&tetra::HierarchyBlockSnapshot::id);
  REQUIRE(found!=checkpoint.blocks.end());
  found->residency=tetra::HierarchyResidencyTier::summary;
  const auto incomplete=tetra_viewer::plan_atmosphere_shadow_front(
      checkpoint,domain,local);
  CHECK_FALSE(incomplete.complete());
  CHECK(incomplete.missing_surface_blocks==
        std::vector<tetra::HierarchyBlockId>{selected});
  CHECK(incomplete.completeness<1.0);
  CHECK_THROWS_AS(static_cast<void>(
      tetra_viewer::publish_atmosphere_shadow_depth(incomplete,7U)),
      std::invalid_argument);
  CHECK_THROWS_AS(static_cast<void>(
      tetra_viewer::publish_atmosphere_shadow_depth(guarded,6U)),
      std::invalid_argument);
}

TEST_CASE("receiver-fitted atmosphere shadow map encloses guarded frustum and sun extrusion") {
  const auto request=tetra_viewer::make_atmosphere_shadow_front_request(
      {10.0,2.0,-4.0},{0.0,0.0,-1.0},{1.0,0.0,0.0},{0.0,1.0,0.0},
      0.5,1.6,100.0,1.2,{-0.8,0.1,-0.3},80.0,{8.0,0.0,-8.0},9U);
  CHECK(request.receiver_bounds.minimum.x<10.0);
  CHECK(request.receiver_bounds.maximum.x>10.0);
  CHECK(request.receiver_bounds.minimum.y<2.0);
  CHECK(request.receiver_bounds.maximum.y>2.0);
  CHECK(request.receiver_bounds.minimum.z<=-104.0);
  const auto caster=tetra_viewer::atmosphere_shadow_caster_bounds(request);
  CHECK(caster.minimum.x<request.receiver_bounds.minimum.x);
  CHECK(caster.maximum.y>request.receiver_bounds.maximum.y);
  CHECK(caster.minimum.z<request.receiver_bounds.minimum.z);
  const auto fit=tetra_viewer::fit_atmosphere_shadow_map(request,1024U);
  CHECK(fit.texel_world_size_x>0.0);
  CHECK(fit.texel_world_size_y>0.0);
  CHECK(fit.depth_world_span>0.0);
  REQUIRE(request.receiver_point_count>0U);
  for(std::uint32_t index=0;index<request.receiver_point_count;++index)
    for(const double extrusion:{0.0,request.caster_reach}){
        const auto point=request.receiver_points[index]-request.render_origin+
            fit.sun_direction*extrusion;
        const auto projected=tetra_viewer::transform_shadow_point(
            fit.matrix,point);
        CHECK(std::abs(projected.x)<=1.00001);
        CHECK(std::abs(projected.y)<=1.00001);
        CHECK(projected.z>=-1.0e-5);
        CHECK(projected.z<=1.00001);
    }
  const auto centre=(caster.minimum+caster.maximum)/2.0;
  const auto toward_sun=tetra_viewer::transform_shadow_point(
      fit.matrix,centre+fit.sun_direction);
  const auto away_from_sun=tetra_viewer::transform_shadow_point(
      fit.matrix,centre-fit.sun_direction);
  CHECK(toward_sun.z<away_from_sun.z);
  const auto moved_request=tetra_viewer::make_atmosphere_shadow_front_request(
      {10.000001,2.000001,-3.999999},{0.0,0.0,-1.0},
      {1.0,0.0,0.0},{0.0,1.0,0.0},0.5,1.6,100.0,1.2,
      {-0.8,0.1,-0.3},80.0,{8.0,0.0,-8.0},10U);
  const auto moved_fit=tetra_viewer::fit_atmosphere_shadow_map(
      moved_request,1024U);
  CHECK(moved_fit.matrix==fit.matrix);
  CHECK_THROWS_AS(static_cast<void>(
      tetra_viewer::fit_atmosphere_shadow_map(request,0U)),
      std::invalid_argument);
}

TEST_CASE("projected radial world cut covers the globe with graded bounded detail") {
  const auto profile=tetra_viewer::production_world_profile();
  tetra::Sphere field;field.kind=profile.shape;field.terrain=profile.terrain;
  field.secondary=profile.octave_detail_amplitude;
  field.frequency=profile.octave_detail_frequency;
  tetra::Camera camera;
  camera.position={0.5,0.72,0.78};camera.forward={0.0,-0.2,-1.0};
  camera.viewport_height_pixels=800.0;camera.aspect_ratio=1.6;
  const auto selection=tetra_viewer::select_world_lod_cut(profile,field,camera);
  CAPTURE(selection.metrics.visited_owners);
  CAPTURE(selection.metrics.logical_owners_before_closure);
  CAPTURE(selection.metrics.logical_owners_after_closure);
  CAPTURE(selection.metrics.minimum_surface_depth);
  CAPTURE(selection.metrics.maximum_surface_depth);
  CAPTURE(selection.metrics.maximum_shared_vertex_depth_delta);
  REQUIRE_FALSE(selection.owners.empty());
  CHECK(selection.metrics.maximum_surface_depth==profile.near_red_depth);
  CHECK(selection.metrics.maximum_shared_vertex_depth_delta<=1U);
  CHECK(selection.metrics.logical_owners_after_closure<1'100'000U);
  CHECK(selection.metrics.horizon_owners==0U);
}

TEST_CASE("production radial world resolves the surface around the player") {
  const auto profile=tetra_viewer::production_world_profile();
  tetra::Sphere field;field.kind=profile.shape;field.terrain=profile.terrain;
  field.secondary=profile.octave_detail_amplitude;
  field.frequency=profile.octave_detail_frequency;
  tetra::Camera camera;
  camera.position={0.5,0.72,0.78};camera.forward={0.0,-0.2,-1.0};
  camera.viewport_height_pixels=800.0;camera.aspect_ratio=1.6;
  const auto selection=tetra_viewer::select_world_lod_cut(profile,field,camera);
  double maximum_near_edge{};
  unsigned int minimum_near_depth=profile.near_red_depth;
  std::size_t near_crossing_owners{};
  std::size_t far_crossing_owners{};
  for(const auto owner:selection.owners){
    const auto normalized=tetra::world_tetrahedron_geometry(owner);
    std::array<tetra::Vec3,4> points{};
    bool negative{},positive{};
    tetra::Vec3 centre{};
    for(std::size_t corner=0;corner<points.size();++corner){
      points[corner]=profile.domain.to_world(normalized[corner]);
      centre=centre+points[corner];
      const double distance=field.signed_distance(points[corner]);
      negative|=distance<0.0;positive|=distance>=0.0;
    }
    centre=centre/4.0;
    const auto offset=centre-camera.position;
    const double camera_distance=std::sqrt(
        offset.x*offset.x+offset.y*offset.y+offset.z*offset.z);
    if(negative&&positive&&camera_distance>10'000.0)
      ++far_crossing_owners;
    if(!(negative&&positive)||camera_distance>0.4)continue;
    ++near_crossing_owners;
    minimum_near_depth=std::min(minimum_near_depth,owner.red_depth());
    for(std::size_t first=0;first<points.size();++first)
      for(std::size_t second=first+1U;second<points.size();++second){
        const auto edge=points[first]-points[second];
        maximum_near_edge=std::max(maximum_near_edge,std::sqrt(
            edge.x*edge.x+edge.y*edge.y+edge.z*edge.z));
      }
  }
  CAPTURE(near_crossing_owners);
  CAPTURE(minimum_near_depth);
  CAPTURE(maximum_near_edge);
  REQUIRE(near_crossing_owners>0U);
  CHECK(far_crossing_owners>0U);
  CHECK(minimum_near_depth==profile.near_red_depth);
  CHECK(maximum_near_edge<=0.09);
}

TEST_CASE("production planet keeps local relief and tall distant landmarks") {
  const auto profile=tetra_viewer::production_world_profile();
  tetra::Sphere field;field.kind=profile.shape;field.terrain=profile.terrain;
  field.secondary=profile.octave_detail_amplitude;
  field.frequency=profile.octave_detail_frequency;
  const double radius=field.terrain.planet_radius;
  const tetra::Vec3 centre{
      field.centre.x,field.centre.y-radius,field.centre.z};
  const auto height=[&](double arc,double azimuth){
    const double angle=arc/radius;
    const tetra::Vec3 direction{
        std::sin(angle)*std::cos(azimuth),std::cos(angle),
        std::sin(angle)*std::sin(azimuth)};
    return -field.signed_distance(centre+direction*radius);
  };
  double local_minimum=std::numeric_limits<double>::infinity();
  double local_maximum=-std::numeric_limits<double>::infinity();
  double distant_minimum=std::numeric_limits<double>::infinity();
  double distant_maximum=-std::numeric_limits<double>::infinity();
  double tallest_arc{},tallest_azimuth{};
  for(std::size_t direction=0U;direction<128U;++direction){
    const double azimuth=2.0*std::numbers::pi*
        static_cast<double>(direction)/128.0;
    for(double arc=3.0;arc<=18.0;arc+=0.5){
      const double sample=height(arc,azimuth);
      local_minimum=std::min(local_minimum,sample);
      local_maximum=std::max(local_maximum,sample);
    }
    for(double arc=96.0;arc<=768.0;arc+=4.0){
      const double sample=height(arc,azimuth);
      distant_minimum=std::min(distant_minimum,sample);
      if(sample>distant_maximum){
        distant_maximum=sample;tallest_arc=arc;tallest_azimuth=azimuth;
      }
    }
  }
  CAPTURE(local_minimum);CAPTURE(local_maximum);
  CAPTURE(distant_minimum);CAPTURE(distant_maximum);
  CAPTURE(tallest_arc);CAPTURE(tallest_azimuth);
  CHECK(local_maximum-local_minimum>0.25);
  CHECK(local_maximum<5.0);
  CHECK(distant_maximum>40.0);
  CHECK(distant_maximum-distant_minimum>60.0);
}

TEST_CASE("batched camera frontier cuts retain exact conformity at every slice") {
  auto profile=tetra_viewer::production_world_profile();
  profile.terrain.planet_radius=0.0;
  profile.domain={.world_origin={-63.5,-63.5,-63.5},.world_extent=128.0};
  profile.background_red_depth=5U;
  profile.near_red_depth=11U;
  profile.view_distance=48.0;
  tetra::Sphere field;field.kind=profile.shape;field.terrain=profile.terrain;
  field.secondary=profile.octave_detail_amplitude;
  field.frequency=profile.octave_detail_frequency;
  tetra::Camera initial_camera;
  initial_camera.position={0.5,0.72,0.78};
  initial_camera.forward={0.0,-0.2,-1.0};
  tetra::WorldConformingClosureCache retained;
  static_cast<void>(tetra_viewer::select_world_lod_cut(
      profile,field,initial_camera,&retained));
  auto camera=initial_camera;camera.position.z-=0.10;
  tetra::WorldConformingClosureCache target_cache;
  static_cast<void>(tetra_viewer::select_world_lod_cut(
      profile,field,camera,&target_cache));
  const auto target=target_cache.requested_owners;
  auto current=retained.requested_owners;
  REQUIRE(current!=target);
  tetra::GeometryExecutor executor({.worker_count=4U,.blocks_per_worker=4U,
                                    .external_callers_may_participate=false});
  unsigned int slices{};
  while(current!=target&&slices<64U){
    current=tetra_viewer::advance_world_requested_frontier(
        current,target,profile.domain,camera,512U);
    tetra::WorldConformingClosureCache cold;
    const auto expected=tetra::close_world_conforming_cut(current,&cold);
    const auto actual=tetra::close_world_conforming_cut(
        current,&retained,{},3U,&executor);
    CAPTURE(slices);CAPTURE(current.size());
    CAPTURE(actual.size());CAPTURE(expected.size());
    CHECK(actual==expected);
    CHECK(retained.green_masks==cold.green_masks);
    ++slices;
  }
  REQUIRE(current==target);
  CHECK(slices>1U);
  CHECK(retained.closed_owners==target_cache.closed_owners);
  CHECK(retained.green_masks==target_cache.green_masks);
}

TEST_CASE("independent requested root cuts recombine to the monolithic target") {
  const auto profile=tetra_viewer::production_world_profile();
  tetra::Sphere field;field.kind=profile.shape;field.terrain=profile.terrain;
  field.secondary=profile.octave_detail_amplitude;
  field.frequency=profile.octave_detail_frequency;
  tetra::Camera camera;camera.position={0.73,0.72,0.54};
  camera.forward={0.0,-0.2,-1.0};camera.viewport_height_pixels=800.0;
  camera.aspect_ratio=1.6;
  tetra::WorldConformingClosureCache complete_cache;
  static_cast<void>(tetra_viewer::select_world_lod_cut(
      profile,field,camera,&complete_cache,{},nullptr,false));
  std::vector<tetra::WorldTetAddress> combined;
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root){
    const auto selected=tetra_viewer::select_world_requested_root_cuts(
        profile,field,camera,static_cast<std::uint16_t>(1U<<root));
    REQUIRE_FALSE(selected.owners.empty());
    CHECK(std::ranges::all_of(selected.owners,[&](const auto owner){
      return owner.root_id()==root;
    }));
    combined.insert(combined.end(),selected.owners.begin(),selected.owners.end());
  }
  CHECK(combined==complete_cache.requested_owners);
  const auto paired=tetra_viewer::select_world_requested_root_cuts(
      profile,field,camera,(1U<<2U)|(1U<<3U));
  CHECK(std::ranges::all_of(paired.owners,[](const auto owner){
    return owner.root_id()==2U||owner.root_id()==3U;
  }));
  CHECK_THROWS_AS(static_cast<void>(
      tetra_viewer::select_world_requested_root_cuts(profile,field,camera,0U)),
      std::invalid_argument);
  CHECK_THROWS_AS(static_cast<void>(
      tetra_viewer::select_world_requested_root_cuts(
          profile,field,camera,std::uint16_t{1U}<<12U)),
      std::invalid_argument);
}

TEST_CASE("root-local target fronts retain complete directory root fallbacks") {
  const auto profile=tetra_viewer::production_world_profile();
  tetra::Sphere field;field.kind=profile.shape;field.terrain=profile.terrain;
  field.secondary=profile.octave_detail_amplitude;
  field.frequency=profile.octave_detail_frequency;
  tetra::Camera camera;camera.position={0.5,0.72,0.78};
  camera.forward={0.0,-0.2,-1.0};camera.viewport_height_pixels=800.0;
  camera.aspect_ratio=1.6;
  tetra_viewer::SparseWorldSurfaceCache surface_cache;
  auto& cache=surface_cache.closure;
  const auto initial=tetra_viewer::select_world_lod_cut(
      profile,field,camera,&cache,{},nullptr,false);
  auto checkpoint=tetra::make_complete_world_cut_checkpoint(
      initial.owners,3U,1U,tetra::HierarchyResidencyTier::surface);
  tetra::WorldCutDirectory directory(std::move(checkpoint));
  auto initial_surface=tetra_viewer::build_sparse_world_derived_surface(
      directory,profile.domain,field,true,{},&surface_cache,{},true,false);
  directory.publish(directory.stage_owned_derived_surfaces(
      std::move(initial_surface.snapshots),2U));
  for(std::uint8_t step=0;step<24U;++step){
    camera.position.z-=0.01;
    const auto root=static_cast<std::uint8_t>(step%
        tetra::bcc_root_tetrahedron_count);
    const auto selected=tetra_viewer::select_world_requested_root_cuts(
        profile,field,camera,static_cast<std::uint16_t>(1U<<root));
    auto target=cache.requested_owners;
    const auto first=std::ranges::lower_bound(
        target,root,{},&tetra::WorldTetAddress::root_id);
    const auto last=std::ranges::upper_bound(
        target,root,{},&tetra::WorldTetAddress::root_id);
    const auto offset=static_cast<std::size_t>(first-target.begin());
    target.erase(first,last);
    target.insert(target.begin()+static_cast<std::ptrdiff_t>(offset),
                  selected.owners.begin(),selected.owners.end());
    const auto front=tetra_viewer::advance_world_requested_frontier(
        cache.requested_owners,target,profile.domain,camera,32U);
    static_cast<void>(tetra::close_world_conforming_cut(
        front,&cache,{},3U));
    const std::array pins{tetra_viewer::WorldVolumePin{
        camera.position,profile.near_volume_radius,
        tetra_viewer::WorldVolumePinKind::player_collision}};
    const auto residency=tetra_viewer::plan_world_residency(
        cache.closed_owners,3U,profile.domain,pins,profile.maximum_volume_blocks);
    CAPTURE(step);CAPTURE(root);CAPTURE(front.size());
    const auto hierarchy_revision=3U+static_cast<std::uint64_t>(step)*2U;
    const auto update=directory.replace_complete_cut(
        cache.dependency_blocks,cache.last_changed_mask_owners,
        residency.surface_blocks,residency.volume_blocks,hierarchy_revision);
    CHECK(update.published_revision==hierarchy_revision);
    surface_cache.closure_source_hierarchy_revision=directory.revision();
    auto surface=tetra_viewer::build_sparse_world_derived_surface(
        directory,profile.domain,field,true,{},&surface_cache,
        residency.volume_blocks,true,false,update.changed_blocks,nullptr,false);
    CHECK(surface_cache.assembled_triangles_use_optimizer_stable_ids);
    CHECK(std::ranges::all_of(surface_cache.assembled_triangles,
        [&](const auto& triangle){
      if(std::ranges::any_of(triangle.vertices,[&](const auto vertex){
           return vertex>=surface_cache.optimizer_stable_keys.size();
         }))return false;
      return true;
    }));
    directory.publish(directory.stage_owned_derived_surfaces(
        std::move(surface.snapshots),hierarchy_revision+1U));
  }
}

TEST_CASE("first person fixed steps are deterministic across frame grouping") {
  tetra::Sphere field;
  field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::FirstPersonController grouped,individual;
  tetra_viewer::FirstPersonInput input;
  input.forward=1.0;input.right=0.25;
  for(int frame=0;frame<120;++frame)
    grouped.advance(1.0/60.0,input,field);
  for(int step=0;step<240;++step)
    individual.advance(1.0/120.0,input,field);
  const auto& first=grouped.state();
  const auto& second=individual.state();
  CHECK(first.feet.x==doctest::Approx(second.feet.x).epsilon(1.0e-12));
  CHECK(first.feet.y==doctest::Approx(second.feet.y).epsilon(1.0e-12));
  CHECK(first.feet.z==doctest::Approx(second.feet.z).epsilon(1.0e-12));
  CHECK(first.velocity.x==doctest::Approx(second.velocity.x).epsilon(1.0e-12));
  CHECK(first.velocity.y==doctest::Approx(second.velocity.y).epsilon(1.0e-12));
  CHECK(first.velocity.z==doctest::Approx(second.velocity.z).epsilon(1.0e-12));
  CHECK(first.grounded==second.grounded);
}

TEST_CASE("first person modifiers select sprint control and combined speeds") {
  tetra::Sphere field;
  field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::FirstPersonConfiguration configuration;
  configuration.ground_acceleration=100'000.0;
  configuration.air_acceleration=100'000.0;
  tetra_viewer::FirstPersonController walking{configuration},
      sprinting{configuration},boosted{configuration},combined{configuration};
  for(int step=0;step<360;++step){
    walking.advance(1.0/120.0,{},field);
    sprinting.advance(1.0/120.0,{},field);
    boosted.advance(1.0/120.0,{},field);
    combined.advance(1.0/120.0,{},field);
  }
  const auto origin=walking.state().feet;
  tetra_viewer::FirstPersonInput walk;walk.forward=1.0;
  auto sprint=walk;sprint.sprint=true;
  auto boost=walk;boost.super_speed=true;
  auto control_shift=boost;control_shift.sprint=true;
  for(int step=0;step<12;++step){
    walking.advance(1.0/120.0,walk,field);
    sprinting.advance(1.0/120.0,sprint,field);
    boosted.advance(1.0/120.0,boost,field);
    combined.advance(1.0/120.0,control_shift,field);
  }
  const auto travelled=[origin](const tetra_viewer::FirstPersonController& controller){
    const auto delta=controller.state().feet-origin;
    return std::hypot(delta.x,delta.z);
  };
  CHECK(travelled(sprinting)>travelled(walking));
  CHECK(travelled(boosted)>travelled(sprinting)*4.0);
  CHECK(travelled(combined)/travelled(boosted)==doctest::Approx(10.0));
  CHECK(tetra_viewer::movement_speed_multiplier(control_shift,configuration)==
        doctest::Approx(
            10.0*tetra_viewer::movement_speed_multiplier(boost,configuration)));
}

TEST_CASE("first person collision and jump use the procedural field") {
  tetra::Sphere field;
  field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::FirstPersonController controller;
  for(int step=0;step<360;++step)
    controller.advance(1.0/120.0,{},field);
  REQUIRE(controller.state().grounded);
  const auto grounded_y=controller.state().feet.y;
  const auto grounded_x=controller.state().feet.x;
  const auto grounded_z=controller.state().feet.z;
  for(int step=0;step<600;++step)
    controller.advance(1.0/120.0,{},field);
  CHECK(controller.state().feet.x==doctest::Approx(grounded_x).epsilon(1.0e-12));
  CHECK(controller.state().feet.z==doctest::Approx(grounded_z).epsilon(1.0e-12));
  tetra_viewer::FirstPersonInput jump;
  jump.jump=true;
  controller.advance(1.0/120.0,jump,field);
  CHECK_FALSE(controller.state().grounded);
  CHECK(controller.state().velocity.y>0.0);
  for(int step=0;step<20;++step)
    controller.advance(1.0/120.0,{},field);
  CHECK(controller.state().feet.y>grounded_y);
  const auto bottom_centre=controller.state().feet+tetra::Vec3{0.0,0.025,0.0};
  CHECK(field.signed_distance(bottom_centre)>=-1.0e-10);
}

TEST_CASE("first person supports one air jump and never queues a landing jump") {
  tetra::Sphere field;
  field.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::FirstPersonController controller;
  for(int step=0;step<360;++step)
    controller.advance(1.0/120.0,{},field);
  REQUIRE(controller.state().grounded);

  tetra_viewer::FirstPersonInput jump;
  jump.jump=true;
  controller.advance(1.0/120.0,jump,field);
  controller.advance(1.0/120.0,{},field);
  REQUIRE_FALSE(controller.state().grounded);
  for(int step=0;step<12;++step)
    controller.advance(1.0/120.0,{},field);

  const double before_air_jump=controller.state().velocity.y;
  controller.advance(1.0/120.0,jump,field);
  CHECK(controller.state().velocity.y>before_air_jump);
  CHECK(controller.state().velocity.y>0.6);
  controller.advance(1.0/120.0,{},field);

  // A third distinct press is ignored. The old implementation retained this
  // press until contact and launched the character again on landing.
  for(int step=0;step<12;++step)
    controller.advance(1.0/120.0,{},field);
  const double before_rejected_jump=controller.state().velocity.y;
  controller.advance(1.0/120.0,jump,field);
  CHECK(controller.state().velocity.y<before_rejected_jump);
  controller.advance(1.0/120.0,{},field);
  for(int step=0;step<600&&!controller.state().grounded;++step)
    controller.advance(1.0/120.0,{},field);
  REQUIRE(controller.state().grounded);
  controller.advance(1.0/120.0,{},field);
  CHECK(controller.state().grounded);
  CHECK(controller.state().velocity.y==doctest::Approx(0.0));
}

TEST_CASE("first person traverses production player scale terrain") {
  const auto profile=tetra_viewer::production_world_profile();
  tetra::Sphere field;
  field.kind=profile.shape;field.terrain=profile.terrain;
  field.secondary=profile.octave_detail_amplitude;
  field.frequency=profile.octave_detail_frequency;
  tetra_viewer::FirstPersonController controller;
  for(int step=0;step<360;++step)
    controller.advance(1.0/120.0,{},field);
  REQUIRE(controller.state().grounded);
  const auto start=controller.state().feet;
  double minimum_y=start.y,maximum_y=start.y;
  std::size_t grounded_steps{};
  tetra_viewer::FirstPersonInput input;input.forward=1.0;
  for(int step=0;step<1800;++step){
    controller.advance(1.0/120.0,input,field);
    const auto& state=controller.state();
    minimum_y=std::min(minimum_y,state.feet.y);
    maximum_y=std::max(maximum_y,state.feet.y);
    grounded_steps+=state.grounded?1U:0U;
    const auto bottom=state.feet+tetra::Vec3{0.0,0.025,0.0};
    CHECK(field.signed_distance(bottom)>=-1.0e-9);
  }
  const auto end=controller.state().feet;
  CAPTURE(start.x);CAPTURE(start.y);CAPTURE(start.z);
  CAPTURE(end.x);CAPTURE(end.y);CAPTURE(end.z);
  CAPTURE(minimum_y);CAPTURE(maximum_y);CAPTURE(grounded_steps);
  CHECK(std::hypot(end.x-start.x,end.z-start.z)>5.0);
  CHECK(maximum_y-minimum_y>0.04);
  CHECK(grounded_steps>1780U);
}

TEST_CASE("first person controller bounds frame debt and rejects steep contacts") {
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::FirstPersonController clamped,reference;
  tetra_viewer::FirstPersonInput input;
  input.forward=1.0;
  clamped.advance(10.0,input,terrain);
  reference.advance(0.1,input,terrain);
  CHECK(clamped.state().feet.x==doctest::Approx(reference.state().feet.x));
  CHECK(clamped.state().feet.y==doctest::Approx(reference.state().feet.y));
  CHECK(clamped.state().feet.z==doctest::Approx(reference.state().feet.z));
  clamped.look(100000.0,-100000.0);
  const auto direction=clamped.forward();
  CHECK(std::sqrt(direction.x*direction.x+direction.y*direction.y+
                  direction.z*direction.z)==doctest::Approx(1.0));
  CHECK(std::abs(direction.y)<1.0);

  tetra::Sphere sphere;
  tetra_viewer::FirstPersonController steep;
  steep.state().feet={0.84,0.475,0.5};
  steep.advance(1.0/120.0,{},sphere);
  CHECK_FALSE(steep.state().grounded);
  CHECK(steep.state().contact_normal.x>0.9);

  tetra_viewer::FirstPersonController steep_snap;
  steep_snap.state().feet={0.89,0.475,0.5};
  steep_snap.state().grounded=true;
  const double steep_x=steep_snap.state().feet.x;
  steep_snap.advance(1.0/120.0,{},sphere);
  CHECK_FALSE(steep_snap.state().grounded);
  CHECK(steep_snap.state().feet.x==doctest::Approx(steep_x));
}

TEST_CASE("first person mouse look uses the world application's expected axes") {
  tetra_viewer::FirstPersonController horizontal,vertical;
  const auto initial=horizontal.forward();
  horizontal.look(20.0,0.0);
  vertical.look(0.0,20.0);
  CHECK(horizontal.forward().x<initial.x);
  CHECK(vertical.forward().y<initial.y);
}

TEST_CASE("captured world pointer is exclusively owned by camera look") {
  CHECK(tetra_viewer::world_ui_accepts_pointer(false));
  CHECK_FALSE(tetra_viewer::world_ui_accepts_pointer(true));
}

TEST_CASE("free fly retains the last gameplay camera for terrain LOD") {
  tetra::Camera locked;
  locked.position={1.0,2.0,3.0};
  tetra::Camera gameplay=locked;
  gameplay.position={2.0,3.0,4.0};
  auto resolved=tetra_viewer::resolve_world_lod_camera(
      gameplay,false,locked);
  CHECK(resolved.position.x==2.0);
  CHECK(locked.position.x==2.0);
  tetra::Camera free_fly=gameplay;
  free_fly.position={20.0,30.0,40.0};
  resolved=tetra_viewer::resolve_world_lod_camera(free_fly,true,locked);
  CHECK(resolved.position.x==2.0);
  CHECK(resolved.position.y==3.0);
  CHECK(resolved.position.z==4.0);
  resolved=tetra_viewer::resolve_world_lod_camera(free_fly,false,locked);
  CHECK(resolved.position.x==20.0);
  CHECK(locked.position.z==40.0);
}

TEST_CASE("implicit edge intersection preserves an endpoint already on the surface") {
  tetra::Sphere terrain;terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  const tetra::Vec3 surface{terrain.centre.x,terrain.centre.y,
                            terrain.centre.z};
  const tetra::Vec3 below=surface+tetra::Vec3{0.0,-2.0,0.0};
  const auto forward=terrain.edge_intersection(surface,below);
  const auto reverse=terrain.edge_intersection(below,surface);
  CHECK(forward.x==surface.x);CHECK(forward.y==surface.y);
  CHECK(forward.z==surface.z);
  CHECK(reverse.x==surface.x);CHECK(reverse.y==surface.y);
  CHECK(reverse.z==surface.z);
}

TEST_CASE("world headless traces are deterministic and reject bad commands") {
  std::ostringstream first,second,errors;
  const auto script="idle:180,look:25:-10,forward:120,jump:1,idle:120";
  CHECK(tetra_viewer::run_world_script(script,first,errors)==0);
  CHECK(errors.str().empty());
  CHECK(tetra_viewer::run_world_script(script,second,errors)==0);
  CHECK(first.str()==second.str());
  CHECK(first.str().find("\"event\":\"world_trace\"")!=std::string::npos);
  std::ostringstream rejected_errors,rejected_output;
  CHECK(tetra_viewer::run_world_script(
            "teleport:2",rejected_output,rejected_errors)==2);
  CHECK_FALSE(rejected_errors.str().empty());
}

TEST_CASE("world runtime capture is deterministic and produced without graphics") {
  const auto directory=std::filesystem::temp_directory_path();
  const auto first_path=directory/"tetra-world-capture-first.ppm";
  const auto second_path=directory/"tetra-world-capture-second.ppm";
  std::ostringstream first_output,second_output,errors;
  REQUIRE(tetra_viewer::capture_world_runtime(
              first_path.string(),first_output,errors)==0);
  REQUIRE(tetra_viewer::capture_world_runtime(
              second_path.string(),second_output,errors)==0);
  CHECK(errors.str().empty());
  const auto read=[](const std::filesystem::path& path){
    std::ifstream input(path,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),{});
  };
  const auto first=read(first_path),second=read(second_path);
  REQUIRE(first.starts_with("P6\n768 480\n255\n"));
  CHECK(first==second);
  CHECK(first_output.str().find("\"event\":\"world_capture\"")!=
        std::string::npos);
  std::ostringstream rejected_output,rejected_errors;
  CHECK(tetra_viewer::capture_world_runtime_view(
      first_path.string(),{1.0,2.0,3.0},{1.0,2.0,3.0},
      rejected_output,rejected_errors)==2);
  CHECK_FALSE(rejected_errors.str().empty());
  std::filesystem::remove(first_path);
  std::filesystem::remove(second_path);
}

TEST_CASE("monolithic terrain runtime publishes only complete background slices") {
  auto profile=tetra_viewer::production_world_profile();
  profile.maximum_depth=16U;
  tetra_viewer::MonolithicTerrainRuntime runtime(profile);
  tetra::Camera camera;
  camera.position={0.5,0.75,0.8};
  camera.forward={0.0,-0.25,-1.0};
  runtime.set_camera(camera,false);
  CHECK_FALSE(runtime.update());
  CHECK(runtime.diagnostics().busy);
  bool published=false;
  const auto publication_deadline=
      std::chrono::steady_clock::now()+std::chrono::seconds(20);
  while(std::chrono::steady_clock::now()<publication_deadline&&!published){
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    published=runtime.update();
  }
  REQUIRE(published);
  CHECK(runtime.diagnostics().positive_volumes);
  CHECK(runtime.diagnostics().conforming_faces);
  const auto scene_deadline=
      std::chrono::steady_clock::now()+std::chrono::seconds(20);
  while(std::chrono::steady_clock::now()<scene_deadline&&
        runtime.diagnostics().scene_generation==0U){
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    static_cast<void>(runtime.update());
  }
  const auto diagnostics=runtime.diagnostics();
  REQUIRE(diagnostics.scene_generation>0U);
  CHECK(diagnostics.hierarchy_hash!=0U);
  CHECK(diagnostics.conforming_volume_hash!=0U);
  CHECK(diagnostics.connected_surface_hash!=0U);
  CHECK(diagnostics.render_hash!=0U);
  CHECK(diagnostics.field_sample_hash!=0U);
  const auto convergence_deadline=
      std::chrono::steady_clock::now()+std::chrono::seconds(20);
  while(std::chrono::steady_clock::now()<convergence_deadline&&
        (!runtime.diagnostics().converged||runtime.diagnostics().busy)){
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    static_cast<void>(runtime.update());
  }
  REQUIRE(runtime.diagnostics().converged);
  const auto near_cells=runtime.diagnostics().logical_cells;
  camera.position={0.5,3.0,12.0};
  const auto toward=tetra::Vec3{0.5,0.5,0.5}-camera.position;
  const double toward_length=std::sqrt(toward.x*toward.x+toward.y*toward.y+
                                       toward.z*toward.z);
  camera.forward=toward/toward_length;
  runtime.set_camera(camera,false);
  const auto far_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
  do{
    static_cast<void>(runtime.update());
    if(runtime.diagnostics().converged&&!runtime.diagnostics().busy)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }while(std::chrono::steady_clock::now()<far_deadline);
  REQUIRE(runtime.diagnostics().converged);
  CHECK(runtime.diagnostics().logical_cells<near_cells);
}

TEST_CASE("blocked world runtime spans old boundaries and refines and simplifies in background") {
  auto profile=tetra_viewer::production_world_profile();
  profile.terrain.planet_radius=0.0;
  profile.domain={.world_origin={-63.5,-63.5,-63.5},.world_extent=128.0};
  profile.background_red_depth=5U;
  profile.near_red_depth=11U;
  profile.view_distance=48.0;
  profile.maximum_depth=16U;
  profile.budgets.maximum_work_units=10'000'000U;
  tetra_viewer::BlockedTerrainRuntime runtime(profile);
  const auto initial=runtime.diagnostics();
  REQUIRE(initial.converged);
  REQUIRE(initial.scene_generation>0U);
  CHECK(initial.published_camera_position.x==0.5);
  CHECK(initial.published_camera_position.y==0.72);
  CHECK(initial.published_camera_position.z==0.78);
  CHECK(initial.logical_cells>0U);
  CHECK(initial.active_tetrahedra>=initial.logical_cells);
  CHECK(initial.retained_cache_bytes>0U);
  CHECK(initial.resident_bytes<512U*1024U*1024U);
  CHECK(initial.volume_hierarchy_blocks==initial.resident_volume_blocks);
  CHECK(initial.surface_hierarchy_blocks>initial.volume_hierarchy_blocks);
  CHECK(initial.resident_volume_cells<initial.active_tetrahedra);
  CHECK(initial.resident_volume_cells==initial.conforming_cells_materialized);
  CHECK(initial.conforming_cells_materialized<initial.active_tetrahedra/10U);
  CHECK(initial.green_cells_enumerated<initial.active_tetrahedra/2U);
  CHECK(initial.surface_candidate_owners<initial.logical_cells/2U);
  CHECK(initial.surface_candidate_blocks<initial.hierarchy_blocks);
  CHECK(initial.retained_surface_certificate_bytes>0U);
  CHECK(initial.rebuilt_surface_certificates==initial.logical_cells);
  CHECK(initial.optimizer_dependency_vertices>0U);
  CHECK(initial.affected_optimizer_vertices==
        initial.optimizer_dependency_vertices);
  CHECK(initial.retained_optimizer_dependency_bytes>0U);
  CHECK(initial.closure_requested_owners_scanned>0U);
  CHECK(initial.closure_proof_nodes>initial.retained_promotion_proofs);
  CHECK(initial.retained_promotion_proofs==initial.promoted_closure_owners);
  CHECK(initial.retained_closure_proof_bytes>0U);
  CHECK(initial.retained_closure_dependency_bytes>0U);
  CHECK(initial.closure_dependency_blocks_rebuilt>0U);
  CHECK(initial.closure_dependency_blocks_reused==0U);
  CHECK(initial.closure_dependency_candidate_blocks==0U);
  CHECK(initial.closure_dependency_owners_evaluated==0U);
  CHECK(initial.closure_rounds>0U);
  CHECK(initial.changed_closure_requested_owners==
        initial.closure_requested_owners_scanned);
  CHECK(initial.updated_split_ancestors>0U);
  CHECK(initial.updated_split_ancestors<
        initial.closure_requested_owners_scanned);
  CHECK(initial.rebuilt_closure_masks==initial.logical_cells);
  CHECK(initial.closure_masks_evaluated==initial.logical_cells);
  CHECK(initial.resident_volume_blocks<=initial.maximum_volume_blocks);
  CHECK(initial.hierarchy_demand_epoch==1U);
  CHECK(initial.hierarchy_demand_records==initial.hierarchy_blocks);
  CHECK(initial.hierarchy_blocks<=initial.maximum_hierarchy_blocks);
  CHECK(initial.retained_hierarchy_demand_bytes>0U);
  CHECK(initial.visible_hierarchy_blocks>0U);
  CHECK(initial.guard_hierarchy_blocks>0U);
  CHECK(initial.cold_hierarchy_blocks>0U);
  CHECK(initial.player_hierarchy_blocks>=initial.resident_volume_blocks);
  double minimum_x=std::numeric_limits<double>::infinity();
  double maximum_x=-std::numeric_limits<double>::infinity();
  double maximum_y=-std::numeric_limits<double>::infinity();
  double maximum_field_error{};tetra::Vec3 maximum_error_point{};
  const auto& world_vertices=runtime.scene().triangle_vertices;
  REQUIRE(world_vertices.size()%3U==0U);
  std::size_t nonfinite_normals{},nonunit_normals{},shader_sentinel_faces{},
      inconsistent_face_normals{},misoriented_normals{},degenerate_faces{};
  for(std::size_t triangle=0;triangle<world_vertices.size();triangle+=3U){
    const auto& first=world_vertices[triangle];
    const auto& second=world_vertices[triangle+1U];
    const auto& third=world_vertices[triangle+2U];
    const double normal_length=std::sqrt(
        first.normal[0]*first.normal[0]+first.normal[1]*first.normal[1]+
        first.normal[2]*first.normal[2]);
    nonfinite_normals+=!std::isfinite(normal_length)?1U:0U;
    for(std::size_t corner=1U;corner<3U;++corner)
      for(std::size_t axis=0U;axis<3U;++axis)
        inconsistent_face_normals+=
            world_vertices[triangle+corner].normal[axis]!=first.normal[axis]?1U:0U;
    const tetra::Vec3 a{first.position[0],first.position[1],first.position[2]};
    const tetra::Vec3 b{second.position[0],second.position[1],second.position[2]};
    const tetra::Vec3 c{third.position[0],third.position[1],third.position[2]};
    const auto ab=b-a,ac=c-a;
    const tetra::Vec3 geometric{
        ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,
        ab.x*ac.y-ab.y*ac.x};
    const double geometric_length=std::sqrt(
        geometric.x*geometric.x+geometric.y*geometric.y+
        geometric.z*geometric.z);
    if(geometric_length<=1.0e-12){++degenerate_faces;continue;}
    nonunit_normals+=std::abs(normal_length-1.0)>1.0e-5?1U:0U;
    shader_sentinel_faces+=normal_length<=0.0001?1U:0U;
    misoriented_normals+=geometric.x*first.normal[0]+
        geometric.y*first.normal[1]+geometric.z*first.normal[2]<=0.0?1U:0U;
  }
  CAPTURE(degenerate_faces);
  CHECK(nonfinite_normals==0U);
  CHECK(nonunit_normals==0U);
  CHECK(shader_sentinel_faces==0U);
  CHECK(inconsistent_face_normals==0U);
  CHECK(misoriented_normals==0U);
  for(const auto& vertex:world_vertices){
    const tetra::Vec3 point{
        runtime.scene().render_origin.x+vertex.position[0],
        runtime.scene().render_origin.y+vertex.position[1],
        runtime.scene().render_origin.z+vertex.position[2]};
    minimum_x=std::min(minimum_x,point.x);maximum_x=std::max(maximum_x,point.x);
    maximum_y=std::max(maximum_y,point.y);
    const double error=std::abs(runtime.signed_distance(point));
    if(error>maximum_field_error){maximum_field_error=error;maximum_error_point=point;}
  }
  CAPTURE(maximum_field_error);CAPTURE(maximum_error_point.x);
  CAPTURE(maximum_error_point.y);CAPTURE(maximum_error_point.z);
  CHECK(maximum_field_error<2.0e-5);
  CHECK(minimum_x<0.0);CHECK(maximum_x>1.0);
  CHECK(maximum_y>4.0);

  tetra::Camera camera;
  camera.position={0.5,0.72,0.78};camera.forward={0.0,-0.2,-1.0};
  // The complete surface cut is direction independent, so a pure rapid turn
  // is already prepared and must not schedule expensive geometry work.
  camera.forward={0.0,-0.2,1.0};runtime.set_camera(camera,false);
  CHECK_FALSE(runtime.update());
  CHECK_FALSE(runtime.diagnostics().busy);
  CHECK(runtime.diagnostics().scene_generation==initial.scene_generation);
  camera.forward={0.0,-0.2,-1.0};runtime.set_camera(camera,false);
  const std::array interaction_pins{
      tetra_viewer::WorldVolumePin{
          {2.0,0.5,0.5},0.3,
          tetra_viewer::WorldVolumePinKind::terrain_edit},
      tetra_viewer::WorldVolumePin{
          {2.0,0.5,0.5},0.3,
          tetra_viewer::WorldVolumePinKind::physics}};
  runtime.set_volume_pins(std::vector<tetra_viewer::WorldVolumePin>(
      interaction_pins.begin(),interaction_pins.end()));
  // Each sample is below the rebuild threshold; cumulative motion must still
  // be compared with the last submitted demand, not the previous frame.
  for(std::size_t step=1U;step<=150U;++step){
    camera.position.x=0.5+0.01*static_cast<double>(step);
    runtime.set_camera(camera,true);
  }
  const auto submit_start=std::chrono::steady_clock::now();
  CHECK_FALSE(runtime.update());
  CHECK(std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-submit_start).count()<50.0);
  CHECK(runtime.diagnostics().busy);
  CHECK(runtime.diagnostics().scene_generation==initial.scene_generation);
  const auto wait_for=[&](std::chrono::seconds timeout){
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    while(std::chrono::steady_clock::now()<deadline){
      static_cast<void>(runtime.update());
      if(runtime.diagnostics().converged&&!runtime.diagnostics().busy)return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  const bool moved_converged=wait_for(std::chrono::seconds(10));
  CAPTURE(runtime.diagnostics().budget_exceeded);
  CAPTURE(runtime.diagnostics().resident_bytes);
  CAPTURE(runtime.diagnostics().cpu_high_water_bytes);
  CAPTURE(runtime.diagnostics().busy);
  REQUIRE(moved_converged);
  CHECK(runtime.diagnostics().scene_generation>initial.scene_generation);
  CHECK(runtime.diagnostics().affected_optimizer_vertices>0U);
  CHECK(runtime.diagnostics().affected_optimizer_vertices<
        runtime.diagnostics().optimizer_dependency_vertices);
  CHECK(runtime.diagnostics().changed_closure_requested_owners>0U);
  CHECK(runtime.diagnostics().changed_closure_requested_owners<
        runtime.diagnostics().closure_requested_owners_scanned);
  CHECK(runtime.diagnostics().updated_split_ancestors<
        runtime.diagnostics().changed_closure_requested_owners);
  CHECK(runtime.diagnostics().promoted_closure_owners<
        runtime.diagnostics().retained_promotion_proofs);
  CHECK(runtime.diagnostics().closure_dependency_blocks_reused>0U);
  CHECK(runtime.diagnostics().closure_dependency_blocks_rebuilt>0U);
  CHECK(runtime.diagnostics().reused_surface_blocks>0U);
  CHECK(runtime.diagnostics().positive_volumes);
  CHECK(runtime.diagnostics().conforming_faces);
  CHECK(runtime.diagnostics().promoted_volume_blocks>0U);
  CHECK(runtime.diagnostics().demoted_volume_blocks>0U);
  CHECK(runtime.diagnostics().terrain_edit_volume_blocks>0U);
  CHECK(runtime.diagnostics().physics_volume_blocks>0U);
  CHECK(runtime.diagnostics().edit_hierarchy_blocks>0U);
  CHECK(runtime.diagnostics().physics_hierarchy_blocks>0U);
  CHECK(runtime.diagnostics().predicted_hierarchy_blocks>0U);
  CHECK(runtime.diagnostics().recent_hierarchy_blocks>0U);
  CHECK(runtime.diagnostics().resident_volume_blocks<
        runtime.diagnostics().player_collision_volume_blocks+
        runtime.diagnostics().terrain_edit_volume_blocks+
        runtime.diagnostics().physics_volume_blocks);
  const auto boundary_cells=runtime.diagnostics().logical_cells;

  runtime.set_volume_pins({});
  camera.position={2.0,3.0,8.0};
  const auto target=tetra::Vec3{2.0,0.5,0.5}-camera.position;
  const double length=std::sqrt(target.x*target.x+target.y*target.y+
                                target.z*target.z);
  camera.forward=target/length;runtime.set_camera(camera,false);
  REQUIRE(wait_for(std::chrono::seconds(10)));
  CHECK(runtime.diagnostics().logical_cells<boundary_cells);
  const auto final=runtime.diagnostics();
  const auto budgets=profile.budgets;
  CHECK(final.resident_bytes<=budgets.maximum_cpu_bytes);
  CHECK(final.cpu_high_water_bytes<=budgets.maximum_cpu_bytes);
  CHECK(final.triangle_high_water<=budgets.maximum_triangles);
  CHECK(final.work_high_water<=budgets.maximum_work_units);
  CHECK(final.upload_high_water_bytes<=budgets.maximum_upload_bytes);
  CHECK(final.budget_rejected_builds==0U);
  CHECK(final.evicted_hierarchy_demand_blocks>0U);
  CHECK(final.demoted_hierarchy_demand_blocks>0U);

  camera.position={0.5,0.72,0.78};camera.forward={0.0,-0.2,-1.0};
  runtime.set_camera(camera,false);
  REQUIRE(wait_for(std::chrono::seconds(10)));
  const auto reversed=runtime.diagnostics();
  CHECK(reversed.hierarchy_hash==initial.hierarchy_hash);
  CHECK(reversed.conforming_volume_hash==initial.conforming_volume_hash);
  CHECK(reversed.connected_surface_hash==initial.connected_surface_hash);
  CHECK(reversed.render_hash==initial.render_hash);
  CHECK(reversed.recent_hierarchy_blocks>0U);
}

TEST_CASE("blocked world resource rejection preserves the complete published front") {
  auto profile=tetra_viewer::production_world_profile();
  tetra_viewer::BlockedTerrainRuntime runtime(profile);
  auto constrained=profile.budgets;constrained.maximum_upload_bytes=1U;
  runtime.set_resource_budgets(constrained);
  const auto initial=runtime.diagnostics();
  const auto initial_vertices=runtime.scene().triangle_vertices;
  tetra::Camera camera;
  camera.position={0.5,0.72,0.68};camera.forward={0.0,-0.2,-1.0};
  runtime.set_camera(camera,false);
  CHECK_FALSE(runtime.update());
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
  while(runtime.diagnostics().busy&&std::chrono::steady_clock::now()<deadline){
    static_cast<void>(runtime.update());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto rejected=runtime.diagnostics();
  REQUIRE_FALSE(rejected.busy);
  CHECK(rejected.budget_exceeded);
  CHECK(rejected.budget_rejected_builds==1U);
  CHECK(rejected.scene_generation==initial.scene_generation);
  CHECK(rejected.hierarchy_hash==initial.hierarchy_hash);
  CHECK(rejected.conforming_volume_hash==initial.conforming_volume_hash);
  CHECK(rejected.render_hash==initial.render_hash);
  CHECK(rejected.discarded_work_units>0U);
  const auto& retained=runtime.scene().triangle_vertices;
  REQUIRE(retained.size()==initial_vertices.size());
  CHECK(std::memcmp(retained.data(),initial_vertices.data(),
                    retained.size()*sizeof(tetra_viewer::SceneVertex))==0);

  runtime.set_resource_budgets(profile.budgets);
  camera.position.z=0.58;runtime.set_camera(camera,false);
  CHECK_FALSE(runtime.update());
  const auto recovery_deadline=
      std::chrono::steady_clock::now()+std::chrono::seconds(15);
  while(std::chrono::steady_clock::now()<recovery_deadline){
    static_cast<void>(runtime.update());
    if(runtime.diagnostics().converged&&!runtime.diagnostics().busy)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto recovered=runtime.diagnostics();
  REQUIRE(recovered.converged);CHECK_FALSE(recovered.busy);
  CHECK_FALSE(recovered.budget_exceeded);
  CHECK(recovered.budget_rejected_builds==1U);
  CHECK(recovered.scene_generation>initial.scene_generation);
  CHECK(recovered.hierarchy_hash!=initial.hierarchy_hash);

  const auto recovered_vertices=runtime.scene().triangle_vertices;
  runtime.set_volume_pins({tetra_viewer::WorldVolumePin{
      camera.position,profile.domain.world_extent,
      tetra_viewer::WorldVolumePinKind::terrain_edit}});
  CHECK_FALSE(runtime.update());
  const auto volume_budget_deadline=
      std::chrono::steady_clock::now()+std::chrono::seconds(10);
  while(runtime.diagnostics().busy&&
        std::chrono::steady_clock::now()<volume_budget_deadline){
    static_cast<void>(runtime.update());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto volume_rejected=runtime.diagnostics();
  REQUIRE_FALSE(volume_rejected.busy);
  CHECK(volume_rejected.budget_exceeded);
  CHECK(volume_rejected.budget_rejected_builds==2U);
  CHECK(volume_rejected.scene_generation==recovered.scene_generation);
  CHECK(volume_rejected.hierarchy_hash==recovered.hierarchy_hash);
  CHECK(volume_rejected.conforming_volume_hash==recovered.conforming_volume_hash);
  CHECK(volume_rejected.connected_surface_hash==recovered.connected_surface_hash);
  CHECK(volume_rejected.render_hash==recovered.render_hash);
  const auto& volume_retained=runtime.scene().triangle_vertices;
  REQUIRE(volume_retained.size()==recovered_vertices.size());
  CHECK(std::memcmp(volume_retained.data(),recovered_vertices.data(),
                    volume_retained.size()*sizeof(tetra_viewer::SceneVertex))==0);

  runtime.set_volume_pins({});
  runtime.set_hierarchy_block_budget(tetra::bcc_root_tetrahedron_count);
  CHECK_FALSE(runtime.update());
  const auto hierarchy_budget_deadline=
      std::chrono::steady_clock::now()+std::chrono::seconds(10);
  while(runtime.diagnostics().busy&&
        std::chrono::steady_clock::now()<hierarchy_budget_deadline){
    static_cast<void>(runtime.update());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto hierarchy_rejected=runtime.diagnostics();
  REQUIRE_FALSE(hierarchy_rejected.busy);
  CHECK(hierarchy_rejected.budget_exceeded);
  CHECK(hierarchy_rejected.budget_rejected_builds==3U);
  CHECK(hierarchy_rejected.scene_generation==recovered.scene_generation);
  CHECK(hierarchy_rejected.hierarchy_hash==recovered.hierarchy_hash);
  CHECK(hierarchy_rejected.conforming_volume_hash==
        recovered.conforming_volume_hash);
  CHECK(hierarchy_rejected.connected_surface_hash==
        recovered.connected_surface_hash);
  CHECK(hierarchy_rejected.render_hash==recovered.render_hash);

  runtime.set_hierarchy_block_budget(profile.maximum_hierarchy_blocks);
  CHECK_FALSE(runtime.update());
  const auto hierarchy_recovery_deadline=
      std::chrono::steady_clock::now()+std::chrono::seconds(15);
  while(std::chrono::steady_clock::now()<hierarchy_recovery_deadline){
    static_cast<void>(runtime.update());
    if(runtime.diagnostics().converged&&!runtime.diagnostics().busy)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto hierarchy_recovered=runtime.diagnostics();
  REQUIRE(hierarchy_recovered.converged);
  CHECK_FALSE(hierarchy_recovered.busy);
  CHECK_FALSE(hierarchy_recovered.budget_exceeded);
  CHECK(hierarchy_recovered.budget_rejected_builds==3U);
  CHECK(hierarchy_recovered.scene_generation>recovered.scene_generation);
  CHECK(hierarchy_recovered.hierarchy_hash==recovered.hierarchy_hash);
  CHECK(hierarchy_recovered.connected_surface_hash==
        recovered.connected_surface_hash);
}

TEST_CASE("production world cold construction can begin without blocking presentation") {
  const auto started=std::chrono::steady_clock::now();
  auto startup=tetra_viewer::make_production_terrain_runtime_async();
  const double submission_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-started).count();
  CHECK(submission_milliseconds<250.0);
  CHECK(startup.wait_for(std::chrono::seconds(0))!=std::future_status::deferred);
  auto runtime=startup.get();
  REQUIRE(runtime!=nullptr);
  CHECK(runtime->diagnostics().scene_generation>0U);
}

TEST_CASE("blocked world supersession cancels stale work and converges to newest pose") {
  const auto profile=tetra_viewer::production_world_profile();
  tetra_viewer::BlockedTerrainRuntime runtime(profile);
  tetra::Camera camera;
  camera.position={0.6,0.72,0.75};camera.forward={0.0,-0.2,-1.0};
  runtime.set_camera(camera,true);CHECK_FALSE(runtime.update());
  const auto canceled_before=runtime.diagnostics().canceled_builds;
  for(std::size_t step=0;step<3U;++step){
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    camera.position={0.65+0.08*static_cast<double>(step),0.72,
                     0.70-0.03*static_cast<double>(step)};
    const auto target=tetra::Vec3{camera.position.x,0.5,camera.position.z-0.3};
    const auto delta=target-camera.position;
    const auto length=std::sqrt(
        delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    camera.forward=delta/length;
    runtime.set_camera(camera,false);
    const auto cancellation_deadline=
        std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<cancellation_deadline){
      static_cast<void>(runtime.update());
      if(runtime.diagnostics().canceled_builds>=canceled_before+step+1U&&
         runtime.diagnostics().busy)break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(runtime.diagnostics().canceled_builds>=canceled_before+step+1U);
  }
  const auto wait=[&](tetra_viewer::BlockedTerrainRuntime& candidate){
    const auto deadline=
        std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(std::chrono::steady_clock::now()<deadline){
      static_cast<void>(candidate.update());
      const auto diagnostics=candidate.diagnostics();
      if(!diagnostics.busy&&diagnostics.converged)return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  REQUIRE(wait(runtime));
  const auto latest=runtime.diagnostics();
  CHECK(latest.superseded_builds>=3U);
  CHECK(latest.canceled_builds>=canceled_before+3U);
  CHECK(latest.maximum_cancellation_latency_milliseconds<2000.0);
  CHECK_FALSE(latest.budget_exceeded);

  tetra_viewer::BlockedTerrainRuntime oracle(profile);
  oracle.set_camera(camera,false);
  REQUIRE(wait(oracle));
  const auto expected=oracle.diagnostics();
  CHECK(latest.hierarchy_hash==expected.hierarchy_hash);
  CHECK(latest.conforming_volume_hash==expected.conforming_volume_hash);
  CHECK(latest.connected_surface_hash==expected.connected_surface_hash);
  CHECK(latest.render_hash==expected.render_hash);
  CHECK(latest.hierarchy_demand_epoch==expected.hierarchy_demand_epoch);
  CHECK(latest.hierarchy_demand_hash==expected.hierarchy_demand_hash);
  CHECK(latest.hierarchy_demand_records==expected.hierarchy_demand_records);
  CHECK(latest.visible_hierarchy_blocks==expected.visible_hierarchy_blocks);
  CHECK(latest.guard_hierarchy_blocks==expected.guard_hierarchy_blocks);
  CHECK(latest.predicted_hierarchy_blocks==expected.predicted_hierarchy_blocks);
  CHECK(latest.recent_hierarchy_blocks==expected.recent_hierarchy_blocks);
  CHECK(latest.cold_hierarchy_blocks==expected.cold_hierarchy_blocks);
}

TEST_CASE("blocked world publishes fronts during continuous interactive movement") {
  const auto profile=tetra_viewer::production_world_profile();
  tetra_viewer::BlockedTerrainRuntime runtime(profile);
  const auto initial=runtime.diagnostics();
  tetra::Camera camera;
  camera.position={0.60,0.72,0.75};camera.forward={0.0,-0.2,-1.0};
  runtime.set_camera(camera,true);
  CHECK_FALSE(runtime.update());
  REQUIRE(runtime.diagnostics().busy);
  const auto canceled_before=runtime.diagnostics().canceled_builds;
  const auto superseded_before=runtime.diagnostics().superseded_builds;

  bool published_while_moving=false;
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
  for(std::size_t sample=0U;std::chrono::steady_clock::now()<deadline;++sample){
    camera.position.x=sample%2U==0U?0.65:0.60;
    runtime.set_camera(camera,true);
    if(runtime.update()){
      published_while_moving=true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  CHECK(published_while_moving);
  CHECK(runtime.diagnostics().scene_generation>initial.scene_generation);
  CHECK(runtime.diagnostics().canceled_builds==canceled_before);
  CHECK(runtime.diagnostics().superseded_builds==superseded_before);
  // The just-published front is complete, while a coalesced follow-up may
  // already be running for the most recent pose.
  CHECK(runtime.diagnostics().positive_volumes);
  CHECK(runtime.diagnostics().conforming_faces);

  const auto settle=[&]{
    const auto settle_deadline=
        std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(std::chrono::steady_clock::now()<settle_deadline){
      static_cast<void>(runtime.update());
      const auto diagnostics=runtime.diagnostics();
      if(!diagnostics.busy&&diagnostics.converged)return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  REQUIRE(settle());
  const auto submitted=runtime.diagnostics().submitted_builds;
  const auto settled_generation=runtime.diagnostics().scene_generation;
  const auto settled_position=runtime.diagnostics().published_camera_position;
  for(std::size_t sample=0U;sample<32U;++sample){
    camera.position=settled_position;
    camera.position.x+=(sample%2U==0U?1.0:-1.0)*1.0e-10;
    runtime.set_camera(camera,true);
    CHECK_FALSE(runtime.update());
  }
  CHECK_FALSE(runtime.diagnostics().busy);
  CHECK(runtime.diagnostics().submitted_builds==submitted);
  CHECK(runtime.diagnostics().scene_generation==settled_generation);
  camera.position.x=runtime.diagnostics().published_camera_position.x+0.001;
  camera.position.y=runtime.diagnostics().published_camera_position.y;
  camera.position.z=runtime.diagnostics().published_camera_position.z;
  runtime.set_camera(camera,false);
  static_cast<void>(runtime.update());
  CHECK(runtime.diagnostics().submitted_builds==submitted+1U);
  REQUIRE(settle());
  CHECK(runtime.diagnostics().published_camera_position.x==camera.position.x);
  CHECK(runtime.diagnostics().published_camera_position.y==camera.position.y);
  CHECK(runtime.diagnostics().published_camera_position.z==camera.position.z);
  const auto settled_submissions=runtime.diagnostics().submitted_builds;
  const auto final_generation=runtime.diagnostics().scene_generation;
  const auto final_position=runtime.diagnostics().published_camera_position;
  for(std::size_t sample=0U;sample<32U;++sample){
    camera.position=final_position;
    camera.position.y+=(sample%2U==0U?1.0:-1.0)*1.0e-4;
    runtime.set_camera(camera,false);
    CHECK_FALSE(runtime.update());
  }
  CHECK_FALSE(runtime.diagnostics().busy);
  CHECK(runtime.diagnostics().submitted_builds==settled_submissions);
  CHECK(runtime.diagnostics().scene_generation==final_generation);
}

TEST_CASE("blocked world schedules accumulated camera rotation without translation") {
  const auto profile=tetra_viewer::production_world_profile();
  tetra_viewer::BlockedTerrainRuntime runtime(profile);
  const auto initial=runtime.diagnostics();
  tetra::Camera camera;
  camera.position=initial.published_camera_position;
  camera.viewport_height_pixels=800.0;
  camera.aspect_ratio=1.6;
  constexpr double vertical=-0.19611613513818404;
  constexpr double horizontal=0.9805806756909202;
  for(std::size_t sample=1U;sample<=100U;++sample){
    const double angle=0.001*static_cast<double>(sample);
    camera.forward={horizontal*std::sin(angle),vertical,
                    -horizontal*std::cos(angle)};
    runtime.set_camera(camera,true);
  }
  CHECK_FALSE(runtime.update());
  REQUIRE(runtime.diagnostics().busy);
  CHECK(runtime.diagnostics().submitted_builds==initial.submitted_builds+1U);

  runtime.set_camera(camera,false);
  const auto deadline=std::chrono::steady_clock::now()+
      std::chrono::seconds(60);
  while(std::chrono::steady_clock::now()<deadline){
    static_cast<void>(runtime.update());
    if(runtime.diagnostics().converged&&!runtime.diagnostics().busy)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto rotated=runtime.diagnostics();
  CAPTURE(rotated.busy);
  CAPTURE(rotated.budget_exceeded);
  CAPTURE(rotated.submitted_builds);
  CAPTURE(rotated.canceled_builds);
  CAPTURE(rotated.scene_generation);
  CAPTURE(rotated.logical_cells);
  CAPTURE(rotated.hierarchy_blocks);
  CAPTURE(rotated.maximum_hierarchy_blocks);
  CAPTURE(rotated.visible_hierarchy_blocks);
  CAPTURE(rotated.guard_hierarchy_blocks);
  CAPTURE(rotated.predicted_hierarchy_blocks);
  CAPTURE(rotated.cpu_high_water_bytes);
  CAPTURE(profile.budgets.maximum_cpu_bytes);
  CAPTURE(rotated.triangle_high_water);
  CAPTURE(profile.budgets.maximum_triangles);
  CAPTURE(rotated.work_high_water);
  CAPTURE(profile.budgets.maximum_work_units);
  CAPTURE(rotated.upload_high_water_bytes);
  CAPTURE(profile.budgets.maximum_upload_bytes);
  REQUIRE(rotated.converged);
  CHECK_FALSE(rotated.busy);
  CHECK(rotated.scene_generation>initial.scene_generation);
  CHECK(rotated.hierarchy_hash!=initial.hierarchy_hash);
  CHECK(rotated.published_camera_position.x==camera.position.x);
  CHECK(rotated.published_camera_position.y==camera.position.y);
  CHECK(rotated.published_camera_position.z==camera.position.z);
  CHECK(rotated.published_camera_forward.x==camera.forward.x);
  CHECK(rotated.published_camera_forward.y==camera.forward.y);
  CHECK(rotated.published_camera_forward.z==camera.forward.z);
  CHECK(rotated.positive_volumes);
  CHECK(rotated.conforming_faces);

  const auto settled_submissions=rotated.submitted_builds;
  const auto settled_generation=rotated.scene_generation;
  for(std::size_t sample=0U;sample<32U;++sample){
    const double noise=(sample%2U==0U?1.0:-1.0)*1.0e-8;
    camera.forward={horizontal*std::sin(0.1+noise),vertical,
                    -horizontal*std::cos(0.1+noise)};
    runtime.set_camera(camera,false);
    CHECK_FALSE(runtime.update());
  }
  CHECK_FALSE(runtime.diagnostics().busy);
  CHECK(runtime.diagnostics().submitted_builds==settled_submissions);
  CHECK(runtime.diagnostics().scene_generation==settled_generation);
}

TEST_CASE("LOD camera pose manipulation changes directional refinement visibility") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  tetra::Camera camera;
  tetra_viewer::LodCameraPose pose;
  pose.apply(camera);
  const auto leaf=mesh.active_leaves().front();
  CHECK(tetra::projected_tetrahedron_diameter(mesh,leaf,camera)>0.0);

  pose.translate(tetra_viewer::CameraGizmoAxis::x,0.25);
  pose.rotate(tetra_viewer::CameraGizmoAxis::y,std::acos(-1.0));
  pose.apply(camera);
  CHECK(camera.position.x==doctest::Approx(0.75));
  CHECK(camera.forward.z==doctest::Approx(1.0));
  CHECK(tetra::projected_tetrahedron_diameter(mesh,leaf,camera)==0.0);
}

TEST_CASE("camera projection reports size independently from exact frustum relevance") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  tetra::Camera camera;
  const auto leaf=mesh.active_leaves().front();
  const auto visible=tetra::projected_tetrahedron(mesh,leaf,camera);
  REQUIRE(visible.intersects_frustum);
  CHECK(visible.diameter_pixels>0.0);

  camera.forward={0.0,0.0,1.0};
  const auto behind=tetra::projected_tetrahedron(mesh,leaf,camera);
  CHECK_FALSE(behind.intersects_frustum);
  CHECK(behind.diameter_pixels>0.0);
  CHECK(tetra::projected_tetrahedron_diameter(mesh,leaf,camera)==0.0);
  CHECK(tetra::standby_projected_tetrahedron_diameter(
            mesh,leaf,tetra::prepare_camera_projection(camera))>0.0);
}

TEST_CASE("camera projection follows viewport field of view aspect and singular poses") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const auto leaf=mesh.active_leaves().front();
  tetra::Camera camera;
  const auto baseline=tetra::projected_tetrahedron(mesh,leaf,camera);
  REQUIRE(baseline.intersects_frustum);
  REQUIRE(std::isfinite(baseline.diameter_pixels));
  REQUIRE(baseline.diameter_pixels>0.0);

  camera.viewport_height_pixels*=0.5;
  CHECK(tetra::projected_tetrahedron(mesh,leaf,camera).diameter_pixels==
        doctest::Approx(baseline.diameter_pixels*0.5).epsilon(1.0e-12));
  camera.viewport_height_pixels*=2.0;
  camera.vertical_fov_radians*=0.5;
  CHECK(tetra::projected_tetrahedron(mesh,leaf,camera).diameter_pixels>
        baseline.diameter_pixels);

  camera=tetra::Camera{};
  camera.aspect_ratio=2.5;
  const auto wide=tetra::projected_tetrahedron(mesh,leaf,camera);
  CHECK(wide.intersects_frustum);
  CHECK(wide.diameter_pixels==
        doctest::Approx(baseline.diameter_pixels).epsilon(1.0e-12));

  camera.position={0.5,0.5,1.0}; // near plane crosses the tetrahedron
  const auto crossing=tetra::projected_tetrahedron(mesh,leaf,camera);
  CHECK(crossing.intersects_frustum);
  CHECK(std::isfinite(crossing.diameter_pixels));
  CHECK(crossing.diameter_pixels>0.0);

  camera.position={0.5,0.5,0.5}; // camera inside the cell
  const auto inside=tetra::projected_tetrahedron(mesh,leaf,camera);
  CHECK(inside.intersects_frustum);
  CHECK(std::isfinite(inside.diameter_pixels));
  CHECK(inside.diameter_pixels>0.0);
}

TEST_CASE("guard near and cold camera demands remain meaningful outside the view") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const auto leaf=mesh.active_leaves().front();
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::exact_frustum;
  configuration.camera_lod_metric=tetra::CameraLodMetric::projected_diameter;

  camera.forward={0.0,0.0,1.0};
  auto demand=tetra::camera_lod_demand(mesh,leaf,camera,configuration);
  CHECK(demand.zone==tetra::CameraLodZone::cold);
  CHECK(demand.projected_diameter_pixels==0.0);

  configuration.camera_lod_policy=tetra::CameraLodPolicy::guarded;
  demand=tetra::camera_lod_demand(mesh,leaf,camera,configuration);
  CHECK(demand.zone==tetra::CameraLodZone::cold);
  CHECK(demand.projected_diameter_pixels>0.0);
  CHECK(demand.quality_multiplier==configuration.cold_quality_multiplier);

  configuration.near_radius=4.0;
  demand=tetra::camera_lod_demand(mesh,leaf,camera,configuration);
  CHECK(demand.zone==tetra::CameraLodZone::near);
  CHECK(demand.quality_multiplier==configuration.near_quality_multiplier);

  constexpr double yaw=0.6981317007977318;
  camera.forward={std::sin(yaw),0.0,-std::cos(yaw)};
  configuration.near_radius=0.0;
  configuration.guard_frustum_scale=2.0;
  demand=tetra::camera_lod_demand(mesh,leaf,camera,configuration);
  CHECK(demand.zone==tetra::CameraLodZone::guard);
  CHECK(demand.projected_diameter_pixels>0.0);
}

TEST_CASE("prepared and batched projection preserve scalar camera semantics") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  mesh.refine_all_binary();
  const auto tetrahedra=mesh.conforming_volume().addresses();
  std::vector<double> batch(tetrahedra.size());
  std::array<tetra::Camera,4> cameras{};
  cameras[0]=tetra::Camera{};
  cameras[1]=tetra::Camera{{0.5,0.5,-1.0},0.9,1080.0,{0.0,0.0,1.0},
                           {0.0,1.0,0.0},16.0/9.0};
  cameras[2]=tetra::Camera{{0.5,0.5,0.5000000000001},0.6,720.0,
                           {1.0,0.0,0.0},{0.0,1.0,0.0},1.0};
  cameras[3]=tetra::Camera{{2.0,2.0,2.0},1.1,1440.0,
                           {-1.0,-1.0,-1.0},{0.0,1.0,0.0},2.0};
  for(const auto& camera:cameras){
    const auto prepared=tetra::prepare_camera_projection(camera);
    tetra::projected_tetrahedron_diameters(mesh,tetrahedra,prepared,batch);
    for(std::size_t index=0;index<tetrahedra.size();++index){
      const double individual=tetra::projected_tetrahedron_diameter(
          mesh,tetrahedra[index],camera);
      CHECK(batch[index]==doctest::Approx(individual).epsilon(1.0e-13));
      CHECK(std::isfinite(batch[index]));
      CHECK(batch[index]>=0.0);
    }
  }
  CHECK_THROWS_AS(tetra::projected_tetrahedron_diameters(
      mesh,tetrahedra,tetra::prepare_camera_projection(cameras[0]),
      std::span<double>{batch}.first(batch.size()-1U)),std::invalid_argument);
}

TEST_CASE("Vulkan viewport projection matches the rendered gizmo orientation") {
  constexpr double fov=0.7853981633974483;
  const auto centre=tetra_viewer::project_to_vulkan_viewport(
      {0.0,0.0,-1.0},{0.0,0.0,0.0},{0.0,0.0,-1.0},
      {1.0,0.0,0.0},{0.0,1.0,0.0},fov,800.0,600.0);
  REQUIRE(centre.visible);
  CHECK(centre.x==doctest::Approx(400.0));
  CHECK(centre.y==doctest::Approx(300.0));

  const auto positive_up=tetra_viewer::project_to_vulkan_viewport(
      {0.0,0.25,-1.0},{0.0,0.0,0.0},{0.0,0.0,-1.0},
      {1.0,0.0,0.0},{0.0,1.0,0.0},fov,800.0,600.0);
  CHECK(positive_up.y>centre.y);
  CHECK_FALSE(tetra_viewer::project_to_vulkan_viewport(
      {0.0,0.0,1.0},{0.0,0.0,0.0},{0.0,0.0,-1.0},
      {1.0,0.0,0.0},{0.0,1.0,0.0},fov,800.0,600.0).visible);
}

TEST_CASE("camera manipulator keeps a constant screen radius and shared handle geometry") {
  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,0.0};
  view.forward={0.0,0.0,-1.0};
  view.right={1.0,0.0,0.0};
  view.up={0.0,1.0,0.0};
  view.viewport_width=1200.0;
  view.viewport_height=800.0;
  for(const double depth:{0.25,1.0,12.0}){
    tetra_viewer::LodCameraPose pose;
    pose.position={0.0,0.0,-depth};
    const auto geometry=tetra_viewer::build_camera_handle_geometry(
        pose,tetra_viewer::CameraGizmoMode::translate,
        tetra_viewer::ManipulatorSpace::world,view,96.0);
    REQUIRE(geometry.world_scale>0.0);
    REQUIRE(geometry.segments.size()==3U);
    REQUIRE(geometry.triangles.size()==72U);
    REQUIRE(geometry.quads.size()==4U);
    CHECK(geometry.quads[0].filled);
    CHECK_FALSE(geometry.quads.back().filled);
    const auto pivot=tetra_viewer::project_to_vulkan_viewport(
        pose.position,view.position,view.forward,view.right,view.up,
        view.vertical_fov_radians,view.viewport_width,view.viewport_height);
    const auto x_end=tetra_viewer::project_to_vulkan_viewport(
        geometry.segments.front().second,view.position,view.forward,view.right,view.up,
        view.vertical_fov_radians,view.viewport_width,view.viewport_height);
    CHECK(std::abs(x_end.x-pivot.x)==doctest::Approx(96.0).epsilon(1.0e-12));

    const auto rotation=tetra_viewer::build_camera_handle_geometry(
        pose,tetra_viewer::CameraGizmoMode::rotate,
        tetra_viewer::ManipulatorSpace::world,view,96.0);
    REQUIRE(rotation.rings.size()==5U);
    CHECK(rotation.rings[0].radius/rotation.world_scale==doctest::Approx(0.82));
    CHECK(rotation.rings[3].handle==tetra_viewer::CameraHandle::rotate_view);
    CHECK(rotation.rings[3].radius/rotation.world_scale==doctest::Approx(1.0));
    CHECK(rotation.rings[4].handle==tetra_viewer::CameraHandle::rotate_arcball);
    CHECK(rotation.rings[4].radius/rotation.world_scale==doctest::Approx(0.09));
  }
}

TEST_CASE("camera manipulator local basis is right handed and pose repair is orthonormal") {
  tetra_viewer::LodCameraPose pose;
  pose.forward={1.0,2.0,-3.0};
  pose.up={1.0,2.0,-3.0};
  tetra_viewer::orthonormalize_camera_pose(pose);
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto length=[&](tetra::Vec3 value){return std::sqrt(dot(value,value));};
  CHECK(length(pose.forward)==doctest::Approx(1.0));
  CHECK(length(pose.up)==doctest::Approx(1.0));
  CHECK(dot(pose.forward,pose.up)==doctest::Approx(0.0).scale(1.0));
  const auto basis=tetra_viewer::manipulator_basis(
      pose,tetra_viewer::ManipulatorSpace::local);
  CHECK(length(basis.x)==doctest::Approx(1.0));
  CHECK(length(basis.y)==doctest::Approx(1.0));
  CHECK(length(basis.z)==doctest::Approx(1.0));
  CHECK(dot(basis.x,basis.y)==doctest::Approx(0.0).scale(1.0));
  CHECK(dot(basis.x,basis.z)==doctest::Approx(0.0).scale(1.0));
  CHECK(dot(basis.y,basis.z)==doctest::Approx(0.0).scale(1.0));
}

TEST_CASE("camera manipulator ray constraints remain stable away from parallel cases") {
  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,0.0};
  view.viewport_width=800.0;
  view.viewport_height=600.0;
  const auto centre=tetra_viewer::manipulator_view_ray(view,400.0,300.0);
  CHECK(centre.direction.x==doctest::Approx(0.0));
  CHECK(centre.direction.y==doctest::Approx(0.0));
  CHECK(centre.direction.z==doctest::Approx(-1.0));
  const auto plane=tetra_viewer::intersect_drag_plane(
      centre,{0.0,0.0,-3.0},{0.0,0.0,1.0});
  REQUIRE(plane.has_value());
  CHECK(plane->z==doctest::Approx(-3.0));
  const auto parameter=tetra_viewer::closest_axis_parameter(
      tetra_viewer::manipulator_view_ray(view,500.0,300.0),
      {0.0,0.0,-3.0},{1.0,0.0,0.0});
  REQUIRE(parameter.has_value());
  CHECK(*parameter>0.0);
  CHECK_FALSE(tetra_viewer::closest_axis_parameter(
      centre,{0.0,0.0,-3.0},{0.0,0.0,-1.0}).has_value());
}

TEST_CASE("camera manipulator angles arcball and hit priority are deterministic") {
  CHECK(tetra_viewer::signed_rotation_angle(
      {1.0,0.0,0.0},{0.0,1.0,0.0},{0.0,0.0,1.0})==
      doctest::Approx(std::acos(-1.0)*0.5));
  const auto centre=tetra_viewer::arcball_vector(100.0,100.0,100.0,100.0,50.0);
  CHECK(centre.x==doctest::Approx(0.0));
  CHECK(centre.y==doctest::Approx(0.0));
  CHECK(centre.z==doctest::Approx(1.0));

  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,0.0};
  view.viewport_width=800.0;
  view.viewport_height=600.0;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,0.0,-3.0};
  const auto geometry=tetra_viewer::build_camera_handle_geometry(
      pose,tetra_viewer::CameraGizmoMode::translate,
      tetra_viewer::ManipulatorSpace::world,view);
  const auto hit=tetra_viewer::hit_test_camera_handles(geometry,view,400.0,300.0);
  CHECK(hit.handle==tetra_viewer::CameraHandle::move_view);
}

TEST_CASE("camera manipulator drags are start-relative cancellable and undoable") {
  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,0.0};
  view.viewport_width=800.0;
  view.viewport_height=600.0;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,0.0,-3.0};
  tetra_viewer::CameraManipulator manipulator;
  manipulator.mode=tetra_viewer::CameraGizmoMode::translate;
  REQUIRE(manipulator.begin_drag(
      tetra_viewer::CameraHandle::move_x,pose,view,430.0,300.0));
  REQUIRE(manipulator.update_drag(pose,view,500.0,300.0));
  const auto moved=pose;
  CHECK(moved.position.x>0.0);
  CHECK(moved.position.y==doctest::Approx(0.0));
  CHECK(moved.position.z==doctest::Approx(-3.0));
  CHECK(manipulator.finish_drag(pose));
  CHECK(manipulator.can_undo());
  CHECK(manipulator.undo(pose));
  CHECK(pose.position.x==doctest::Approx(0.0));
  CHECK(manipulator.can_redo());
  CHECK(manipulator.redo(pose));
  CHECK(pose.position.x==doctest::Approx(moved.position.x));

  REQUIRE(manipulator.begin_drag(
      tetra_viewer::CameraHandle::move_view,pose,view,400.0,300.0));
  REQUIRE(manipulator.update_drag(pose,view,440.0,330.0));
  REQUIRE(manipulator.cancel_drag(pose));
  CHECK(pose.position.x==doctest::Approx(moved.position.x));
  CHECK(pose.position.y==doctest::Approx(moved.position.y));
  CHECK(pose.position.z==doctest::Approx(moved.position.z));
}

TEST_CASE("camera manipulator plane ring arcball and snapping preserve valid poses") {
  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,0.0};
  view.viewport_width=800.0;
  view.viewport_height=600.0;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,0.0,-3.0};
  tetra_viewer::CameraManipulator manipulator;
  manipulator.mode=tetra_viewer::CameraGizmoMode::translate;
  manipulator.snap.enabled=true;
  manipulator.snap.translation_step=0.25;
  REQUIRE(manipulator.begin_drag(
      tetra_viewer::CameraHandle::move_xy,pose,view,430.0,330.0));
  REQUIRE(manipulator.update_drag(pose,view,500.0,380.0));
  CHECK(std::fmod(std::abs(pose.position.x)+1.0e-12,0.25)==
        doctest::Approx(0.0).epsilon(1.0e-8));
  CHECK(std::fmod(std::abs(pose.position.y)+1.0e-12,0.25)==
        doctest::Approx(0.0).epsilon(1.0e-8));
  CHECK(manipulator.finish_drag(pose));

  tetra_viewer::LodCameraPose absolute_pose;
  absolute_pose.position={0.13,0.0,-3.0};
  tetra_viewer::CameraManipulator absolute;
  absolute.mode=tetra_viewer::CameraGizmoMode::translate;
  absolute.snap.enabled=true;
  absolute.snap.mode=tetra_viewer::ManipulatorSnapSettings::Mode::absolute;
  absolute.snap.translation_step=0.25;
  REQUIRE(absolute.begin_drag(
      tetra_viewer::CameraHandle::move_x,absolute_pose,view,430.0,300.0));
  REQUIRE(absolute.update_drag(absolute_pose,view,500.0,300.0));
  CHECK(absolute_pose.position.x/0.25==
        doctest::Approx(std::round(absolute_pose.position.x/0.25)));
  CHECK(absolute.finish_drag(absolute_pose));

  manipulator.mode=tetra_viewer::CameraGizmoMode::rotate;
  manipulator.snap.enabled=false;
  REQUIRE(manipulator.begin_drag(
      tetra_viewer::CameraHandle::rotate_view,pose,view,480.0,300.0));
  REQUIRE(manipulator.update_drag(pose,view,400.0,380.0));
  CHECK(manipulator.finish_drag(pose));
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  CHECK(dot(pose.forward,pose.up)==doctest::Approx(0.0).scale(1.0));

  REQUIRE(manipulator.begin_drag(
      tetra_viewer::CameraHandle::rotate_arcball,pose,view,400.0,300.0));
  REQUIRE(manipulator.update_drag(pose,view,450.0,330.0));
  CHECK(manipulator.finish_drag(pose));
  CHECK(dot(pose.forward,pose.up)==doctest::Approx(0.0).scale(1.0));
}

TEST_CASE("LOD camera frustum uses the exact field of view aspect and pose") {
  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,2.0};
  view.forward={0.0,0.0,-1.0};
  view.viewport_width=1200.0;
  view.viewport_height=800.0;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,0.0,-1.0};
  tetra::Camera camera;
  camera.vertical_fov_radians=std::acos(-1.0)/3.0;
  camera.aspect_ratio=2.0;
  const auto frustum=tetra_viewer::build_lod_camera_frustum(pose,camera,view);
  REQUIRE(frustum.segments.size()==9U);
  const auto first_corner=frustum.segments[0].second;
  const auto second_corner=frustum.segments[1].second;
  const auto fourth_corner=frustum.segments[3].second;
  const double width=std::abs(first_corner.x-second_corner.x);
  const double height=std::abs(first_corner.y-fourth_corner.y);
  CHECK(width/height==doctest::Approx(camera.aspect_ratio).epsilon(1.0e-12));
  const double depth=pose.position.z-first_corner.z;
  CHECK((height*0.5)/depth==
        doctest::Approx(std::tan(camera.vertical_fov_radians*0.5)).epsilon(1.0e-12));
}

TEST_CASE("LOD camera frustum retains its screen shape across editor zoom") {
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,0.0,-1.0};
  tetra::Camera camera;
  camera.aspect_ratio=1.5;
  const auto screen_shape=[&](double view_z){
    tetra_viewer::ManipulatorView view;
    view.position={0.0,0.0,view_z};
    view.forward={0.0,0.0,-1.0};
    view.viewport_width=1200.0;
    view.viewport_height=800.0;
    view.vertical_fov_radians=camera.vertical_fov_radians;
    const auto frustum=tetra_viewer::build_lod_camera_frustum(pose,camera,view);
    REQUIRE(frustum.segments.size()==9U);
    std::array<tetra_viewer::ViewportPoint,4> corners{};
    for(std::size_t corner=0;corner<corners.size();++corner)
      corners[corner]=tetra_viewer::project_to_vulkan_viewport(
          frustum.segments[corner].second,view.position,view.forward,
          view.right,view.up,view.vertical_fov_radians,
          view.viewport_width,view.viewport_height);
    return std::array<double,2>{
        std::hypot(corners[0].x-corners[1].x,
                   corners[0].y-corners[1].y),
        std::hypot(corners[0].x-corners[3].x,
                   corners[0].y-corners[3].y)};
  };
  const auto close=screen_shape(2.0);
  const auto distant=screen_shape(8.0);
  CHECK(distant[0]==doctest::Approx(close[0]).epsilon(1.0e-12));
  CHECK(distant[1]==doctest::Approx(close[1]).epsilon(1.0e-12));
}

TEST_CASE("camera manipulator survives long rotations poles and edge-on fallback") {
  tetra_viewer::LodCameraPose pose;
  for(std::size_t turn=0;turn<10000U;++turn){
    const tetra::Vec3 axis=turn%3U==0U?tetra::Vec3{1.0,0.0,0.0}:
        (turn%3U==1U?tetra::Vec3{0.0,1.0,0.0}:tetra::Vec3{0.0,0.0,1.0});
    tetra_viewer::rotate_camera_pose(pose,axis,0.013);
  }
  const auto dot=[](tetra::Vec3 a,tetra::Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
  const auto length=[&](tetra::Vec3 value){return std::sqrt(dot(value,value));};
  CHECK(length(pose.forward)==doctest::Approx(1.0).epsilon(1.0e-12));
  CHECK(length(pose.up)==doctest::Approx(1.0).epsilon(1.0e-12));
  CHECK(dot(pose.forward,pose.up)==doctest::Approx(0.0).scale(1.0));

  tetra_viewer::ManipulatorView view;
  view.position={0.0,0.0,0.0};view.viewport_width=800.0;view.viewport_height=600.0;
  pose.position={0.0,0.0,-3.0};
  tetra_viewer::CameraManipulator manipulator;
  manipulator.mode=tetra_viewer::CameraGizmoMode::rotate;
  REQUIRE(manipulator.begin_drag(
      tetra_viewer::CameraHandle::rotate_x,pose,view,400.0,300.0));
  REQUIRE(manipulator.update_drag(pose,view,450.0,350.0));
  CHECK(manipulator.finish_drag(pose));
  CHECK(length(pose.forward)==doctest::Approx(1.0));
  CHECK(dot(pose.forward,pose.up)==doctest::Approx(0.0).scale(1.0));
}

TEST_CASE("camera manipulator screen size is invariant under viewport pixel density") {
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,0.0,-4.0};
  for(const auto dimensions:{std::pair{800.0,600.0},std::pair{1600.0,1200.0},
                             std::pair{1800.0,600.0}}){
    tetra_viewer::ManipulatorView view;
    view.position={0.0,0.0,0.0};view.viewport_width=dimensions.first;
    view.viewport_height=dimensions.second;
    const auto geometry=tetra_viewer::build_camera_handle_geometry(
        pose,tetra_viewer::CameraGizmoMode::translate,
        tetra_viewer::ManipulatorSpace::world,view,96.0);
    const auto pivot=tetra_viewer::project_to_vulkan_viewport(
        pose.position,view.position,view.forward,view.right,view.up,
        view.vertical_fov_radians,view.viewport_width,view.viewport_height);
    const auto endpoint=tetra_viewer::project_to_vulkan_viewport(
        geometry.segments.front().second,view.position,view.forward,view.right,view.up,
        view.vertical_fov_radians,view.viewport_width,view.viewport_height);
    CHECK(std::hypot(endpoint.x-pivot.x,endpoint.y-pivot.y)==
          doctest::Approx(96.0).epsilon(1.0e-12));
  }
  tetra_viewer::ManipulatorView behind;
  behind.position={0.0,0.0,-5.0};
  CHECK(tetra_viewer::manipulator_world_scale(behind,pose.position,96.0)==0.0);
}

TEST_CASE("camera manipulator mode selection and ImGui capture boundaries are explicit") {
  using tetra_viewer::CameraGizmoMode;
  using tetra_viewer::CameraHandle;
  CHECK(tetra_viewer::preferred_axis_handle(CameraGizmoMode::select,0U)==
        CameraHandle::none);
  CHECK(tetra_viewer::preferred_axis_handle(CameraGizmoMode::translate,0U)==
        CameraHandle::move_x);
  CHECK(tetra_viewer::preferred_axis_handle(CameraGizmoMode::translate,2U)==
        CameraHandle::move_z);
  CHECK(tetra_viewer::preferred_axis_handle(CameraGizmoMode::rotate,1U)==
        CameraHandle::rotate_y);
  CHECK(tetra_viewer::preferred_axis_handle(CameraGizmoMode::rotate,3U)==
        CameraHandle::none);

  CHECK(tetra_viewer::manipulator_pointer_input_allowed(false,false,false));
  CHECK_FALSE(tetra_viewer::manipulator_pointer_input_allowed(true,false,false));
  CHECK_FALSE(tetra_viewer::manipulator_pointer_input_allowed(false,true,false));
  CHECK_FALSE(tetra_viewer::manipulator_pointer_input_allowed(false,false,true));
}

TEST_CASE("camera manipulator remains selected while an empty-space drag orbits the view") {
  tetra_viewer::EmptyViewportGesture gesture;

  gesture.begin(true,100.0,100.0);
  gesture.update(112.0,106.0);
  CHECK_FALSE(gesture.pending_deselect());
  CHECK_FALSE(gesture.finish_should_deselect());

  gesture.begin(true,100.0,100.0);
  gesture.update(101.0,101.0);
  CHECK(gesture.pending_deselect());
  CHECK(gesture.finish_should_deselect());

  gesture.begin(false,100.0,100.0);
  CHECK_FALSE(gesture.finish_should_deselect());
  gesture.begin(true,100.0,100.0,true);
  CHECK_FALSE(gesture.finish_should_deselect());
}

TEST_CASE("headless Maya-style camera manipulations reconcile and validate LOD") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=3,gizmo-move=local:z:1,gizmo-rotate=world:y:180,"
      "gizmo-move=world:z:-1,validate,stats",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"event\":\"command\",\"command\":\"gizmo-move=local:z:1\"")!=
        std::string::npos);
  CHECK(text.find("\"event\":\"command\",\"command\":\"gizmo-rotate=world:y:180\"")!=
        std::string::npos);
  CHECK(text.find("\"event\":\"validation\",\"valid\":true")!=std::string::npos);
  CHECK(text.find("\"lod_camera\":[0.500")!=std::string::npos);
  CHECK(text.find("\"lod_direction\":[")!=std::string::npos);
}

TEST_CASE("headless Maya-style camera commands reject malformed input") {
  for(const std::string command:{
          "gizmo-move=world:x", "gizmo-move=screen:x:1",
          "gizmo-move=world:q:1", "gizmo-rotate=local:z:nan",
          "gizmo-rotate=local:z:inf", "gizmo-rotate=local:z:1:extra"}){
    std::ostringstream output,errors;
    CHECK(tetra_viewer::run_script(command,output,errors)==2);
    CHECK(output.str().find("\"event\":\"command\"")==std::string::npos);
    CHECK(errors.str().find(
        "manipulator command requires space axis and finite amount")!=
        std::string::npos);
  }
}

TEST_CASE("variational whole-cell cut is deterministic manifold and hierarchy-owned") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.35};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,15));
  const auto revision=mesh.revision();
  const auto first=tetra::build_whole_cell_cut(mesh,sphere);
  const auto second=tetra::build_whole_cell_cut(mesh,sphere);
  REQUIRE(first.selected_cells>0);
  REQUIRE(first.boundary_faces.size()>0);
  CHECK(first.nonmanifold_boundary_edges==0);
  CHECK(first.boundary_edges*2==first.boundary_faces.size()*3);
  CHECK(first.selected_words==second.selected_words);
  CHECK(first.hash==second.hash);
  CHECK(mesh.revision()==revision);
  for(const auto& face:first.boundary_faces){
    REQUIRE(face.inside_leaf<mesh.active_leaves().size());
    CHECK(first.selected(face.inside_leaf));
    const auto& tet=mesh.tetrahedron(mesh.active_leaves()[face.inside_leaf]).vertices;
    for(const auto vertex:face.vertices)
      CHECK(std::ranges::find(tet,vertex)!=tet.end());
  }
}

TEST_CASE("variational whole-cell cut improves conservative volume bias without new geometry") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.35};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,15));
  tetra::WholeCellOptions conservative;
  conservative.method=tetra::WholeCellSelectionMethod::all_vertices_inside;
  const auto inner=tetra::build_whole_cell_cut(mesh,sphere,conservative);
  const auto optimized=tetra::build_whole_cell_cut(mesh,sphere);
  const double exact=4.0*std::acos(-1.0)*sphere.radius*sphere.radius*sphere.radius/3.0;
  CHECK(std::abs(optimized.selected_volume-exact)<std::abs(inner.selected_volume-exact));
  CHECK(optimized.nonmanifold_boundary_edges==0);
  CHECK(optimized.solve_milliseconds<100.0);
}

TEST_CASE("whole-cell cut remains manifold across hierarchy families and displaced spheres") {
  const std::array spheres{
      tetra::Sphere{{0.50,0.50,0.50},0.30},
      tetra::Sphere{{0.43,0.56,0.47},0.34},
  };
  const tetra::Camera camera{};
  for(const auto method:tetra::subdivision_methods){
    auto mesh=tetra::TetMesh::make_unit_cube(method);
    static_cast<void>(tetra::refine_to_sphere(mesh,spheres[1],camera,60.0,9));
    for(const auto& sphere:spheres){
      const auto cut=tetra::build_whole_cell_cut(mesh,sphere);
      CAPTURE(tetra::subdivision_method_key(method));
      CAPTURE(sphere.radius);
      CHECK(cut.selected_cells>0);
      CHECK(cut.boundary_faces.size()>0);
      CHECK(cut.nonmanifold_boundary_edges==0);
      CHECK(cut.boundary_components==1);
      CHECK(cut.boundary_edges*2==cut.boundary_faces.size()*3);
    }
  }
}

TEST_CASE("whole-cell target refinement follows its selected boundary and reports its limit") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  constexpr double threshold=55.0;
  const auto result=tetra::refine_to_whole_cell_surface(mesh,sphere,camera,threshold,8);
  const auto cut=tetra::build_whole_cell_cut(mesh,sphere);
  bool oversized_at_limit=false;
  for(const auto& face:cut.boundary_faces){
    const auto id=mesh.active_leaves()[face.inside_leaf];
    if(tetra::projected_tetrahedron_diameter(mesh,id,camera)>threshold){
      CHECK(mesh.refinement_depth(id)>=8);
      oversized_at_limit=true;
    }
  }
  CHECK(result.reached_depth_limit==oversized_at_limit);
  CHECK(cut.nonmanifold_boundary_edges==0);
}

TEST_CASE("whole-cell cutaway preserves the authoritative selection hash") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_whole_cell_surface(mesh,sphere,camera,40.0,9));
  const auto uncut=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::full_tetrahedra,
      tetra_viewer::MaterialRule::variational_smooth,true,false,true,false,
      false,false,2.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  const auto cutaway=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::full_tetrahedra,
      tetra_viewer::MaterialRule::variational_smooth,true,false,true,false,
      true,true,0.5,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  CHECK(uncut.whole_cell_hash!=0);
  CHECK(cutaway.whole_cell_hash==uncut.whole_cell_hash);
  CHECK(cutaway.selected_count==uncut.selected_count);
  CHECK(cutaway.whole_cell_boundary_faces==uncut.whole_cell_boundary_faces);
  CHECK(cutaway.visible_volume_face_triangles>0);
}

TEST_CASE("whole-cell cutaway retains the smooth surface and adds only cut faces") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  mesh.refine_all_binary();
  const tetra::Sphere sphere{};
  const auto uncut=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::variational_smooth,true,false,true,true,
      false,false,0.5,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  const auto scene=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::variational_smooth,true,false,true,true,
      true,true,0.5,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  REQUIRE(scene.visible_volume_face_triangles>0);
  CHECK(scene.triangle_vertices.size()==
        uncut.triangle_vertices.size()+scene.visible_volume_face_triangles*3U);
  CHECK(std::ranges::any_of(scene.triangle_vertices,[](const auto& vertex){
    return vertex.diagnostics[0]>-0.5F;
  }));

  tetra_viewer::SceneCache cache;
  REQUIRE(cache.update_scene(
      mesh,sphere,0,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::variational_smooth,true,false,true,true,
      true,true,0.5,tetra_viewer::VolumeConnectionMethod::hierarchy_cells));
  const auto& cached=cache.scene();
  REQUIRE(cached.visible_volume_face_triangles>0);
  CHECK(cached.triangle_vertices.size()==
        uncut.triangle_vertices.size()+cached.visible_volume_face_triangles*3U);
  CHECK(std::ranges::any_of(cached.triangle_vertices,[](const auto& vertex){
    return vertex.diagnostics[0]>-0.5F;
  }));
}

TEST_CASE("scripted cutaway never silently replaces the selected volume method") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-surface-method=surface-optimization,set-volume-connection=hierarchy-cells,"
      "set-solid-volume=on,set-x-cut=0.5,prepare-scene",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"volume_connection\":\"hierarchy-cells\"")!=std::string::npos);
  CHECK(text.find("\"connected_volume_tetrahedra\":0")!=std::string::npos);
}

TEST_CASE("interactive smooth cutaway preserves the selected whole hierarchy cells") {
  using tetra_viewer::SurfaceMethod;
  using tetra_viewer::VolumeConnectionMethod;
  CHECK(tetra_viewer::resolve_interactive_volume_connection(
      SurfaceMethod::surface_optimization,VolumeConnectionMethod::hierarchy_cells,true)==
      VolumeConnectionMethod::hierarchy_cells);
  CHECK(tetra_viewer::resolve_interactive_volume_connection(
      SurfaceMethod::surface_optimization,VolumeConnectionMethod::hierarchy_cells,false)==
      VolumeConnectionMethod::hierarchy_cells);
  CHECK(tetra_viewer::resolve_interactive_volume_connection(
      SurfaceMethod::full_tetrahedra,VolumeConnectionMethod::hierarchy_cells,true)==
      VolumeConnectionMethod::hierarchy_cells);
  CHECK(tetra_viewer::resolve_interactive_volume_connection(
      SurfaceMethod::surface_optimization,VolumeConnectionMethod::quality_stencils,true)==
      VolumeConnectionMethod::quality_stencils);
}

TEST_CASE("viewer defaults pair terrain with a compatible BCC volume method") {
  constexpr tetra::AdaptationConfiguration adaptation;
  CHECK(tetra_viewer::default_subdivision_method==tetra::SubdivisionMethod::bcc_red_green);
  CHECK(tetra_viewer::default_implicit_shape==tetra::ImplicitShapeKind::perlin_terrain);
  CHECK(tetra_viewer::default_volume_connection_for_shape(
      tetra_viewer::default_implicit_shape)==
      tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  CHECK(tetra::implicit_shape_default_secondary(tetra::ImplicitShapeKind::merging_spheres)==
        doctest::Approx(0.17));
  CHECK(tetra_viewer::default_surface_method==tetra_viewer::SurfaceMethod::surface_optimization);
  CHECK(tetra_viewer::default_volume_connection_method==
        tetra_viewer::VolumeConnectionMethod::fixed_surface_shell);
  CHECK(adaptation.update_scheduler==tetra::UpdateScheduler::classify_and_stream);
  CHECK(adaptation.candidate_traversal==tetra::CandidateTraversal::active_cut_scan);
  CHECK(adaptation.closure_execution==tetra::ClosureExecution::sparse_frontier);
  CHECK(adaptation.layer_storage==tetra::LayerStorage::flat_packed);
  CHECK(adaptation.adjacency==tetra::AdjacencyRepresentation::logical_face_table);
  CHECK(adaptation.kernel_order==tetra::KernelOrder::address_order);
  CHECK(tetra_viewer::default_surface_draw_chunk_strategy==
        tetra_viewer::SurfaceDrawChunkStrategy::fixed_capacity);
  CHECK(tetra_viewer::default_mesh_update_time_budget_milliseconds==
        doctest::Approx(4.0));

  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script("stats",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  const auto initialized_end=text.find('\n');
  REQUIRE(initialized_end!=std::string::npos);
  const auto initialized=text.substr(0,initialized_end);
  CHECK(initialized.find("\"subdivision_method\":\"bcc-red-green\"")!=std::string::npos);
  CHECK(initialized.find("\"surface_method\":\"surface-optimization\"")!=std::string::npos);
  CHECK(initialized.find("\"volume_connection\":\"fixed-surface-shell\"")!=std::string::npos);
  CHECK(initialized.find("\"x_cutaway\":true")!=std::string::npos);
  CHECK(initialized.find("\"x_cut_position\":1.000")!=std::string::npos);

  std::ostringstream validation_output,validation_errors;
  REQUIRE(tetra_viewer::run_script("validate-volume",validation_output,validation_errors)==0);
  CHECK(validation_errors.str().empty());
  CHECK(validation_output.str().find("\"authoritative_complex\":true")!=std::string::npos);
  CHECK(validation_output.str().find("\"graded_parent_band\":true")!=std::string::npos);
  CHECK(validation_output.str().find("\"unmatched_non_surface_faces\":0")!=std::string::npos);
}

TEST_CASE("whole hierarchy cells stay available and authoritative in optimized cutaways") {
  using tetra_viewer::SurfaceMethod;
  using tetra_viewer::VolumeConnectionMethod;
  CHECK(tetra_viewer::volume_connection_available(
      SurfaceMethod::surface_optimization,VolumeConnectionMethod::hierarchy_cells));
  for(const auto surface:tetra_viewer::surface_methods)
    for(const auto volume:tetra_viewer::volume_connection_methods)
      for(const bool solid_cutaway:{false,true})
        CHECK(tetra_viewer::resolve_interactive_volume_connection(
            surface,volume,solid_cutaway)==volume);

  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-x-cut=0.5,set-volume-connection=hierarchy-cells,prepare-scene",
      output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"surface_method\":\"surface-optimization\"")!=std::string::npos);
  CHECK(text.find("\"volume_connection\":\"hierarchy-cells\"")!=std::string::npos);
  CHECK(text.find("\"x_cutaway\":true")!=std::string::npos);
  CHECK(text.find("\"visible_volume_face_triangles\":0")==std::string::npos);
  CHECK(text.find("\"connected_volume_tetrahedra\":0")!=std::string::npos);
}

TEST_CASE("root cells use stable sentinel-prefixed path addresses") {
  const auto mesh = tetra::TetMesh::make_unit_cube();
  CHECK(mesh.layers().size() == 1);
  CHECK(mesh.layers()[0].tetrahedra.size() == 6);
  CHECK(mesh.active_leaves().size() == 6);
  for (std::uint8_t root = 0; root < 6; ++root) {
    const auto id = tetra::make_tet_id(root, 1);
    CHECK(tetra::tet_root(id) == root);
    CHECK(tetra::tet_path(id) == 1);
    CHECK(tetra::tet_depth(id) == 0);
    CHECK(mesh.tetrahedron(id).address == id);
  }
  CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("planet hierarchy addresses preserve deep BCC red paths and page prefixes") {
  auto address=tetra::WorldTetAddress::root(11U);
  std::array<std::uint8_t,tetra::maximum_world_red_depth> digits{};
  for(std::size_t depth=0;depth<digits.size();++depth){
    digits[depth]=static_cast<std::uint8_t>((depth*5U+3U)%8U);
    address=address.child(digits[depth]);
    CHECK(address.root_id()==11U);
    CHECK(address.red_depth()==depth+1U);
  }
  CHECK_THROWS_AS(static_cast<void>(address.child(0U)),std::overflow_error);
  for(std::size_t depth=digits.size();depth>0U;--depth){
    CHECK(address.red_depth()==depth);
    address=address.parent();
  }
  CHECK(address==tetra::WorldTetAddress::root(11U));
  CHECK_THROWS_AS(static_cast<void>(address.parent()),std::out_of_range);

  tetra::TetId local=tetra::make_tet_id(7U,1U);
  constexpr std::array<std::uint8_t,5> local_digits{{3U,7U,0U,6U,2U}};
  for(const auto digit:local_digits)
    local=tetra::make_tet_id(tetra::tet_root(local),(tetra::tet_path(local)<<3U)|digit);
  const auto world=tetra::world_tet_address(local);
  CHECK(world.root_id()==7U);
  CHECK(world.red_depth()==local_digits.size());
  REQUIRE(tetra::local_tet_id(world).has_value());
  CHECK(*tetra::local_tet_id(world)==local);
  const auto page=tetra::world_page_id(world,4U);
  CHECK(page.block_generations==4U);
  CHECK(page.prefix.red_depth()==4U);
  CHECK(page.prefix==world.ancestor(4U));

  auto beyond_local=world;
  while(beyond_local.red_depth()<20U)beyond_local=beyond_local.child(1U);
  CHECK_FALSE(tetra::local_tet_id(beyond_local).has_value());
}

TEST_CASE("BCC root adjacency is reciprocal oriented and shares exact face keys") {
  const auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto& connectivity=tetra::bcc_root_connectivity();
  const auto& adjacency=tetra::bcc_root_face_adjacency();
  REQUIRE(connectivity.size()==12U);
  REQUIRE(adjacency.size()==48U);
  std::size_t boundary_faces{};
  std::size_t internal_faces{};
  std::set<tetra::WorldFaceKey> boundary_keys;
  std::set<std::pair<std::uint8_t,std::uint8_t>> traversed_pairs;
  for(std::uint8_t root=0;root<connectivity.size();++root){
    const auto keys=tetra::world_tetrahedron_vertex_keys(
        mesh,tetra::WorldTetAddress::root(root));
    for(std::uint8_t face=0;face<4U;++face){
      const auto& link=tetra::bcc_root_face(root,face);
      CHECK(link.root==root);
      CHECK(link.local_face==face);
      std::array<std::uint8_t,3> corners{};
      std::size_t out{};
      for(std::uint8_t corner=0;corner<4U;++corner)
        if(corner!=face)corners[out++]=corner;
      const auto key=tetra::world_face_key(
          keys[corners[0]],keys[corners[1]],keys[corners[2]]);
      if(link.boundary()){
        ++boundary_faces;
        CHECK(link.neighbour_face==0xffU);
        CHECK(boundary_keys.insert(key).second);
        continue;
      }
      ++internal_faces;
      const auto& reverse=tetra::bcc_root_face(
          link.neighbour_root,link.neighbour_face);
      CHECK(reverse.neighbour_root==root);
      CHECK(reverse.neighbour_face==face);
      const auto neighbour_keys=tetra::world_tetrahedron_vertex_keys(
          mesh,tetra::WorldTetAddress::root(link.neighbour_root));
      CHECK(key==tetra::world_face_key(
          neighbour_keys[link.neighbour_corner_permutation[0]],
          neighbour_keys[link.neighbour_corner_permutation[1]],
          neighbour_keys[link.neighbour_corner_permutation[2]]));
      for(std::size_t index=0;index<3U;++index){
        CHECK(connectivity[root][corners[index]]==
              connectivity[link.neighbour_root][link.neighbour_corner_permutation[index]]);
        const auto reverse_index=static_cast<std::size_t>(
            link.neighbour_corner_permutation[index]>
            link.neighbour_face?link.neighbour_corner_permutation[index]-1U:
                                link.neighbour_corner_permutation[index]);
        REQUIRE(reverse_index<3U);
        CHECK(reverse.neighbour_corner_permutation[reverse_index]==corners[index]);
      }
      traversed_pairs.emplace(std::min(root,link.neighbour_root),
                              std::max(root,link.neighbour_root));
    }
  }
  CHECK(boundary_faces==12U);
  CHECK(internal_faces==36U);
  CHECK(traversed_pairs.size()==18U);
  CHECK(boundary_keys.size()==12U);
  CHECK_THROWS_AS(static_cast<void>(tetra::bcc_root_face(12U,0U)),std::out_of_range);
  CHECK_THROWS_AS(static_cast<void>(tetra::bcc_root_face(0U,4U)),std::out_of_range);
}

TEST_CASE("world dyadic keys are reduced exact and stable at maximum depth") {
  const auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  for(std::uint8_t root=0;root<12U;++root){
    auto address=tetra::WorldTetAddress::root(root);
    for(unsigned int depth=0;depth<tetra::maximum_world_red_depth;++depth)
      address=address.child(static_cast<std::uint8_t>((root+depth*3U)%8U));
    const auto keys=tetra::world_tetrahedron_vertex_keys(mesh,address);
    CHECK(keys==tetra::world_tetrahedron_vertex_keys(mesh,address));
    for(const auto& key:keys){
      CHECK(key.denominator_exponent<=tetra::maximum_world_red_depth+1U);
      if(key.denominator_exponent>0U){
        const bool reduced=(key.x&1)!=0||(key.y&1)!=0||(key.z&1)!=0;
        CHECK(reduced);
      }
    }
  }
  for(std::uint8_t root=0;root<12U;++root)
    for(std::uint8_t child=0;child<8U;++child){
      const auto address=tetra::WorldTetAddress::root(root).child(child);
      const auto keys=tetra::world_tetrahedron_vertex_keys(mesh,address);
      const auto geometry=tetra::world_tetrahedron_geometry(mesh,address);
      for(std::size_t corner=0;corner<4U;++corner){
        const auto scale=std::ldexp(1.0,-keys[corner].denominator_exponent);
        CHECK(geometry[corner].x==doctest::Approx(keys[corner].x*scale));
        CHECK(geometry[corner].y==doctest::Approx(keys[corner].y*scale));
        CHECK(geometry[corner].z==doctest::Approx(keys[corner].z*scale));
      }
    }
}

TEST_CASE("carried red child geometry matches exact address reconstruction") {
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root){
    auto address=tetra::WorldTetAddress::root(root);
    auto geometry=tetra::world_tetrahedron_geometry(address);
    for(unsigned int depth=0;depth<12U;++depth){
      const auto children=tetra::world_tetrahedron_red_children(geometry);
      for(std::uint8_t child=0;child<8U;++child){
        const auto exact=tetra::world_tetrahedron_geometry(address.child(child));
        for(std::size_t corner=0;corner<4U;++corner){
          CHECK(children[child][corner].x==exact[corner].x);
          CHECK(children[child][corner].y==exact[corner].y);
          CHECK(children[child][corner].z==exact[corner].z);
        }
      }
      const auto chosen=static_cast<std::uint8_t>((root+depth*5U)%8U);
      address=address.child(chosen);geometry=children[chosen];
    }
  }
}

TEST_CASE("global derived vertex identities ignore local orientation and allocation order") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int generation=0;generation<3U;++generation)mesh.refine_all_binary();
  const auto identities=tetra::make_world_vertex_identity_map(mesh);
  REQUIRE(identities.keys.size()==mesh.vertices().size());
  CHECK(std::ranges::all_of(identities.assigned,[](std::uint8_t value){return value!=0U;}));
  for(const auto& layer:mesh.layers())for(const auto& record:layer.tetrahedra){
    if(record.transition_parent!=tetra::invalid_tet||tetra::tet_depth(record.address)%3U!=0U)
      continue;
    const auto expected=tetra::world_tetrahedron_vertex_keys(
        tetra::world_tet_address(record.address));
    for(std::size_t corner=0;corner<4U;++corner)
      CHECK(identities.at(record.vertices[corner])==expected[corner]);
  }
  const auto first=identities.at(0U),second=identities.at(1U);
  CHECK(tetra::world_edge_intersection_key(first,second)==
        tetra::world_edge_intersection_key(second,first));
  std::array<tetra::WorldVertexKey,4> corners{{identities.at(0U),identities.at(1U),
      identities.at(2U),identities.at(3U)}};
  auto reversed=corners;std::ranges::reverse(reversed);
  CHECK(tetra::world_cell_interior_key(corners)==tetra::world_cell_interior_key(reversed));
  for(std::size_t vertex=0;vertex<mesh.vertices().size();++vertex)
    CHECK(tetra::world_vertex_key(mesh.vertices()[vertex])==identities.at(
        static_cast<tetra::VertexId>(vertex)));
}

TEST_CASE("safe warp limits merge exactly across arbitrary incident partitions") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  mesh.refine_selected_binary({mesh.logical_red_owners().front()});
  const auto identities=tetra::make_world_vertex_identity_map(mesh);
  std::vector<tetra::WorldIncidentTetrahedron> all,even,odd;
  std::size_t index{};
  for(const auto id:mesh.conforming_volume().addresses()){
    const auto& vertices=mesh.tetrahedron(id).vertices;
    tetra::WorldIncidentTetrahedron incident;
    for(std::size_t corner=0;corner<4U;++corner){
      incident.vertices[corner]=identities.at(vertices[corner]);
      incident.positions[corner]=mesh.vertices()[vertices[corner]];
    }
    all.push_back(incident);(index++%2U==0U?even:odd).push_back(incident);
  }
  const auto expected=tetra::world_safe_warp_limits(all);
  auto merged=tetra::world_safe_warp_limits(even);
  const auto second=tetra::world_safe_warp_limits(odd);
  merged.insert(merged.end(),second.begin(),second.end());
  std::ranges::sort(merged,{},&tetra::WorldSafeWarpLimit::vertex);
  std::vector<tetra::WorldSafeWarpLimit> reduced;
  for(const auto limit:merged){
    if(reduced.empty()||reduced.back().vertex!=limit.vertex)reduced.push_back(limit);
    else reduced.back().radius=std::min(reduced.back().radius,limit.radius);
  }
  REQUIRE(reduced.size()==expected.size());
  for(std::size_t limit=0;limit<expected.size();++limit){
    CHECK(reduced[limit].vertex==expected[limit].vertex);
    CHECK(reduced[limit].radius==doctest::Approx(expected[limit].radius).epsilon(1.0e-15));
  }
}

TEST_CASE("Gate 2A contracts remain distinct immutable and storage independent") {
  static_assert(!std::is_same_v<tetra::HierarchyBlockSnapshot,
                               tetra::HierarchyAddressRangeJob>);
  static_assert(!std::is_same_v<tetra::HierarchyAddressRangeJob,
                               tetra::WorldTransaction>);
  static_assert(!std::is_same_v<tetra::WorldTransaction,
                               tetra::RetainedRenderChunk>);
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  mesh.refine_all_binary();
  tetra::TetMeshHierarchyAccess access(mesh);
  const tetra::ReadOnlyHierarchyAccess& hierarchy=access;
  CHECK(hierarchy.revision()==mesh.revision());
  REQUIRE(hierarchy.logical_owner_count()==mesh.logical_red_owners().size());
  for(std::size_t index=0;index<hierarchy.logical_owner_count();++index){
    const auto address=hierarchy.logical_owner(index);
    CHECK(hierarchy.resident(address));
    CHECK(hierarchy.vertex_keys(address)==
          tetra::world_tetrahedron_vertex_keys(mesh,address));
  }
  CHECK_FALSE(hierarchy.resident(
      tetra::WorldTetAddress::root(0U).child(7U).child(7U)));

  const auto first_id=tetra::hierarchy_block_id(hierarchy.logical_owner(0U),3U);
  auto first=std::make_shared<tetra::HierarchyBlockSnapshot>();
  first->id=first_id;first->source_revision=4U;first->metrics.retained_bytes=19U;
  auto second=std::make_shared<tetra::HierarchyBlockSnapshot>();
  second->id=tetra::HierarchyBlockId{tetra::WorldTetAddress::root(11U),3U};
  second->source_revision=4U;second->metrics.retained_bytes=23U;
  tetra::WorldRevisionManifest manifest(5U,4U,{*second,*first});
  CHECK(manifest.revision()==5U);
  CHECK(manifest.parent_revision()==4U);
  REQUIRE(manifest.blocks().size()==2U);
  CHECK(manifest.blocks()[0]->id<manifest.blocks()[1]->id);
  CHECK(manifest.metrics().changed_blocks==2U);
  CHECK(manifest.metrics().affected_blocks==2U);
  CHECK(manifest.metrics().retained_bytes==42U);
  CHECK_THROWS_AS(tetra::WorldRevisionManifest(4U,4U,{}),std::invalid_argument);
  CHECK_THROWS_AS(tetra::WorldRevisionManifest(5U,4U,{*first,*first}),
                  std::invalid_argument);
}

TEST_CASE("sparse world cut exactly reproduces the monolithic oracle without a global leaf array") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int generation=0;generation<3U;++generation)
    mesh.refine_all_binary();
  std::vector<tetra::WorldTetAddress> expected;
  expected.reserve(mesh.logical_red_owners().size());
  for(const auto owner:mesh.logical_red_owners())
    expected.push_back(tetra::world_tet_address(owner));
  std::ranges::sort(expected);
  for(const unsigned int generations:{2U,3U,4U,5U}){
    const auto checkpoint=tetra::make_world_cut_checkpoint(
        mesh,generations,17U);
    tetra::WorldCutDirectory directory(checkpoint);
    std::vector<tetra::WorldTetAddress> actual;
    directory.for_each_logical_owner(
        [&](tetra::WorldTetAddress owner){actual.push_back(owner);});
    std::ranges::sort(actual);
    CAPTURE(generations);
    CHECK(actual==expected);
    CHECK(directory.logical_owner_count()==expected.size());
    CHECK(directory.metrics().effective_logical_owners==expected.size());
    CHECK(directory.metrics().stored_logical_owners>=expected.size());
    for(std::size_t index=0;index<expected.size();index+=97U){
      const auto found=directory.lookup(expected[index]);
      REQUIRE(found);
      CHECK(found.logical_owner==expected[index]);
      CHECK(directory.resident(expected[index]));
      CHECK(found.comparisons<=directory.metrics().maximum_lookup_comparisons);
    }
    auto reordered=checkpoint;
    std::ranges::reverse(reordered.blocks);
    CHECK(reordered.canonical_hash()==checkpoint.canonical_hash());
    tetra::WorldCutDirectory reloaded(std::move(reordered));
    CHECK(reloaded.canonical_cut_hash()==directory.canonical_cut_hash());
    CHECK(reloaded.checkpoint().canonical_hash()==checkpoint.canonical_hash());
  }

  std::map<tetra::WorldVertexKey,std::set<tetra::HierarchyBlockId>> incidents;
  for(const auto owner:expected)
    for(const auto key:tetra::world_tetrahedron_vertex_keys(owner))
      incidents[key].insert(tetra::hierarchy_block_id(owner,2U));
  const auto shared_across_blocks=std::ranges::count_if(
      incidents,[](const auto& entry){return entry.second.size()>1U;});
  CHECK(shared_across_blocks>0U);
}

TEST_CASE("sparse world cut reconstructs the exact conforming green volume") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int generation=0;generation<2U;++generation)
    mesh.refine_all_binary();
  // Make the cut genuinely mixed-depth so reconstruction must emit green
  // transition cells rather than merely copying uniform red owners.
  REQUIRE(mesh.refine_selected_binary({mesh.logical_red_owners().front()}));
  tetra::WorldCutDirectory directory(
      tetra::make_world_cut_checkpoint(mesh,3U,9U));
  const auto reconstructed=tetra::reconstruct_world_conforming_volume(directory);
  CHECK(reconstructed.logical_owners==mesh.logical_red_owners().size());
  CHECK(reconstructed.transition_cells>0U);

  using CellKey=std::pair<tetra::WorldTetAddress,
      std::array<tetra::WorldVertexKey,4>>;
  std::vector<CellKey> expected,actual;
  const auto identities=tetra::make_world_vertex_identity_map(mesh);
  const auto volume=mesh.conforming_volume();
  expected.reserve(volume.size());
  for(std::size_t index=0;index<volume.size();++index){
    const auto cell=volume.cell(index);
    std::array<tetra::WorldVertexKey,4> keys{};
    const auto& vertices=mesh.tetrahedron(cell.address).vertices;
    for(std::size_t corner=0;corner<4U;++corner)
      keys[corner]=identities.at(vertices[corner]);
    std::ranges::sort(keys);
    expected.emplace_back(tetra::world_tet_address(cell.logical_owner),keys);
  }
  actual.reserve(reconstructed.cells.size());
  for(const auto& cell:reconstructed.cells){
    auto keys=cell.vertices;std::ranges::sort(keys);
    actual.emplace_back(cell.logical_owner,keys);
  }
  std::ranges::sort(expected);std::ranges::sort(actual);
  CHECK(actual==expected);

  std::map<std::array<tetra::WorldVertexKey,3>,std::size_t> face_incidence;
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  for(const auto& cell:reconstructed.cells)for(const auto face:faces){
    std::array<tetra::WorldVertexKey,3> key{{cell.vertices[face[0]],
        cell.vertices[face[1]],cell.vertices[face[2]]}};
    std::ranges::sort(key);++face_incidence[key];
  }
  CHECK(std::ranges::all_of(face_incidence,
      [](const auto& entry){return entry.second==1U||entry.second==2U;}));
}

TEST_CASE("blocked conforming volume is an exact local reusable reconstruction") {
  auto make_mesh=[] {
    auto mesh=tetra::TetMesh::make_unit_cube(
        tetra::SubdivisionMethod::bcc_red_green);
    mesh.refine_all_binary();mesh.refine_all_binary();
    if(!mesh.refine_selected_binary({mesh.logical_red_owners().front()}))
      throw std::logic_error("test mesh refinement failed");
    return mesh;
  };
  const auto flatten=[](const tetra::WorldBlockedConformingVolume& blocked){
    std::vector<tetra::WorldConformingCell> cells;
    cells.reserve(blocked.cells);
    for(const auto& block:blocked.blocks)
      cells.insert(cells.end(),block->cells.begin(),block->cells.end());
    return cells;
  };
  const auto equivalent=[](std::span<const tetra::WorldConformingCell> left,
                           std::span<const tetra::WorldConformingCell> right){
    if(left.size()!=right.size())return false;
    for(std::size_t index=0;index<left.size();++index){
      auto left_keys=left[index].vertices,right_keys=right[index].vertices;
      std::ranges::sort(left_keys);std::ranges::sort(right_keys);
      if(left[index].logical_owner!=right[index].logical_owner||
         left_keys!=right_keys)return false;
    }
    return true;
  };

  auto mesh=make_mesh();
  tetra::WorldConformingClosureCache closure;
  std::vector<tetra::WorldTetAddress> initial_owners;
  for(const auto owner:mesh.logical_red_owners())
    initial_owners.push_back(tetra::world_tet_address(owner));
  const auto closed=tetra::close_world_conforming_cut(
      initial_owners,&closure);
  tetra::WorldCutDirectory directory(tetra::make_complete_world_cut_checkpoint(
      closed,2U,1U,tetra::HierarchyResidencyTier::conforming_volume));
  const auto cold=tetra::reconstruct_world_conforming_volume(directory,&closure);
  const auto blocked=tetra::reconstruct_blocked_world_conforming_volume(
      directory,closure);
  const auto blocked_cells=flatten(blocked);
  CHECK(equivalent(blocked_cells,cold.cells));
  CHECK(blocked.rebuilt_blocks==blocked.blocks.size());
  CHECK(blocked.reused_blocks==0U);
  CHECK(blocked.retained_bytes<
        blocked.cells*sizeof(tetra::WorldConformingCell)*2U);

  const auto repeated=tetra::reconstruct_blocked_world_conforming_volume(
      directory,closure,&blocked);
  REQUIRE(repeated.blocks.size()==blocked.blocks.size());
  CHECK(repeated.reused_blocks==blocked.blocks.size());
  CHECK(repeated.rebuilt_blocks==0U);
  for(std::size_t index=0;index<blocked.blocks.size();++index)
    CHECK(repeated.blocks[index].get()==blocked.blocks[index].get());

  std::vector<tetra::HierarchyBlockId> materialized{
      blocked.blocks.front()->id,blocked.blocks.back()->id};
  std::ranges::sort(materialized);
  materialized.erase(std::unique(materialized.begin(),materialized.end()),
                     materialized.end());
  const auto filtered=tetra::reconstruct_blocked_world_conforming_volume(
      directory,closure,&blocked,materialized,true);
  CHECK(filtered.blocks.size()==materialized.size());
  CHECK(filtered.cells==blocked.cells);
  CHECK(filtered.logical_owners==blocked.logical_owners);
  CHECK(filtered.canonical_hash==blocked.canonical_hash);
  CHECK(filtered.retained_bytes<blocked.retained_bytes);
  for(const auto& block:filtered.blocks){
    const auto previous=std::ranges::lower_bound(
        blocked.blocks,block->id,{},[](const auto& candidate){return candidate->id;});
    REQUIRE(previous!=blocked.blocks.end());
    CHECK(previous->get()==block.get());
  }
  // The production three-generation surface-only path must not sort every
  // closed owner merely to retain a small volume subset. It uses the closure
  // dependency runs for the requested blocks while preserving exact global
  // cell summaries and byte-identical retained blocks.
  tetra::WorldCutDirectory directory3(tetra::make_complete_world_cut_checkpoint(
      closed,3U,1U,tetra::HierarchyResidencyTier::conforming_volume));
  const auto blocked3=tetra::reconstruct_blocked_world_conforming_volume(
      directory3,closure);
  std::vector<tetra::HierarchyBlockId> materialized3{
      blocked3.blocks.front()->id,blocked3.blocks.back()->id};
  std::ranges::sort(materialized3);
  materialized3.erase(std::unique(materialized3.begin(),materialized3.end()),
                      materialized3.end());
  const auto restricted3=tetra::reconstruct_blocked_world_conforming_volume(
      directory3,closure,&blocked3,materialized3,true,false);
  CHECK(restricted3.canonical_hash==0U);
  CHECK(restricted3.cells==blocked3.cells);
  CHECK(restricted3.transition_cells==blocked3.transition_cells);
  CHECK(restricted3.logical_owners==blocked3.logical_owners);
  CHECK(restricted3.blocks.size()==materialized3.size());
  CHECK(restricted3.owners_considered<restricted3.logical_owners);
  std::size_t expected_materialized_cells{};
  for(const auto id:materialized3){
    const auto found=std::ranges::lower_bound(
        blocked3.blocks,id,{},[](const auto& candidate){return candidate->id;});
    REQUIRE(found!=blocked3.blocks.end());
    REQUIRE((*found)->id==id);
    expected_materialized_cells+=(*found)->cells.size();
  }
  CHECK(restricted3.materialized_cells==expected_materialized_cells);
  const std::span<const tetra::HierarchyBlockId> no_blocks;
  const auto surface_only=tetra::reconstruct_blocked_world_conforming_volume(
      directory,closure,&filtered,no_blocks,true);
  CHECK(surface_only.blocks.empty());
  CHECK(surface_only.cells==blocked.cells);
  CHECK(surface_only.canonical_hash==blocked.canonical_hash);
  CHECK(surface_only.retained_bytes==0U);

  const auto selected=mesh.logical_red_owners().back();
  REQUIRE(mesh.refine_selected_binary({selected}));
  std::vector<tetra::WorldTetAddress> moved_owners;
  for(const auto owner:mesh.logical_red_owners())
    moved_owners.push_back(tetra::world_tet_address(owner));
  const auto moved_closed=tetra::close_world_conforming_cut(
      moved_owners,&closure);
  tetra::WorldCutDirectory moved_directory(
      tetra::make_complete_world_cut_checkpoint(
          moved_closed,2U,2U,tetra::HierarchyResidencyTier::conforming_volume));
  const auto moved=tetra::reconstruct_blocked_world_conforming_volume(
      moved_directory,closure,&repeated);
  CHECK(moved.reused_blocks>0U);
  CHECK(moved.rebuilt_blocks>0U);
  CHECK(moved.rebuilt_blocks<moved.blocks.size());
  const auto moved_flat=tetra::reconstruct_world_conforming_volume(
      moved_directory,&closure);
  CHECK(equivalent(flatten(moved),moved_flat.cells));

  auto forged=repeated;
  bool forged_one{};
  for(std::size_t moved_index=0;moved_index<moved.blocks.size()&&!forged_one;
      ++moved_index){
    const auto old=std::ranges::lower_bound(
        forged.blocks,moved.blocks[moved_index]->id,{},
        [](const auto& block){return block->id;});
    if(old==forged.blocks.end()||(*old)->id!=moved.blocks[moved_index]->id||
       old->get()==moved.blocks[moved_index].get())continue;
    auto collision=std::make_shared<tetra::WorldConformingBlockSnapshot>(**old);
    collision->owner_mask_hash=moved.blocks[moved_index]->owner_mask_hash;
    *old=std::move(collision);forged_one=true;
  }
  REQUIRE(forged_one);
  const auto collision_checked=
      tetra::reconstruct_blocked_world_conforming_volume(
          moved_directory,closure,&forged);
  CHECK(collision_checked.rebuilt_blocks>0U);
  CHECK(equivalent(flatten(collision_checked),moved_flat.cells));

  // Returning to an earlier cut is an eviction/regeneration oracle: blocks
  // whose owner/mask signature survived are retained, and every regenerated
  // block remains byte-for-byte equivalent to a cold reconstruction.
  auto restored_mesh=make_mesh();
  std::vector<tetra::WorldTetAddress> restored_owners;
  for(const auto owner:restored_mesh.logical_red_owners())
    restored_owners.push_back(tetra::world_tet_address(owner));
  const auto restored_closed=tetra::close_world_conforming_cut(
      restored_owners,&closure);
  tetra::WorldCutDirectory restored_directory(
      tetra::make_complete_world_cut_checkpoint(
          restored_closed,2U,3U,tetra::HierarchyResidencyTier::conforming_volume));
  const auto restored=tetra::reconstruct_blocked_world_conforming_volume(
      restored_directory,closure,&moved);
  CHECK(restored.reused_blocks>0U);
  CHECK(restored.rebuilt_blocks>0U);
  CHECK(equivalent(flatten(restored),
      tetra::reconstruct_world_conforming_volume(
          restored_directory,&closure).cells));
}

TEST_CASE("direct sparse cut closure conservatively satisfies transactional grading") {
  std::vector<tetra::WorldTetAddress> raw;
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
    raw.push_back(tetra::WorldTetAddress::root(root));
  auto split_raw=[&](tetra::WorldTetAddress owner){
    const auto found=std::ranges::find(raw,owner);REQUIRE(found!=raw.end());
    raw.erase(found);
    for(std::uint8_t child=0;child<8U;++child)raw.push_back(owner.child(child));
    std::ranges::sort(raw);
  };

  tetra::WorldCutDirectory staged(tetra::make_sparse_world_cut_checkpoint(
      raw,3U,1U,tetra::HierarchyResidencyTier::conforming_volume));
  auto target=tetra::WorldTetAddress::root(0U);
  for(std::uint64_t revision=2U;revision<=4U;++revision){
    split_raw(target);
    const std::array edit{tetra::WorldTopologyEdit{
        target,tetra::WorldTopologyOperation::split}};
    const auto transaction=staged.stage_transaction(edit,revision);
    staged.publish(transaction.manifest);
    target=target.child(0U);
  }
  const auto closed=tetra::close_world_conforming_cut(raw);
  std::vector<tetra::WorldTetAddress> expected;
  staged.for_each_logical_owner(
      [&](tetra::WorldTetAddress owner){expected.push_back(owner);});
  std::ranges::sort(expected);
  CHECK(closed.size()>=expected.size());
  std::map<tetra::WorldVertexKey,std::pair<unsigned int,unsigned int>> depths;
  for(const auto owner:closed)for(const auto key:tetra::world_tetrahedron_vertex_keys(owner)){
    auto [found,inserted]=depths.emplace(key,std::pair{owner.red_depth(),owner.red_depth()});
    if(!inserted){found->second.first=std::min(found->second.first,owner.red_depth());
      found->second.second=std::max(found->second.second,owner.red_depth());}
  }
  CHECK(std::ranges::all_of(depths,[](const auto& entry){
    return entry.second.second<=entry.second.first+1U;
  }));
  tetra::WorldCutDirectory direct(tetra::make_sparse_world_cut_checkpoint(
      closed,3U,1U,tetra::HierarchyResidencyTier::conforming_volume));
  CHECK_FALSE(tetra::reconstruct_world_conforming_volume(direct).cells.empty());
}

TEST_CASE("retained conformity geometry cache is exact reusable and bounded") {
  std::vector<tetra::WorldTetAddress> raw;
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
    raw.push_back(tetra::WorldTetAddress::root(root));
  const auto split=[&](tetra::WorldTetAddress owner){
    const auto found=std::ranges::find(raw,owner);REQUIRE(found!=raw.end());
    raw.erase(found);
    for(std::uint8_t child=0;child<8U;++child)raw.push_back(owner.child(child));
    std::ranges::sort(raw);
  };
  split(tetra::WorldTetAddress::root(0U));
  split(tetra::WorldTetAddress::root(0U).child(0U));
  split(tetra::WorldTetAddress::root(0U).child(0U).child(0U));
  const auto cold=tetra::close_world_conforming_cut(raw);
  tetra::WorldConformingClosureCache cache;
  cache.maximum_entries=128U;
  const auto first=tetra::close_world_conforming_cut(raw,&cache);
  const auto populated=cache.geometry.size();
  CHECK(first==cold);
  CHECK(populated>0U);
  CHECK(populated<=cache.maximum_entries);
  CHECK_FALSE(cache.proof_nodes.empty());
  CHECK(cache.promotion_proofs.size()==cache.last_promoted_owners);
  for(std::size_t proof=0;proof<cache.proof_nodes.size();++proof){
    const auto& node=cache.proof_nodes[proof];
    CHECK(node.input_count<=node.inputs.size());
    for(std::size_t input=0;input<node.input_count;++input)
      CHECK(node.inputs[input]<proof);
  }
  CHECK(std::ranges::is_sorted(cache.promotion_proofs));
  for(const auto& promotion:cache.promotion_proofs){
    REQUIRE(promotion.proof<cache.proof_nodes.size());
    const auto& proof=cache.proof_nodes[promotion.proof];
    CHECK(proof.address==promotion.address);
    CHECK((proof.kind==tetra::WorldClosureProofKind::vertex_promotion||
           proof.kind==tetra::WorldClosureProofKind::mask_promotion));
  }
  std::set<tetra::WorldEdgeKey> retained_active_edges;
  for(const auto& proof:cache.proof_nodes)
    if(proof.kind==tetra::WorldClosureProofKind::split_ancestor_edge||
       proof.kind==tetra::WorldClosureProofKind::green_edge||
       proof.kind==tetra::WorldClosureProofKind::promotion_edge)
      retained_active_edges.insert(proof.edge);
  constexpr std::array<std::array<std::size_t,2>,6> owner_edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  REQUIRE(cache.closed_owners.size()==cache.green_masks.size());
  for(std::size_t owner=0;owner<cache.closed_owners.size();++owner){
    const auto keys=tetra::world_tetrahedron_vertex_keys(
        cache.closed_owners[owner]);
    for(std::size_t edge=0;edge<owner_edges.size();++edge)
      CHECK(retained_active_edges.contains(tetra::world_edge_key(
                keys[owner_edges[edge][0]],keys[owner_edges[edge][1]]))==
            ((cache.green_masks[owner]&(1U<<edge))!=0U));
  }
  CHECK_FALSE(cache.requested_split_ancestors.empty());
  CHECK(std::ranges::is_sorted(cache.requested_split_ancestors,{},
      &tetra::WorldConformingSplitAncestor::address));
  CHECK(cache.split_edges_complete);
  CHECK_FALSE(cache.split_edges.empty());
  for(std::size_t edge=0;edge<cache.split_edges.size();++edge){
    const auto& entry=cache.split_edges[edge];
    CHECK(entry.ancestor_supports()>0U);
    REQUIRE(entry.proof()<cache.proof_nodes.size());
    const auto& proof=cache.proof_nodes[entry.proof()];
    CHECK((proof.kind==tetra::WorldClosureProofKind::split_ancestor_edge||
           proof.kind==tetra::WorldClosureProofKind::green_edge||
           proof.kind==tetra::WorldClosureProofKind::promotion_edge));
    if(edge>0U)CHECK(cache.proof_nodes[cache.split_edges[edge-1U].proof()].edge<
                     proof.edge);
  }
  CHECK_FALSE(cache.dependency_blocks.empty());
  CHECK(std::ranges::is_sorted(cache.dependency_blocks,{},
      [](const auto& block){return block->id;}));
  CHECK(std::ranges::is_sorted(cache.vertex_block_records));
  CHECK(cache.last_dependency_blocks_rebuilt==cache.dependency_blocks.size());
  CHECK(cache.last_dependency_blocks_reused==0U);
  CHECK(cache.last_dependency_retained_bytes>0U);
  CHECK_FALSE(cache.vertex_depths.empty());
  CHECK(std::ranges::is_sorted(cache.vertex_depths,{},
      &tetra::WorldClosureVertexDepth::key));
  for(const auto& entry:cache.vertex_depths){
    CHECK(std::ranges::binary_search(cache.closed_owners,entry.owner));
    CHECK(entry.owner.red_depth()==entry.depth);
    const auto keys=tetra::world_tetrahedron_vertex_keys(entry.owner);
    CHECK(std::ranges::find(keys,entry.key)!=keys.end());
  }
  const auto repeated=tetra::close_world_conforming_cut(raw,&cache);
  CHECK(repeated==cold);
  CHECK(cache.geometry.size()==populated);
  CHECK(cache.last_requested_owners_scanned==raw.size());
  CHECK(cache.last_reused_masks==cache.green_masks.size());
  CHECK(cache.last_rebuilt_masks==0U);
  CHECK(cache.last_changed_requested_owners==0U);
  CHECK(cache.last_split_ancestor_updates==0U);
  CHECK(cache.last_dependency_blocks_reused==cache.dependency_blocks.size());
  CHECK(cache.last_dependency_blocks_rebuilt==0U);

  std::stop_source canceled;canceled.request_stop();
  const auto cached_owners=cache.closed_owners;
  const auto cached_ancestors=cache.requested_split_ancestors;
  const auto cached_split_edges=cache.split_edges;
  const auto cached_proofs=cache.proof_nodes;
  const auto cached_promotions=cache.promotion_proofs;
  const auto cached_dependency_blocks=cache.dependency_blocks;
  const auto cached_vertex_records=cache.vertex_block_records;
  const auto cached_free_dependency_ids=cache.free_dependency_block_ids;
  const auto cached_next_dependency_id=cache.next_dependency_block_id;
  const auto cached_vertex_depths=cache.vertex_depths;
  CHECK_THROWS_AS(static_cast<void>(tetra::close_world_conforming_cut(
      raw,&cache,canceled.get_token())),std::runtime_error);
  CHECK(cache.closed_owners==cached_owners);
  CHECK(cache.requested_split_ancestors==cached_ancestors);
  CHECK(cache.split_edges==cached_split_edges);
  CHECK(cache.proof_nodes==cached_proofs);
  CHECK(cache.promotion_proofs==cached_promotions);
  CHECK(cache.dependency_blocks==cached_dependency_blocks);
  CHECK(cache.vertex_block_records==cached_vertex_records);
  CHECK(cache.free_dependency_block_ids==cached_free_dependency_ids);
  CHECK(cache.next_dependency_block_id==cached_next_dependency_id);
  CHECK(cache.vertex_depths==cached_vertex_depths);

  split(tetra::WorldTetAddress::root(1U));
  const auto moved=tetra::close_world_conforming_cut(raw,&cache);
  CHECK(moved==tetra::close_world_conforming_cut(raw));
  CHECK(cache.requested_owners==raw);
  CHECK(cache.last_changed_requested_owners==9U);
  CHECK(cache.last_split_ancestor_updates==1U);
  CHECK(cache.last_reused_masks==cache.green_masks.size());
  CHECK(cache.last_rebuilt_masks==0U);
  CHECK(cache.geometry.size()<=cache.maximum_entries);
  CHECK(cache.last_dependency_blocks_reused>0U);
  CHECK(cache.last_dependency_blocks_reused+
        cache.last_dependency_blocks_rebuilt==cache.dependency_blocks.size());

  tetra::WorldConformingClosureCache collision_cache;
  collision_cache.dependency_fingerprint_bits=1U;
  CHECK(tetra::close_world_conforming_cut(raw,&collision_cache)==
        tetra::close_world_conforming_cut(raw));
  CHECK_FALSE(collision_cache.vertex_block_records.empty());
  CHECK(std::ranges::all_of(collision_cache.vertex_block_records,
      [](std::uint64_t record){return (record>>32U)<=1U;}));
  CHECK(std::ranges::adjacent_find(collision_cache.vertex_block_records,
      [](std::uint64_t first,std::uint64_t second){
        return (first>>32U)==(second>>32U);
      })!=collision_cache.vertex_block_records.end());
  split(tetra::WorldTetAddress::root(2U));
  const auto collision_warm=tetra::close_world_conforming_cut(
      raw,&collision_cache);
  tetra::WorldConformingClosureCache collision_cold;
  collision_cold.dependency_fingerprint_bits=1U;
  CHECK(collision_warm==tetra::close_world_conforming_cut(
      raw,&collision_cold));
  CHECK(collision_cache.green_masks==collision_cold.green_masks);
  CHECK(collision_cache.last_dependency_candidate_blocks>0U);
  CHECK(collision_cache.last_dependency_owners_evaluated>0U);
  collision_cache.dependency_fingerprint_bits=0U;
  CHECK_THROWS_AS(static_cast<void>(tetra::close_world_conforming_cut(
      raw,&collision_cache)),std::invalid_argument);
}

TEST_CASE("causal world closure proofs survive alternating refinement and coarsening") {
  std::vector<tetra::WorldTetAddress> requested;
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
    requested.push_back(tetra::WorldTetAddress::root(root));
  tetra::WorldConformingClosureCache retained;
  bool observed_sparse_dependency_query=false;
  for(unsigned int step=0;step<24U;++step){
    if(step<14U||step%3U!=0U){
      for(unsigned int edit=0;edit<4U;++edit){
        const auto candidate=static_cast<std::size_t>(
            (step*37U+edit*53U+5U)%requested.size());
        const auto owner=requested[candidate];
        if(owner.red_depth()<5U){
          requested.erase(
              requested.begin()+static_cast<std::ptrdiff_t>(candidate));
          for(std::uint8_t child=0;child<8U;++child)
            requested.push_back(owner.child(child));
        }
      }
    }else{
      std::optional<tetra::WorldTetAddress> parent;
      for(const auto owner:requested){
        if(owner.red_depth()==0U)continue;
        const auto candidate=owner.parent();
        bool complete=true;
        for(std::uint8_t child=0;child<8U;++child)
          complete&=std::ranges::find(requested,candidate.child(child))!=
              requested.end();
        if(complete){parent=candidate;break;}
      }
      if(parent){
        for(std::uint8_t child=0;child<8U;++child)
          std::erase(requested,parent->child(child));
        requested.push_back(*parent);
      }
    }
    std::ranges::sort(requested);
    tetra::WorldConformingClosureCache cold;
    const auto expected=tetra::close_world_conforming_cut(requested,&cold);
    const auto previous_owners=retained.closed_owners;
    const auto previous_masks=retained.green_masks;
    const auto actual=tetra::close_world_conforming_cut(requested,&retained);
    observed_sparse_dependency_query|=
        retained.last_dependency_owners_evaluated>0U;
    CHECK(actual==expected);
    CHECK(retained.green_masks==cold.green_masks);
    CHECK(retained.vertex_depths==cold.vertex_depths);
    CHECK(retained.last_masks_evaluated<=retained.closed_owners.size());
    CHECK(retained.last_promoted_owners<=retained.promotion_proofs.size());
    std::vector<tetra::WorldTetAddress> expected_changed_owners;
    std::vector<tetra::HierarchyBlockId> expected_changed_blocks;
    std::size_t previous{},current{};
    while(previous<previous_owners.size()||current<actual.size()){
      if(previous==previous_owners.size()||
         (current<actual.size()&&actual[current]<previous_owners[previous])){
        expected_changed_owners.push_back(actual[current]);
        expected_changed_blocks.push_back(tetra::hierarchy_block_id(
            actual[current],3U));
        ++current;continue;
      }
      if(current==actual.size()||previous_owners[previous]<actual[current]){
        expected_changed_blocks.push_back(tetra::hierarchy_block_id(
            previous_owners[previous],3U));
        ++previous;continue;
      }
      if(previous>=previous_masks.size()||
         previous_masks[previous]!=retained.green_masks[current]){
        expected_changed_owners.push_back(actual[current]);
        expected_changed_blocks.push_back(tetra::hierarchy_block_id(
            actual[current],3U));
      }
      ++previous;++current;
    }
    std::ranges::sort(expected_changed_blocks);
    expected_changed_blocks.erase(std::unique(expected_changed_blocks.begin(),
        expected_changed_blocks.end()),expected_changed_blocks.end());
    CHECK(retained.last_changed_mask_owners==expected_changed_owners);
    CHECK(retained.last_changed_mask_blocks==expected_changed_blocks);
    for(std::size_t proof=0;proof<retained.proof_nodes.size();++proof)
      for(std::size_t input=0;
          input<retained.proof_nodes[proof].input_count;++input)
        CHECK(retained.proof_nodes[proof].inputs[input]<proof);
  }
  CHECK(observed_sparse_dependency_query);
}

TEST_CASE("native sparse world surface is watertight and publishable without a mesh") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int generation=0;generation<3U;++generation)
    mesh.refine_all_binary();
  tetra::WorldCutDirectory directory(
      tetra::make_world_cut_checkpoint(mesh,3U,21U));
  const tetra::Sphere sphere;
  const tetra::WorldStreamingDemand::Domain domain{};
  tetra_viewer::SparseWorldSurfaceCache cache;
  const auto surface=tetra_viewer::build_sparse_world_derived_surface(
      directory,domain,sphere,false,{},&cache);
  REQUIRE_FALSE(surface.vertices.empty());
  CHECK(surface.triangles.size()==tetra::extract_isosurface(mesh,sphere).size());
  CHECK(surface.metrics.surface_blocks==surface.snapshots.size());
  for(const auto& vertex:surface.vertices)
    CHECK(std::abs(sphere.signed_distance(vertex.position))<1.0e-10);

  std::map<std::array<tetra::WorldDerivedVertexKey,2>,std::size_t> edges;
  for(const auto& triangle:surface.triangles){
    for(std::size_t corner=0;corner<3U;++corner){
      std::array key{triangle.vertices[corner],
                     triangle.vertices[(corner+1U)%3U]};
      std::ranges::sort(key);++edges[key];
    }
  }
  CHECK(std::ranges::all_of(edges,
      [](const auto& entry){return entry.second==2U;}));

  directory.publish(directory.stage_derived_surfaces(
      surface.snapshots,directory.revision()+1U));
  const auto assembled=tetra_viewer::assemble_blocked_derived_surface(directory);
  CHECK(assembled.canonical_surface_hash==surface.canonical_surface_hash);
  REQUIRE(assembled.vertices.size()==surface.vertices.size());
  for(std::size_t index=0;index<surface.vertices.size();++index){
    CHECK(assembled.vertices[index].key==surface.vertices[index].key);
    CHECK(assembled.vertices[index].position.x==surface.vertices[index].position.x);
    CHECK(assembled.vertices[index].position.y==surface.vertices[index].position.y);
    CHECK(assembled.vertices[index].position.z==surface.vertices[index].position.z);
  }
  CHECK(assembled.triangles==surface.triangles);

  const auto retained=tetra_viewer::build_sparse_world_derived_surface(
      directory,domain,sphere,false,{},&cache);
  CHECK(retained.canonical_surface_hash==surface.canonical_surface_hash);
  CHECK(retained.metrics.computed_intersections==0U);
  CHECK(retained.metrics.rebuilt_surface_blocks==0U);
  CHECK(retained.metrics.reused_surface_blocks==surface.snapshots.size());
  CHECK(cache.intersections.size()==surface.vertices.size());
  CHECK(cache.snapshots.size()==surface.snapshots.size());
  REQUIRE(cache.raw_blocks.size()==cache.snapshots.size());
  for(std::size_t block=0;block<cache.snapshots.size();++block){
    CHECK(cache.raw_blocks[block]->id==cache.snapshots[block].id);
    CHECK(cache.raw_blocks[block]->source_hierarchy_revision<=
          cache.snapshots[block].source_hierarchy_revision);
    CHECK(cache.raw_blocks[block]->vertices.size()==
          cache.snapshots[block].vertices.size());
    CHECK(cache.raw_blocks[block]->triangles.size()==
          cache.snapshots[block].triangles.size());
  }

  // Selective volume retention changes storage only: a cold extraction still
  // publishes exactly the same authoritative connected surface.
  tetra_viewer::SparseWorldSurfaceCache selective_cache;
  const std::array retained_blocks{cache.conforming.blocks.front()->id};
  const auto selective=tetra_viewer::build_sparse_world_derived_surface(
      directory,domain,sphere,false,{},&selective_cache,retained_blocks,true);
  CHECK(selective.canonical_surface_hash==surface.canonical_surface_hash);
  REQUIRE(selective.vertices.size()==surface.vertices.size());
  for(std::size_t index=0;index<surface.vertices.size();++index){
    CHECK(selective.vertices[index].key==surface.vertices[index].key);
    CHECK(selective.vertices[index].position.x==surface.vertices[index].position.x);
    CHECK(selective.vertices[index].position.y==surface.vertices[index].position.y);
    CHECK(selective.vertices[index].position.z==surface.vertices[index].position.z);
  }
  CHECK(selective.triangles==surface.triangles);
  REQUIRE(selective_cache.conforming.blocks.size()==1U);
  CHECK(selective_cache.conforming.blocks.front()->id==retained_blocks.front());
  CHECK(selective.metrics.conforming_cells==surface.metrics.conforming_cells);
  CHECK(selective_cache.conforming.cells<selective.metrics.conforming_cells);
  CHECK(selective_cache.conforming.retained_bytes<cache.conforming.retained_bytes);

  // The production surface path expands only certified surface candidates and
  // does not allocate a conforming volume when no gameplay pin requests one.
  tetra_viewer::SparseWorldSurfaceCache direct_cache;
  const auto certificate_count=[](
      const tetra_viewer::SparseWorldSurfaceCache& cached){
    std::size_t result{};
    for(const auto& block:cached.surface_certificate_blocks)
      result+=block->certificates.size();
    return result;
  };
  const auto direct=tetra_viewer::build_sparse_world_derived_surface(
      directory,domain,sphere,false,{},&direct_cache,{},true,false);
  CHECK(direct.canonical_surface_hash==surface.canonical_surface_hash);
  CHECK(direct.triangles==surface.triangles);
  CHECK(direct.metrics.conforming_cells_materialized==0U);
  CHECK(direct_cache.conforming.blocks.empty());
  CHECK(direct.metrics.surface_candidate_owners<
        direct_cache.closure.closed_owners.size());
  CHECK(direct.metrics.green_cells_enumerated<direct.metrics.conforming_cells);
  CHECK(certificate_count(direct_cache)==
        direct_cache.closure.closed_owners.size());
  const auto direct_repeated=tetra_viewer::build_sparse_world_derived_surface(
      directory,domain,sphere,false,{},&direct_cache,{},true,false);
  CHECK(direct_repeated.canonical_surface_hash==direct.canonical_surface_hash);
  CHECK(direct_repeated.metrics.rebuilt_surface_certificates==0U);
  CHECK(direct_repeated.metrics.reused_surface_certificates==
        certificate_count(direct_cache));
  CHECK(direct_repeated.metrics.surface_classification_samples==0U);

  // Surface cost and output stay fixed while gameplay residency promotes and
  // then demotes the entire conforming volume behind that surface.
  std::vector<tetra::HierarchyBlockId> all_volume_blocks;
  all_volume_blocks.reserve(cache.conforming.blocks.size());
  for(const auto& block:cache.conforming.blocks)
    all_volume_blocks.push_back(block->id);
  tetra_viewer::SparseWorldSurfaceCache promoted_cache;
  const auto promoted=tetra_viewer::build_sparse_world_derived_surface(
      directory,domain,sphere,false,{},&promoted_cache,all_volume_blocks,
      true,false);
  CHECK(promoted.canonical_surface_hash==direct.canonical_surface_hash);
  CHECK(promoted.triangles==direct.triangles);
  CHECK(promoted.metrics.surface_candidate_owners==
        direct.metrics.surface_candidate_owners);
  CHECK(promoted.metrics.conforming_cells_materialized==
        promoted_cache.conforming.cells);
  CHECK(promoted.metrics.conforming_cells_materialized>0U);
  CHECK(promoted_cache.conforming.blocks.size()==all_volume_blocks.size());
  const auto demoted=tetra_viewer::build_sparse_world_derived_surface(
      directory,domain,sphere,false,{},&promoted_cache,{},true,false);
  CHECK(demoted.canonical_surface_hash==direct.canonical_surface_hash);
  CHECK(demoted.triangles==direct.triangles);
  CHECK(demoted.metrics.conforming_cells_materialized==0U);
  CHECK(promoted_cache.conforming.blocks.empty());

  auto changed_sphere=sphere;changed_sphere.radius*=0.9;
  const auto changed_field=tetra_viewer::build_sparse_world_derived_surface(
      directory,domain,changed_sphere,false,{},&direct_cache,{},true,false);
  CHECK(changed_field.metrics.rebuilt_surface_certificates==
        certificate_count(direct_cache));
  CHECK(changed_field.canonical_surface_hash!=direct.canonical_surface_hash);

  const auto flat_scene=tetra_viewer::prepare_blocked_derived_surface_scene(
      surface,sphere,true,true,{});
  const auto retained_scene=tetra_viewer::prepare_retained_blocked_scene(
      surface,sphere,true,true,{},cache);
  REQUIRE(retained_scene.scene.triangle_vertices.size()==
          flat_scene.triangle_vertices.size());
  CHECK(tetra_viewer::surface_geometry_hashes(retained_scene.scene)==
        tetra_viewer::surface_geometry_hashes(flat_scene));
  CHECK(retained_scene.rebuilt_blocks==surface.snapshots.size());
  const auto reused_scene=tetra_viewer::prepare_retained_blocked_scene(
      surface,sphere,true,true,{},cache);
  CHECK(reused_scene.reused_blocks==surface.snapshots.size());
  CHECK(reused_scene.rebuilt_blocks==0U);
  const auto rebased_scene=tetra_viewer::prepare_retained_blocked_scene(
      surface,sphere,true,true,{8.0,0.0,0.0},cache);
  CHECK(rebased_scene.rebuilt_blocks==surface.snapshots.size());
}

TEST_CASE("sparse world surface cache localizes topology edits and matches cold extraction") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  mesh.refine_all_binary();mesh.refine_all_binary();
  tetra::WorldCutDirectory initial(
      tetra::make_world_cut_checkpoint(mesh,2U,1U));
  tetra_viewer::SparseWorldSurfaceCache cache;
  const tetra::Sphere sphere;
  const tetra::WorldStreamingDemand::Domain domain{};
  const auto baseline=tetra_viewer::build_sparse_world_derived_surface(
      initial,domain,sphere,false,{},&cache);
  REQUIRE_FALSE(baseline.snapshots.empty());

  const auto selected_found=std::ranges::find_if(
      mesh.logical_red_owners(),[&](tetra::TetId owner){
        const auto& cell=mesh.tetrahedron(owner);
        bool negative{},positive{};
        for(const auto vertex:cell.vertices){
          const auto distance=sphere.signed_distance(mesh.vertices()[vertex]);
          negative|=distance<0.0;positive|=distance>=0.0;
        }
        return negative&&positive;
      });
  REQUIRE(selected_found!=mesh.logical_red_owners().end());
  const auto selected=*selected_found;
  REQUIRE(mesh.refine_selected_binary({selected}));
  tetra::WorldCutDirectory changed(
      tetra::make_world_cut_checkpoint(mesh,2U,2U));
  const auto warm=tetra_viewer::build_sparse_world_derived_surface(
      changed,domain,sphere,false,{},&cache);
  const auto cold=tetra_viewer::build_sparse_world_derived_surface(
      changed,domain,sphere,false);
  CHECK(warm.triangles.size()==cold.triangles.size());
  CHECK(warm.canonical_surface_hash==cold.canonical_surface_hash);
  CHECK(warm.metrics.conforming_volume_hash==cold.metrics.conforming_volume_hash);
  CHECK(warm.metrics.reused_surface_blocks>0U);
  CHECK(warm.metrics.rebuilt_surface_blocks>0U);
  CHECK(warm.metrics.computed_intersections<cold.metrics.computed_intersections);
  CHECK(cache.intersections.size()==warm.vertices.size());
  CHECK(cache.snapshots.size()==warm.snapshots.size());
  REQUIRE(cache.raw_blocks.size()==cache.snapshots.size());
  for(std::size_t block=0;block<cache.snapshots.size();++block){
    CHECK(cache.raw_blocks[block]->id==cache.snapshots[block].id);
    CHECK(cache.raw_blocks[block]->source_hierarchy_revision<=
          cache.snapshots[block].source_hierarchy_revision);
    CHECK(cache.raw_blocks[block]->vertices.size()==
          cache.snapshots[block].vertices.size());
    CHECK(cache.raw_blocks[block]->triangles.size()==
          cache.snapshots[block].triangles.size());
  }

  tetra_viewer::SparseWorldSurfaceCache optimized_cache;
  static_cast<void>(tetra_viewer::build_sparse_world_derived_surface(
      initial,domain,sphere,true,{},&optimized_cache));
  const auto optimized_warm=tetra_viewer::build_sparse_world_derived_surface(
      changed,domain,sphere,true,{},&optimized_cache);
  const auto optimized_cold=tetra_viewer::build_sparse_world_derived_surface(
      changed,domain,sphere,true);
  CHECK(optimized_warm.canonical_surface_hash==
        optimized_cold.canonical_surface_hash);
  CHECK(optimized_warm.metrics.rebuilt_surface_blocks>0U);
  CHECK(optimized_warm.metrics.affected_optimizer_vertices<=
        optimized_warm.metrics.optimizer_dependency_vertices);
  CHECK(optimized_cache.optimizer_incident_hashes.size()==
        optimized_cache.intersections.size());
  CHECK(optimized_cache.optimizer_neighbor_offsets.size()==
        optimized_cache.intersections.size()+1U);
  const auto optimized_repeated=tetra_viewer::build_sparse_world_derived_surface(
      changed,domain,sphere,true,{},&optimized_cache);
  CHECK(optimized_repeated.canonical_surface_hash==
        optimized_cold.canonical_surface_hash);
  CHECK(optimized_repeated.metrics.rebuilt_surface_blocks==0U);
  CHECK(optimized_cache.assembled_vertices.size()==
        optimized_repeated.vertices.size());
  CHECK(optimized_cache.assembled_triangles.size()==
        optimized_repeated.triangles.size());
  CHECK(std::ranges::all_of(optimized_cache.assembled_vertices,
      [](const auto& vertex){return vertex.references>0U;}));
  CHECK(std::ranges::all_of(optimized_cache.assembled_triangles,
      [](const auto& triangle){return triangle.references==1U;}));
}

TEST_CASE("world cut child publication and eviction atomically reveal coarse ancestors") {
  auto target=tetra::WorldTetAddress::root(0U);
  for(unsigned int depth=0;depth<10U;++depth)
    target=target.child(static_cast<std::uint8_t>((depth*5U+1U)%8U));
  const std::array targets{target};
  const auto available=tetra::make_sparse_world_cut_checkpoint(targets,3U,1U);
  auto roots=available;
  roots.blocks.erase(std::remove_if(roots.blocks.begin(),roots.blocks.end(),
      [](const auto& block){return block.id.prefix.red_depth()!=0U;}),roots.blocks.end());
  roots.metrics={};
  tetra::WorldCutDirectory directory(std::move(roots));
  const auto coarse=directory.lookup(target);
  REQUIRE(coarse);
  CHECK(coarse.logical_owner.red_depth()==3U);
  CHECK(coarse.fallback_levels>0U);
  const auto coarse_count=directory.logical_owner_count();

  std::vector<tetra::HierarchyBlockId> all;
  for(const auto& block:available.blocks)all.push_back(block.id);
  const auto refined=directory.reconcile(available,all,all.size(),2U);
  CHECK(refined.metrics.loaded_blocks==available.blocks.size()-12U);
  CHECK(refined.metrics.evicted_blocks==0U);
  REQUIRE(directory.lookup(target));
  CHECK(directory.lookup(target).logical_owner==target);
  CHECK(directory.logical_owner_count()>coarse_count);
  const auto refined_hash=directory.canonical_cut_hash();

  std::vector<tetra::HierarchyBlockId> root_ids;
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
    root_ids.push_back({tetra::WorldTetAddress::root(root),3U});
  const auto coarsened=directory.reconcile(available,root_ids,12U,3U);
  CHECK(coarsened.metrics.evicted_blocks==available.blocks.size()-12U);
  CHECK(coarsened.metrics.fallback_owners_exposed==coarsened.metrics.evicted_blocks);
  CHECK(directory.logical_owner_count()==coarse_count);
  CHECK(directory.lookup(target).logical_owner==coarse.logical_owner);
  CHECK(directory.canonical_cut_hash()!=refined_hash);

  static_cast<void>(directory.reconcile(available,all,all.size(),4U));
  CHECK(directory.canonical_cut_hash()==refined_hash);
  const auto saved=directory.checkpoint();
  tetra::WorldCutDirectory restored(saved);
  CHECK(restored.canonical_cut_hash()==refined_hash);
  CHECK(restored.lookup(target).logical_owner==target);
}

TEST_CASE("world block demand is deterministic bounded and preserves ancestor closure") {
  std::vector<tetra::WorldTetAddress> targets;
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
    for(unsigned int branch=0;branch<6U;++branch){
      auto address=tetra::WorldTetAddress::root(root);
      for(unsigned int depth=0;depth<30U;++depth)
        address=address.child(static_cast<std::uint8_t>(
            (root*3U+branch*5U+depth*7U)%8U));
      targets.push_back(address);
    }
  const auto available=tetra::make_sparse_world_cut_checkpoint(targets,3U,1U);
  tetra::WorldStreamingDemand demand;
  demand.camera_world_position={0.15,0.2,0.25};
  demand.player_world_position={0.2,0.2,0.2};
  demand.camera_radius=0.45;demand.player_radius=0.08;
  demand.camera_red_depth=18U;demand.player_red_depth=30U;
  demand.maximum_blocks=160U;
  const auto first=tetra::select_world_blocks(available,demand);
  const auto repeated=tetra::select_world_blocks(available,demand);
  CHECK(first.blocks==repeated.blocks);
  auto planet_demand=demand;
  planet_demand.domain.world_origin={-6371000.0,-6371000.0,-6371000.0};
  planet_demand.domain.world_extent=12742000.0;
  planet_demand.camera_world_position=planet_demand.domain.to_world(
      demand.camera_world_position);
  planet_demand.player_world_position=planet_demand.domain.to_world(
      demand.player_world_position);
  planet_demand.camera_radius*=planet_demand.domain.world_extent;
  planet_demand.player_radius*=planet_demand.domain.world_extent;
  CHECK(tetra::select_world_blocks(available,planet_demand).blocks==first.blocks);
  CHECK(first.metrics.selected_blocks==first.blocks.size());
  CHECK(first.blocks.size()<=demand.maximum_blocks);
  CHECK(first.blocks.size()>=tetra::bcc_root_tetrahedron_count);
  const std::set<tetra::HierarchyBlockId> selected(
      first.blocks.begin(),first.blocks.end());
  for(auto id:first.blocks)while(id.prefix.red_depth()>0U){
    id={id.prefix.ancestor(id.prefix.red_depth()-id.block_generations),
        id.block_generations};
    CHECK(selected.contains(id));
  }

  auto root_checkpoint=available;
  root_checkpoint.blocks.erase(std::remove_if(
      root_checkpoint.blocks.begin(),root_checkpoint.blocks.end(),
      [](const auto& block){return block.id.prefix.red_depth()!=0U;}),
      root_checkpoint.blocks.end());
  tetra::WorldCutDirectory directory(std::move(root_checkpoint));
  const auto near_update=directory.reconcile(
      available,first.blocks,demand.maximum_blocks,2U);
  CHECK(near_update.metrics.loaded_blocks>0U);
  CHECK(directory.metrics().blocks==first.blocks.size());
  demand.camera_world_position={0.85,0.8,0.75};
  demand.player_world_position={0.8,0.8,0.8};
  const auto moved=tetra::select_world_blocks(available,demand);
  CHECK(moved.blocks!=first.blocks);
  const auto moved_update=directory.reconcile(
      available,moved.blocks,demand.maximum_blocks,3U);
  CHECK(moved_update.metrics.loaded_blocks>0U);
  CHECK(moved_update.metrics.evicted_blocks>0U);
  CHECK(moved_update.metrics.update_milliseconds>=0.0);
  CHECK(directory.metrics().blocks<=demand.maximum_blocks);
}

TEST_CASE("world revision manifest rejects stale or invalid partial publication atomically") {
  auto target=tetra::WorldTetAddress::root(3U);
  for(unsigned int depth=0;depth<7U;++depth)target=target.child(2U);
  const std::array targets{target};
  const auto available=tetra::make_sparse_world_cut_checkpoint(targets,3U,1U);
  auto roots=available;
  roots.blocks.erase(std::remove_if(roots.blocks.begin(),roots.blocks.end(),
      [](const auto& block){return block.id.prefix.red_depth()!=0U;}),roots.blocks.end());
  tetra::WorldCutDirectory directory(std::move(roots));
  const auto before=directory.checkpoint().canonical_hash();
  const auto child=std::ranges::find_if(available.blocks,
      [](const auto& block){return block.id.prefix.red_depth()==3U;});
  REQUIRE(child!=available.blocks.end());
  tetra::WorldRevisionManifest accepted(2U,1U,{*child});
  directory.publish(accepted);
  CHECK(directory.revision()==2U);
  CHECK(directory.metrics().blocks==13U);
  const auto published=directory.checkpoint().canonical_hash();
  CHECK(published!=before);
  CHECK_THROWS_AS(directory.publish(accepted),std::invalid_argument);
  CHECK(directory.checkpoint().canonical_hash()==published);

  auto invalid=*child;
  invalid.id.prefix=invalid.id.prefix.child(0U);
  tetra::WorldRevisionManifest broken(3U,2U,{std::move(invalid)});
  CHECK_THROWS_AS(directory.publish(broken),std::invalid_argument);
  CHECK(directory.revision()==2U);
  CHECK(directory.checkpoint().canonical_hash()==published);
}

TEST_CASE("world transactions stage privately and publish split merge atomically") {
  for(const unsigned int width:{1U,2U,3U,4U}){
    tetra::WorldCutDirectory directory(tetra::make_sparse_world_cut_checkpoint(
        {},width,1U,tetra::HierarchyResidencyTier::conforming_volume));
    const auto parent=tetra::WorldTetAddress::root(0U);
    const auto original=directory.canonical_cut_hash();
    const auto staged=directory.stage_transaction(
        {{{parent,tetra::WorldTopologyOperation::split}}},2U);
    CAPTURE(width);
    CHECK(directory.revision()==1U);
    CHECK(directory.canonical_cut_hash()==original);
    CHECK(staged.transaction.metrics.requested_edits==1U);
    CHECK(staged.transaction.metrics.result_logical_owners==19U);
    CHECK(staged.transaction.metrics.affected_blocks>0U);
    CHECK(staged.transaction.metrics.dependency_reads>0U);
    directory.publish(staged.manifest);
    CHECK(directory.revision()==2U);
    CHECK(directory.logical_owner_count()==19U);
    for(std::uint8_t child=0;child<8U;++child)
      CHECK(directory.lookup(parent.child(child)).logical_owner==parent.child(child));

    const auto merged=directory.stage_transaction(
        {{{parent,tetra::WorldTopologyOperation::merge}}},3U);
    CHECK(directory.logical_owner_count()==19U);
    directory.publish(merged.manifest);
    CHECK(directory.logical_owner_count()==12U);
    CHECK(directory.canonical_cut_hash()==original);
  }
}

TEST_CASE("world transaction ordering cancellation and stale staging are deterministic") {
  const auto checkpoint=tetra::make_sparse_world_cut_checkpoint({},2U,1U);
  tetra::WorldCutDirectory first(checkpoint),second(checkpoint);
  const std::array edits{
      tetra::WorldTopologyEdit{tetra::WorldTetAddress::root(11U),
                               tetra::WorldTopologyOperation::split},
      tetra::WorldTopologyEdit{tetra::WorldTetAddress::root(0U),
                               tetra::WorldTopologyOperation::split}};
  auto reversed=edits;std::ranges::reverse(reversed);
  const auto staged_first=first.stage_transaction(edits,2U);
  const auto stale=first.stage_transaction(
      {{{tetra::WorldTetAddress::root(5U),tetra::WorldTopologyOperation::split}}},2U);
  const auto staged_second=second.stage_transaction(reversed,2U);
  CHECK(staged_first.transaction.canonical_hash==staged_second.transaction.canonical_hash);
  CHECK(staged_first.transaction.affected_blocks==staged_second.transaction.affected_blocks);
  first.publish(staged_first.manifest);second.publish(staged_second.manifest);
  CHECK(first.canonical_cut_hash()==second.canonical_cut_hash());
  const auto published=first.canonical_cut_hash();
  CHECK_THROWS_AS(first.publish(stale.manifest),std::invalid_argument);
  CHECK(first.canonical_cut_hash()==published);

  std::size_t polls{};
  const auto cancel_target=first.logical_owner(0U);
  CHECK_THROWS_AS(static_cast<void>(first.stage_transaction(
      {{{cancel_target,tetra::WorldTopologyOperation::split}}},
      3U,[&]{return ++polls>1U;})),std::runtime_error);
  CHECK(first.revision()==2U);
  CHECK(first.canonical_cut_hash()==published);
}

TEST_CASE("world transactions reproduce monolithic BCC logical cuts across block widths") {
  for(const unsigned int width:{1U,2U,3U,4U,5U}){
    auto oracle=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
    tetra::WorldCutDirectory directory(
        tetra::make_world_cut_checkpoint(oracle,width,1U));
    std::uint64_t revision=1U;
    for(unsigned int step=0;step<3U;++step){
      const auto local=oracle.logical_red_owners().front();
      const auto world=tetra::world_tet_address(local);
      REQUIRE(oracle.refine_selected_binary({local}));
      auto staged=directory.stage_transaction(
          {{{world,tetra::WorldTopologyOperation::split}}},++revision);
      directory.publish(staged.manifest);
      std::vector<tetra::WorldTetAddress> expected,actual;
      for(const auto owner:oracle.logical_red_owners())
        expected.push_back(tetra::world_tet_address(owner));
      directory.for_each_logical_owner(
          [&](tetra::WorldTetAddress owner){actual.push_back(owner);});
      std::ranges::sort(expected);std::ranges::sort(actual);
      CAPTURE(width);CAPTURE(step);
      CAPTURE(expected.size());CAPTURE(actual.size());
      CHECK(actual==expected);
    }
  }
}

TEST_CASE("complete world cut checkpoint exactly matches sparse construction") {
  std::vector<tetra::WorldTetAddress> leaves;
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
    leaves.push_back(tetra::WorldTetAddress::root(root));
  const auto split=[&](tetra::WorldTetAddress owner){
    const auto found=std::ranges::find(leaves,owner);REQUIRE(found!=leaves.end());
    leaves.erase(found);
    for(std::uint8_t child=0;child<8U;++child)leaves.push_back(owner.child(child));
  };
  split(tetra::WorldTetAddress::root(0U));
  split(tetra::WorldTetAddress::root(0U).child(3U));
  split(tetra::WorldTetAddress::root(7U));
  std::ranges::sort(leaves);
  for(const unsigned int width:{1U,2U,3U,4U}){
    const auto sparse=tetra::make_sparse_world_cut_checkpoint(
        leaves,width,17U,tetra::HierarchyResidencyTier::conforming_volume);
    const auto complete=tetra::make_complete_world_cut_checkpoint(
        leaves,width,17U,tetra::HierarchyResidencyTier::conforming_volume);
    CAPTURE(width);
    CHECK(complete.canonical_hash()==sparse.canonical_hash());
    tetra::WorldCutDirectory sparse_directory(sparse),complete_directory(complete);
    CHECK(complete_directory.canonical_cut_hash()==
          sparse_directory.canonical_cut_hash());
    CHECK(complete_directory.logical_owner_count()==leaves.size());
  }
}

TEST_CASE("retained world checkpoint reuses payloads and rolls back invalid adoption") {
  auto first=tetra::make_sparse_world_cut_checkpoint(
      {},3U,1U,tetra::HierarchyResidencyTier::conforming_volume);
  tetra::WorldDerivedSurfaceSnapshot surface;
  surface.id=first.blocks.front().id;surface.source_hierarchy_revision=1U;
  first.surfaces.push_back(surface);
  tetra::WorldCutDirectory directory(first);
  const auto original_block=directory.hierarchy_blocks().front();
  const auto original_surface=directory.derived_surfaces().front();

  auto unchanged=directory.checkpoint();unchanged.revision=2U;
  const auto reused=directory.adopt_retained(std::move(unchanged));
  CHECK(reused.metrics.reused_blocks==first.blocks.size());
  CHECK(reused.metrics.loaded_blocks==0U);
  CHECK(reused.metrics.reused_surfaces==1U);
  CHECK(directory.hierarchy_blocks().front()==original_block);
  CHECK(directory.derived_surfaces().front()==original_surface);

  auto changed=directory.checkpoint();changed.revision=3U;
  changed.blocks.front().residency=tetra::HierarchyResidencyTier::surface;
  changed.surfaces.clear();
  const auto replaced=directory.adopt_retained(std::move(changed));
  CHECK(replaced.metrics.loaded_blocks==1U);
  CHECK(replaced.metrics.reused_blocks==first.blocks.size()-1U);
  CHECK(directory.hierarchy_blocks().front()!=original_block);
  CHECK(directory.derived_surfaces().empty());

  const auto valid_hash=directory.checkpoint().canonical_hash();
  auto invalid=directory.checkpoint();invalid.revision=4U;
  invalid.blocks.erase(invalid.blocks.begin());
  CHECK_THROWS(static_cast<void>(directory.adopt_retained(std::move(invalid))));
  CHECK(directory.revision()==3U);
  CHECK(directory.checkpoint().canonical_hash()==valid_hash);

  const auto retained_block=directory.hierarchy_blocks().back();
  auto direct_checkpoint=directory.checkpoint();direct_checkpoint.revision=4U;
  tetra::WorldCutDirectory direct_candidate(std::move(direct_checkpoint));
  const auto direct=directory.adopt_retained(std::move(direct_candidate));
  CHECK(direct.metrics.reused_blocks==directory.hierarchy_blocks().size());
  CHECK(direct.metrics.loaded_blocks==0U);
  CHECK(directory.hierarchy_blocks().back()==retained_block);
  CHECK(directory.revision()==4U);

  auto wrong_width=tetra::make_sparse_world_cut_checkpoint(
      {},2U,5U,tetra::HierarchyResidencyTier::surface);
  tetra::WorldCutDirectory wrong_width_candidate(std::move(wrong_width));
  CHECK_THROWS(static_cast<void>(
      directory.adopt_retained(std::move(wrong_width_candidate))));
  CHECK(directory.revision()==4U);
}

TEST_CASE("retained complete cut rebuilds only changed hierarchy paths") {
  std::vector<tetra::WorldTetAddress> coarse;
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root)
    coarse.push_back(tetra::WorldTetAddress::root(root));
  const auto split=[](auto leaves,tetra::WorldTetAddress owner){
    const auto found=std::ranges::find(leaves,owner);REQUIRE(found!=leaves.end());
    leaves.erase(found);
    for(std::uint8_t child=0;child<8U;++child)
      leaves.push_back(owner.child(child));
    std::ranges::sort(leaves);return leaves;
  };
  const auto first=split(coarse,coarse.front());
  const auto second=split(first,coarse.front().child(3U));
  auto initial=tetra::make_complete_world_cut_checkpoint(
      first,3U,1U,tetra::HierarchyResidencyTier::surface);
  tetra::WorldCutDirectory directory(initial);
  const auto retained_root=directory.hierarchy_blocks().back();
  auto expected=tetra::make_complete_world_cut_checkpoint(
      second,3U,2U,tetra::HierarchyResidencyTier::surface);
  std::vector<tetra::HierarchyBlockId> expected_blocks;
  for(const auto& block:expected.blocks)expected_blocks.push_back(block.id);
  std::vector<tetra::WorldTetAddress> changed{
      coarse.front().child(3U)};
  for(std::uint8_t child=0;child<8U;++child)
    changed.push_back(coarse.front().child(3U).child(child));
  std::ranges::sort(changed);
  const auto refined=directory.replace_complete_cut(
      second,changed,expected_blocks,{},2U);
  tetra::WorldCutDirectory expected_directory(expected);
  CHECK(directory.canonical_cut_hash()==expected_directory.canonical_cut_hash());
  REQUIRE(directory.hierarchy_blocks().size()==expected.blocks.size());
  for(std::size_t block=0;block<expected.blocks.size();++block){
    CHECK(directory.hierarchy_blocks()[block]->id==expected.blocks[block].id);
    CHECK(directory.hierarchy_blocks()[block]->residency==
          expected.blocks[block].residency);
    CHECK(directory.hierarchy_blocks()[block]->logical_owners==
          expected.blocks[block].logical_owners);
    CHECK(directory.hierarchy_blocks()[block]->resident_records==
          expected.blocks[block].resident_records);
  }
  CHECK(refined.metrics.loaded_blocks>0U);
  CHECK(refined.metrics.reused_blocks>0U);
  CHECK(std::ranges::is_sorted(refined.changed_blocks));
  CHECK(refined.changed_blocks.size()==
        refined.metrics.loaded_blocks+refined.metrics.evicted_blocks);
  CHECK(!std::ranges::binary_search(
      refined.changed_blocks,retained_root->id));
  CHECK(directory.hierarchy_blocks().back()==retained_root);

  auto coarse_expected=tetra::make_complete_world_cut_checkpoint(
      first,3U,3U,tetra::HierarchyResidencyTier::surface);
  std::vector<tetra::HierarchyBlockId> coarse_blocks;
  for(const auto& block:coarse_expected.blocks)coarse_blocks.push_back(block.id);
  const auto simplified=directory.replace_complete_cut(
      first,changed,coarse_blocks,{},3U);
  tetra::WorldCutDirectory coarse_directory(coarse_expected);
  CHECK(directory.canonical_cut_hash()==coarse_directory.canonical_cut_hash());
  REQUIRE(directory.hierarchy_blocks().size()==coarse_expected.blocks.size());
  for(std::size_t block=0;block<coarse_expected.blocks.size();++block){
    CHECK(directory.hierarchy_blocks()[block]->id==coarse_expected.blocks[block].id);
    CHECK(directory.hierarchy_blocks()[block]->residency==
          coarse_expected.blocks[block].residency);
    CHECK(directory.hierarchy_blocks()[block]->logical_owners==
          coarse_expected.blocks[block].logical_owners);
    CHECK(directory.hierarchy_blocks()[block]->resident_records==
          coarse_expected.blocks[block].resident_records);
  }
  CHECK(simplified.metrics.evicted_blocks+simplified.metrics.loaded_blocks>0U);
  CHECK(simplified.metrics.reused_blocks>0U);
  CHECK(std::ranges::is_sorted(simplified.changed_blocks));
  CHECK(simplified.changed_blocks.size()==
        simplified.metrics.loaded_blocks+simplified.metrics.evicted_blocks);
}

TEST_CASE("world closure crosses root seams and shared ownership is canonical") {
  auto oracle=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::WorldCutDirectory directory(tetra::make_world_cut_checkpoint(oracle,1U,1U));
  const auto root=tetra::WorldTetAddress::root(0U);
  auto first=directory.stage_transaction(
      {{{root,tetra::WorldTopologyOperation::split}}},2U);
  directory.publish(first.manifest);
  REQUIRE(oracle.refine_selected_binary({tetra::make_tet_id(0U,1U)}));
  const auto child=tetra::world_tet_address(oracle.logical_red_owners().front());
  auto second=directory.stage_transaction(
      {{{child,tetra::WorldTopologyOperation::split}}},3U);
  CHECK(std::ranges::any_of(second.transaction.closure_edits,
      [](tetra::WorldTetAddress owner){return owner.root_id()!=0U;}));
  CHECK(second.transaction.affected_blocks.size()>1U);
  std::vector<tetra::WorldTetAddress> incidents{root, tetra::WorldTetAddress::root(1U)};
  CHECK(tetra::world_shared_entity_owner(incidents)==root);
  std::ranges::reverse(incidents);
  CHECK(tetra::world_shared_entity_owner(incidents)==root);
  CHECK_THROWS_AS(static_cast<void>(tetra::world_shared_entity_owner({})),
                  std::invalid_argument);
}

TEST_CASE("dependency certificates reject stale block payloads before publication") {
  const auto checkpoint=tetra::make_sparse_world_cut_checkpoint({},3U,1U);
  tetra::WorldCutDirectory directory(checkpoint);
  const auto& block=checkpoint.blocks.front();
  tetra::WorldBlockDependency stale{
      block.id,block.source_revision,tetra::hierarchy_block_canonical_hash(block)^1U};
  tetra::WorldRevisionManifest manifest(2U,1U,{},{{stale}});
  const auto before=directory.checkpoint().canonical_hash();
  CHECK_THROWS_AS(directory.publish(manifest),std::invalid_argument);
  CHECK(directory.revision()==1U);
  CHECK(directory.checkpoint().canonical_hash()==before);
}

TEST_CASE("closure-expanded world transactions can be reversed as one safe merge group") {
  tetra::WorldCutDirectory directory(
      tetra::make_sparse_world_cut_checkpoint({},1U,1U));
  const auto root=tetra::WorldTetAddress::root(0U);
  auto first=directory.stage_transaction(
      {{{root,tetra::WorldTopologyOperation::split}}},2U);
  directory.publish(first.manifest);
  const auto baseline=directory.canonical_cut_hash();
  const auto child=root.child(0U);
  auto refined=directory.stage_transaction(
      {{{child,tetra::WorldTopologyOperation::split}}},3U);
  std::vector<tetra::WorldTopologyEdit> inverse{{
      child,tetra::WorldTopologyOperation::merge}};
  for(const auto owner:refined.transaction.closure_edits)
    inverse.push_back({owner,tetra::WorldTopologyOperation::merge});
  directory.publish(refined.manifest);
  auto merged=directory.stage_transaction(inverse,4U);
  directory.publish(merged.manifest);
  CHECK(directory.canonical_cut_hash()==baseline);
}

TEST_CASE("derived surface snapshots stage privately publish atomically and invalidate with topology") {
  const auto checkpoint=tetra::make_sparse_world_cut_checkpoint({},3U,1U);
  tetra::WorldCutDirectory directory(checkpoint);
  const auto root=tetra::WorldTetAddress::root(0U);
  const tetra::HierarchyBlockId block{root,3U};
  const auto keys=tetra::world_tetrahedron_vertex_keys(root);
  const auto points=tetra::world_tetrahedron_geometry(root);
  tetra::WorldDerivedSurfaceSnapshot surface;
  surface.id=block;surface.source_hierarchy_revision=1U;
  surface.metrics.optimizer_passes=5U;surface.metrics.dependency_halo_rings=5U;
  for(std::size_t vertex=0;vertex<3U;++vertex)
    surface.vertices.push_back({tetra::world_hierarchy_vertex_key(keys[vertex]),points[vertex]});
  surface.triangles.push_back({{{surface.vertices[2].key,surface.vertices[0].key,
                                 surface.vertices[1].key}},root});
  surface.dependency_blocks.push_back({tetra::WorldTetAddress::root(1U),3U});
  const auto staged=directory.stage_derived_surfaces({{surface}},2U);
  CHECK(directory.revision()==1U);
  CHECK_FALSE(directory.surface(block));
  CHECK(staged.metrics().changed_surfaces==1U);
  CHECK(staged.dependencies().size()==2U);
  directory.publish(staged);
  REQUIRE(directory.surface(block));
  CHECK(directory.surface(block)->metrics.vertices==3U);
  CHECK(directory.surface(block)->metrics.triangles==1U);
  CHECK(directory.surface(block)->metrics.dependency_halo_rings==5U);
  CHECK(directory.metrics().derived_surface_blocks==1U);
  tetra::WorldCutDirectory restored(directory.checkpoint());
  REQUIRE(restored.surface(block));
  CHECK(restored.surface(block)->canonical_hash()==directory.surface(block)->canonical_hash());

  const auto topology=directory.stage_transaction(
      {{{root,tetra::WorldTopologyOperation::split}}},3U);
  CHECK(topology.manifest.removed_surfaces().size()==1U);
  directory.publish(topology.manifest);
  CHECK_FALSE(directory.surface(block));
  CHECK(directory.revision()==3U);
}

TEST_CASE("derived surface staging rejects cancellation stale halos and partial invalidation") {
  const auto checkpoint=tetra::make_sparse_world_cut_checkpoint({},3U,1U);
  tetra::WorldCutDirectory directory(checkpoint);
  const tetra::HierarchyBlockId block{tetra::WorldTetAddress::root(0U),3U};
  tetra::WorldDerivedSurfaceSnapshot surface;
  surface.id=block;surface.source_hierarchy_revision=1U;
  surface.metrics.optimizer_passes=5U;surface.metrics.dependency_halo_rings=5U;
  surface.dependency_blocks.push_back(
      {tetra::WorldTetAddress::root(1U).child(0U),3U});
  CHECK_THROWS_AS(static_cast<void>(directory.stage_derived_surfaces({{surface}},2U)),
                  std::invalid_argument);
  surface.dependency_blocks.clear();
  CHECK_THROWS_AS(static_cast<void>(directory.stage_derived_surfaces(
      {{surface}},2U,[]{return true;})),std::runtime_error);

  auto accepted=directory.stage_derived_surfaces({{surface}},2U);
  directory.publish(accepted);
  auto changed=checkpoint.blocks.front();
  changed.source_revision=3U;
  tetra::WorldRevisionManifest partial(3U,2U,{changed});
  CHECK_THROWS_AS(directory.publish(partial),std::invalid_argument);
  CHECK(directory.revision()==2U);
  CHECK(directory.surface(block));
}

TEST_CASE("derived surfaces invalidate when a separate halo block changes") {
  const auto checkpoint=tetra::make_sparse_world_cut_checkpoint({},3U,1U);
  tetra::WorldCutDirectory directory(checkpoint);
  const tetra::HierarchyBlockId owner{tetra::WorldTetAddress::root(0U),3U};
  const tetra::HierarchyBlockId halo{tetra::WorldTetAddress::root(1U),3U};
  tetra::WorldDerivedSurfaceSnapshot surface;
  surface.id=owner;surface.source_hierarchy_revision=1U;
  surface.dependency_blocks.push_back(halo);
  surface.metrics.optimizer_passes=5U;surface.metrics.dependency_halo_rings=5U;
  auto staged=directory.stage_derived_surfaces({{surface}},2U);
  directory.publish(staged);
  REQUIRE(directory.surface(owner));

  const auto halo_block=std::ranges::find(
      checkpoint.blocks,halo,&tetra::HierarchyBlockSnapshot::id);
  REQUIRE(halo_block!=checkpoint.blocks.end());
  auto changed=*halo_block;changed.source_revision=3U;
  tetra::WorldRevisionManifest stale(3U,2U,{changed});
  CHECK_THROWS_AS(directory.publish(stale),std::invalid_argument);
  CHECK(directory.revision()==2U);
  CHECK(directory.surface(owner));
  auto unsafe_replacement=surface;
  unsafe_replacement.source_hierarchy_revision=2U;
  tetra::WorldRevisionManifest mixed(3U,2U,{changed},{},{},
                                      {unsafe_replacement});
  CHECK_THROWS_AS(directory.publish(mixed),std::invalid_argument);
  CHECK(directory.revision()==2U);

  auto topology=directory.stage_transaction(
      {{{halo.prefix,tetra::WorldTopologyOperation::split}}},3U);
  CHECK(std::ranges::binary_search(topology.manifest.removed_surfaces(),owner));
  directory.publish(topology.manifest);
  CHECK_FALSE(directory.surface(owner));
}

TEST_CASE("derived surface hashes and manifests ignore payload request ordering") {
  const auto owner=tetra::WorldTetAddress::root(0U);
  const auto hierarchy_keys=tetra::world_tetrahedron_vertex_keys(owner);
  const auto points=tetra::world_tetrahedron_geometry(owner);
  tetra::WorldDerivedSurfaceSnapshot first;
  first.id={owner,3U};first.source_hierarchy_revision=1U;
  first.metrics.optimizer_passes=5U;first.metrics.dependency_halo_rings=5U;
  for(std::size_t vertex=0;vertex<4U;++vertex)
    first.vertices.push_back(
        {tetra::world_hierarchy_vertex_key(hierarchy_keys[vertex]),points[vertex]});
  first.triangles={
      {{{first.vertices[0].key,first.vertices[2].key,first.vertices[1].key}},owner},
      {{{first.vertices[3].key,first.vertices[1].key,first.vertices[2].key}},owner}};
  first.dependency_blocks={
      {tetra::WorldTetAddress::root(2U),3U},
      {tetra::WorldTetAddress::root(1U),3U}};
  auto reordered=first;
  std::ranges::reverse(reordered.vertices);
  std::ranges::reverse(reordered.triangles);
  std::ranges::reverse(reordered.triangles.front().vertices);
  std::ranges::reverse(reordered.dependency_blocks);
  CHECK(reordered.canonical_hash()==first.canonical_hash());

  tetra::WorldDerivedSurfaceSnapshot later=first;
  later.id={tetra::WorldTetAddress::root(4U),3U};
  tetra::WorldRevisionManifest manifest(2U,1U,{}, {}, {},
      {later,first},{{tetra::WorldTetAddress::root(5U),3U},
                     {tetra::WorldTetAddress::root(3U),3U}});
  REQUIRE(manifest.surfaces().size()==2U);
  CHECK(manifest.surfaces()[0]->id==first.id);
  CHECK(manifest.surfaces()[1]->id==later.id);
  REQUIRE(manifest.removed_surfaces().size()==2U);
  CHECK(manifest.removed_surfaces()[0]<manifest.removed_surfaces()[1]);
  CHECK(manifest.metrics().changed_surfaces==2U);
  CHECK(manifest.metrics().removed_surfaces==2U);
}

TEST_CASE("root seam surface ownership is the lowest canonical incident address") {
  using Face=std::array<tetra::WorldVertexKey,3>;
  std::map<Face,std::vector<tetra::WorldTetAddress>> incidents;
  constexpr std::array<std::array<std::size_t,3>,4> face_corners{{
      {{1U,2U,3U}},{{0U,2U,3U}},{{0U,1U,3U}},{{0U,1U,2U}}}};
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root){
    const auto address=tetra::WorldTetAddress::root(root);
    const auto keys=tetra::world_tetrahedron_vertex_keys(address);
    for(const auto corners:face_corners){
      Face face{{keys[corners[0]],keys[corners[1]],keys[corners[2]]}};
      std::ranges::sort(face);incidents[face].push_back(address);
    }
  }
  std::size_t seam_faces{};
  std::optional<tetra::WorldTetAddress> first_owner;
  for(const auto& [face,face_incidents]:incidents){
    (void)face;
    if(face_incidents.size()<2U)continue;
    ++seam_faces;
    auto request_order=face_incidents;
    const auto expected=*std::ranges::min_element(request_order);
    if(!first_owner)first_owner=expected;
    CHECK(tetra::world_shared_entity_owner(request_order)==expected);
    std::ranges::reverse(request_order);
    CHECK(tetra::world_shared_entity_owner(request_order)==expected);
  }
  CHECK(seam_faces>1U);
  REQUIRE(first_owner);
  auto descendant=*first_owner;
  for(unsigned int depth=0;depth<4U;++depth)descendant=descendant.child(0U);
  CHECK(tetra::hierarchy_block_id(descendant,3U).prefix==descendant.ancestor(3U));
  CHECK(tetra::hierarchy_block_id(descendant.ancestor(3U),3U).prefix==
        tetra::WorldTetAddress::root(first_owner->root_id()));
  CHECK(tetra::hierarchy_block_id(descendant,2U).prefix==descendant.ancestor(2U));
}

TEST_CASE("maximum-depth sparse world remains bounded and root-seam keys stay exact") {
  std::vector<tetra::WorldTetAddress> targets;
  for(std::uint8_t root=0;root<tetra::bcc_root_tetrahedron_count;++root){
    auto address=tetra::WorldTetAddress::root(root);
    for(unsigned int depth=0;depth<tetra::maximum_world_red_depth;++depth)
      address=address.child(static_cast<std::uint8_t>((root+depth*5U)%8U));
    targets.push_back(address);
  }
  const auto checkpoint=tetra::make_sparse_world_cut_checkpoint(targets,3U,9U);
  tetra::WorldCutDirectory directory(checkpoint);
  CHECK(checkpoint.metrics.maximum_depth==36U);
  CHECK(checkpoint.metrics.blocks<=12U*(1U+tetra::maximum_world_red_depth/3U));
  CHECK(directory.metrics().blocks==checkpoint.metrics.blocks);
  CHECK(directory.metrics().retained_bytes==checkpoint.metrics.retained_bytes);
  for(const auto target:targets){
    const auto lookup=directory.lookup(target);
    REQUIRE(lookup);
    CHECK(lookup.logical_owner==target);
    CHECK(directory.vertex_keys(target)==tetra::world_tetrahedron_vertex_keys(target));
  }
  for(const auto& face:tetra::bcc_root_face_adjacency()){
    if(face.boundary())continue;
    const auto first=tetra::world_tetrahedron_vertex_keys(
        tetra::WorldTetAddress::root(face.root));
    const auto second=tetra::world_tetrahedron_vertex_keys(
        tetra::WorldTetAddress::root(face.neighbour_root));
    std::array<std::uint8_t,3> corners{};std::size_t output{};
    for(std::uint8_t corner=0;corner<4U;++corner)
      if(corner!=face.local_face)corners[output++]=corner;
    CHECK(tetra::world_face_key(first[corners[0]],first[corners[1]],first[corners[2]])==
          tetra::world_face_key(second[face.neighbour_corner_permutation[0]],
                                second[face.neighbour_corner_permutation[1]],
                                second[face.neighbour_corner_permutation[2]]));
  }
}

TEST_CASE("single-root blocked views exactly reproduce monolithic BCC address sets") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  for(unsigned int generation=0;generation<4U;++generation)
    mesh.refine_all_binary();
  REQUIRE(mesh.logical_red_owners().size()>=4096U);

  std::vector<tetra::TetId> resident_red;
  for(const auto& layer:mesh.layers())for(const auto& record:layer.tetrahedra)
    if(record.transition_parent==tetra::invalid_tet&&
       tetra::tet_depth(record.address)%3U==0U)
      resident_red.push_back(record.address);
  std::ranges::sort(resident_red);
  auto logical=std::vector<tetra::TetId>(
      mesh.logical_red_owners().begin(),mesh.logical_red_owners().end());
  auto conforming=std::vector<tetra::TetId>(
      mesh.conforming_volume().addresses().begin(),
      mesh.conforming_volume().addresses().end());
  std::ranges::sort(logical);
  std::ranges::sort(conforming);

  std::map<tetra::VertexId,tetra::WorldVertexKey> vertex_keys;
  for(const auto& layer:mesh.layers())for(const auto& record:layer.tetrahedra){
    if(record.transition_parent!=tetra::invalid_tet||
       tetra::tet_depth(record.address)%3U!=0U)
      continue;
    const auto world=tetra::world_tet_address(record.address);
    const auto geometry=tetra::world_tetrahedron_geometry(mesh,world);
    const auto keys=tetra::world_tetrahedron_vertex_keys(mesh,world);
    for(std::size_t corner=0;corner<4U;++corner){
      const auto expected=mesh.vertices()[record.vertices[corner]];
      CHECK(geometry[corner].x==doctest::Approx(expected.x).epsilon(1.0e-14));
      CHECK(geometry[corner].y==doctest::Approx(expected.y).epsilon(1.0e-14));
      CHECK(geometry[corner].z==doctest::Approx(expected.z).epsilon(1.0e-14));
      const auto [found,inserted]=vertex_keys.emplace(record.vertices[corner],keys[corner]);
      if(!inserted)CHECK(found->second==keys[corner]);
    }
    CHECK(tetra::world_edge_key(keys[0],keys[1])==
          tetra::world_edge_key(keys[1],keys[0]));
    CHECK(tetra::world_face_key(keys[0],keys[1],keys[2])==
          tetra::world_face_key(keys[2],keys[0],keys[1]));
  }

  std::uint64_t logical_hash{},conforming_hash{};
  for(const unsigned int generations:{3U,4U,5U}){
    const auto blocked=tetra::BlockedHierarchyView::build(mesh,generations);
    auto blocked_resident=blocked.resident_red().reconstructed_sources();
    auto blocked_logical=blocked.logical_cut().reconstructed_sources();
    auto reverse_logical=blocked.logical_cut().reconstructed_sources(true);
    auto blocked_conforming=blocked.conforming_volume().reconstructed_sources();
    auto reverse_conforming=blocked.conforming_volume().reconstructed_sources(true);
    const auto permuted_sources=[](const tetra::BlockedAddressSet& set){
      std::vector<std::size_t> order(set.blocks.size());
      std::iota(order.begin(),order.end(),0U);
      // Deterministic coprime-key ordering simulates arbitrary block
      // compaction without making the regression probabilistic.
      std::ranges::sort(order,[count=order.size()](std::size_t first,std::size_t second){
        return ((first*2654435761ULL+17ULL)%std::max<std::size_t>(count,1U))<
               ((second*2654435761ULL+17ULL)%std::max<std::size_t>(count,1U));
      });
      std::vector<tetra::TetId> result;
      result.reserve(set.source_addresses.size());
      for(const auto block_index:order){
        const auto& block=set.blocks[block_index];
        for(std::size_t offset=block.count;offset>0U;--offset)
          result.push_back(set.source_addresses[block.begin+offset-1U]);
      }
      return result;
    };
    auto permuted_logical=permuted_sources(blocked.logical_cut());
    auto permuted_conforming=permuted_sources(blocked.conforming_volume());
    std::ranges::sort(blocked_resident);
    std::ranges::sort(blocked_logical);
    std::ranges::sort(reverse_logical);
    std::ranges::sort(blocked_conforming);
    std::ranges::sort(reverse_conforming);
    std::ranges::sort(permuted_logical);
    std::ranges::sort(permuted_conforming);
    CAPTURE(generations);
    CAPTURE(blocked.metrics().blocks);
    CAPTURE(blocked.metrics().maximum_block_entries);
    CAPTURE(blocked.metrics().retained_bytes);
    CHECK(blocked_resident==resident_red);
    CHECK(blocked_logical==logical);
    CHECK(reverse_logical==logical);
    CHECK(permuted_logical==logical);
    CHECK(blocked_conforming==conforming);
    CHECK(reverse_conforming==conforming);
    CHECK(permuted_conforming==conforming);
    CHECK(blocked.metrics().resident_red_records==resident_red.size());
    CHECK(blocked.metrics().logical_owners==logical.size());
    CHECK(blocked.metrics().conforming_cells==conforming.size());
    CHECK(blocked.metrics().blocks>=12U);
    CHECK(blocked.metrics().maximum_block_entries>0U);
    std::size_t expected_terminal=1U;
    for(unsigned int generation=0;generation<generations;++generation)
      expected_terminal*=8U;
    CHECK(blocked.metrics().full_block_terminal_capacity==expected_terminal);
    CHECK(blocked.metrics().full_block_hierarchy_capacity==
          (expected_terminal*8U-1U)/7U);
    CHECK(blocked.metrics().maximum_lookup_comparisons>0U);
    for(const auto owner:blocked.logical_cut().owner_addresses){
      const auto* block=blocked.logical_cut().find_block(owner);
      REQUIRE(block!=nullptr);
      CHECK(block->count>0U);
    }
    if(logical_hash==0U){
      logical_hash=blocked.logical_cut().canonical_hash();
      conforming_hash=blocked.conforming_volume().canonical_hash();
    }else{
      CHECK(blocked.logical_cut().canonical_hash()==logical_hash);
      CHECK(blocked.conforming_volume().canonical_hash()==conforming_hash);
    }
  }
}

TEST_CASE("blocked hierarchy inspection leaves production terrain geometry unchanged") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  static_cast<void>(tetra::refine_to_sphere(mesh,terrain,camera,40.0,16));
  const auto prepare=[&]{
    return tetra_viewer::prepare_scene(
        mesh,terrain,tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::variational_smooth,true,false,true,false,
        false,false,1.0,tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  };
  const auto before=prepare();
  const auto before_hash=tetra_viewer::surface_geometry_hashes(before);
  std::uint64_t field_hash=1469598103934665603ULL;
  for(const auto point:mesh.vertices()){
    const auto bits=std::bit_cast<std::uint64_t>(terrain.signed_distance(point));
    field_hash^=bits;field_hash*=1099511628211ULL;
  }
  for(const unsigned int generations:{3U,4U,5U}){
    const auto blocked=tetra::BlockedHierarchyView::build(mesh,generations);
    CHECK(blocked.metrics().logical_owners==mesh.logical_red_owners().size());
    CHECK(blocked.metrics().conforming_cells==mesh.conforming_volume().size());
  }
  const auto after=prepare();
  CHECK(tetra_viewer::surface_geometry_hashes(after)==before_hash);
  std::uint64_t repeated_field_hash=1469598103934665603ULL;
  for(const auto point:mesh.vertices()){
    const auto bits=std::bit_cast<std::uint64_t>(terrain.signed_distance(point));
    repeated_field_hash^=bits;repeated_field_hash*=1099511628211ULL;
  }
  CHECK(repeated_field_hash==field_hash);
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("headless world block benchmark selects the measured bounded layout") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script("benchmark-world-blocks",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"block_generations\":3,\"exact\":true")!=std::string::npos);
  CHECK(text.find("\"block_generations\":4,\"exact\":true")!=std::string::npos);
  CHECK(text.find("\"block_generations\":5,\"exact\":true")!=std::string::npos);
  CHECK(text.find("\"full_block_hierarchy_capacity\":4681")!=std::string::npos);
  CHECK(text.find("\"full_block_hierarchy_capacity\":37449")!=std::string::npos);
  CHECK(text.find("\"event\":\"world_block_selection\",\"selected_generations\":3")!=
        std::string::npos);
  CHECK(text.find("\"all_exact\":true")!=std::string::npos);
}

TEST_CASE("headless world directory benchmark streams maximum-depth sparse terrain") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script("benchmark-world-directory",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"event\":\"world_directory_benchmark\"")!=std::string::npos);
  CHECK(text.find("\"maximum_red_depth\":38")!=std::string::npos);
  CHECK(text.find("\"world_extent_metres\":12742000")!=std::string::npos);
  CHECK(text.find("\"second_loaded_blocks\":0")==std::string::npos);
  CHECK(text.find("\"second_evicted_blocks\":0")==std::string::npos);
  CHECK(text.find("\"geometry_hash\":0")==std::string::npos);
  CHECK(text.find("\"reload_exact\":true")!=std::string::npos);
}

TEST_CASE("24-tet half-edge cube matches the paper construction") {
  const auto mesh = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  CHECK(mesh.subdivision_method() == tetra::SubdivisionMethod::maubach_halfedge_24);
  CHECK(mesh.vertices().size() == 15);
  CHECK(mesh.layers().size() == 1);
  CHECK(mesh.layers()[0].tetrahedra.size() == 24);
  CHECK(mesh.active_leaves().size() == 24);

  std::vector<std::uint64_t> cube_edges;
  for (std::uint8_t root = 0; root < 24; ++root) {
    const auto id = tetra::make_tet_id(root, 1);
    const auto& tet = mesh.tetrahedron(id);
    CHECK(tet.address == id);
    CHECK(tet.vertices[0] < 8);
    CHECK(tet.vertices[1] >= 9);
    CHECK(tet.vertices[1] < 15);
    CHECK(tet.vertices[2] == 8);
    CHECK(tet.vertices[3] < 8);
    const auto a = mesh.vertices()[tet.vertices[0]];
    const auto b = mesh.vertices()[tet.vertices[3]];
    const auto delta = b - a;
    CHECK(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z == doctest::Approx(1.0));
    const auto first = std::min(tet.vertices[0], tet.vertices[3]);
    const auto second = std::max(tet.vertices[0], tet.vertices[3]);
    cube_edges.push_back((static_cast<std::uint64_t>(first) << 32U) | second);
    CHECK(mesh.signed_volume(id) == doctest::Approx(1.0 / 24.0));
  }
  std::ranges::sort(cube_edges);
  CHECK(cube_edges.size() == 24);
  for (std::size_t edge = 0; edge < cube_edges.size(); edge += 2)
    CHECK(cube_edges[edge] == cube_edges[edge + 1]);
  CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("24-tet reflected ordering supports local deep Maubach refinement") {
  auto mesh = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  auto address = tetra::make_tet_id(0, 1);
  for (unsigned int depth = 0; depth < 20; ++depth) {
    mesh.refine_selected_binary({address});
    address = tetra::tet_child(address, false);
    CHECK(tetra::tet_depth(address) == depth + 1);
    CHECK(mesh.tetrahedron(address).address == address);
  }
  CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("every 24-tet root diamond can be refined locally") {
  for (std::uint8_t root = 0; root < 24; ++root) {
    auto mesh = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
    mesh.refine_selected_binary({tetra::make_tet_id(root, 1)});
    CAPTURE(root);
    CHECK(mesh.active_leaves().size() == 26);
    CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
    CHECK(mesh.has_positive_active_volumes());
    CHECK(mesh.has_conforming_active_faces());
  }
}

TEST_CASE("24-tet hierarchy is deterministic across repeated refinement") {
  auto first = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  auto second = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  for (int pass = 0; pass < 6; ++pass) {
    first.refine_all_binary();
    second.refine_all_binary();
  }
  CHECK(first.active_leaves() == second.active_leaves());
  CHECK(first.active_leaves().size() == 24 * 64);
  CHECK(first.layers().size() == second.layers().size());
  for (std::size_t depth = 0; depth < first.layers().size(); ++depth) {
    const auto& a = first.layers()[depth].tetrahedra;
    const auto& b = second.layers()[depth].tetrahedra;
    CHECK(a.size() == b.size());
    for (std::size_t index = 0; index < a.size(); ++index) {
      CHECK(a[index].address == b[index].address);
      CHECK(a[index].vertices == b[index].vertices);
    }
  }
  CHECK(first.has_conforming_active_faces());
}

TEST_CASE("longest-edge refinement bisects a centre-star edge face-to-face") {
  auto mesh = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::longest_edge_bisection);
  CHECK(mesh.subdivision_method() == tetra::SubdivisionMethod::longest_edge_bisection);
  CHECK(mesh.active_leaves().size() == 12);
  mesh.refine_selected_binary({tetra::make_tet_id(0, 1)});
  // The face diagonal is a boundary edge shared by the two roots carrying
  // that face, so its complete edge star is bisected together.
  CHECK(mesh.active_leaves().size() == 14);
  CHECK(mesh.vertices().size() == 10);
  CHECK(mesh.vertices().back().x == doctest::Approx(0.5));
  CHECK(mesh.vertices().back().y == doctest::Approx(0.5));
  CHECK(mesh.vertices().back().z == doctest::Approx(0.0));
  CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("longest-edge hierarchy supports deterministic deep local refinement") {
  auto first = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::longest_edge_bisection);
  auto second = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::longest_edge_bisection);
  auto first_address = tetra::make_tet_id(0, 1);
  auto second_address = first_address;
  for (unsigned int depth = 0; depth < 12; ++depth) {
    first.refine_selected_binary({first_address});
    second.refine_selected_binary({second_address});
    first_address = tetra::tet_child(first_address, false);
    second_address = tetra::tet_child(second_address, false);
    CHECK(first.tetrahedron(first_address).address == first_address);
    CHECK(second.tetrahedron(second_address).address == second_address);
  }
  CHECK(first.active_leaves() == second.active_leaves());
  CHECK(first.total_active_volume() == doctest::Approx(1.0));
  CHECK(first.has_positive_active_volumes());
  CHECK(first.has_symmetric_active_adjacency());
  CHECK(first.has_conforming_active_faces());
}

TEST_CASE("longest-edge and Maubach produce distinct dual-contour surfaces") {
  auto maubach = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  auto longest = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::longest_edge_bisection);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(maubach, sphere, camera, 80.0, 3));
  static_cast<void>(tetra::refine_to_sphere(longest, sphere, camera, 80.0, 3));
  const auto maubach_scene = tetra_viewer::prepare_scene(
      maubach, sphere, tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside, true, false, true, false);
  const auto longest_scene = tetra_viewer::prepare_scene(
      longest, sphere, tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside, true, false, true, false);
  REQUIRE(maubach_scene.triangle_vertices.size() == longest_scene.triangle_vertices.size());
  bool different = false;
  for (std::size_t index = 0; index < maubach_scene.triangle_vertices.size() && !different; ++index)
    for (std::size_t axis = 0; axis < 3; ++axis)
      different |= maubach_scene.triangle_vertices[index].position[axis] !=
                   longest_scene.triangle_vertices[index].position[axis];
  CHECK(different);
}

TEST_CASE("paper-derived octasection methods write eight packed children per parent") {
  for(const auto method:{tetra::SubdivisionMethod::bey_red_fixed,
                         tetra::SubdivisionMethod::bey_red_shortest,
                         tetra::SubdivisionMethod::eight_tetrahedra_longest_edge}){
    auto mesh=tetra::TetMesh::make_unit_cube(method);
    mesh.refine_selected_binary({tetra::make_tet_id(0,1)});
    CHECK(mesh.active_leaves().size()==48);
    CHECK(mesh.layers().size()==4);
    CHECK(mesh.layers()[3].tetrahedra.size()==48);
    for(const auto id:mesh.active_leaves())CHECK(tetra::tet_depth(id)==3);
    CHECK(mesh.total_active_volume()==doctest::Approx(1.0));
    CHECK(mesh.has_positive_active_volumes());
    CHECK(mesh.has_conforming_active_faces());
  }
}

TEST_CASE("fixed Bey descendants retain the Kuhn orthoscheme shape class") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bey_red_fixed);
  for(unsigned int generation=0;generation<4;++generation)mesh.refine_all_binary();
  double minimum_normalized_volume=std::numeric_limits<double>::infinity();
  for(const auto id:mesh.active_leaves()){
    const auto& tet=mesh.tetrahedron(id);
    double longest_squared=0.0;
    for(std::size_t first=0;first<4;++first)for(std::size_t second=first+1;second<4;++second){
      const auto delta=mesh.vertices()[tet.vertices[second]]-mesh.vertices()[tet.vertices[first]];
      longest_squared=std::max(longest_squared,delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    }
    minimum_normalized_volume=std::min(
        minimum_normalized_volume,mesh.signed_volume(id)/std::pow(longest_squared,1.5));
  }
  CHECK(mesh.active_leaves().size()==6U*8U*8U*8U*8U);
  CAPTURE(minimum_normalized_volume);
  CHECK(minimum_normalized_volume>0.03);
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("BCC red-green refinement creates conforming terminal transition families") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  const auto result=tetra::refine_to_sphere(mesh,sphere,camera,28.0,9);
  CHECK(result.iterations==3);
  std::size_t green=0;
  for(const auto id:mesh.active_leaves()){
    green+=mesh.tetrahedron(id).transition_parent!=tetra::invalid_tet?1U:0U;
    CHECK(mesh.refinement_depth(id)<=9);
  }
  CHECK(green>0);
  CHECK(mesh.total_active_volume()==doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("logical and conforming cut contracts separate BCC owners from transitions") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));

  const auto logical=mesh.logical_cut();
  const auto conforming=mesh.conforming_volume();
  const std::array<tetra::Triangle,1> triangles{};
  const tetra::SurfaceOnlyView surface(mesh,triangles);
  REQUIRE(conforming.current());
  REQUIRE_FALSE(logical.owners.empty());
  REQUIRE(conforming.size()>=logical.owners.size());
  for(std::size_t index=0;index<conforming.size();++index){
    const auto cell=conforming.cell(index);
    CHECK(std::binary_search(logical.owners.begin(),logical.owners.end(),cell.logical_owner));
    CHECK(cell.transition==(cell.address!=cell.logical_owner));
  }

  mesh.reset_active_hierarchy();
  CHECK_FALSE(conforming.current());
  CHECK_THROWS_AS(static_cast<void>(conforming.cell(0)),std::logic_error);
  CHECK_THROWS_AS(static_cast<void>(conforming.addresses()),std::logic_error);
  CHECK_THROWS_AS(static_cast<void>(conforming.size()),std::logic_error);
  CHECK_FALSE(surface.current());
  CHECK_THROWS_AS(static_cast<void>(surface.triangles()),std::logic_error);
}

TEST_CASE("BCC red families coarsen transactionally and reuse resident storage") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto parent=mesh.logical_cut().owners.front();
  const auto parent_vertices=mesh.tetrahedron(parent).vertices;
  constexpr std::array<std::array<std::size_t,2>,6> edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  REQUIRE(mesh.refine_selected_binary({parent}));
  std::array<std::uint32_t,6> refined_edge_references{};
  for(std::size_t edge=0;edge<edges.size();++edge){
    refined_edge_references[edge]=mesh.logical_edge_reference_count(
        parent_vertices[edges[edge][0]],parent_vertices[edges[edge][1]]);
    CHECK(refined_edge_references[edge]>=1U);
  }
  const auto refined=mesh.logical_cut();
  for(std::uint32_t child=0;child<8U;++child)
    REQUIRE(std::binary_search(refined.owners.begin(),refined.owners.end(),
        tetra::make_tet_id(tetra::tet_root(parent),
                           (tetra::tet_path(parent)<<3U)|child)));

  const auto resident_red_tetrahedra=[&mesh]{
    std::size_t count{};
    for(const auto& layer:mesh.layers())
      count+=static_cast<std::size_t>(std::ranges::count_if(
          layer.tetrahedra,[](const auto& tet){
            return tet.transition_parent==tetra::invalid_tet;
          }));
    return count;
  };
  const auto resident_tetrahedra=mesh.tetrahedron_count();
  const auto resident_red=resident_red_tetrahedra();
  const auto resident_vertices=mesh.vertices().size();
  const auto refined_revision=mesh.revision();
  const auto old_view=mesh.conforming_volume();
  REQUIRE(mesh.coarsen_selected_red({parent}));
  for(std::size_t edge=0;edge<edges.size();++edge)
    CHECK(mesh.logical_edge_reference_count(
        parent_vertices[edges[edge][0]],parent_vertices[edges[edge][1]])+1U==
        refined_edge_references[edge]);
  const auto coarsened=mesh.logical_cut();
  CHECK(mesh.revision()==refined_revision+1U);
  CHECK_FALSE(old_view.current());
  CHECK(std::binary_search(coarsened.owners.begin(),coarsened.owners.end(),parent));
  CHECK(coarsened.owners.size()+7U==refined.owners.size());
  CHECK(resident_red_tetrahedra()==resident_red);
  CHECK(mesh.vertices().size()==resident_vertices);
  CHECK(mesh.total_active_volume()==doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_conforming_active_faces());

  REQUIRE(mesh.refine_selected_binary({parent}));
  for(std::size_t edge=0;edge<edges.size();++edge)
    CHECK(mesh.logical_edge_reference_count(
        parent_vertices[edges[edge][0]],parent_vertices[edges[edge][1]])==
        refined_edge_references[edge]);
  CHECK(mesh.logical_cut().owners==refined.owners);
  CHECK(mesh.tetrahedron_count()==resident_tetrahedra);
  CHECK(resident_red_tetrahedra()==resident_red);
  CHECK(mesh.vertices().size()==resident_vertices);
  REQUIRE(mesh.coarsen_selected_red({parent}));
  CHECK(mesh.logical_cut().owners==coarsened.owners);
  mesh.reset_active_hierarchy();
  for(std::size_t edge=0;edge<edges.size();++edge)
    CHECK(mesh.logical_edge_reference_count(
        parent_vertices[edges[edge][0]],parent_vertices[edges[edge][1]])==0U);
}

TEST_CASE("BCC desired-edge references cross root boundaries without duplicate edges") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto roots=mesh.logical_red_owners();
  std::array<tetra::TetId,2> selected{{tetra::invalid_tet,tetra::invalid_tet}};
  std::array<tetra::VertexId,2> shared{};
  for(std::size_t first=0;first<roots.size()&&selected[0]==tetra::invalid_tet;++first)
    for(std::size_t second=first+1;second<roots.size();++second){
      std::vector<tetra::VertexId> intersection;
      auto a=mesh.tetrahedron(roots[first]).vertices;
      auto b=mesh.tetrahedron(roots[second]).vertices;
      std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());
      std::set_intersection(a.begin(),a.end(),b.begin(),b.end(),
                            std::back_inserter(intersection));
      if(intersection.size()!=2U)continue;
      selected={roots[first],roots[second]};
      shared={intersection[0],intersection[1]};
      break;
    }
  REQUIRE(selected[0]!=tetra::invalid_tet);
  REQUIRE(mesh.refine_selected_binary({selected[0],selected[1]}));
  CHECK(mesh.logical_edge_reference_count(shared[0],shared[1])>=2U);
  CHECK(std::ranges::any_of(mesh.conforming_volume().addresses(),[&](tetra::TetId address){
    const auto cell=mesh.tetrahedron(address);
    return cell.transition_parent!=tetra::invalid_tet&&
        tetra::tet_root(cell.transition_parent)!=tetra::tet_root(selected[0])&&
        tetra::tet_root(cell.transition_parent)!=tetra::tet_root(selected[1]);
  }));
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("BCC coarsening rejects incomplete families without observable mutation") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto parent=mesh.logical_cut().owners.front();
  const auto before_owners=mesh.logical_cut().owners;
  const auto before_addresses=std::vector<tetra::TetId>(
      mesh.conforming_volume().addresses().begin(),mesh.conforming_volume().addresses().end());
  const auto before_revision=mesh.revision();
  const auto before_tetrahedra=mesh.tetrahedron_count();
  const auto before_vertices=mesh.vertices().size();
  CHECK_FALSE(mesh.coarsen_selected_red({parent}));
  CHECK(mesh.revision()==before_revision);
  CHECK(mesh.logical_cut().owners==before_owners);
  CHECK(std::ranges::equal(mesh.conforming_volume().addresses(),before_addresses));
  CHECK(mesh.tetrahedron_count()==before_tetrahedra);
  CHECK(mesh.vertices().size()==before_vertices);
}

TEST_CASE("BCC coarsening rejects a complete family blocked by neighbouring conformity") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto roots=mesh.logical_cut().owners;
  REQUIRE(mesh.refine_selected_binary(roots));
  const auto before=mesh.logical_cut().owners;
  const auto revision=mesh.revision();
  const auto blocked=std::ranges::find_if(roots,[&](tetra::TetId parent){
    bool complete=true;
    for(std::uint32_t child=0;child<8U;++child)
      complete&=std::binary_search(before.begin(),before.end(),tetra::make_tet_id(
          tetra::tet_root(parent),(tetra::tet_path(parent)<<3U)|child));
    return complete&&!mesh.can_coarsen_selected_red({parent});
  });
  REQUIRE(blocked!=roots.end());
  CHECK_FALSE(mesh.coarsen_selected_red({*blocked}));
  CHECK(mesh.revision()==revision);
  CHECK(mesh.logical_cut().owners==before);
}

TEST_CASE("packed pinned-descendant summaries block and release derefinement") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto parent=mesh.logical_cut().owners.front();
  REQUIRE(mesh.refine_selected_binary({parent}));
  const auto child=tetra::make_tet_id(
      tetra::tet_root(parent),tetra::tet_path(parent)<<3U);
  REQUIRE(std::binary_search(mesh.logical_red_owners().begin(),
                             mesh.logical_red_owners().end(),child));
  const auto pin_revision=mesh.pinned_revision();
  REQUIRE(mesh.set_logical_owner_pinned(child,true));
  CHECK(mesh.pinned_revision()==pin_revision+1U);
  CHECK(mesh.logical_owner_pinned(child));
  CHECK(mesh.has_pinned_descendant(child));
  CHECK(mesh.has_pinned_descendant(parent));
  CHECK_FALSE(mesh.can_coarsen_selected_red({parent}));

  tetra::Sphere shape;
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  configuration.candidate_traversal=tetra::CandidateTraversal::hierarchy_bounds;
  tetra::AdaptationPlanningCache cache;
  static_cast<void>(tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,4,&cache));
  const auto& parent_layer=cache.layers[tetra::tet_depth(parent)];
  const auto found=std::lower_bound(
      parent_layer.addresses.begin(),parent_layer.addresses.end(),parent);
  REQUIRE(found!=parent_layer.addresses.end());
  const auto index=static_cast<std::size_t>(found-parent_layer.addresses.begin());
  CHECK((parent_layer.pinned_descendant_words[index/64U]&
         (std::uint64_t{1}<<(index%64U)))!=0U);
  CHECK(cache.pinned_revision==mesh.pinned_revision());

  REQUIRE(mesh.set_logical_owner_pinned(child,false));
  CHECK_FALSE(mesh.logical_owner_pinned(child));
  CHECK_FALSE(mesh.has_pinned_descendant(parent));
}

TEST_CASE("logical red owners retain packed transition mask and stencil state") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  REQUIRE(mesh.refine_selected_binary({mesh.logical_red_owners().front()}));
  const auto& owners=mesh.logical_red_owners();
  const auto masks=mesh.logical_midpoint_masks();
  const auto stencils=mesh.logical_stencil_choices();
  REQUIRE(masks.size()==owners.size());
  REQUIRE(stencils.size()==owners.size());
  std::vector<tetra::TetId> transition_parents;
  for(std::size_t index=0;index<mesh.conforming_volume().size();++index){
    const auto cell=mesh.conforming_volume().cell(index);
    if(cell.transition)transition_parents.push_back(cell.logical_owner);
  }
  std::sort(transition_parents.begin(),transition_parents.end());
  transition_parents.erase(
      std::unique(transition_parents.begin(),transition_parents.end()),
      transition_parents.end());
  constexpr std::array<std::array<std::size_t,2>,6> owner_edges{{
      {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  for(std::size_t index=0;index<owners.size();++index){
    CHECK(masks[index]<63U);
    CHECK(stencils[index]==masks[index]);
    const bool has_transition=std::binary_search(
        transition_parents.begin(),transition_parents.end(),owners[index]);
    CHECK(has_transition==(masks[index]!=0U));
    const auto& vertices=mesh.tetrahedron(owners[index]).vertices;
    for(std::size_t edge=0;edge<owner_edges.size();++edge)
      if(mesh.logical_edge_reference_count(
             vertices[owner_edges[edge][0]],vertices[owner_edges[edge][1]])>0U)
        CHECK((masks[index]&(1U<<edge))!=0U);
  }
}

TEST_CASE("unchanged logical owners retain identical packed green ranges") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  REQUIRE(mesh.refine_selected_binary({mesh.logical_red_owners().front()}));
  struct DerivedRange { std::uint64_t hash{}; std::vector<tetra::TetId> addresses; };
  std::map<tetra::TetId,DerivedRange> before;
  const auto capture=[&](auto& destination){
    const auto hashes=mesh.logical_derived_hashes();
    const auto offsets=mesh.logical_derived_offsets();
    const auto addresses=mesh.logical_derived_addresses();
    REQUIRE(hashes.size()==mesh.logical_red_owners().size());
    REQUIRE(offsets.size()==hashes.size()+1U);
    for(std::size_t owner=0;owner<hashes.size();++owner)
      destination.emplace(mesh.logical_red_owners()[owner],DerivedRange{
          hashes[owner],std::vector<tetra::TetId>(
              addresses.begin()+static_cast<std::ptrdiff_t>(offsets[owner]),
              addresses.begin()+static_cast<std::ptrdiff_t>(offsets[owner+1U]))});
  };
  capture(before);
  const auto next=std::ranges::find_if(mesh.logical_red_owners(),[](tetra::TetId owner){
    return tetra::tet_depth(owner)==3U;
  });
  REQUIRE(next!=mesh.logical_red_owners().end());
  REQUIRE(mesh.refine_selected_binary({*next}));
  std::map<tetra::TetId,DerivedRange> after;
  capture(after);
  std::size_t retained_green_owners{};
  for(const auto& [owner,range]:before){
    const auto found=after.find(owner);
    if(found==after.end()||range.hash==0U)continue;
    if(found->second.hash==range.hash&&found->second.addresses==range.addresses){
      CHECK_FALSE(std::binary_search(mesh.last_dirty_logical_owners().begin(),
                                     mesh.last_dirty_logical_owners().end(),owner));
      ++retained_green_owners;
    }
  }
  CHECK(retained_green_owners>0U);
  CHECK(mesh.last_bcc_update_metrics().green_records_generated<
        mesh.logical_derived_addresses().size());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("adaptation planner retains packed current mark and command layers") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere shape;
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  tetra::AdaptationPlanningCache cache;
  const auto verify=[&](const tetra::AdaptationPlan& plan){
    for(std::size_t index=1;index<plan.commands.size();++index)
      CHECK(tetra::tet_depth(plan.commands[index-1].logical_owner)>=
            tetra::tet_depth(plan.commands[index].logical_owner));
    std::size_t packed_addresses{};
    for(const auto& layer:cache.transaction_layers){
      packed_addresses+=layer.addresses.size();
      CHECK(layer.current_status_words.size()==(layer.addresses.size()+63U)/64U);
      CHECK(layer.desired_mark_words.size()==(layer.addresses.size()+63U)/64U);
      CHECK(layer.command_words.size()==(layer.addresses.size()+31U)/32U);
      for(std::size_t index=0;index<layer.addresses.size();++index){
        const auto command=std::ranges::find_if(plan.commands,[&](const auto& candidate){
          return candidate.logical_owner==layer.addresses[index];
        });
        const auto encoded=(layer.command_words[index/32U]>>((index%32U)*2U))&3U;
        const bool current=(layer.current_status_words[index/64U]&
                            (std::uint64_t{1}<<(index%64U)))!=0U;
        const bool desired=(layer.desired_mark_words[index/64U]&
                            (std::uint64_t{1}<<(index%64U)))!=0U;
        if(command==plan.commands.end()){
          CHECK(encoded==0U);CHECK_FALSE(current);CHECK_FALSE(desired);
        }else if(command->kind==tetra::AdaptationCommandKind::split){
          CHECK(encoded==1U);CHECK_FALSE(current);CHECK(desired);
        }else if(command->kind==tetra::AdaptationCommandKind::merge){
          CHECK(encoded==2U);CHECK(current);CHECK_FALSE(desired);
        }
      }
    }
    CHECK(packed_addresses>=mesh.logical_red_owners().size());
  };

  const auto split=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,0,&cache);
  REQUIRE(split.planned_splits>0U);
  CHECK(split.requested_splits>=split.planned_splits);
  verify(split);
  std::vector<std::array<std::size_t,4>> capacities;
  for(const auto& layer:cache.transaction_layers)
    capacities.push_back({layer.addresses.capacity(),
                          layer.current_status_words.capacity(),
                          layer.desired_mark_words.capacity(),
                          layer.command_words.capacity()});
  const auto repeated=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,0,&cache);
  CHECK(repeated.commands==split.commands);
  verify(repeated);
  for(std::size_t depth=0;depth<cache.transaction_layers.size();++depth){
    const auto& layer=cache.transaction_layers[depth];
    const std::array<std::size_t,4> retained{{
        layer.addresses.capacity(),layer.current_status_words.capacity(),
        layer.desired_mark_words.capacity(),layer.command_words.capacity()}};
    CHECK(capacities[depth]==retained);
  }

  REQUIRE(tetra::commit_adaptation(mesh,split,configuration).status==
          tetra::AdaptationCommitStatus::committed);
  camera.position={0.5,0.5,100.0};
  camera.forward={0.0,0.0,1.0};
  const auto merge=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,0,&cache);
  REQUIRE(merge.planned_merges>0U);
  CHECK(merge.requested_merges>=merge.planned_merges);
  verify(merge);
}

TEST_CASE("adaptation planning is budgeted non-mutating and revision checked") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=2;
  const auto revision=mesh.revision();
  const auto plan=tetra::plan_adaptation(mesh,sphere,camera,28.0,9,configuration,17);
  CHECK(mesh.revision()==revision);
  CHECK(plan.base_revision==revision);
  CHECK(plan.field_revision==17);
  CHECK(plan.requested_splits>=plan.planned_splits);
  CHECK(plan.requested_splits>configuration.operation_budget);
  CHECK(plan.planned_splits<=configuration.operation_budget);
  REQUIRE(plan.planned_splits>0);
  CHECK(plan.planned_merges==0);

  const auto wrong_field=tetra::commit_adaptation(mesh,plan,configuration,18);
  CHECK(wrong_field.status==tetra::AdaptationCommitStatus::stale_plan);
  CHECK(wrong_field.operations.requested_splits==plan.requested_splits);
  CHECK(wrong_field.operations.admissible_splits==plan.planned_splits);
  CHECK(wrong_field.operations.stale_splits==plan.planned_splits);
  CHECK(wrong_field.operations.committed_splits==0U);
  CHECK(wrong_field.operations.rejected_splits==0U);
  CHECK(wrong_field.operations.conformity_expanded_splits==0U);
  auto changed_configuration=configuration;
  changed_configuration.operation_budget=3;
  CHECK(tetra::commit_adaptation(mesh,plan,changed_configuration,17).status==
        tetra::AdaptationCommitStatus::stale_plan);
  CHECK(mesh.revision()==revision);

  REQUIRE(mesh.refine_selected_binary({mesh.logical_cut().owners.back()}));
  const auto owners_after_external_change=mesh.logical_cut().owners;
  const auto stale=tetra::commit_adaptation(mesh,plan,configuration,17);
  CHECK(stale.status==tetra::AdaptationCommitStatus::stale_plan);
  CHECK(mesh.logical_cut().owners==owners_after_external_change);
}

TEST_CASE("canceled adaptation planning never commits partial topology") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  tetra::AdaptationConfiguration configuration;
  tetra::AdaptationPlanningCache cache;
  std::stop_source cancellation;
  cancellation.request_stop();
  const auto revision=mesh.revision();
  const auto owners=mesh.logical_cut().owners;
  const auto plan=tetra::plan_adaptation(
      mesh,sphere,camera,4.0,9U,configuration,0U,&cache,
      cancellation.get_token());
  CHECK(plan.canceled);
  CHECK(plan.commands.empty());
  CHECK(mesh.revision()==revision);
  CHECK(mesh.logical_cut().owners==owners);
  const auto result=tetra::adapt_to_surface(
      mesh,sphere,camera,4.0,9U,configuration,0U,&cache,
      cancellation.get_token());
  CHECK(result.canceled);
  CHECK(result.status==tetra::AdaptationCommitStatus::no_change);
  CHECK(result.resulting_revision==revision);
  CHECK(mesh.revision()==revision);
  CHECK(mesh.logical_cut().owners==owners);
}

TEST_CASE("adaptation commit metrics cover the complete operation lifecycle") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=1U;
  const auto split_plan=tetra::plan_adaptation(
      mesh,sphere,camera,28.0,6,configuration,0);
  REQUIRE(split_plan.requested_splits>0U);
  REQUIRE(split_plan.planned_splits==1U);
  const auto split=tetra::commit_adaptation(mesh,split_plan,configuration,0);
  REQUIRE(split.status==tetra::AdaptationCommitStatus::committed);
  CHECK(split.operations.requested_splits==split_plan.requested_splits);
  CHECK(split.operations.admissible_splits==split_plan.planned_splits);
  CHECK(split.operations.committed_splits==split.accepted_splits);
  CHECK(split.operations.committed_splits>=split.operations.admissible_splits);
  CHECK(split.operations.conformity_expanded_splits==
        split.operations.committed_splits-split.operations.admissible_splits);
  CHECK(split.operations.committed_merges==0U);

  tetra::AdaptationPlan rejected_plan;
  rejected_plan.base_revision=mesh.revision();
  rejected_plan.field_revision=0U;
  rejected_plan.configuration=configuration;
  rejected_plan.requested_splits=1U;
  rejected_plan.planned_splits=1U;
  rejected_plan.commands.push_back(
      {split_plan.commands.front().logical_owner,
       tetra::AdaptationCommandKind::split});
  const auto rejected=tetra::commit_adaptation(
      mesh,rejected_plan,configuration,0);
  CHECK(rejected.status==tetra::AdaptationCommitStatus::rejected);
  CHECK(rejected.operations.admissible_splits==1U);
  CHECK(rejected.operations.rejected_splits==1U);
  CHECK(rejected.operations.stale_splits==0U);
  CHECK(rejected.operations.committed_splits==0U);

  camera.position={0.5,0.5,100.0};
  camera.forward={0.0,0.0,-1.0};
  tetra::AdaptationPlanningCache cache;
  const auto merge_plan=tetra::plan_adaptation(
      mesh,sphere,camera,28.0,6,configuration,0,&cache);
  REQUIRE(merge_plan.planned_merges>0U);
  const auto merge=tetra::commit_adaptation(mesh,merge_plan,configuration,0);
  REQUIRE(merge.status==tetra::AdaptationCommitStatus::committed);
  CHECK(merge.operations.requested_merges==merge_plan.requested_merges);
  CHECK(merge.operations.admissible_merges==merge_plan.planned_merges);
  CHECK(merge.operations.committed_merges==merge.accepted_merges);
  CHECK(merge.operations.conformity_expanded_merges==
        merge.operations.committed_merges-merge.operations.admissible_merges);
  CHECK(merge.operations.committed_splits==0U);
}

TEST_CASE("mesh snapshot byte accounting follows live packed storage") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto snapshot_bytes=mesh.snapshot_copy_bytes();
  const auto initial_resident_bytes=mesh.resident_storage_bytes();
  CHECK(snapshot_bytes==sizeof(tetra::TetMesh));
  CHECK(initial_resident_bytes>snapshot_bytes);
  auto initial_copy=mesh;
  CHECK(initial_copy.snapshot_copy_bytes()==snapshot_bytes);
  CHECK(initial_copy.resident_storage_bytes()==initial_resident_bytes);
  CHECK(mesh.shares_storage_with(initial_copy));
  CHECK(mesh.storage_use_count()==2);
  const auto initial_revision=initial_copy.revision();
  const auto initial_owners=initial_copy.logical_cut().owners;
  REQUIRE(mesh.refine_selected_binary({mesh.logical_cut().owners.front()}));
  CHECK_FALSE(mesh.shares_storage_with(initial_copy));
  CHECK(initial_copy.revision()==initial_revision);
  CHECK(initial_copy.logical_cut().owners==initial_owners);
  const auto refined_bytes=mesh.resident_storage_bytes();
  CHECK(refined_bytes>initial_resident_bytes);
  auto refined_copy=mesh;
  CHECK(refined_copy.snapshot_copy_bytes()==snapshot_bytes);
  CHECK(refined_copy.resident_storage_bytes()==refined_bytes);
  CHECK(refined_copy.shares_storage_with(mesh));
}

TEST_CASE("incremental BCC adaptation splits nearby and derefines for a distant camera") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera near_camera;
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=4096;
  bool split=false;
  for(std::size_t step=0;step<16;++step){
    const auto result=tetra::adapt_to_surface(
        mesh,sphere,near_camera,28.0,9,configuration,1);
    if(result.status==tetra::AdaptationCommitStatus::no_change)break;
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    split|=result.accepted_splits>0;
  }
  REQUIRE(split);
  const auto refined_count=mesh.logical_cut().owners.size();
  REQUIRE(refined_count>6);

  tetra::Camera far_camera=near_camera;
  far_camera.position={0.5,0.5,100.0};
  bool merged=false;
  for(std::size_t step=0;step<16;++step){
    const auto result=tetra::adapt_to_surface(
        mesh,sphere,far_camera,28.0,9,configuration,1);
    if(result.status==tetra::AdaptationCommitStatus::no_change)break;
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    merged|=result.accepted_merges>0;
  }
  CHECK(merged);
  CHECK(mesh.logical_cut().owners.size()<refined_count);
  CHECK(mesh.total_active_volume()==doctest::Approx(1.0));
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("accepted adaptation records replay and reverse the actual logical delta") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  const tetra::AdaptationConfiguration configuration;
  const auto source_logical=mesh.logical_cut().owners;
  const auto source_conforming=std::vector<tetra::TetId>(
      mesh.conforming_volume().addresses().begin(),mesh.conforming_volume().addresses().end());
  const auto plan=tetra::plan_adaptation(mesh,sphere,camera,28.0,3,configuration,9);
  const auto committed=tetra::commit_adaptation(mesh,plan,configuration,9);
  REQUIRE(committed.status==tetra::AdaptationCommitStatus::committed);
  REQUIRE_FALSE(committed.replay.forward_commands.empty());
  CHECK(committed.bcc_metrics.full_cut_cells_scanned>0);
  CHECK(committed.bcc_metrics.closure_cells_examined>0);
  CHECK(committed.bcc_metrics.logical_owners_changed>0);
  CHECK(committed.bcc_metrics.edge_tables_rebuilt==0);
  CHECK(committed.bcc_metrics.face_tables_rebuilt==0);
  CHECK(committed.replay.source_owner_hash==tetra::logical_owner_hash(source_logical));
  const auto target_logical=mesh.logical_cut().owners;
  const auto target_conforming=std::vector<tetra::TetId>(
      mesh.conforming_volume().addresses().begin(),mesh.conforming_volume().addresses().end());
  CHECK(committed.replay.target_owner_hash==tetra::logical_owner_hash(target_logical));

  const auto reversed=tetra::replay_adaptation(
      mesh,committed.replay,true,configuration,9);
  REQUIRE(reversed.status==tetra::AdaptationCommitStatus::committed);
  CHECK(mesh.logical_cut().owners==source_logical);
  CHECK(std::ranges::equal(mesh.conforming_volume().addresses(),source_conforming));

  const auto replayed=tetra::replay_adaptation(
      mesh,committed.replay,false,configuration,9);
  REQUIRE(replayed.status==tetra::AdaptationCommitStatus::committed);
  CHECK(mesh.logical_cut().owners==target_logical);
  CHECK(std::ranges::equal(mesh.conforming_volume().addresses(),target_conforming));
  CHECK(tetra::replay_adaptation(mesh,committed.replay,false,configuration,9).status==
        tetra::AdaptationCommitStatus::stale_plan);
}

TEST_CASE("incremental BCC reverse camera paths are deterministic and storage stable") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::AdaptationConfiguration configuration;
  const auto converge=[&]{
    for(std::size_t step=0;step<32;++step){
      const auto result=tetra::adapt_to_surface(
          mesh,terrain,camera,40.0,6,configuration,4);
      if(result.status==tetra::AdaptationCommitStatus::no_change)return;
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    }
    FAIL("incremental adaptation did not converge");
  };
  converge();
  const auto near_logical=mesh.logical_cut().owners;
  const auto near_conforming=std::vector<tetra::TetId>(
      mesh.conforming_volume().addresses().begin(),mesh.conforming_volume().addresses().end());
  const auto near_red_records=static_cast<std::size_t>(std::accumulate(
      mesh.layers().begin(),mesh.layers().end(),std::size_t{},
      [](std::size_t sum,const tetra::TetLayer& layer){
        return sum+static_cast<std::size_t>(std::ranges::count_if(
            layer.tetrahedra,[](const auto& tet){
              return tet.transition_parent==tetra::invalid_tet;
            }));
      }));
  const auto near_vertices=mesh.vertices().size();

  camera.position={-1.0,0.7,0.5};
  camera.forward={-1.0,0.0,0.0};
  converge();
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  converge();
  CHECK(mesh.logical_cut().owners==near_logical);
  CHECK(std::ranges::equal(mesh.conforming_volume().addresses(),near_conforming));
  const auto final_red_records=static_cast<std::size_t>(std::accumulate(
      mesh.layers().begin(),mesh.layers().end(),std::size_t{},
      [](std::size_t sum,const tetra::TetLayer& layer){
        return sum+static_cast<std::size_t>(std::ranges::count_if(
            layer.tetrahedra,[](const auto& tet){
              return tet.transition_parent==tetra::invalid_tet;
            }));
      }));
  CHECK(final_red_records==near_red_records);
  CHECK(mesh.vertices().size()==near_vertices);
}

TEST_CASE("incremental BCC adaptation honors a lowered depth limit before new splits") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));
  REQUIRE(std::ranges::any_of(mesh.logical_cut().owners,[](tetra::TetId owner){
    return tetra::tet_depth(owner)>3U;
  }));
  tetra::AdaptationConfiguration configuration;
  for(std::size_t step=0;step<16;++step){
    const auto result=tetra::adapt_to_surface(
        mesh,sphere,camera,28.0,3,configuration,2);
    if(result.status==tetra::AdaptationCommitStatus::no_change)break;
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    CHECK(result.accepted_splits==0);
  }
  for(const auto owner:mesh.logical_cut().owners)CHECK(tetra::tet_depth(owner)<=3U);
}

TEST_CASE("adaptation hysteresis guards both split and merge boundaries") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::exact_frustum;
  configuration.camera_lod_metric=tetra::CameraLodMetric::projected_diameter;
  const auto owner=mesh.logical_cut().owners.front();
  const double diameter=tetra::projected_tetrahedron_diameter(mesh,owner,camera);
  const double split_boundary=diameter/configuration.split_hysteresis;
  const auto guarded=tetra::plan_adaptation(
      mesh,sphere,camera,split_boundary*1.001,3,configuration);
  CHECK(std::ranges::none_of(guarded.commands,[&](const auto& command){
    return command.kind==tetra::AdaptationCommandKind::split&&
           command.logical_owner==owner;
  }));
  const auto splitting=tetra::plan_adaptation(
      mesh,sphere,camera,split_boundary*0.999,3,configuration);
  CHECK(std::ranges::any_of(splitting.commands,[&](const auto& command){
    return command.kind==tetra::AdaptationCommandKind::split&&
           command.logical_owner==owner;
  }));

  REQUIRE(mesh.refine_selected_binary({owner}));
  const double parent_diameter=tetra::projected_tetrahedron_diameter(mesh,owner,camera);
  const double merge_boundary=parent_diameter/configuration.merge_hysteresis;
  const auto retained=tetra::plan_adaptation(
      mesh,sphere,camera,merge_boundary*0.999,3,configuration);
  CHECK(std::ranges::none_of(retained.commands,[&](const auto& command){
    return command.kind==tetra::AdaptationCommandKind::merge&&
           command.logical_owner==owner;
  }));
  const auto merging=tetra::plan_adaptation(
      mesh,sphere,camera,merge_boundary*1.001,3,configuration);
  CHECK(std::ranges::any_of(merging.commands,[&](const auto& command){
    return command.kind==tetra::AdaptationCommandKind::merge&&
           command.logical_owner==owner;
  }));
}

TEST_CASE("adaptation split commands win a simultaneous merge opportunity") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const auto roots=mesh.logical_cut().owners;
  const auto merge_parent=roots.back();
  REQUIRE(mesh.refine_selected_binary({merge_parent}));
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={-1.0,0.5,0.5};
  const auto direction=terrain.centre-camera.position;
  const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                direction.z*direction.z);
  camera.forward=direction/length;
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=4096;
  configuration.split_hysteresis=1.01;
  configuration.merge_hysteresis=1.0;

  bool exercised_conflict=false;
  for(double threshold=1.0;threshold<=1000.0;threshold+=1.0){
    const auto plan=tetra::plan_adaptation(
        mesh,terrain,camera,threshold,6,configuration);
    const bool has_split=std::ranges::any_of(plan.commands,[](const auto& command){
      return command.kind==tetra::AdaptationCommandKind::split;
    });
    const auto logical=mesh.logical_cut();
    bool complete_family=true;
    for(std::uint32_t child=0;child<8U;++child)
      complete_family&=std::binary_search(logical.owners.begin(),logical.owners.end(),
          tetra::make_tet_id(tetra::tet_root(merge_parent),
              (tetra::tet_path(merge_parent)<<3U)|child));
    const bool has_merge_opportunity=complete_family&&
        tetra::projected_tetrahedron_diameter(mesh,merge_parent,camera)<
            threshold*configuration.merge_hysteresis;
    if(!has_split||!has_merge_opportunity)continue;
    exercised_conflict=true;
    CHECK(std::ranges::all_of(plan.commands,[](const auto& command){
      return command.kind==tetra::AdaptationCommandKind::split;
    }));
    break;
  }
  CHECK(exercised_conflict);
}

TEST_CASE("one hundred incremental camera updates remain conforming and allocation stable") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  std::size_t stable_red_records{},stable_vertices{};
  for(std::size_t update=0;update<100;++update){
    const double angle=2.0*std::acos(-1.0)*static_cast<double>(update%20U)/20.0;
    camera.position={0.5+1.7*std::cos(angle),0.8+0.35*std::sin(angle*2.0),
                     0.5+1.7*std::sin(angle)};
    const auto direction=terrain.centre-camera.position;
    const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                  direction.z*direction.z);
    camera.forward=direction/length;
    bool converged=false;
    for(std::size_t transaction=0;transaction<24;++transaction){
      const auto result=tetra::adapt_to_surface(
          mesh,terrain,camera,40.0,6,configuration,3);
      if(result.status==tetra::AdaptationCommitStatus::no_change){converged=true;break;}
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    }
    REQUIRE(converged);
    REQUIRE(mesh.has_positive_active_volumes());
    REQUIRE(mesh.has_conforming_active_faces());
    if(update==19U){
      stable_vertices=mesh.vertices().size();
      for(const auto& layer:mesh.layers())
        stable_red_records+=static_cast<std::size_t>(std::ranges::count_if(
            layer.tetrahedra,[](const auto& tet){
              return tet.transition_parent==tetra::invalid_tet;
            }));
    }
  }
  std::size_t final_red_records{};
  for(const auto& layer:mesh.layers())
    final_red_records+=static_cast<std::size_t>(std::ranges::count_if(
        layer.tetrahedra,[](const auto& tet){
          return tet.transition_parent==tetra::invalid_tet;
        }));
  CHECK(mesh.vertices().size()==stable_vertices);
  CHECK(final_red_records==stable_red_records);

  const auto& logical=mesh.logical_red_owners();
  CHECK(std::ranges::is_sorted(logical));
  CHECK(std::adjacent_find(logical.begin(),logical.end())==logical.end());
  std::vector<tetra::TetId> derived;
  derived.reserve(mesh.conforming_volume().size());
  for(const auto address:mesh.conforming_volume().addresses()){
    const auto& record=mesh.tetrahedron(address);
    derived.push_back(record.transition_parent==tetra::invalid_tet
                          ?address:record.transition_parent);
  }
  std::sort(derived.begin(),derived.end());
  derived.erase(std::unique(derived.begin(),derived.end()),derived.end());
  CHECK(derived==logical);
  double logical_volume{};
  for(const auto owner:logical){
    logical_volume+=mesh.signed_volume(owner);
    for(unsigned int ancestor_depth=tetra::tet_depth(owner);ancestor_depth>=3U;){
      ancestor_depth-=3U;
      const auto ancestor=tetra::make_tet_id(
          tetra::tet_root(owner),tetra::tet_path(owner)>>
              (tetra::tet_depth(owner)-ancestor_depth));
      CHECK_FALSE(std::binary_search(logical.begin(),logical.end(),ancestor));
      if(ancestor_depth==0U)break;
    }
  }
  CHECK(logical_volume==doctest::Approx(1.0).epsilon(1.0e-10));
}

TEST_CASE("operation budgets converge to one deterministic cut with progress-only commits") {
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  std::vector<std::pair<std::uint64_t,std::uint64_t>> hashes;
  for(const std::uint32_t budget:{1U,7U,4096U}){
    auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
    tetra::AdaptationConfiguration configuration;
    configuration.operation_budget=budget;
    tetra::AdaptationPlanningCache cache;
    bool converged=false;
    for(std::size_t transaction=0;transaction<512U;++transaction){
      const auto before=tetra::logical_owner_hash(mesh.logical_cut().owners);
      const auto result=tetra::adapt_to_surface(
          mesh,terrain,camera,28.0,6,configuration,4,&cache);
      const auto after=tetra::logical_owner_hash(mesh.logical_cut().owners);
      if(result.status==tetra::AdaptationCommitStatus::no_change){
        CHECK(after==before);converged=true;break;
      }
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
      CHECK(after!=before);
      CHECK((result.accepted_splits==0U)!=(result.accepted_merges==0U));
      CHECK(mesh.has_positive_active_volumes());
      CHECK(mesh.has_conforming_active_faces());
    }
    REQUIRE(converged);
    hashes.emplace_back(tetra::logical_owner_hash(mesh.logical_cut().owners),
                        tetra::logical_owner_hash(mesh.conforming_volume().addresses()));
  }
  CHECK(std::ranges::all_of(hashes,[&](const auto& hash){return hash==hashes.front();}));
}

TEST_CASE("singular camera locations and supported depth extremes remain finite and conforming") {
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  const std::array positions{
      tetra::Vec3{0.5,0.5,0.5},tetra::Vec3{0.0,0.0,0.0},
      tetra::Vec3{0.5,0.5,0.0},tetra::Vec3{1.0,1.0,1.0}};
  for(const auto position:positions){
    auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
    tetra::Camera camera;
    camera.position=position;
    camera.forward={0.0,0.0,-1.0};
    tetra::AdaptationConfiguration configuration;
    tetra::AdaptationPlanningCache cache;
    for(std::size_t transaction=0;transaction<64U;++transaction){
      const auto result=tetra::adapt_to_surface(
          mesh,terrain,camera,40.0,6,configuration,2,&cache);
      if(result.status==tetra::AdaptationCommitStatus::no_change)break;
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    }
    CAPTURE(position.x);CAPTURE(position.y);CAPTURE(position.z);
    CHECK(mesh.has_positive_active_volumes());
    CHECK(mesh.has_conforming_active_faces());
    for(const auto vertex:mesh.vertices()){
      CHECK(std::isfinite(vertex.x));CHECK(std::isfinite(vertex.y));
      CHECK(std::isfinite(vertex.z));
    }
  }

  for(const unsigned int maximum_depth:{0U,1U,16U,32U}){
    auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
    tetra::Camera away;
    away.position={0.5,0.5,3.0};away.forward={0.0,0.0,1.0};
    tetra::AdaptationConfiguration configuration;
    configuration.camera_lod_policy=tetra::CameraLodPolicy::exact_frustum;
    configuration.camera_lod_metric=tetra::CameraLodMetric::projected_diameter;
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,away,28.0,maximum_depth,configuration,6);
    CAPTURE(maximum_depth);
    CHECK(result.status==tetra::AdaptationCommitStatus::no_change);
    CHECK(mesh.logical_cut().owners.size()==12U);
    CHECK(mesh.has_conforming_active_faces());
  }
}

TEST_CASE("bounded hierarchy traversal converges to the exhaustive active-cut oracle") {
  auto exhaustive=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  auto bounded=exhaustive;
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::AdaptationConfiguration scan_configuration;
  scan_configuration.camera_lod_policy=tetra::CameraLodPolicy::exact_frustum;
  scan_configuration.camera_lod_metric=tetra::CameraLodMetric::projected_diameter;
  tetra::AdaptationConfiguration bounded_configuration=scan_configuration;
  bounded_configuration.candidate_traversal=tetra::CandidateTraversal::hierarchy_bounds;
  tetra::AdaptationPlanningCache bounded_cache;
  const auto converge=[&](tetra::TetMesh& mesh,const auto& configuration,
                          tetra::AdaptationPlanningCache* cache){
    for(std::size_t step=0;step<24;++step){
      const auto result=tetra::adapt_to_surface(
          mesh,terrain,camera,40.0,6,configuration,12,cache);
      if(result.status==tetra::AdaptationCommitStatus::no_change)return;
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    }
    FAIL("candidate traversal did not converge");
  };
  converge(exhaustive,scan_configuration,nullptr);
  converge(bounded,bounded_configuration,&bounded_cache);
  CHECK(bounded.logical_cut().owners==exhaustive.logical_cut().owners);
  CHECK(std::ranges::equal(bounded.conforming_volume().addresses(),
                           exhaustive.conforming_volume().addresses()));

  const auto stationary=tetra::plan_adaptation(
      bounded,terrain,camera,40.0,6,bounded_configuration,12,&bounded_cache);
  CHECK(stationary.commands.empty());
  CHECK(stationary.logical_candidates==0);
  CHECK(stationary.field_classifications==0);
  CHECK(stationary.exact_field_evaluations==0);
  CHECK(stationary.projection_evaluations==0);
  CHECK(stationary.hierarchy_nodes_visited==0);
  CHECK(stationary.classification_ms==0.0);
  CHECK(stationary.family_resolution_ms==0.0);
  CHECK(stationary.summary_build_ms==0.0);

  camera.position={-1.0,0.7,0.5};
  const auto direction=terrain.centre-camera.position;
  const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                direction.z*direction.z);
  camera.forward=direction/(-length);
  const auto scan_plan=tetra::plan_adaptation(
      exhaustive,terrain,camera,40.0,9,scan_configuration,12);
  const auto bounded_plan=tetra::plan_adaptation(
      bounded,terrain,camera,40.0,9,bounded_configuration,12,&bounded_cache);
  CHECK(bounded_plan.commands==scan_plan.commands);
  CHECK(bounded_plan.summary_build_ms==0.0);
  CHECK(bounded_plan.hierarchy_nodes_visited>0);
  CHECK(bounded_plan.frustum_subtrees_rejected+bounded_plan.field_subtrees_rejected>0);
  CAPTURE(scan_plan.field_classifications);
  CAPTURE(bounded_plan.field_classifications);
  CAPTURE(scan_plan.exact_field_evaluations);
  CAPTURE(bounded_plan.exact_field_evaluations);
  CAPTURE(bounded_plan.exact_field_evaluations_avoided);
  CHECK(bounded_plan.exact_field_evaluations<=scan_plan.exact_field_evaluations);
  CHECK(bounded_plan.exact_field_evaluations_avoided>0);
}

TEST_CASE("incremental red-owner convergence covers the conforming-cell refinement oracle") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::exact_frustum;
  configuration.camera_lod_metric=tetra::CameraLodMetric::projected_diameter;
  configuration.split_hysteresis=1.0;
  configuration.operation_budget=4096;
  for(std::size_t step=0;step<64;++step){
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,16,configuration,5);
    if(result.status==tetra::AdaptationCommitStatus::no_change)break;
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    REQUIRE(step<63);
  }
  auto marked=tetra::mark_oversized_intersections(mesh,terrain,camera,28.0);
  std::vector<tetra::TetId> eligible_owners;
  for(const auto address:marked){
    const auto& record=mesh.tetrahedron(address);
    const auto owner=record.transition_parent==tetra::invalid_tet
        ?address:record.transition_parent;
    if(tetra::tet_depth(owner)+3U<=16U)eligible_owners.push_back(owner);
  }
  std::sort(eligible_owners.begin(),eligible_owners.end());
  eligible_owners.erase(std::unique(eligible_owners.begin(),eligible_owners.end()),
                        eligible_owners.end());
  CHECK(eligible_owners.empty());

  auto oracle=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  static_cast<void>(tetra::refine_to_sphere(
      oracle,terrain,camera,28.0*configuration.split_hysteresis,16));
  CHECK(oracle.logical_cut().owners==mesh.logical_cut().owners);
  CHECK(std::ranges::equal(oracle.conforming_volume().addresses(),
                           mesh.conforming_volume().addresses()));
}

TEST_CASE("adaptation summaries rebuild only for field or resident red changes") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  configuration.candidate_traversal=tetra::CandidateTraversal::hierarchy_bounds;
  configuration.operation_budget=4096;
  tetra::AdaptationPlanningCache cache;

  const auto initial=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,7,&cache);
  CHECK(cache.field_revision==7);
  CHECK(cache.resident_revision==mesh.resident_revision());
  REQUIRE(cache.resident_red_records>0);
  const auto initial_records=cache.resident_red_records;

  const auto unchanged=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,7,&cache);
  CHECK(unchanged.summary_build_ms==0.0);
  CHECK(cache.resident_red_records==initial_records);

  const auto changed_field=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,8,&cache);
  CHECK(cache.field_revision==8);
  CHECK(changed_field.exact_field_evaluations>=cache.resident_red_records);

  REQUIRE_FALSE(changed_field.commands.empty());
  const auto old_resident_revision=mesh.resident_revision();
  const auto split=tetra::commit_adaptation(mesh,changed_field,configuration,8);
  REQUIRE(split.status==tetra::AdaptationCommitStatus::committed);
  CHECK(mesh.resident_revision()>old_resident_revision);
  const auto after_growth=tetra::plan_adaptation(
      mesh,shape,camera,28.0,6,configuration,8,&cache);
  CHECK(cache.resident_revision==mesh.resident_revision());
  CHECK(cache.resident_red_records>initial_records);
  CHECK(after_growth.exact_field_evaluations>=cache.resident_red_records);

  tetra::Camera far_camera=camera;
  far_camera.position={0.5,0.5,100.0};
  const auto merge_plan=tetra::plan_adaptation(
      mesh,shape,far_camera,28.0,6,configuration,8,&cache);
  REQUIRE(merge_plan.planned_merges>0);
  const auto resident_before_merge=mesh.resident_revision();
  const auto resident_records_before_merge=cache.resident_red_records;
  const auto merge=tetra::commit_adaptation(mesh,merge_plan,configuration,8);
  REQUIRE(merge.status==tetra::AdaptationCommitStatus::committed);
  CHECK(mesh.resident_revision()==resident_before_merge);
  const auto after_green_regeneration=tetra::plan_adaptation(
      mesh,shape,far_camera,28.0,6,configuration,8,&cache);
  CHECK(cache.resident_revision==resident_before_merge);
  CHECK(cache.resident_red_records==resident_records_before_merge);
  CHECK(cache.field_revision==8);
  CHECK(cache.active_revision==mesh.revision());

  for(std::size_t depth=3;depth<cache.layers.size();++depth){
    const auto& child_layer=cache.layers[depth];
    for(std::size_t index=0;index<child_layer.addresses.size();++index){
      const auto child=child_layer.addresses[index];
      const auto parent=tetra::make_tet_id(
          tetra::tet_root(child),tetra::tet_path(child)>>3U);
      const auto& parent_layer=cache.layers[tetra::tet_depth(parent)];
      const auto found=std::lower_bound(
          parent_layer.addresses.begin(),parent_layer.addresses.end(),parent);
      REQUIRE(found!=parent_layer.addresses.end());
      REQUIRE(*found==parent);
      const auto parent_index=static_cast<std::size_t>(found-parent_layer.addresses.begin());
      CHECK(child_layer.spatial_minimum[index].x>=
            parent_layer.spatial_minimum[parent_index].x-1.0e-12);
      CHECK(child_layer.spatial_minimum[index].y>=
            parent_layer.spatial_minimum[parent_index].y-1.0e-12);
      CHECK(child_layer.spatial_minimum[index].z>=
            parent_layer.spatial_minimum[parent_index].z-1.0e-12);
      CHECK(child_layer.spatial_maximum[index].x<=
            parent_layer.spatial_maximum[parent_index].x+1.0e-12);
      CHECK(child_layer.spatial_maximum[index].y<=
            parent_layer.spatial_maximum[parent_index].y+1.0e-12);
      CHECK(child_layer.spatial_maximum[index].z<=
            parent_layer.spatial_maximum[parent_index].z+1.0e-12);
      CHECK(parent_layer.deepest_resident_depth[parent_index]>=
            child_layer.deepest_resident_depth[index]);
    }
  }
  for(const auto owner:mesh.logical_red_owners()){
    const auto& layer=cache.layers[tetra::tet_depth(owner)];
    const auto found=std::lower_bound(layer.addresses.begin(),layer.addresses.end(),owner);
    REQUIRE(found!=layer.addresses.end());
    REQUIRE(*found==owner);
    const auto index=static_cast<std::size_t>(found-layer.addresses.begin());
    CHECK(layer.deepest_active_depth[index]>=tetra::tet_depth(owner));
  }
}

TEST_CASE("bounded hierarchy traversal matches exhaustive traversal for every implicit shape") {
  tetra::Camera camera;
  camera.position={-0.4,0.9,1.6};
  camera.forward={0.5144957554275265,-0.2057983021710106,-0.8327549871121958};
  tetra::AdaptationConfiguration scan_configuration;
  scan_configuration.operation_budget=4096;
  auto bounded_configuration=scan_configuration;
  bounded_configuration.candidate_traversal=tetra::CandidateTraversal::hierarchy_bounds;

  for(const auto kind:tetra::implicit_shape_kinds){
    CAPTURE(tetra::implicit_shape_name(kind));
    tetra::Sphere shape;
    shape.kind=kind;
    shape.secondary=tetra::implicit_shape_default_secondary(kind);
    auto exhaustive=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
    auto bounded=exhaustive;
    tetra::AdaptationPlanningCache cache;
    for(std::size_t step=0;step<12;++step){
      const auto scan=tetra::adapt_to_surface(
          exhaustive,shape,camera,48.0,6,scan_configuration,3);
      const auto pruned=tetra::adapt_to_surface(
          bounded,shape,camera,48.0,6,bounded_configuration,3,&cache);
      REQUIRE(pruned.status==scan.status);
      CHECK(bounded.logical_cut().owners==exhaustive.logical_cut().owners);
      CHECK(std::ranges::equal(bounded.conforming_volume().addresses(),
                               exhaustive.conforming_volume().addresses()));
      if(scan.status==tetra::AdaptationCommitStatus::no_change)break;
      REQUIRE(scan.status==tetra::AdaptationCommitStatus::committed);
      REQUIRE(step<11);
    }
  }
}

TEST_CASE("spatial runs converge to each materialized LOD strategy's exact hashes") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::Camera camera;
  camera.position={-0.4,0.9,1.6};
  camera.forward={0.5144957554275265,-0.2057983021710106,-0.8327549871121958};
  for(const auto strategy:{tetra::LodUpdateStrategy::transactional_active_cut,
                           tetra::LodUpdateStrategy::saturated_clusters}){
    CAPTURE(tetra::strategy_key(strategy));
    tetra::AdaptationConfiguration scan_configuration;
    scan_configuration.lod_update=strategy;
    scan_configuration.operation_budget=4096;
    auto spatial_configuration=scan_configuration;
    spatial_configuration.candidate_traversal=tetra::CandidateTraversal::spatial_runs;
    auto scan=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
    auto spatial=scan;
    tetra::AdaptationPlanningCache spatial_cache;
    std::size_t run_tests{};
    for(std::size_t step=0;step<16;++step){
      const auto baseline=tetra::adapt_to_surface(
          scan,shape,camera,48.0,6,scan_configuration,7);
      const auto plan=tetra::plan_adaptation(
          spatial,shape,camera,48.0,6,spatial_configuration,7,&spatial_cache);
      run_tests+=plan.spatial_run_bound_tests;
      CHECK(plan.spatial_index_bytes>0);
      CHECK(plan.spatial_run_count>0);
      const auto indexed=tetra::commit_adaptation(
          spatial,plan,spatial_configuration,7);
      REQUIRE(indexed.status==baseline.status);
      CHECK(spatial.logical_cut().owners==scan.logical_cut().owners);
      CHECK(std::ranges::equal(spatial.conforming_volume().addresses(),
                               scan.conforming_volume().addresses()));
      if(baseline.status==tetra::AdaptationCommitStatus::no_change)break;
      REQUIRE(baseline.status==tetra::AdaptationCommitStatus::committed);
      REQUIRE(step<15);
    }
    CHECK(run_tests>0);
  }
}

TEST_CASE("sparse dense and hybrid closure commit identical deterministic cuts") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::Camera camera;
  tetra::AdaptationConfiguration sparse_configuration;
  sparse_configuration.operation_budget=4096;
  auto dense_configuration=sparse_configuration;
  dense_configuration.closure_execution=tetra::ClosureExecution::dense_level_sweep;
  auto hybrid_configuration=sparse_configuration;
  hybrid_configuration.closure_execution=tetra::ClosureExecution::hybrid;
  hybrid_configuration.hybrid_frontier_ratio=0.20;
  auto sparse=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  auto dense=sparse,hybrid=sparse;
  std::size_t dense_sweeps{},hybrid_work{};
  for(std::size_t step=0;step<12;++step){
    camera.position={0.5+0.04*static_cast<double>(step),0.7,
                     1.8-0.03*static_cast<double>(step)};
    const auto sparse_result=tetra::adapt_to_surface(
        sparse,shape,camera,48.0,6,sparse_configuration,3);
    const auto dense_result=tetra::adapt_to_surface(
        dense,shape,camera,48.0,6,dense_configuration,3);
    const auto hybrid_result=tetra::adapt_to_surface(
        hybrid,shape,camera,48.0,6,hybrid_configuration,3);
    REQUIRE(dense_result.status==sparse_result.status);
    REQUIRE(hybrid_result.status==sparse_result.status);
    CHECK(dense.logical_cut().owners==sparse.logical_cut().owners);
    CHECK(hybrid.logical_cut().owners==sparse.logical_cut().owners);
    CHECK(std::ranges::equal(dense.conforming_volume().addresses(),
                             sparse.conforming_volume().addresses()));
    CHECK(std::ranges::equal(hybrid.conforming_volume().addresses(),
                             sparse.conforming_volume().addresses()));
    dense_sweeps+=dense_result.bcc_metrics.dense_sweeps;
    hybrid_work+=hybrid_result.bcc_metrics.dense_sweeps+
                 hybrid_result.bcc_metrics.sparse_frontier_pops;
  }
  CHECK(dense_sweeps>0);
  CHECK(hybrid_work>0);

  const auto stale_plan=tetra::plan_adaptation(
      dense,shape,camera,48.0,9,dense_configuration,3);
  auto changed=dense_configuration;
  changed.hybrid_frontier_ratio=0.8;
  CHECK(tetra::commit_adaptation(dense,stale_plan,changed,3).status==
        tetra::AdaptationCommitStatus::stale_plan);
}

TEST_CASE("persistent schedulers match streamed hashes through motion reversals and teleports") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::AdaptationConfiguration streamed_configuration;
  streamed_configuration.operation_budget=64;
  auto queued_configuration=streamed_configuration;
  queued_configuration.update_scheduler=
      tetra::UpdateScheduler::persistent_split_merge_queues;
  auto blocks_configuration=streamed_configuration;
  blocks_configuration.update_scheduler=tetra::UpdateScheduler::hybrid_queued_blocks;
  auto streamed=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  auto queued=streamed,blocks=streamed;
  tetra::AdaptationPlanningCache queued_cache,blocks_cache;
  const std::array<tetra::Vec3,12> path{{
      {0.50,0.50,2.00},{0.51,0.50,1.95},{0.52,0.51,1.90},
      {2.00,2.00,3.00},{-1.00,0.20,2.50},{0.52,0.51,1.90},
      {0.51,0.50,1.95},{0.50,0.50,2.00},{0.50,0.50,2.00},
      {0.51,0.50,1.95},{0.52,0.51,1.90},{0.52,0.51,1.90}}};
  std::size_t pushes{},useful{},recomputations{},block_streams{},avoided{};
  const auto converge=[&](tetra::TetMesh& mesh,const auto& configuration,
                          tetra::AdaptationPlanningCache* cache,
                          const tetra::Camera& camera){
    for(std::size_t frame=0;frame<128U;++frame){
      const auto plan=tetra::plan_adaptation(
          mesh,shape,camera,48.0,6,configuration,5,cache);
      if(cache==&queued_cache){
        pushes+=plan.scheduler_queue_pushes;
        useful+=plan.scheduler_useful_pops;
        recomputations+=plan.scheduler_priority_recomputations;
        avoided+=plan.scheduler_candidates_avoided;
      }else if(cache==&blocks_cache)block_streams+=plan.scheduler_block_streams;
      const auto result=tetra::commit_adaptation(
          mesh,plan,configuration,5,cache);
      if(result.status==tetra::AdaptationCommitStatus::no_change)return true;
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    }
    return false;
  };
  for(const auto position:path){
    tetra::Camera camera;
    camera.position=position;
    const auto direction=shape.centre-position;
    const double length=std::sqrt(direction.x*direction.x+
                                  direction.y*direction.y+
                                  direction.z*direction.z);
    camera.forward=direction/length;
    REQUIRE(converge(streamed,streamed_configuration,nullptr,camera));
    REQUIRE(converge(queued,queued_configuration,&queued_cache,camera));
    REQUIRE(converge(blocks,blocks_configuration,&blocks_cache,camera));
    CHECK(queued.logical_cut().owners==streamed.logical_cut().owners);
    CHECK(blocks.logical_cut().owners==streamed.logical_cut().owners);
    CHECK(std::ranges::equal(queued.conforming_volume().addresses(),
                             streamed.conforming_volume().addresses()));
    CHECK(std::ranges::equal(blocks.conforming_volume().addresses(),
                             streamed.conforming_volume().addresses()));
  }
  CHECK(pushes>0);
  CHECK(useful>0);
  CHECK(recomputations>0);
  CHECK(recomputations<=useful);
  CHECK(block_streams>0);
  CHECK(avoided>0);
}

TEST_CASE("persistent schedulers seed the active cut once across camera requests") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=4096;
  configuration.update_scheduler=
      tetra::UpdateScheduler::persistent_split_merge_queues;
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::AdaptationPlanningCache cache;
  tetra::Camera first_camera;
  first_camera.position={0.5,0.5,1.5};

  const auto initial_owners=mesh.logical_red_owners().size();
  const auto first=tetra::plan_adaptation(
      mesh,shape,first_camera,48.0,3,configuration,9,&cache);
  REQUIRE_FALSE(first.commands.empty());
  CHECK(first.scheduler_seed_scans==1U);
  CHECK(first.scheduler_seed_candidates==initial_owners);
  CHECK(first.scheduler_queue_pushes==
        cache.split_queue.size()+cache.merge_queue.size());
  CHECK(cache.scheduler_seeded);
  const auto split_capacity=cache.split_queue.capacity();
  const auto merge_capacity=cache.merge_queue.capacity();

  auto moved_camera=first_camera;
  moved_camera.position.x+=0.05;
  const auto second=tetra::plan_adaptation(
      mesh,shape,moved_camera,48.0,3,configuration,9,&cache);
  CHECK(second.scheduler_seed_scans==0U);
  CHECK(second.scheduler_seed_candidates==0U);
  CHECK(second.scheduler_queue_pushes==0U);
  CHECK(cache.split_queue.capacity()==split_capacity);
  CHECK(cache.merge_queue.capacity()==merge_capacity);

  REQUIRE(tetra::commit_adaptation(mesh,second,configuration,9,&cache).status==
          tetra::AdaptationCommitStatus::committed);
  const auto after_commit=tetra::plan_adaptation(
      mesh,shape,moved_camera,48.0,3,configuration,9,&cache);
  CHECK(after_commit.scheduler_seed_scans==0U);
  CHECK(after_commit.scheduler_seed_candidates==0U);
  CHECK(after_commit.scheduler_incremental_candidates>0U);
  CHECK(after_commit.scheduler_conformity_candidates>0U);
  CHECK(after_commit.scheduler_queue_pushes>0U);

  tetra::AdaptationPlanningCache canceled_cache;
  std::stop_source stop;
  stop.request_stop();
  const auto canceled=tetra::plan_adaptation(
      mesh,shape,moved_camera,48.0,3,configuration,9,&canceled_cache,
      stop.get_token());
  CHECK(canceled.canceled);
  CHECK_FALSE(canceled_cache.scheduler_seeded);
  CHECK(canceled_cache.split_queue.empty());
  CHECK(canceled_cache.merge_queue.empty());
}

TEST_CASE("persistent scheduler narrows independent discovery after convergence") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::exact_frustum;
  configuration.camera_lod_metric=tetra::CameraLodMetric::projected_diameter;
  configuration.operation_budget=4096U;
  configuration.update_scheduler=
      tetra::UpdateScheduler::persistent_split_merge_queues;
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::AdaptationPlanningCache cache;
  tetra::Camera camera;
  camera.position={0.5,0.5,1.5};

  bool converged=false;
  for(std::size_t iteration=0;iteration<16U;++iteration){
    const auto plan=tetra::plan_adaptation(
        mesh,shape,camera,48.0,3,configuration,10,&cache);
    const auto result=tetra::commit_adaptation(
        mesh,plan,configuration,10,&cache);
    if(result.status==tetra::AdaptationCommitStatus::no_change){
      converged=true;
      break;
    }
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
  }
  REQUIRE(converged);
  const auto owner_count=mesh.logical_red_owners().size();
  CHECK(std::accumulate(cache.scheduler_active_depth_counts.begin(),
                        cache.scheduler_active_depth_counts.end(),
                        std::size_t{})==owner_count);

  const auto stationary=tetra::plan_adaptation(
      mesh,shape,camera,48.0,3,configuration,10,&cache);
  CHECK(stationary.commands.empty());
  CHECK(stationary.logical_candidates==0U);
  CHECK(stationary.scheduler_priority_recomputations==0U);
  const auto active_priority_count=static_cast<std::size_t>(std::ranges::count_if(
      cache.split_queue,[](const auto& entry){
        return entry.has_priority&&entry.last_priority>0.0;
      }));
  INFO("active split priorities after convergence: ",active_priority_count);
  CHECK(active_priority_count==0U);

  camera.position.x+=0.001;
  const auto moved=tetra::plan_adaptation(
      mesh,shape,camera,48.0,3,configuration,10,&cache);
  CHECK(moved.logical_candidates<owner_count);
  CHECK(moved.scheduler_candidates_avoided==owner_count-moved.logical_candidates);
  CHECK(moved.scheduler_candidates_avoided>0U);
  CHECK(cache.scheduler_entry_scratch.empty());

  const auto raised_depth=tetra::plan_adaptation(
      mesh,shape,camera,48.0,6,configuration,10,&cache);
  CHECK_FALSE(raised_depth.commands.empty());
  CHECK(raised_depth.scheduler_priority_recomputations>0U);
}

TEST_CASE("persistent scheduler reseeds once after a camera teleport") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=1U;
  configuration.update_scheduler=
      tetra::UpdateScheduler::persistent_split_merge_queues;
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::AdaptationPlanningCache cache;
  tetra::Camera camera;
  camera.position={0.5,0.5,2.0};

  const auto initial=tetra::plan_adaptation(
      mesh,shape,camera,48.0,3,configuration,12,&cache);
  REQUIRE(initial.scheduler_seed_scans==1U);
  CHECK(initial.scheduler_fallbacks==0U);

  camera.position={0.5,0.5,-2.0};
  camera.forward={0.0,0.0,1.0};
  const auto teleported=tetra::plan_adaptation(
      mesh,shape,camera,48.0,3,configuration,12,&cache);
  CHECK(teleported.scheduler_fallbacks==1U);
  CHECK(teleported.scheduler_seed_scans==1U);
  CHECK(teleported.scheduler_seed_candidates==mesh.logical_red_owners().size());
  CHECK(cache.scheduler_useful_pops_since_reseed==
        mesh.logical_red_owners().size());
  CHECK(cache.scheduler_stale_pops_since_reseed==0U);

  const auto continuation=tetra::plan_adaptation(
      mesh,shape,camera,48.0,3,configuration,12,&cache);
  CHECK(continuation.scheduler_fallbacks==0U);
  CHECK(continuation.scheduler_seed_scans==0U);
}

TEST_CASE("persistent scheduler reseeds after an excessive stale pop ratio") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=1U;
  configuration.update_scheduler=
      tetra::UpdateScheduler::persistent_split_merge_queues;
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::AdaptationPlanningCache cache;
  tetra::Camera camera;
  camera.position={0.5,0.5,1.5};

  const auto initial=tetra::plan_adaptation(
      mesh,shape,camera,48.0,3,configuration,13,&cache);
  REQUIRE(initial.scheduler_seed_scans==1U);
  for(std::size_t index=0;index<64U;++index)
    cache.split_queue.push_back({
        tetra::invalid_tet,mesh.revision(),0U,
        1.0e30+static_cast<double>(index),0.0,true});
  cache.scheduler_heaps_valid=false;

  const auto recovered=tetra::plan_adaptation(
      mesh,shape,camera,48.0,3,configuration,13,&cache);
  CHECK(recovered.scheduler_stale_pops==64U);
  CHECK(recovered.scheduler_fallbacks==1U);
  CHECK(recovered.scheduler_seed_scans==1U);
  CHECK(cache.scheduler_stale_pops_since_reseed==0U);
  CHECK(cache.scheduler_useful_pops_since_reseed==0U);
  CHECK(std::ranges::none_of(cache.split_queue,[](const auto& entry){
    return entry.address==tetra::invalid_tet;
  }));

  const auto continuation=tetra::plan_adaptation(
      mesh,shape,camera,48.0,3,configuration,13,&cache);
  CHECK(continuation.scheduler_fallbacks==0U);
  CHECK(continuation.scheduler_seed_scans==0U);
}

TEST_CASE("persistent scheduler enqueues only committed families and conformity neighbours") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::exact_frustum;
  configuration.camera_lod_metric=tetra::CameraLodMetric::projected_diameter;
  configuration.operation_budget=4096U;
  configuration.update_scheduler=
      tetra::UpdateScheduler::persistent_split_merge_queues;
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::AdaptationPlanningCache cache;
  tetra::Camera near_camera;
  near_camera.position={0.5,0.5,1.5};

  const auto split_plan=tetra::plan_adaptation(
      mesh,shape,near_camera,48.0,3,configuration,11,&cache);
  REQUIRE_FALSE(split_plan.commands.empty());
  const auto split=tetra::commit_adaptation(
      mesh,split_plan,configuration,11,&cache);
  REQUIRE(split.status==tetra::AdaptationCommitStatus::committed);
  REQUIRE(split.accepted_splits>0U);
  CHECK(cache.pending_scheduler_incremental_candidates>0U);
  CHECK(cache.pending_scheduler_conformity_candidates>=
        mesh.last_dirty_logical_owners().size());
  CHECK(cache.pending_scheduler_queue_pushes>0U);
  const auto queued=[&](const auto& queue,tetra::TetId address){
    return std::ranges::any_of(queue,[&](const auto& entry){
      return entry.address==address;
    });
  };
  for(const auto& command:split.replay.forward_commands){
    if(command.kind!=tetra::AdaptationCommandKind::split)continue;
    CHECK(queued(cache.merge_queue,command.logical_owner));
    for(std::uint32_t child=0;child<8U;++child)
      CHECK(queued(cache.split_queue,tetra::make_tet_id(
          tetra::tet_root(command.logical_owner),
          (tetra::tet_path(command.logical_owner)<<3U)|child)));
  }
  const auto unique_queue=[&](const auto& queue,std::size_t membership_count){
    std::vector<tetra::TetId> addresses;
    addresses.reserve(queue.size());
    for(const auto& entry:queue)addresses.push_back(entry.address);
    std::sort(addresses.begin(),addresses.end());
    CHECK(std::adjacent_find(addresses.begin(),addresses.end())==addresses.end());
    CHECK(addresses.size()==membership_count);
  };
  unique_queue(cache.split_queue,cache.split_queue_membership.count);
  unique_queue(cache.merge_queue,cache.merge_queue_membership.count);

  const auto reported=tetra::plan_adaptation(
      mesh,shape,near_camera,48.0,3,configuration,11,&cache);
  CHECK(reported.scheduler_seed_scans==0U);
  CHECK(reported.scheduler_incremental_candidates>0U);
  CHECK(reported.scheduler_conformity_candidates>0U);
  CHECK(reported.scheduler_queue_pushes>0U);
  CHECK(cache.pending_scheduler_queue_pushes==0U);

  auto far_camera=near_camera;
  far_camera.position={8.0,8.0,8.0};
  bool merged=false;
  for(std::size_t iteration=0;iteration<8U&&!merged;++iteration){
    const auto plan=tetra::plan_adaptation(
        mesh,shape,far_camera,48.0,3,configuration,11,&cache);
    const auto result=tetra::commit_adaptation(
        mesh,plan,configuration,11,&cache);
    if(result.status==tetra::AdaptationCommitStatus::no_change)break;
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    if(result.accepted_merges==0U)continue;
    merged=true;
    for(const auto& command:result.replay.forward_commands)
      if(command.kind==tetra::AdaptationCommandKind::merge)
        CHECK(queued(cache.split_queue,command.logical_owner));
  }
  CHECK(merged);
  unique_queue(cache.split_queue,cache.split_queue_membership.count);
  unique_queue(cache.merge_queue,cache.merge_queue_membership.count);
}

TEST_CASE("persistent scheduler matches streamed oracle through small-motion stress") {
  tetra::Sphere shape;
  shape.kind=tetra::ImplicitShapeKind::perlin_terrain;
  shape.secondary=tetra::implicit_shape_default_secondary(shape.kind);
  tetra::AdaptationConfiguration streamed_configuration;
  streamed_configuration.operation_budget=64U;
  auto queued_configuration=streamed_configuration;
  queued_configuration.update_scheduler=
      tetra::UpdateScheduler::persistent_split_merge_queues;
  auto streamed=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  auto queued=streamed;
  tetra::AdaptationPlanningCache cache;
  std::size_t avoided{},fallbacks{};
  const auto converge_streamed=[&](const tetra::Camera& camera){
    for(std::size_t transaction=0;transaction<64U;++transaction){
      const auto result=tetra::adapt_to_surface(
          streamed,shape,camera,48.0,6,streamed_configuration,14);
      if(result.status==tetra::AdaptationCommitStatus::no_change)return true;
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    }
    return false;
  };
  const auto converge_queued=[&](const tetra::Camera& camera){
    for(std::size_t transaction=0;transaction<64U;++transaction){
      const auto plan=tetra::plan_adaptation(
          queued,shape,camera,48.0,6,queued_configuration,14,&cache);
      avoided+=plan.scheduler_candidates_avoided;
      fallbacks+=plan.scheduler_fallbacks;
      const auto result=tetra::commit_adaptation(
          queued,plan,queued_configuration,14,&cache);
      if(result.status==tetra::AdaptationCommitStatus::no_change)return true;
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    }
    return false;
  };
  for(std::size_t update=0;update<100U;++update){
    const double angle=0.01*static_cast<double>(update);
    tetra::Camera camera;
    camera.position={0.5+1.5*std::sin(angle),
                     0.75+0.1*std::sin(2.0*angle),
                     0.5+1.5*std::cos(angle)};
    const auto direction=shape.centre-camera.position;
    const double length=std::sqrt(direction.x*direction.x+
                                  direction.y*direction.y+
                                  direction.z*direction.z);
    camera.forward=direction/length;
    REQUIRE(converge_streamed(camera));
    REQUIRE(converge_queued(camera));
    CHECK(queued.logical_cut().owners==streamed.logical_cut().owners);
    CHECK(std::ranges::equal(queued.conforming_volume().addresses(),
                             streamed.conforming_volume().addresses()));
    CHECK(cache.split_queue.size()==cache.split_queue_membership.count);
    CHECK(cache.merge_queue.size()==cache.merge_queue_membership.count);
  }
  CHECK(avoided>0U);
  CHECK(fallbacks==0U);
}

TEST_CASE("adaptation capabilities reject surface-only volume claims") {
  tetra::AdaptationConfiguration configuration;
  CHECK(tetra::valid(configuration));
  const auto volume=tetra::capabilities(tetra::LodUpdateStrategy::transactional_active_cut);
  CHECK(tetra::has_capability(volume,tetra::AdaptationCapability::conforming_volume));
  CHECK(tetra::has_capability(volume,tetra::AdaptationCapability::cutaway));
  const auto minimal=tetra::capabilities(tetra::LodUpdateStrategy::minimal_surface_hierarchy);
  CHECK(tetra::has_capability(minimal,tetra::AdaptationCapability::surface_extraction));
  CHECK_FALSE(tetra::has_capability(minimal,tetra::AdaptationCapability::conforming_volume));
  CHECK_FALSE(tetra::has_capability(minimal,tetra::AdaptationCapability::volume_export));
  configuration.merge_hysteresis=configuration.split_hysteresis;
  CHECK_FALSE(tetra::valid(configuration));
}

TEST_CASE("camera LOD compatibility matrix capability-gates every strategy axis") {
  constexpr std::array updates{
      tetra::LodUpdateStrategy::transactional_active_cut,
      tetra::LodUpdateStrategy::saturated_clusters,
      tetra::LodUpdateStrategy::relevant_surface_hierarchy,
      tetra::LodUpdateStrategy::minimal_surface_hierarchy,
      tetra::LodUpdateStrategy::on_demand_render_traversal,
      tetra::LodUpdateStrategy::full_rebuild_oracle};
  constexpr std::array schedulers{
      tetra::UpdateScheduler::classify_and_stream,
      tetra::UpdateScheduler::persistent_split_merge_queues,
      tetra::UpdateScheduler::hybrid_queued_blocks};
  constexpr std::array traversals{
      tetra::CandidateTraversal::active_cut_scan,
      tetra::CandidateTraversal::hierarchy_bounds,
      tetra::CandidateTraversal::spatial_runs};
  constexpr std::array policies{
      tetra::CameraLodPolicy::exact_frustum,
      tetra::CameraLodPolicy::guarded,
      tetra::CameraLodPolicy::guarded_recent,
      tetra::CameraLodPolicy::guarded_predicted};
  constexpr std::array metrics{
      tetra::CameraLodMetric::projected_diameter,
      tetra::CameraLodMetric::geometric_error};
  for(const auto update:updates){
    const bool materialized=
        update==tetra::LodUpdateStrategy::transactional_active_cut||
        update==tetra::LodUpdateStrategy::saturated_clusters;
    const bool cutaway=tetra::has_capability(
        tetra::capabilities(update),tetra::AdaptationCapability::cutaway);
    CHECK(cutaway==(materialized||
        update==tetra::LodUpdateStrategy::full_rebuild_oracle));
    for(const auto scheduler:schedulers)
      for(const auto traversal:traversals)
        for(const auto policy:policies)
          for(const auto metric:metrics){
            tetra::AdaptationConfiguration configuration;
            configuration.lod_update=update;
            configuration.update_scheduler=scheduler;
            configuration.candidate_traversal=traversal;
            configuration.camera_lod_policy=policy;
            configuration.camera_lod_metric=metric;
            const bool axes_available=materialized||
                (scheduler==tetra::UpdateScheduler::classify_and_stream&&
                 traversal==tetra::CandidateTraversal::active_cut_scan);
            CHECK(tetra::implemented(configuration)==axes_available);
          }
  }
}

TEST_CASE("saturated LOD plans complete red clusters from the maximum member error") {
  tetra::AdaptationConfiguration configuration;
  configuration.lod_update=tetra::LodUpdateStrategy::saturated_clusters;
  configuration.operation_budget=4096;
  REQUIRE(tetra::implemented(configuration));
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere shape{};
  const tetra::Camera camera{};

  const auto root_plan=tetra::plan_adaptation(
      mesh,shape,camera,28.0,9,configuration,4);
  REQUIRE(root_plan.supported);
  const auto root_count=static_cast<std::size_t>(std::ranges::count_if(
      mesh.layers().front().tetrahedra,
      [](const auto& record){return record.transition_parent==tetra::invalid_tet;}));
  REQUIRE(root_plan.planned_splits==root_count);
  CHECK(std::ranges::all_of(root_plan.commands,[](const auto& command){
    return command.kind==tetra::AdaptationCommandKind::split&&
        tetra::tet_depth(command.logical_owner)==0U;
  }));
  REQUIRE(tetra::commit_adaptation(mesh,root_plan,configuration,4).status==
          tetra::AdaptationCommitStatus::committed);

  const auto child_plan=tetra::plan_adaptation(
      mesh,shape,camera,28.0,9,configuration,4);
  REQUIRE(child_plan.supported);
  REQUIRE(child_plan.planned_splits>0);
  CHECK(child_plan.planned_splits%8U==0U);
  std::vector<tetra::TetId> parents;
  for(const auto& command:child_plan.commands){
    REQUIRE(command.kind==tetra::AdaptationCommandKind::split);
    parents.push_back(tetra::make_tet_id(
        tetra::tet_root(command.logical_owner),tetra::tet_path(command.logical_owner)>>3U));
  }
  std::sort(parents.begin(),parents.end());
  for(std::size_t begin=0;begin<parents.size();begin+=8U){
    REQUIRE(begin+8U<=parents.size());
    CHECK(std::ranges::all_of(std::span{parents}.subspan(begin,8U),
                             [&](tetra::TetId parent){return parent==parents[begin];}));
  }
}

TEST_CASE("fixed-field surface hierarchies are packed smaller and camera reusable") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere shape{};
  tetra::Camera camera{};
  REQUIRE(tetra::refine_to_sphere(mesh,shape,camera,28.0,9).iterations>0);
  tetra::FixedFieldSurfaceHierarchy hierarchy;
  REQUIRE(tetra::update_fixed_field_surface_hierarchy(hierarchy,mesh,shape,11));
  REQUIRE(hierarchy.rebuild_count==1);
  REQUIRE(hierarchy.relevant_clusters>0);
  REQUIRE(hierarchy.minimal_clusters>0);
  CHECK(hierarchy.minimal_clusters<hierarchy.relevant_clusters);
  CHECK(hierarchy.retained_bytes>0);

  const auto relevant=tetra::select_fixed_field_surface_cut(
      hierarchy,mesh,camera,40.0,9,
      tetra::LodUpdateStrategy::relevant_surface_hierarchy);
  const auto minimal=tetra::select_fixed_field_surface_cut(
      hierarchy,mesh,camera,40.0,9,
      tetra::LodUpdateStrategy::minimal_surface_hierarchy);
  REQUIRE_FALSE(relevant.empty());
  REQUIRE_FALSE(minimal.empty());
  CHECK_FALSE(tetra::extract_isosurface(mesh,shape,relevant).empty());
  CHECK_FALSE(tetra::extract_isosurface(mesh,shape,minimal).empty());
  const auto queried=tetra::query_relevant_surface_hierarchy(
      hierarchy,mesh,{0.4,0.4,0.4},{0.6,0.6,0.6});
  CHECK_FALSE(queried.empty());

  camera.position={0.2,0.7,2.0};
  static_cast<void>(tetra::select_fixed_field_surface_cut(
      hierarchy,mesh,camera,40.0,9,
      tetra::LodUpdateStrategy::relevant_surface_hierarchy));
  CHECK_FALSE(tetra::update_fixed_field_surface_hierarchy(hierarchy,mesh,shape,11));
  CHECK(hierarchy.rebuild_count==1);
  REQUIRE(tetra::update_fixed_field_surface_hierarchy(hierarchy,mesh,shape,12));
  CHECK(hierarchy.rebuild_count==2);
}

TEST_CASE("preorder surface traversal addresses children and renders without an active cut") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere shape{};
  const tetra::Camera camera{};
  REQUIRE(tetra::refine_to_sphere(mesh,shape,camera,28.0,9).iterations>0);
  tetra::FixedFieldSurfaceHierarchy fixed;
  REQUIRE(tetra::update_fixed_field_surface_hierarchy(fixed,mesh,shape,6));
  tetra::PreorderSurfaceHierarchy preorder;
  REQUIRE(tetra::update_preorder_surface_hierarchy(preorder,fixed));
  REQUIRE(preorder.addresses.size()==preorder.descendant_counts.size());
  REQUIRE(preorder.child_indices.size()==preorder.addresses.size()*8U);
  REQUIRE_FALSE(preorder.roots.empty());
  for(std::size_t index=0;index<preorder.addresses.size();++index){
    CHECK(index+preorder.descendant_counts[index]<preorder.addresses.size());
    for(std::uint32_t child=0;child<8U;++child){
      const auto child_index=preorder.child_indices[index*8U+child];
      if(child_index==std::numeric_limits<std::uint32_t>::max())continue;
      REQUIRE(child_index>index);
      CHECK(child_index<=index+preorder.descendant_counts[index]);
      CHECK(preorder.addresses[child_index]==tetra::make_tet_id(
          tetra::tet_root(preorder.addresses[index]),
          (tetra::tet_path(preorder.addresses[index])<<3U)|child));
    }
  }
  std::vector<tetra::Triangle> shallow_triangles,deep_triangles;
  const auto shallow=tetra::render_preorder_surface(
      preorder,mesh,shape,camera,400.0,9,shallow_triangles);
  const auto deep=tetra::render_preorder_surface(
      preorder,mesh,shape,camera,20.0,9,deep_triangles);
  REQUIRE(shallow.generated_triangles>0);
  REQUIRE(deep.generated_triangles>0);
  CHECK(deep.nodes_visited>=shallow.nodes_visited);
  CHECK(deep.selected_nodes>=shallow.selected_nodes);
  CHECK_FALSE(tetra::update_preorder_surface_hierarchy(preorder,fixed));
  CHECK(preorder.rebuild_count==1);
}

TEST_CASE("RSB supercube parity mapping fills exactly eight plus twenty-four plus twenty-four slots") {
  std::array<bool,56> occupied{};
  std::array<std::size_t,3> class_counts{};
  for(unsigned int x=0;x<4U;++x)
    for(unsigned int y=0;y<4U;++y)
      for(unsigned int z=0;z<4U;++z){
        const unsigned int odd=(x&1U)+(y&1U)+(z&1U);
        const auto location=tetra::rsb_supercube_location(
            {static_cast<double>(x)/2.0,static_cast<double>(y)/2.0,
             static_cast<double>(z)/2.0},0);
        if(odd==0U){CHECK_FALSE(location.valid);continue;}
        REQUIRE(location.valid);
        REQUIRE(location.slot<occupied.size());
        CHECK_FALSE(occupied[location.slot]);
        occupied[location.slot]=true;
        ++class_counts[location.diamond_class];
      }
  CHECK(std::ranges::all_of(occupied,[](bool value){return value;}));
  CHECK(class_counts==std::array<std::size_t,3>{{8U,24U,24U}});
}

TEST_CASE("packed layer layouts and kernel orders preserve classification hashes") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere shape{};
  tetra::Camera camera{};
  REQUIRE(tetra::refine_to_sphere(mesh,shape,camera,52.0,6).iterations>0);
  std::optional<std::uint64_t> topology_hash,classification_hash;
  for(const auto storage:{tetra::LayerStorage::flat_packed,
                          tetra::LayerStorage::mutable_macro_blocks,
                          tetra::LayerStorage::occupancy_bit_macro_blocks,
                          tetra::LayerStorage::address_runs}){
    for(const auto order:{tetra::KernelOrder::address_order,
                          tetra::KernelOrder::orientation_buckets,
                          tetra::KernelOrder::fused_macro_blocks}){
      CAPTURE(tetra::strategy_key(storage));
      CAPTURE(tetra::strategy_key(order));
      const auto experiment=tetra::build_layer_storage_experiment(
          mesh,shape,storage,order);
      REQUIRE_FALSE(experiment.canonical_addresses.empty());
      CHECK(experiment.metrics.live_bytes>0);
      CHECK(experiment.metrics.retained_bytes>=experiment.metrics.live_bytes);
      CHECK(experiment.metrics.candidate_throughput_per_second>0.0);
      if(topology_hash){
        CHECK(experiment.metrics.topology_hash==*topology_hash);
        CHECK(experiment.metrics.classification_hash==*classification_hash);
      }else{
        topology_hash=experiment.metrics.topology_hash;
        classification_hash=experiment.metrics.classification_hash;
      }
      if(storage==tetra::LayerStorage::address_runs)
        CHECK(experiment.metrics.address_run_count>0);
      if(storage==tetra::LayerStorage::mutable_macro_blocks||
         storage==tetra::LayerStorage::occupancy_bit_macro_blocks){
        CHECK(experiment.metrics.block_count>0);
        CHECK(experiment.metrics.maximum_block_occupancy<=64U);
      }
    }
  }

  auto rsb=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  for(std::size_t step=0;step<5;++step)rsb.refine_all_binary();
  const auto supercubes=tetra::build_layer_storage_experiment(
      rsb,shape,tetra::LayerStorage::occupancy_bit_macro_blocks,
      tetra::KernelOrder::fused_macro_blocks);
  CHECK(supercubes.metrics.source_rsb_diamonds>0);
  CHECK(supercubes.metrics.invalid_supercube_diamonds==0);
  CHECK(supercubes.metrics.supercube_diamonds==
        supercubes.metrics.source_rsb_diamonds);
  CHECK(supercubes.metrics.supercube_count>0);
}

TEST_CASE("packed adjacency representations agree for transitions boundaries and whole-cell cutaways") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere shape{};
  tetra::Camera camera{};
  REQUIRE(tetra::refine_to_sphere(mesh,shape,camera,52.0,6).iterations>0);
  REQUIRE(mesh.conforming_volume().size()!=mesh.logical_cut().owners.size());
  const std::array representations{
      tetra::AdjacencyRepresentation::path_arithmetic,
      tetra::AdjacencyRepresentation::packed_half_facets,
      tetra::AdjacencyRepresentation::logical_face_table,
      tetra::AdjacencyRepresentation::reconstruction_oracle};
  std::optional<std::uint64_t> multiplicity_hash,adjacency_hash;
  for(const auto representation:representations){
    CAPTURE(tetra::strategy_key(representation));
    const auto experiment=tetra::build_adjacency_experiment(mesh,representation);
    CHECK(experiment.metrics.nonmanifold_faces==0);
    CHECK(experiment.metrics.manifold_pairs>0);
    CHECK(experiment.metrics.boundary_faces>0);
    CHECK(experiment.metrics.retained_bytes>0);
    CHECK(experiment.metrics.dirty_half_facets_updated>0);
    if(multiplicity_hash){
      CHECK(experiment.metrics.owner_multiplicity_hash==*multiplicity_hash);
      CHECK(experiment.metrics.oriented_adjacency_hash==*adjacency_hash);
    }else{
      multiplicity_hash=experiment.metrics.owner_multiplicity_hash;
      adjacency_hash=experiment.metrics.oriented_adjacency_hash;
    }
    if(representation==tetra::AdjacencyRepresentation::path_arithmetic)
      CHECK(experiment.metrics.template_wired_half_facets+
            experiment.metrics.path_exceptions>0);
    if(representation==tetra::AdjacencyRepresentation::packed_half_facets)
      CHECK_FALSE(experiment.vertex_anchors.empty());
  }

  std::vector<tetra::TetId> cutaway;
  for(const auto address:mesh.conforming_volume().addresses()){
    tetra::Vec3 centre{};
    for(const auto vertex:mesh.tetrahedron(address).vertices)
      centre=centre+mesh.vertices()[vertex];
    if((centre/4.0).x<=0.5)cutaway.push_back(address);
  }
  REQUIRE_FALSE(cutaway.empty());
  multiplicity_hash.reset();adjacency_hash.reset();
  for(const auto representation:representations){
    const auto experiment=tetra::build_adjacency_experiment(
        mesh,representation,cutaway);
    CHECK(experiment.metrics.nonmanifold_faces==0);
    if(multiplicity_hash){
      CHECK(experiment.metrics.owner_multiplicity_hash==*multiplicity_hash);
      CHECK(experiment.metrics.oriented_adjacency_hash==*adjacency_hash);
    }else{
      multiplicity_hash=experiment.metrics.owner_multiplicity_hash;
      adjacency_hash=experiment.metrics.oriented_adjacency_hash;
    }
  }
}

TEST_CASE("mixed-depth dual ownership exactly applies missing finer and ordering rules") {
  const auto id=[](std::uint8_t root,unsigned int depth,tetra::TetId suffix=0U){
    return tetra::make_tet_id(
        root,(tetra::TetId{1}<<depth)|(suffix&((tetra::TetId{1}<<depth)-1U)));
  };
  const tetra::MixedDepthDualStarTopology complete{true,true,8U,6U};
  const tetra::TetId same_first=id(1U,3U,2U);
  const tetra::TetId same_second=id(2U,3U,1U);
  const std::array same_depth{same_second,same_first,same_second};
  auto resolved=tetra::resolve_mixed_depth_dual_owner(same_depth,complete);
  CHECK(resolved.decision==tetra::MixedDepthDualDecision::accepted);
  CHECK(resolved.owner==std::min(same_first,same_second));
  CHECK(tetra::evaluate_mixed_depth_dual_contender(
      same_depth,resolved.owner,complete)==tetra::MixedDepthDualDecision::accepted);
  CHECK(tetra::evaluate_mixed_depth_dual_contender(
      same_depth,std::max(same_first,same_second),complete)==
      tetra::MixedDepthDualDecision::same_level_predecessor);

  const tetra::TetId coarse=id(0U,3U,7U);
  const tetra::TetId fine_second=id(3U,6U,5U);
  const tetra::TetId fine_first=id(2U,6U,63U);
  std::array mixed{fine_second,coarse,fine_first,coarse};
  resolved=tetra::resolve_mixed_depth_dual_owner(mixed,complete);
  CHECK(resolved.owner==std::min(fine_first,fine_second));
  CHECK(tetra::evaluate_mixed_depth_dual_contender(mixed,coarse,complete)==
        tetra::MixedDepthDualDecision::finer_level_owner);
  CHECK(tetra::evaluate_mixed_depth_dual_contender(
      mixed,std::max(fine_first,fine_second),complete)==
      tetra::MixedDepthDualDecision::same_level_predecessor);
  std::ranges::reverse(mixed);
  CHECK(tetra::resolve_mixed_depth_dual_owner(mixed,complete).owner==resolved.owner);

  CHECK(tetra::resolve_mixed_depth_dual_owner(
      mixed,{false,true,8U,6U}).decision==
      tetra::MixedDepthDualDecision::missing_incident_cell);
  CHECK(tetra::resolve_mixed_depth_dual_owner(
      mixed,{true,false,8U,6U}).decision==
      tetra::MixedDepthDualDecision::nonmanifold_star);
  CHECK(tetra::resolve_mixed_depth_dual_owner(
      mixed,{true,true,3U,4U}).decision==
      tetra::MixedDepthDualDecision::degenerate_star);
  CHECK(tetra::resolve_mixed_depth_dual_owner(
      mixed,{true,true,4U,3U}).decision==
      tetra::MixedDepthDualDecision::degenerate_star);
  const std::array malformed{tetra::invalid_tet};
  CHECK(tetra::resolve_mixed_depth_dual_owner(malformed,complete).decision==
        tetra::MixedDepthDualDecision::malformed_incident);
  CHECK(tetra::evaluate_mixed_depth_dual_contender(mixed,id(5U,3U),complete)==
        tetra::MixedDepthDualDecision::malformed_incident);
}

TEST_CASE("mixed-depth dual root-domain stars distinguish boundary and interior") {
  const auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const auto index=tetra::build_mixed_depth_dual_index(mesh);
  REQUIRE_FALSE(index.candidates.empty());
  CHECK(index.hierarchy_revision==mesh.revision());
  std::size_t boundary{},interior{};
  for(const auto& candidate:index.candidates){
    const auto& point=mesh.vertices()[candidate.primal_vertex];
    const bool on_domain_boundary=point.x==0.0||point.x==1.0||
        point.y==0.0||point.y==1.0||point.z==0.0||point.z==1.0;
    if(on_domain_boundary){
      ++boundary;
      CHECK(candidate.owner==tetra::invalid_tet);
      CHECK(candidate.decision==
            tetra::MixedDepthDualDecision::missing_incident_cell);
    }else{
      ++interior;
      CHECK(candidate.owner!=tetra::invalid_tet);
      CHECK(candidate.decision==tetra::MixedDepthDualDecision::accepted);
    }
  }
  CHECK(boundary>0U);
  CHECK(interior>0U);
}

TEST_CASE("packed mixed-depth dual stars resolve BCC transitions deterministically") {
  for(const auto strategy:{tetra::BccTransitionStrategy::crystalline_restricted,
                           tetra::BccTransitionStrategy::complete_minimal}){
    CAPTURE(static_cast<unsigned int>(strategy));
    auto mesh=tetra::TetMesh::make_unit_cube(
        tetra::SubdivisionMethod::bcc_red_green);
    if(strategy!=mesh.transition_strategy())REQUIRE(mesh.set_transition_strategy(strategy));
    tetra::Sphere shape{{0.5,0.5,0.5},0.31};
    tetra::Camera camera;
    camera.position={0.3,0.6,1.4};
    REQUIRE(tetra::refine_to_sphere(mesh,shape,camera,44.0,9).iterations>0);
    const auto index=tetra::build_mixed_depth_dual_index(mesh);
    REQUIRE_FALSE(index.candidates.empty());
    CHECK(index.hierarchy_revision==mesh.revision());
    CHECK(index.incidents.size()==mesh.conforming_volume().size()*4U);
    std::size_t accepted{},boundary{},transition_incidents{},mixed_depth{};
    std::uint32_t previous_incident_end{},previous_contender_end{};
    tetra::VertexId previous_vertex{};
    bool first=true;
    for(const auto& candidate:index.candidates){
      CHECK(candidate.incident_begin==previous_incident_end);
      CHECK(candidate.contender_begin==previous_contender_end);
      CHECK(candidate.incident_count>0U);
      CHECK(candidate.contender_count>0U);
      if(!first)CHECK(previous_vertex<candidate.primal_vertex);
      first=false;previous_vertex=candidate.primal_vertex;
      const auto incidents=std::span{index.incidents}.subspan(
          candidate.incident_begin,candidate.incident_count);
      const auto contenders=std::span{index.contenders}.subspan(
          candidate.contender_begin,candidate.contender_count);
      std::set<tetra::TetId> incident_addresses,owner_addresses;
      std::set<std::uint32_t> depths;
      for(const auto& incident:incidents){
        CHECK(incident_addresses.insert(incident.conforming_cell).second);
        CHECK(incident.owner_depth==tetra::tet_depth(incident.logical_owner));
        if(incident.transition){
          ++transition_incidents;
          CHECK(incident.conforming_cell!=incident.logical_owner);
        }
      }
      std::size_t accepted_contenders{};
      for(const auto& contender:contenders){
        CHECK(owner_addresses.insert(contender.logical_owner).second);
        CHECK(contender.owner_depth==tetra::tet_depth(contender.logical_owner));
        depths.insert(contender.owner_depth);
        accepted_contenders+=contender.decision==
            tetra::MixedDepthDualDecision::accepted?1U:0U;
      }
      mixed_depth+=depths.size()>1U?1U:0U;
      if(candidate.decision==tetra::MixedDepthDualDecision::accepted){
        ++accepted;
        CHECK(candidate.owner!=tetra::invalid_tet);
        CHECK(accepted_contenders==1U);
        const auto found=std::ranges::find_if(contenders,[](const auto& contender){
          return contender.decision==tetra::MixedDepthDualDecision::accepted;
        });
        REQUIRE(found!=contenders.end());
        CHECK(found->logical_owner==candidate.owner);
      }else{
        CHECK(candidate.owner==tetra::invalid_tet);
        CHECK(accepted_contenders==0U);
        boundary+=candidate.decision==
            tetra::MixedDepthDualDecision::missing_incident_cell?1U:0U;
      }
      previous_incident_end=candidate.incident_begin+candidate.incident_count;
      previous_contender_end=candidate.contender_begin+candidate.contender_count;
    }
    CHECK(previous_incident_end==index.incidents.size());
    CHECK(previous_contender_end==index.contenders.size());
    CHECK(accepted>0U);
    CHECK(boundary>0U);
    CHECK(transition_incidents>0U);
    CHECK(mixed_depth>0U);
  }
}

TEST_CASE("mixed-depth barycentric dual extraction is closed outward and unique") {
  using Point=std::array<std::uint64_t,3>;
  using Edge=std::array<Point,2>;
  using Face=std::array<Point,3>;
  const auto point_key=[](tetra::Vec3 point){
    return Point{{std::bit_cast<std::uint64_t>(point.x==0.0?0.0:point.x),
                  std::bit_cast<std::uint64_t>(point.y==0.0?0.0:point.y),
                  std::bit_cast<std::uint64_t>(point.z==0.0?0.0:point.z)}};
  };
  for(const auto strategy:{tetra::BccTransitionStrategy::crystalline_restricted,
                           tetra::BccTransitionStrategy::complete_minimal}){
    CAPTURE(static_cast<unsigned int>(strategy));
    auto mesh=tetra::TetMesh::make_unit_cube(
        tetra::SubdivisionMethod::bcc_red_green);
    if(strategy!=mesh.transition_strategy())REQUIRE(mesh.set_transition_strategy(strategy));
    tetra::Sphere sphere{{0.5,0.5,0.5},0.27};
    tetra::Camera camera;
    camera.position={0.35,0.55,1.4};
    REQUIRE(tetra::refine_to_sphere(mesh,sphere,camera,45.0,9).iterations>0);
    const auto triangles=tetra::extract_mixed_depth_dual_isosurface(mesh,sphere);
    REQUIRE_FALSE(triangles.empty());
    std::map<Edge,std::size_t> edges;
    std::map<Face,std::size_t> faces;
    for(const auto& triangle:triangles){
      const auto normal=[](tetra::Vec3 a,tetra::Vec3 b){
        return tetra::Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,
                           a.x*b.y-a.y*b.x};
      }(triangle.b-triangle.a,triangle.c-triangle.a);
      const auto centre=(triangle.a+triangle.b+triangle.c)/3.0;
      const auto outward=sphere.normal(centre);
      CHECK(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z>0.0);
      Face face{{point_key(triangle.a),point_key(triangle.b),point_key(triangle.c)}};
      std::sort(face.begin(),face.end());
      ++faces[face];
      for(const auto pair:std::array<std::array<std::size_t,2>,3>{{
              {{0U,1U}},{{1U,2U}},{{2U,0U}}}}){
        Edge edge{{face[pair[0]],face[pair[1]]}};
        if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
        ++edges[edge];
      }
    }
    CHECK(std::ranges::all_of(faces,[](const auto& item){return item.second==1U;}));
    CHECK(std::ranges::all_of(edges,[](const auto& item){return item.second==2U;}));
  }
}

TEST_CASE("mixed-depth dual patches retain complete vertex-star dependencies") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.27};
  tetra::Camera camera;
  camera.position={0.4,0.6,1.3};
  REQUIRE(tetra::refine_to_sphere(mesh,sphere,camera,44.0,9).iterations>0);
  tetra::MixedDepthDualPatchBuilder builder;
  builder.rebuild_index(mesh);
  REQUIRE(builder.retained_bytes()>0U);
  REQUIRE_FALSE(builder.dependencies().empty());
  for(const auto& candidate:builder.index().candidates){
    if(candidate.decision!=tetra::MixedDepthDualDecision::accepted)continue;
    const auto contenders=std::span{builder.index().contenders}.subspan(
        candidate.contender_begin,candidate.contender_count);
    for(const auto& contender:contenders)
      CHECK(std::binary_search(
          builder.dependencies().begin(),builder.dependencies().end(),
          tetra::MixedDepthDualPatchDependency{
              candidate.owner,contender.logical_owner}));
  }
  std::vector<tetra::MixedDepthDualPatchTriangle> all,selected;
  builder.generate_patches(mesh,sphere,{},all);
  REQUIRE_FALSE(all.empty());
  const auto selected_owner=all[all.size()/2U].patch_owner;
  const std::array selected_owners{selected_owner};
  builder.generate_patches(mesh,sphere,selected_owners,selected);
  REQUIRE_FALSE(selected.empty());
  CHECK(std::ranges::all_of(selected,[&](const auto& triangle){
    return triangle.patch_owner==selected_owner;
  }));
  CHECK(std::ranges::count(all,selected_owner,
      &tetra::MixedDepthDualPatchTriangle::patch_owner)==selected.size());
  const auto monolithic=tetra::extract_mixed_depth_dual_isosurface(mesh,sphere);
  REQUIRE(monolithic.size()==all.size());
  for(std::size_t index=0;index<all.size();++index){
    CHECK(all[index].triangle.a.x==monolithic[index].a.x);
    CHECK(all[index].triangle.a.y==monolithic[index].a.y);
    CHECK(all[index].triangle.a.z==monolithic[index].a.z);
    CHECK(all[index].triangle.b.x==monolithic[index].b.x);
    CHECK(all[index].triangle.b.y==monolithic[index].b.y);
    CHECK(all[index].triangle.b.z==monolithic[index].b.z);
    CHECK(all[index].triangle.c.x==monolithic[index].c.x);
    CHECK(all[index].triangle.c.y==monolithic[index].c.y);
    CHECK(all[index].triangle.c.z==monolithic[index].c.z);
  }
  REQUIRE(mesh.refine_selected_binary({mesh.logical_red_owners().front()}));
  CHECK_THROWS_AS(builder.generate_patches(mesh,sphere,{},all),std::logic_error);
}

TEST_CASE("mixed-depth dual owner patches match monolithic topology through refinement") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.27};
  tetra::Camera camera;
  camera.position={0.4,0.6,1.3};
  REQUIRE(tetra::refine_to_sphere(mesh,sphere,camera,44.0,6).iterations>0);
  tetra_viewer::SceneCache cache;
  const auto update=[&]{
    return cache.update_scene(
        mesh,sphere,17U,tetra_viewer::SurfaceMethod::mixed_depth_dual,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  };
  const auto monolithic=[&]{
    return tetra_viewer::prepare_scene(
        mesh,sphere,tetra_viewer::SurfaceMethod::mixed_depth_dual,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  };
  REQUIRE(update());
  auto metrics=cache.surface_patch_metrics();
  CHECK(metrics.active);
  CHECK(metrics.full_rebuild);
  CHECK(metrics.output_triangles>0U);
  CHECK(cache.scene().mixed_depth_dual_triangles==metrics.output_triangles);
  CHECK(tetra_viewer::surface_geometry_hashes(cache.scene())==
        tetra_viewer::surface_geometry_hashes(monolithic()));

  const auto split=std::ranges::find_if(
      mesh.logical_red_owners(),[&](tetra::TetId owner){
        return tetra::classify_tetrahedron(mesh,owner,sphere)==
            tetra::SurfaceRelation::intersecting;
      });
  REQUIRE(split!=mesh.logical_red_owners().end());
  REQUIRE(mesh.refine_selected_binary({*split}));
  REQUIRE(update());
  metrics=cache.surface_patch_metrics();
  CHECK_FALSE(metrics.full_rebuild);
  CHECK(metrics.rebuilt_patches>0U);
  CHECK(metrics.reused_patches>0U);
  CHECK(metrics.rebuilt_patches<cache.surface_patch_records().size());
  CHECK(tetra_viewer::surface_geometry_hashes(cache.scene())==
        tetra_viewer::surface_geometry_hashes(monolithic()));
}

TEST_CASE("mixed-depth dual is selectable in headless and interactive registries") {
  CHECK(std::ranges::find(
      tetra_viewer::surface_methods,
      tetra_viewer::SurfaceMethod::mixed_depth_dual)!=
      tetra_viewer::surface_methods.end());
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=6,set-volume-connection=hierarchy-cells,"
      "set-surface-method=mixed-depth-dual,prepare-scene,stats",
      output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"surface_method\":\"mixed-depth-dual\"")!=std::string::npos);
  CHECK(text.find("\"mixed_depth_dual_triangles\":0")==std::string::npos);
  CHECK(text.find("\"surface_patch_neighbourhood\":\"incident-vertex-star\"")!=
        std::string::npos);
  CHECK(text.find("\"surface_patch_active\":true")!=std::string::npos);
}

TEST_CASE("parallel cavity policies preserve serial topology and command hashes") {
  const auto source=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere shape{};
  tetra::Camera camera{};
  tetra::AdaptationConfiguration configuration;
  configuration.operation_budget=4096;
  const auto plan=tetra::plan_adaptation(
      source,shape,camera,28.0,6,configuration,2);
  REQUIRE(plan.supported);
  REQUIRE_FALSE(plan.commands.empty());
  const auto batches=tetra::partition_conflict_free_cavities(source,plan.commands);
  REQUIRE(batches.offsets.size()>1);
  for(std::size_t batch=0;batch+1U<batches.offsets.size();++batch){
    std::set<tetra::VertexId> vertices;
    for(std::size_t offset=batches.offsets[batch];offset<batches.offsets[batch+1U];
        ++offset){
      const auto index=batches.command_indices[offset];
      for(const auto vertex:source.tetrahedron(plan.commands[index].logical_owner).vertices)
        CHECK(vertices.insert(vertex).second);
    }
  }

  auto serial_mesh=source;
  const auto serial=tetra::commit_adaptation_parallel(
      serial_mesh,plan,configuration,2,tetra::ParallelCommitPolicy::serial_oracle,1);
  REQUIRE(serial.commit.status==tetra::AdaptationCommitStatus::committed);
  const auto logical_hash=tetra::logical_owner_hash(serial_mesh.logical_cut().owners);
  std::vector<tetra::TetId> conforming(
      serial_mesh.conforming_volume().addresses().begin(),
      serial_mesh.conforming_volume().addresses().end());

  for(const auto policy:{tetra::ParallelCommitPolicy::deterministic_cavity_batches,
                         tetra::ParallelCommitPolicy::optimistic_cavity_locks}){
    for(const std::size_t threads:{1U,2U,4U}){
      CAPTURE(tetra::strategy_key(policy));CAPTURE(threads);
      for(std::size_t repeat=0;repeat<2U;++repeat){
        auto mesh=source;
        const auto result=tetra::commit_adaptation_parallel(
            mesh,plan,configuration,2,policy,threads);
        REQUIRE(result.commit.status==tetra::AdaptationCommitStatus::committed);
        CHECK(tetra::logical_owner_hash(mesh.logical_cut().owners)==logical_hash);
        CHECK(std::ranges::equal(mesh.conforming_volume().addresses(),conforming));
        CHECK(result.metrics.command_log_hash==serial.metrics.command_log_hash);
        CHECK(result.metrics.successful_commits==plan.commands.size());
        CHECK(result.metrics.rollbacks==result.metrics.conflicts);
        CHECK(result.metrics.thread_count==threads);
      }
    }
  }
}

TEST_CASE("BCC surface LOD leaves tetrahedra far from the surface coarse") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.22};
  tetra::Camera camera;
  camera.position={0.5,0.5,1.1};
  camera.viewport_height_pixels=800.0;
  const auto result=tetra::refine_to_sphere(mesh,sphere,camera,18.0,9);
  REQUIRE(result.iterations>1);
  unsigned int deepest_surface{};
  unsigned int deepest_far{};
  std::size_t far_cells{};
  for(const auto id:mesh.active_leaves()){
    const auto& tet=mesh.tetrahedron(id).vertices;
    tetra::Vec3 centre{};
    for(const auto vertex:tet)centre=centre+mesh.vertices()[vertex];
    centre=centre/4.0;
    const auto offset=centre-sphere.centre;
    const double distance=std::sqrt(offset.x*offset.x+offset.y*offset.y+offset.z*offset.z);
    const auto depth=mesh.refinement_depth(id);
    if(tetra::classify_tetrahedron(mesh,id,sphere)==tetra::SurfaceRelation::intersecting)
      deepest_surface=std::max(deepest_surface,depth);
    if(distance>0.55){deepest_far=std::max(deepest_far,depth);++far_cells;}
  }
  REQUIRE(far_cells>0);
  CHECK(deepest_surface>=6);
  CHECK(deepest_far+6<=deepest_surface);
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("camera LOD reset reuses packed hierarchy while refining and coarsening") {
  const tetra::Sphere sphere{};
  for(const auto method:{tetra::SubdivisionMethod::maubach_diamond,
                         tetra::SubdivisionMethod::bcc_red_green}){
    CAPTURE(tetra::subdivision_method_name(method));
    auto mesh=tetra::TetMesh::make_unit_cube(method);
    tetra::Camera camera;
    tetra::ImplicitValueCache field_cache;
    static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9,&field_cache));
    const auto cached_distances=field_cache.vertex_distances;
    CHECK(field_cache.has_sampled_surface);
    const auto near_leaves=mesh.active_leaves();
    const auto resident_tetrahedra=mesh.tetrahedron_count();
    const auto resident_vertices=mesh.vertices().size();
    const auto resident_layers=mesh.layers().size();
    REQUIRE(near_leaves.size()>(method==tetra::SubdivisionMethod::bcc_red_green?12U:6U));

    mesh.reset_active_hierarchy();
    CHECK(mesh.active_leaves().size()==
          (method==tetra::SubdivisionMethod::bcc_red_green?12U:6U));
    CHECK(mesh.tetrahedron_count()==resident_tetrahedra);
    CHECK(mesh.vertices().size()==resident_vertices);
    CHECK(mesh.layers().size()==resident_layers);
    CHECK(mesh.has_conforming_active_faces());

    static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9,&field_cache));
    CHECK(field_cache.vertex_distances==cached_distances);
    CHECK(mesh.active_leaves()==near_leaves);
    CHECK(mesh.tetrahedron_count()==resident_tetrahedra);
    CHECK(mesh.vertices().size()==resident_vertices);
    CHECK(mesh.layers().size()==resident_layers);
    CHECK(mesh.has_conforming_active_faces());

    camera.forward={0.0,0.0,1.0};
    mesh.reset_active_hierarchy();
    static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9,&field_cache));
    CHECK(mesh.active_leaves().size()==
          (method==tetra::SubdivisionMethod::bcc_red_green?12U:6U));
    CHECK(mesh.tetrahedron_count()==resident_tetrahedra);
    CHECK(mesh.vertices().size()==resident_vertices);
  }
}

TEST_CASE("repeated BCC terrain camera cycles stabilize packed hierarchy storage") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.0,1.0,0.5};
  camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
  const auto refine=[&]{
    mesh.reset_active_hierarchy();
    static_cast<void>(tetra::refine_to_sphere(mesh,terrain,camera,40.0,6));
  };
  const auto cycle=[&]{
    camera.position={-1.0,0.7,0.5};
    camera.forward={0.9912279006826347,-0.1321637200910179,0.0};
    refine();
    camera.position={0.0,1.0,0.5};
    camera.forward={0.7071067811865475,-0.7071067811865475,0.0};
    refine();
  };

  refine();
  cycle();
  const auto stable_leaves=mesh.active_leaves();
  const auto stable_tetrahedra=mesh.tetrahedron_count();
  const auto stable_vertices=mesh.vertices().size();
  const auto stable_layers=mesh.layers().size();
  const auto stable_scratch=mesh.bcc_scratch_capacities();
  CHECK(stable_scratch.active_edge_nodes>0);
  CHECK(stable_scratch.edge_table>0);
  CHECK(stable_scratch.edge_nodes>0);
  CHECK(stable_scratch.face_table>0);
  CHECK(stable_scratch.face_nodes>0);
  CHECK(stable_scratch.dirty_edges>0);
  CHECK(stable_scratch.dirty_owners>0);
  CHECK(stable_scratch.dirty_faces>0);
  cycle();
  CHECK(mesh.active_leaves()==stable_leaves);
  CHECK(mesh.tetrahedron_count()==stable_tetrahedra);
  CHECK(mesh.vertices().size()==stable_vertices);
  CHECK(mesh.layers().size()==stable_layers);
  CHECK(mesh.bcc_scratch_capacities()==stable_scratch);
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("octasection target refinement advances only at complete three-bit depths") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bey_red_fixed);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  const auto depth_nine=tetra::refine_to_sphere(mesh,sphere,camera,28.0,9);
  CHECK(depth_nine.iterations==3);
  CHECK(depth_nine.reached_depth_limit);
  const auto blocked=tetra::refine_to_sphere(mesh,sphere,camera,28.0,10);
  CHECK(blocked.iterations==0);
  CHECK(blocked.refined_leaves==0);
  CHECK(blocked.reached_depth_limit);
  const auto depth_twelve=tetra::refine_to_sphere(mesh,sphere,camera,28.0,12);
  CHECK(depth_twelve.iterations==1);
  CHECK(depth_twelve.refined_leaves>0);
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("binary diamond split writes two path children per participating tet") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  mesh.refine_selected_binary({tetra::make_tet_id(0, 1)});

  CHECK(mesh.layers().size() == 2);
  CHECK(mesh.layers()[1].tetrahedra.size() == 12);
  CHECK(mesh.active_leaves().size() == 12);
  for (std::uint8_t root = 0; root < 6; ++root) {
    const auto parent = tetra::make_tet_id(root, 1);
    const auto left = tetra::tet_child(parent, false);
    const auto right = tetra::tet_child(parent, true);
    CHECK(tetra::tet_depth(left) == 1);
    CHECK(mesh.tetrahedron(left).address == left);
    CHECK(mesh.tetrahedron(right).address == right);
    CHECK(tetra::tet_refinement_type(parent) == 0);
    CHECK(tetra::tet_refinement_type(left) == 1);
  }
  CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("Maubach children use the address-derived cyclic bisection rule") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const auto root = tetra::make_tet_id(0, 1);
  const auto root_vertices = mesh.tetrahedron(root).vertices;
  mesh.refine_selected_binary({root});
  const auto left = tetra::tet_child(root, false);
  const auto right = tetra::tet_child(root, true);
  const auto left_vertices = mesh.tetrahedron(left).vertices;
  const auto right_vertices = mesh.tetrahedron(right).vertices;
  CHECK(left_vertices[0] == right_vertices[0]);
  CHECK(left_vertices[1] == root_vertices[1]);
  CHECK(left_vertices[2] == root_vertices[2]);
  CHECK(left_vertices[3] == root_vertices[3]);
  CHECK(right_vertices[1] == root_vertices[0]);
  CHECK(right_vertices[2] == root_vertices[1]);
  CHECK(right_vertices[3] == root_vertices[2]);
  const auto midpoint = mesh.vertices()[left_vertices[0]];
  const auto first = mesh.vertices()[root_vertices[0]];
  const auto last = mesh.vertices()[root_vertices[3]];
  CHECK(midpoint.x == doctest::Approx((first.x + last.x) * 0.5));
  CHECK(midpoint.y == doctest::Approx((first.y + last.y) * 0.5));
  CHECK(midpoint.z == doctest::Approx((first.z + last.z) * 0.5));
}

TEST_CASE("path-bit hierarchy supports deep local Maubach refinement") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  auto address = tetra::make_tet_id(0, 1);
  for (unsigned int depth = 0; depth < 20; ++depth) {
    mesh.refine_selected_binary({address});
    address = tetra::tet_child(address, false);
    CHECK(tetra::tet_depth(address) == depth + 1);
    CHECK(tetra::tet_refinement_type(address) == (depth + 1) % 3);
    CHECK(mesh.tetrahedron(address).address == address);
  }
  CHECK(mesh.layers().size() >= 21);
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("binary refinement is deterministic and keeps packed layers") {
  auto first = tetra::TetMesh::make_unit_cube();
  auto second = tetra::TetMesh::make_unit_cube();
  first.refine_all_binary();
  second.refine_all_binary();
  first.refine_all_binary();
  second.refine_all_binary();
  CHECK(first.active_leaves() == second.active_leaves());
  CHECK(first.layers().size() == second.layers().size());
  for (std::size_t depth = 0; depth < first.layers().size(); ++depth) {
    const auto& a = first.layers()[depth].tetrahedra;
    const auto& b = second.layers()[depth].tetrahedra;
    CHECK(a.size() == b.size());
    for (std::size_t index = 0; index < a.size(); ++index) {
      CHECK(a[index].address == b[index].address);
      CHECK(a[index].vertices == b[index].vertices);
    }
  }
  CHECK(first.has_positive_active_volumes());
  CHECK(first.has_conforming_active_faces());
}

TEST_CASE("sphere field and conservative tetrahedron classification") {
  const tetra::Sphere sphere{{0.5, 0.5, 0.5}, 0.25};
  CHECK(sphere.signed_distance({0.5, 0.5, 0.5}) == doctest::Approx(-0.25));
  const auto mesh = tetra::TetMesh::make_unit_cube();
  for (const auto id : mesh.active_leaves()) CHECK(tetra::classify_tetrahedron(mesh, id, sphere) == tetra::SurfaceRelation::intersecting);
}

TEST_CASE("implicit shape catalogue produces finite sign-changing surfaces") {
  for(const auto kind:tetra::implicit_shape_kinds){
    CAPTURE(tetra::implicit_shape_name(kind));
    tetra::Sphere shape;
    shape.kind=kind;
    bool negative=false,positive=false;
    for(int z=0;z<=8;++z)for(int y=0;y<=8;++y)for(int x=0;x<=8;++x){
      const tetra::Vec3 point{x/8.0,y/8.0,z/8.0};
      const double distance=shape.signed_distance(point);
      REQUIRE(std::isfinite(distance));
      negative|=distance<0.0;
      positive|=distance>0.0;
    }
    CHECK(negative);
    CHECK(positive);
    const auto projected=shape.project_to_surface({0.72,0.63,0.58});
    CHECK(std::abs(shape.signed_distance(projected))<1.0e-6);
    const auto normal=shape.normal(projected);
    CHECK(std::isfinite(normal.x));
    CHECK(std::isfinite(normal.y));
    CHECK(std::isfinite(normal.z));
  }

  tetra::Sphere first;
  first.kind=tetra::ImplicitShapeKind::perlin_terrain;
  const tetra::Sphere second=first;
  for(int z=0;z<8;++z)for(int x=0;x<8;++x){
    const tetra::Vec3 point{x/7.0,0.5,z/7.0};
    CHECK(first.signed_distance(point)==second.signed_distance(point));
  }

  const tetra::Vec3 terrain_point{0.31,0.52,0.67};
  const auto analytic=first.normal(terrain_point);
  constexpr double epsilon=1.0e-6;
  tetra::Vec3 finite{
      first.signed_distance({terrain_point.x+epsilon,terrain_point.y,terrain_point.z})-
          first.signed_distance({terrain_point.x-epsilon,terrain_point.y,terrain_point.z}),
      first.signed_distance({terrain_point.x,terrain_point.y+epsilon,terrain_point.z})-
          first.signed_distance({terrain_point.x,terrain_point.y-epsilon,terrain_point.z}),
      first.signed_distance({terrain_point.x,terrain_point.y,terrain_point.z+epsilon})-
          first.signed_distance({terrain_point.x,terrain_point.y,terrain_point.z-epsilon})};
  const double length=std::sqrt(finite.x*finite.x+finite.y*finite.y+finite.z*finite.z);
  finite=finite/length;
  CHECK(analytic.x*finite.x+analytic.y*finite.y+analytic.z*finite.z>1.0-1.0e-8);
}

TEST_CASE("batched implicit fields match the scalar oracle for every shape") {
  std::vector<tetra::Vec3> points;
  for(int index=0;index<257;++index){
    const double value=static_cast<double>(index);
    points.push_back({std::fmod(value*0.6180339887498948,1.4)-0.2,
                      std::fmod(value*0.4142135623730950,1.4)-0.2,
                      std::fmod(value*0.7320508075688772,1.4)-0.2});
  }
  std::vector<double> batch(points.size());
  for(const auto kind:tetra::implicit_shape_kinds){
    CAPTURE(tetra::implicit_shape_name(kind));
    tetra::Sphere shape;
    shape.kind=kind;
    auto samples=points;
    tetra::Vec3 surface_point{0.63,0.5,0.71};
    surface_point.y-=shape.signed_distance(surface_point);
    if(kind==tetra::ImplicitShapeKind::perlin_terrain)samples.push_back(surface_point);
    batch.resize(samples.size());
    tetra::evaluate_signed_distances(shape,samples,batch);
    for(std::size_t index=0;index<samples.size();++index){
      const double scalar=shape.signed_distance(samples[index]);
      REQUIRE(std::isfinite(batch[index]));
      CHECK(batch[index]==doctest::Approx(scalar).epsilon(2.0e-12).scale(1.0));
      if(std::abs(scalar)>1.0e-10)CHECK((batch[index]<0.0)==(scalar<0.0));
    }
  }
  CHECK_THROWS_AS(tetra::evaluate_signed_distances(
      tetra::Sphere{},points,std::span<double>{batch}.first(batch.size()-1U)),
      std::invalid_argument);
}

TEST_CASE("terrain fine octaves remain subordinate to its coarse surface") {
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  const auto height=[&](double x,double z){
    const tetra::Vec3 point{x,terrain.centre.y,z};
    return terrain.centre.y-terrain.signed_distance(point);
  };
  constexpr std::size_t coarse_cells=16U;
  double maximum_midcell_residual{};
  double maximum_sampled_slope{};
  for(std::size_t z=0;z<coarse_cells;++z){
    for(std::size_t x=0;x<coarse_cells;++x){
      const double x0=20.0+static_cast<double>(x)/coarse_cells;
      const double x1=20.0+static_cast<double>(x+1U)/coarse_cells;
      const double z0=20.0+static_cast<double>(z)/coarse_cells;
      const double z1=20.0+static_cast<double>(z+1U)/coarse_cells;
      const double bilinear=0.25*(height(x0,z0)+height(x1,z0)+
                                  height(x0,z1)+height(x1,z1));
      maximum_midcell_residual=std::max(
          maximum_midcell_residual,
          std::abs(height((x0+x1)*0.5,(z0+z1)*0.5)-bilinear));
      const auto normal=terrain.normal({(x0+x1)*0.5,0.5,(z0+z1)*0.5});
      maximum_sampled_slope=std::max(maximum_sampled_slope,
          std::hypot(normal.x,normal.z)/normal.y);
    }
  }
  CAPTURE(maximum_midcell_residual);
  CAPTURE(maximum_sampled_slope);
  CHECK(maximum_midcell_residual<0.03);
  CHECK(maximum_sampled_slope<tetra::terrain_height_slope_bound(terrain));
}

TEST_CASE("mountain terrain has safe spawn plains ranges and conservative gradients") {
  tetra::Sphere terrain;terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  terrain.terrain.landform_amplitude=1.5;
  terrain.terrain.mountain_amplitude=6.0;
  terrain.terrain.spawn_flat_radius=2.0;
  terrain.terrain.spawn_blend_radius=12.0;
  for(int z=-4;z<=4;++z)for(int x=-4;x<=4;++x){
    const double world_x=terrain.centre.x+0.4*x;
    const double world_z=terrain.centre.z+0.4*z;
    if(std::hypot(world_x-terrain.centre.x,world_z-terrain.centre.z)>
       terrain.terrain.spawn_flat_radius)continue;
    const auto sample=tetra::terrain_height_sample(terrain,world_x,world_z);
    CHECK(sample.height==doctest::Approx(terrain.centre.y).epsilon(1.0e-14));
    CHECK(sample.dx==doctest::Approx(0.0));
    CHECK(sample.dz==doctest::Approx(0.0));
    const auto normal=terrain.normal({world_x,sample.height,world_z});
    CHECK(normal.x==doctest::Approx(0.0));
    CHECK(normal.y==doctest::Approx(1.0));
    CHECK(normal.z==doctest::Approx(0.0));
  }

  double minimum=std::numeric_limits<double>::infinity();
  double maximum=-std::numeric_limits<double>::infinity();
  double maximum_distant=-std::numeric_limits<double>::infinity();
  double maximum_distant_x{},maximum_distant_z{};
  double maximum_horizon=-std::numeric_limits<double>::infinity();
  double maximum_horizon_x{},maximum_horizon_z{};
  std::size_t plain_samples{};
  const double slope_bound=tetra::terrain_height_slope_bound(terrain);
  for(int z=-48;z<=48;z+=2)for(int x=-48;x<=48;x+=2){
    const double world_x=terrain.centre.x+x,world_z=terrain.centre.z+z;
    const auto sample=tetra::terrain_height_sample(terrain,world_x,world_z);
    minimum=std::min(minimum,sample.height);maximum=std::max(maximum,sample.height);
    if(std::hypot(static_cast<double>(x),static_cast<double>(z))>=20.0&&
       sample.height>maximum_distant){
      maximum_distant=sample.height;
      maximum_distant_x=world_x;maximum_distant_z=world_z;
    }
    const double distance=std::hypot(static_cast<double>(x),static_cast<double>(z));
    if(distance>=20.0&&distance<=44.0&&sample.height>maximum_horizon){
      maximum_horizon=sample.height;
      maximum_horizon_x=world_x;maximum_horizon_z=world_z;
    }
    plain_samples+=std::abs(sample.height-terrain.centre.y)<0.75?1U:0U;
    CHECK(std::hypot(sample.dx,sample.dz)<=slope_bound+1.0e-12);
    const double local_bound=tetra::terrain_height_slope_bound(
        terrain,world_x,world_z,0.25);
    CHECK(local_bound<=slope_bound+1.0e-12);
    for(const auto offset:std::array<std::array<double,2>,8>{{
            {0.25,0.0},{-0.25,0.0},{0.0,0.25},{0.0,-0.25},
            {0.1767766952966369,0.1767766952966369},
            {-0.1767766952966369,0.1767766952966369},
            {0.1767766952966369,-0.1767766952966369},
            {-0.1767766952966369,-0.1767766952966369}}}){
      const auto neighbour=tetra::terrain_height_sample(
          terrain,world_x+offset[0],world_z+offset[1]);
      CHECK(std::abs(neighbour.height-sample.height)<=
            local_bound*0.25+1.0e-12);
      CHECK(std::hypot(neighbour.dx,neighbour.dz)<=local_bound+1.0e-12);
    }
    constexpr double epsilon=1.0e-5;
    const double finite_dx=(
        tetra::terrain_height_sample(terrain,world_x+epsilon,world_z).height-
        tetra::terrain_height_sample(terrain,world_x-epsilon,world_z).height)/
        (2.0*epsilon);
    const double finite_dz=(
        tetra::terrain_height_sample(terrain,world_x,world_z+epsilon).height-
        tetra::terrain_height_sample(terrain,world_x,world_z-epsilon).height)/
        (2.0*epsilon);
    CHECK(sample.dx==doctest::Approx(finite_dx).epsilon(2.0e-5).scale(1.0));
    CHECK(sample.dz==doctest::Approx(finite_dz).epsilon(2.0e-5).scale(1.0));
  }
  CAPTURE(minimum);CAPTURE(maximum);CAPTURE(maximum_distant);
  CAPTURE(maximum_distant_x);CAPTURE(maximum_distant_z);CAPTURE(plain_samples);
  CAPTURE(maximum_horizon);CAPTURE(maximum_horizon_x);CAPTURE(maximum_horizon_z);
  CHECK(maximum-minimum>4.0);
  CHECK(maximum_distant-terrain.centre.y>3.0);
  CHECK(maximum_horizon-terrain.centre.y>2.0);
  CHECK(plain_samples>400U);
  const auto projected=terrain.project_to_surface(
      {maximum_horizon_x,terrain.centre.y-3.0,maximum_horizon_z});
  CHECK(std::abs(terrain.signed_distance(projected))<1.0e-12);
  const double lipschitz=tetra::implicit_field_lipschitz_bound(terrain);
  CHECK(lipschitz==doctest::Approx(std::sqrt(1.0+slope_bound*slope_bound)));
}

TEST_CASE("production gameplay terrain separates player scales and remains traversable") {
  const auto profile=tetra_viewer::production_world_profile();
  tetra::Sphere terrain;
  terrain.kind=profile.shape;terrain.terrain=profile.terrain;
  terrain.secondary=profile.octave_detail_amplitude;
  terrain.frequency=profile.octave_detail_frequency;

  for(int z=-8;z<=8;++z)for(int x=-8;x<=8;++x){
    const double dx=0.1*x,dz=0.1*z;
    if(std::hypot(dx,dz)>terrain.terrain.spawn_flat_radius)continue;
    const auto sample=tetra::terrain_height_sample(
        terrain,terrain.centre.x+dx,terrain.centre.z+dz);
    CHECK(sample.height==doctest::Approx(terrain.centre.y).epsilon(1.0e-14));
    CHECK(sample.dx==doctest::Approx(0.0));
    CHECK(sample.dz==doctest::Approx(0.0));
  }

  auto unblended=terrain;
  unblended.terrain.spawn_flat_radius=0.0;
  unblended.terrain.spawn_blend_radius=0.0;
  const auto seeded_centre=tetra::terrain_height_sample(
      unblended,unblended.centre.x,unblended.centre.z);
  CHECK(seeded_centre.height==doctest::Approx(unblended.centre.y)
                                  .epsilon(1.0e-14));

  const auto relief=[&](tetra::TerrainParameters parameters){
    tetra::Sphere isolated=terrain;isolated.terrain=parameters;
    isolated.terrain.spawn_flat_radius=0.0;
    isolated.terrain.spawn_blend_radius=0.0;
    double minimum=std::numeric_limits<double>::infinity();
    double maximum=-std::numeric_limits<double>::infinity();
    for(int z=-64;z<=64;++z)for(int x=-64;x<=64;++x){
      const auto sample=tetra::terrain_height_sample(
          isolated,isolated.centre.x+0.125*x,isolated.centre.z+0.125*z);
      minimum=std::min(minimum,sample.height);
      maximum=std::max(maximum,sample.height);
    }
    return maximum-minimum;
  };
  auto hills=profile.terrain;
  hills.landform_amplitude=0.0;hills.mountain_amplitude=0.0;
  hills.gameplay_feature_amplitude=0.0;hills.gameplay_corridor_depth=0.0;
  hills.ground_roughness_amplitude=0.0;
  auto features=hills;
  features.gameplay_hill_amplitude=0.0;
  features.gameplay_feature_amplitude=profile.terrain.gameplay_feature_amplitude;
  auto roughness=features;
  roughness.gameplay_feature_amplitude=0.0;
  roughness.ground_roughness_amplitude=profile.terrain.ground_roughness_amplitude;
  const double hill_relief=relief(hills);
  const double feature_relief=relief(features);
  const double roughness_relief=relief(roughness);
  CAPTURE(hill_relief);CAPTURE(feature_relief);CAPTURE(roughness_relief);
  CHECK(hill_relief>feature_relief*2.0);
  CHECK(feature_relief>roughness_relief*2.0);
  CHECK(roughness_relief>0.01);

  std::vector<double> slopes;
  double local_minimum=std::numeric_limits<double>::infinity();
  double local_maximum=-std::numeric_limits<double>::infinity();
  for(int z=-72;z<=72;++z)for(int x=-72;x<=72;++x){
    const double dx=0.25*x,dz=0.25*z;
    const double radius=std::hypot(dx,dz);
    if(radius<3.0||radius>18.0)continue;
    const double world_x=terrain.centre.x+dx,world_z=terrain.centre.z+dz;
    const auto sample=tetra::terrain_height_sample(terrain,world_x,world_z);
    local_minimum=std::min(local_minimum,sample.height);
    local_maximum=std::max(local_maximum,sample.height);
    slopes.push_back(std::hypot(sample.dx,sample.dz));
    const double local_bound=tetra::terrain_height_slope_bound(
        terrain,world_x,world_z,0.125);
    CHECK(slopes.back()<=local_bound+1.0e-12);
    constexpr double epsilon=1.0e-5;
    const double finite_dx=(tetra::terrain_height_sample(
        terrain,world_x+epsilon,world_z).height-tetra::terrain_height_sample(
        terrain,world_x-epsilon,world_z).height)/(2.0*epsilon);
    const double finite_dz=(tetra::terrain_height_sample(
        terrain,world_x,world_z+epsilon).height-tetra::terrain_height_sample(
        terrain,world_x,world_z-epsilon).height)/(2.0*epsilon);
    CHECK(sample.dx==doctest::Approx(finite_dx).epsilon(2.0e-5).scale(1.0));
    CHECK(sample.dz==doctest::Approx(finite_dz).epsilon(2.0e-5).scale(1.0));
  }
  std::ranges::sort(slopes);
  REQUIRE_FALSE(slopes.empty());
  const double median=slopes[slopes.size()/2U];
  const double percentile_90=slopes[slopes.size()*9U/10U];
  const double maximum_slope=slopes.back();
  CAPTURE(local_minimum);CAPTURE(local_maximum);CAPTURE(median);
  CAPTURE(percentile_90);CAPTURE(maximum_slope);
  CHECK(local_maximum-local_minimum>0.5);
  CHECK(median>0.03);
  CHECK(percentile_90<std::tan(35.0*std::acos(-1.0)/180.0));
  CHECK(maximum_slope<std::tan(52.0*std::acos(-1.0)/180.0));
}

TEST_CASE("every implicit shape refines and coarsens from the LOD camera") {
  for(const auto kind:tetra::implicit_shape_kinds){
    CAPTURE(tetra::implicit_shape_name(kind));
    auto mesh=tetra::TetMesh::make_unit_cube();
    tetra::Sphere shape;
    shape.kind=kind;
    tetra::Camera camera;
    static_cast<void>(tetra::refine_to_sphere(mesh,shape,camera,40.0,6));
    CHECK(mesh.active_leaves().size()>mesh.layers().front().tetrahedra.size());
    const auto stored=mesh.tetrahedron_count();
    camera.forward={0.0,0.0,1.0};
    mesh.reset_active_hierarchy();
    static_cast<void>(tetra::refine_to_sphere(mesh,shape,camera,40.0,6));
    CHECK(mesh.active_leaves().size()==mesh.layers().front().tetrahedra.size());
    CHECK(mesh.tetrahedron_count()==stored);
  }
}

TEST_CASE("surface extraction follows the refined active tetrahedral cut") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{{0.5, 0.5, 0.5}, 0.10};
  const tetra::Camera camera{};
  const auto refinement = tetra::refine_to_sphere(mesh, sphere, camera, 10.0, 6);
  (void)refinement;
  const auto surface = tetra::extract_isosurface(mesh, sphere);
  CHECK_FALSE(surface.empty());
  for (const auto& triangle : surface) {
    CHECK(std::abs(sphere.signed_distance(triangle.a)) < 1e-9);
    CHECK(std::abs(sphere.signed_distance(triangle.b)) < 1e-9);
    CHECK(std::abs(sphere.signed_distance(triangle.c)) < 1e-9);
  }
}

TEST_CASE("tetrahedral dual contouring produces a closed outward surface") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{{0.47, 0.52, 0.49}, 0.31};
  const tetra::Camera camera{{0.8, 0.7, 3.0}, 0.7853981633974483, 800.0};
  static_cast<void>(tetra::refine_to_sphere(mesh, sphere, camera, 38.0, 6));
  const auto surface = tetra::extract_dual_contour(mesh, sphere);
  CHECK_FALSE(surface.empty());

  using PointKey = std::array<long long, 3>;
  using EdgeKey = std::array<PointKey, 2>;
  std::map<EdgeKey, std::size_t> edge_counts;
  const auto point_key = [](tetra::Vec3 point) {
    constexpr double scale = 1.0e11;
    return PointKey{{std::llround(point.x*scale),std::llround(point.y*scale),std::llround(point.z*scale)}};
  };
  const auto add_edge = [&edge_counts, &point_key](tetra::Vec3 first, tetra::Vec3 second) {
    EdgeKey edge{{point_key(first),point_key(second)}};
    if (edge[1] < edge[0]) std::swap(edge[0],edge[1]);
    ++edge_counts[edge];
  };
  for (const auto& triangle : surface) {
    CHECK(std::min({std::abs(sphere.signed_distance(triangle.a)),
                    std::abs(sphere.signed_distance(triangle.b)),
                    std::abs(sphere.signed_distance(triangle.c))}) < 1e-9);
    const auto ab=triangle.b-triangle.a, ac=triangle.c-triangle.a;
    const tetra::Vec3 normal{ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,ab.x*ac.y-ab.y*ac.x};
    const tetra::Vec3 centre{(triangle.a.x+triangle.b.x+triangle.c.x)/3.0,
                             (triangle.a.y+triangle.b.y+triangle.c.y)/3.0,
                             (triangle.a.z+triangle.b.z+triangle.c.z)/3.0};
    const auto outward=centre-sphere.centre;
    CHECK(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z > 0.0);
    add_edge(triangle.a,triangle.b);
    add_edge(triangle.b,triangle.c);
    add_edge(triangle.c,triangle.a);
  }
  for (const auto& [edge,count] : edge_counts) {
    static_cast<void>(edge);
    CAPTURE(count);
    CHECK(count == 2);
  }
}

TEST_CASE("adaptive binary refinement remains conforming") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{{0.43, 0.57, 0.46}, 0.27};
  const tetra::Camera camera{{0.43, 0.57, 3.2}, 0.7853981633974483, 800.0};
  const auto result = tetra::refine_to_sphere(mesh, sphere, camera, 80.0, 2);
  CHECK(result.iterations > 0);
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_conforming_active_faces());
  const auto surface = tetra::extract_isosurface(mesh, sphere);
  CHECK_FALSE(surface.empty());
  for (const auto& triangle : surface) {
    CHECK(std::abs(sphere.signed_distance(triangle.a)) < 1e-9);
    CHECK(std::abs(sphere.signed_distance(triangle.b)) < 1e-9);
    CHECK(std::abs(sphere.signed_distance(triangle.c)) < 1e-9);
  }
}

TEST_CASE("every nonempty root marking closes to a conforming Maubach cut") {
  for (unsigned int mask = 1; mask < 64; ++mask) {
    auto mesh = tetra::TetMesh::make_unit_cube();
    std::vector<tetra::TetId> marked;
    for (std::uint8_t root = 0; root < 6; ++root)
      if ((mask & (1U << root)) != 0) marked.push_back(tetra::make_tet_id(root, 1));
    mesh.refine_selected_binary(marked);
    CAPTURE(mask);
    CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
    CHECK(mesh.has_positive_active_volumes());
    CHECK(mesh.has_conforming_active_faces());
  }
}

TEST_CASE("two interactive binary refinements after adaptive setup remain valid") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{{0.5, 0.5, 0.5}, 0.35};
  const tetra::Camera camera{{0.5, 0.5, 2.5}, 0.7853981633974483, 800.0};
  (void)tetra::refine_to_sphere(mesh, sphere, camera, 80.0, 2);
  mesh.refine_all_binary();
  mesh.refine_all_binary();
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
  CHECK_FALSE(tetra::extract_isosurface(mesh, sphere).empty());
}

TEST_CASE("viewer-scale batched refinement preserves the packed conforming hierarchy") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{{0.5, 0.5, 0.5}, 0.35};
  const tetra::Camera camera{{0.5, 0.5, 3.0}, 0.7853981633974483, 800.0};
  (void)tetra::refine_to_sphere(mesh, sphere, camera, 28.0, 9);
  CHECK(mesh.active_leaves().size() >= 1000);
  mesh.refine_all_binary();
  CHECK(mesh.active_leaves().size() >= 2000);
  CHECK(mesh.total_active_volume() == doctest::Approx(1.0));
  CHECK(mesh.has_positive_active_volumes());
  CHECK(mesh.has_symmetric_active_adjacency());
  CHECK(mesh.has_conforming_active_faces());
  for (const auto& layer : mesh.layers()) {
    for (std::size_t index = 1; index < layer.tetrahedra.size(); ++index)
      CHECK(layer.tetrahedra[index - 1].address < layer.tetrahedra[index].address);
  }
}

TEST_CASE("headless viewer script executes repeated refinement and validation in order") {
  std::ostringstream output;
  std::ostringstream errors;
  const int result = tetra_viewer::run_script("refine-once, refine-once, validate, stats", output, errors);

  CHECK(result == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.starts_with("{\"event\":\"initialized\""));
  CHECK(text.find("\"subdivision_method\":\"bcc-red-green\"")!=std::string::npos);
  CHECK(text.find("\"surface_method\":\"surface-optimization\"")!=std::string::npos);
  CHECK(text.find("\"volume_connection\":\"fixed-surface-shell\"")!=std::string::npos);
  const auto first_refine = text.find("\"command\":\"refine-once\"");
  const auto second_refine = text.find("\"command\":\"refine-once\"", first_refine + 1);
  const auto validation = text.find("{\"event\":\"validation\",\"valid\":true", second_refine);
  const auto stats = text.find("{\"event\":\"stats\"", validation);
  CHECK(first_refine != std::string::npos);
  CHECK(second_refine != std::string::npos);
  CHECK(validation != std::string::npos);
  CHECK(stats != std::string::npos);
  CHECK(text.find("\"maximum_depth\":16") != std::string::npos);
  CHECK(first_refine < second_refine);
  CHECK(second_refine < validation);
  CHECK(validation < stats);
}

TEST_CASE("headless viewer scene preparation emits upload-ready cached geometry") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "refine-once,set-hierarchy-edges=on,prepare-scene,validate",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("{\"event\":\"scene_preparation\"") != std::string::npos);
  CHECK(text.find("\"statistics_ms\":") != std::string::npos);
  CHECK(text.find("\"upload_preparation_ms\":") != std::string::npos);
  CHECK(text.find("\"triangle_vertices\":0") == std::string::npos);
  CHECK(text.find("\"hierarchy_line_vertices\":0") == std::string::npos);
  CHECK(text.find("\"line_vertices\":0") == std::string::npos);
  CHECK(text.find("\"upload_bytes\":0") == std::string::npos);
}

TEST_CASE("headless and Vulkan uploads share exact screen-space line expansion") {
  std::array<tetra_viewer::SceneVertex,2> line{};
  line[0].position[0]=1.0F;
  line[0].position[1]=2.0F;
  line[0].position[2]=3.0F;
  line[1].position[0]=4.0F;
  line[1].position[1]=5.0F;
  line[1].position[2]=6.0F;
  line[0].colour[0]=0.25F;
  line[0].colour[1]=0.50F;
  line[0].colour[2]=0.75F;
  std::vector<tetra_viewer::SceneVertex> ribbons;
  tetra_viewer::expand_line_segments_for_upload(line,ribbons);
  REQUIRE(ribbons.size()==6U);
  for(const auto& vertex:ribbons){
    CHECK(vertex.position[0]==1.0F);
    CHECK(vertex.position[1]==2.0F);
    CHECK(vertex.position[2]==3.0F);
    CHECK(vertex.normal[0]==4.0F);
    CHECK(vertex.normal[1]==5.0F);
    CHECK(vertex.normal[2]==6.0F);
    CHECK(vertex.colour[0]==0.25F);
    CHECK(vertex.colour[1]==0.50F);
    CHECK(vertex.colour[2]==0.75F);
  }
  CHECK(ribbons[0].diagnostics[0]==0.0F);
  CHECK(ribbons[0].diagnostics[1]==-1.0F);
  CHECK(ribbons[2].diagnostics[0]==1.0F);
  CHECK(ribbons[2].diagnostics[1]==1.0F);
  tetra_viewer::expand_line_segments_for_upload({},ribbons);
  CHECK(ribbons.empty());
}

TEST_CASE("surface geometry hashes ignore draw order but preserve winding and edges") {
  const auto vertex=[](float x,float y,float z,float marker=0.0F){
    tetra_viewer::SceneVertex result{};
    result.position[0]=x;result.position[1]=y;result.position[2]=z;
    result.diagnostics[0]=marker;
    return result;
  };
  const auto a=vertex(0.0F,0.0F,0.0F),b=vertex(1.0F,0.0F,0.0F);
  const auto c=vertex(1.0F,1.0F,0.0F),d=vertex(0.0F,1.0F,0.0F);
  tetra_viewer::PreparedScene first;
  first.triangle_vertices={a,b,c,a,c,d};
  first.surface_line_vertices={a,b,b,c,a,c,c,d,a,d};
  const auto first_hashes=tetra_viewer::surface_geometry_hashes(first);
  CHECK(first_hashes.triangle_count==2U);
  CHECK(first_hashes.edge_count==5U);
  CHECK(first_hashes.edge_incidence_count==6U);
  CHECK(first_hashes.wire_edge_count==first_hashes.edge_count);
  CHECK(first_hashes.wire_edge_hash==first_hashes.edge_hash);

  tetra_viewer::PreparedScene reordered;
  reordered.triangle_vertices={c,d,a,b,c,a};
  CHECK(tetra_viewer::surface_geometry_hashes(reordered).triangle_hash==
        first_hashes.triangle_hash);
  CHECK(tetra_viewer::surface_geometry_hashes(reordered).edge_hash==
        first_hashes.edge_hash);

  tetra_viewer::PreparedScene reversed;
  reversed.triangle_vertices={a,c,b,a,c,d};
  const auto reversed_hashes=tetra_viewer::surface_geometry_hashes(reversed);
  CHECK(reversed_hashes.triangle_hash!=first_hashes.triangle_hash);
  CHECK(reversed_hashes.edge_hash==first_hashes.edge_hash);
  CHECK(reversed_hashes.edge_incidence_hash==first_hashes.edge_incidence_hash);

  auto duplicated=first;
  duplicated.triangle_vertices.insert(
      duplicated.triangle_vertices.end(),{a,b,c});
  const auto duplicated_hashes=tetra_viewer::surface_geometry_hashes(duplicated);
  CHECK(duplicated_hashes.edge_hash==first_hashes.edge_hash);
  CHECK(duplicated_hashes.edge_incidence_hash!=first_hashes.edge_incidence_hash);
  CHECK(duplicated_hashes.edge_incidence_count==9U);

  auto reclassified=first;
  for(std::size_t corner=0;corner<3U;++corner)
    reclassified.triangle_vertices[corner].colour[0]=1.0F;
  const auto reclassified_hashes=tetra_viewer::surface_geometry_hashes(reclassified);
  CHECK(reclassified_hashes.triangle_hash==first_hashes.triangle_hash);
  CHECK(reclassified_hashes.material_boundary_hash!=
        first_hashes.material_boundary_hash);

  auto missing_wire=first;
  missing_wire.surface_line_vertices.resize(
      missing_wire.surface_line_vertices.size()-2U);
  const auto missing_wire_hashes=tetra_viewer::surface_geometry_hashes(missing_wire);
  CHECK(missing_wire_hashes.edge_hash==first_hashes.edge_hash);
  CHECK(missing_wire_hashes.wire_edge_hash!=first_hashes.wire_edge_hash);
  CHECK(missing_wire_hashes.wire_edge_count+1U==first_hashes.wire_edge_count);

  const auto volume_a=vertex(2.0F,0.0F,0.0F,-1.0F);
  const auto volume_b=vertex(2.0F,1.0F,0.0F,-1.0F);
  const auto volume_c=vertex(2.0F,0.0F,1.0F,-1.0F);
  first.triangle_vertices.insert(
      first.triangle_vertices.end(),{volume_a,volume_b,volume_c});
  CHECK(tetra_viewer::surface_geometry_hashes(first).triangle_hash==
        first_hashes.triangle_hash);
  CHECK(tetra_viewer::surface_geometry_hashes(first).edge_hash==
        first_hashes.edge_hash);
}

TEST_CASE("viewer submits only exposed faces of full-tetrahedron material") {
  const tetra::Sphere containing_sphere{{0.5, 0.5, 0.5}, 2.0};
  const auto six = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  const auto twenty_four = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  const auto six_scene = tetra_viewer::prepare_scene(
      six, containing_sphere, tetra_viewer::MaterialRule::all_vertices_inside, true, false, false);
  const auto twenty_four_scene = tetra_viewer::prepare_scene(
      twenty_four, containing_sphere, tetra_viewer::MaterialRule::all_vertices_inside, true, false, false);
  CHECK(six_scene.triangle_vertices.size() == 12 * 3);
  CHECK(twenty_four_scene.triangle_vertices.size() == 24 * 3);
}

TEST_CASE("material rules are registered and select distinct full-tetrahedron volumes") {
  CHECK(tetra_viewer::material_rules.size() == 7);
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::all_vertices_inside) == "all-vertices");
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::centroid_inside) == "centroid");
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::majority_vertices_inside) == "vertex-majority");
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::any_overlap) == "any-overlap");
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::variational) == "variational");
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::variational_faithful) == "variational-faithful");
  CHECK(tetra_viewer::material_rule_key(tetra_viewer::MaterialRule::variational_smooth) == "variational-smooth");

  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh, sphere, camera, 28.0, 9));
  const auto conservative = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::MaterialRule::all_vertices_inside, true, false, false);
  const auto centroid = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::MaterialRule::centroid_inside, true, false, false);
  const auto majority = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::MaterialRule::majority_vertices_inside, true, false, false);
  const auto overlap = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::MaterialRule::any_overlap, true, false, false);
  const auto variational = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::MaterialRule::variational, true, false, false);
  CHECK(conservative.selected_count < majority.selected_count);
  CHECK(majority.selected_count < centroid.selected_count);
  CHECK(centroid.selected_count < overlap.selected_count);
  CHECK(conservative.triangle_vertices.size() % 3 == 0);
  CHECK(centroid.triangle_vertices.size() % 3 == 0);
  CHECK(majority.triangle_vertices.size() % 3 == 0);
  CHECK(overlap.triangle_vertices.size() % 3 == 0);
  CHECK(variational.whole_cell_boundary_faces>0);
  CHECK(variational.whole_cell_nonmanifold_edges==0);
  CHECK(variational.whole_cell_hash!=0);
}

TEST_CASE("surface methods include a complete experimental tetrahedral layer") {
  CHECK(tetra_viewer::surface_methods.size() == 7);
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::full_tetrahedra) == "full-tetrahedra");
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::marching_tetrahedra) == "marching-tetrahedra");
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::lattice_cleaving) == "lattice-cleaving");
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::tetrahedral_layer) == "tetrahedral-layer");
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::dual_contouring) == "dual-contouring");
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::four_hexahedra) == "four-hexahedra");
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::mixed_depth_dual) == "mixed-depth-dual");
  CHECK(tetra_viewer::surface_method_key(tetra_viewer::SurfaceMethod::surface_optimization) == "surface-optimization");
  CHECK(std::ranges::find(tetra_viewer::surface_methods,
        tetra_viewer::SurfaceMethod::four_hexahedra)==
        tetra_viewer::surface_methods.end());
  CHECK(std::ranges::find(tetra_viewer::surface_methods,
        tetra_viewer::SurfaceMethod::mixed_depth_dual)!=
        tetra_viewer::surface_methods.end());
  CHECK(tetra_viewer::headless_surface_methods.size()==8U);
  CHECK(std::ranges::find(tetra_viewer::headless_surface_methods,
        tetra_viewer::SurfaceMethod::four_hexahedra)!=
        tetra_viewer::headless_surface_methods.end());

  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh, sphere, camera, 40.0, 4));
  const auto surface = tetra::extract_isosurface(mesh, sphere);
  const auto scene = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::SurfaceMethod::tetrahedral_layer,
      tetra_viewer::MaterialRule::all_vertices_inside, true, true, false);
  CHECK_FALSE(surface.empty());
  CHECK(scene.surface_layer_tetrahedra == surface.size() * 3);
  CHECK(scene.selected_count == scene.surface_layer_tetrahedra);
  CHECK_FALSE(scene.triangle_vertices.empty());
  CHECK_FALSE(scene.hierarchy_line_vertices.empty());
}

TEST_CASE("surface patch dependency contracts cover every registered method") {
  using tetra_viewer::SurfaceMethod;
  using tetra_viewer::SurfacePatchNeighbourhood;
  struct Expected {
    SurfaceMethod method;
    SurfacePatchNeighbourhood neighbourhood;
    unsigned int halo_steps;
    bool patchable;
  };
  constexpr auto global=std::numeric_limits<std::uint8_t>::max();
  constexpr std::array expected{
      Expected{SurfaceMethod::full_tetrahedra,SurfacePatchNeighbourhood::global,
               global,false},
      Expected{SurfaceMethod::marching_tetrahedra,SurfacePatchNeighbourhood::owner,
               0U,true},
      Expected{SurfaceMethod::lattice_cleaving,SurfacePatchNeighbourhood::owner,
               0U,true},
      Expected{SurfaceMethod::tetrahedral_layer,SurfacePatchNeighbourhood::global,
               global,false},
      Expected{SurfaceMethod::dual_contouring,
               SurfacePatchNeighbourhood::incident_edge_star,1U,true},
      Expected{SurfaceMethod::mixed_depth_dual,
               SurfacePatchNeighbourhood::incident_vertex_star,1U,true},
      Expected{SurfaceMethod::surface_optimization,SurfacePatchNeighbourhood::global,
               global,false},
  };
  REQUIRE(expected.size()==tetra_viewer::surface_methods.size());
  for(std::size_t index=0;index<expected.size();++index){
    CAPTURE(tetra_viewer::surface_method_key(expected[index].method));
    CHECK(expected[index].method==tetra_viewer::surface_methods[index]);
    const auto dependency=tetra_viewer::surface_patch_dependency(expected[index].method);
    CHECK(dependency.neighbourhood==expected[index].neighbourhood);
    CHECK(static_cast<unsigned int>(dependency.halo_steps)==expected[index].halo_steps);
    CHECK(dependency.patchable()==expected[index].patchable);
    CHECK_FALSE(dependency.reason.empty());
    CHECK(tetra_viewer::surface_patch_neighbourhood_key(
              dependency.neighbourhood)!="unknown");
  }
  const auto research=tetra_viewer::surface_patch_dependency(
      SurfaceMethod::four_hexahedra);
  CHECK(research.neighbourhood==SurfacePatchNeighbourhood::owner);
  CHECK(research.halo_steps==0U);
  CHECK(research.patchable());
}

TEST_CASE("four-hexahedra extractor is closed and outward across BCC transition strategies") {
  using Point=std::array<std::uint64_t,3>;
  using Edge=std::array<Point,2>;
  using Face=std::array<Point,3>;
  const auto point_key=[](tetra::Vec3 point){
    return Point{{std::bit_cast<std::uint64_t>(point.x==0.0?0.0:point.x),
                  std::bit_cast<std::uint64_t>(point.y==0.0?0.0:point.y),
                  std::bit_cast<std::uint64_t>(point.z==0.0?0.0:point.z)}};
  };
  for(const auto strategy:{tetra::BccTransitionStrategy::crystalline_restricted,
                           tetra::BccTransitionStrategy::complete_minimal}){
    CAPTURE(strategy);
    auto mesh=tetra::TetMesh::make_unit_cube(
        tetra::SubdivisionMethod::bcc_red_green);
    if(strategy==tetra::BccTransitionStrategy::complete_minimal)
      REQUIRE(mesh.set_transition_strategy(strategy));
    const tetra::Sphere sphere{};
    const tetra::Camera camera{};
    static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,42.0,6));
    const auto triangles=tetra::extract_four_hexahedra_isosurface(mesh,sphere);
    REQUIRE_FALSE(triangles.empty());
    std::map<Edge,std::size_t> edges;
    std::map<Face,std::size_t> faces;
    for(const auto& triangle:triangles){
      const auto normal=[](tetra::Vec3 a,tetra::Vec3 b){
        return tetra::Vec3{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,
                           a.x*b.y-a.y*b.x};
      }(triangle.b-triangle.a,triangle.c-triangle.a);
      const auto centre=(triangle.a+triangle.b+triangle.c)/3.0;
      const auto outward=sphere.normal(centre);
      CHECK(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z>0.0);
      Face face{{point_key(triangle.a),point_key(triangle.b),point_key(triangle.c)}};
      std::sort(face.begin(),face.end());
      ++faces[face];
      for(const auto pair:std::array<std::array<std::size_t,2>,3>{{
              {{0U,1U}},{{1U,2U}},{{2U,0U}}}}){
        Edge edge{{face[pair[0]],face[pair[1]]}};
        if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
        ++edges[edge];
      }
    }
    CHECK(std::ranges::all_of(faces,[](const auto& item){return item.second==1U;}));
    CHECK(std::ranges::all_of(edges,[](const auto& item){return item.second==2U;}));
  }
}

TEST_CASE("four-hexahedra owner patches retain field samples and invalidate locally") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,42.0,3));
  tetra_viewer::SceneCache cache;
  const auto update=[&](std::uint64_t field_revision,
                        tetra_viewer::SurfaceMethod method){
    return cache.update_scene(
        mesh,sphere,field_revision,method,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  };
  REQUIRE(update(12U,tetra_viewer::SurfaceMethod::four_hexahedra));
  auto metrics=cache.surface_patch_metrics();
  CHECK(metrics.active);
  CHECK(metrics.full_rebuild);
  CHECK(metrics.field_sample_records==mesh.conforming_volume().size());
  CHECK(metrics.evaluated_field_samples==
        metrics.field_sample_records*tetra::four_hexahedra_field_samples_per_cell);
  CHECK(metrics.reused_field_samples==0U);
  CHECK(cache.scene().four_hexahedra_triangles==metrics.output_triangles);
  const auto monolithic=[&]{
    return tetra_viewer::prepare_scene(
        mesh,sphere,tetra_viewer::SurfaceMethod::four_hexahedra,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  };
  CHECK(tetra_viewer::surface_geometry_hashes(cache.scene())==
        tetra_viewer::surface_geometry_hashes(monolithic()));

  REQUIRE(update(12U,tetra_viewer::SurfaceMethod::marching_tetrahedra));
  REQUIRE(update(12U,tetra_viewer::SurfaceMethod::four_hexahedra));
  metrics=cache.surface_patch_metrics();
  CHECK(metrics.full_rebuild);
  CHECK(metrics.evaluated_field_samples==0U);
  CHECK(metrics.reused_field_samples==
        metrics.field_sample_records*tetra::four_hexahedra_field_samples_per_cell);

  const auto split=std::ranges::find_if(
      mesh.logical_red_owners(),[&](tetra::TetId owner){
        return tetra::classify_tetrahedron(mesh,owner,sphere)==
            tetra::SurfaceRelation::intersecting;
      });
  REQUIRE(split!=mesh.logical_red_owners().end());
  REQUIRE(mesh.refine_selected_binary({*split}));
  REQUIRE(update(12U,tetra_viewer::SurfaceMethod::four_hexahedra));
  metrics=cache.surface_patch_metrics();
  CHECK_FALSE(metrics.full_rebuild);
  CHECK(metrics.rebuilt_patches>0U);
  CHECK(metrics.rebuilt_patches<cache.surface_patch_records().size());
  CHECK(metrics.reused_patches>0U);
  CHECK(metrics.field_sample_records==mesh.conforming_volume().size());
  CHECK(tetra_viewer::surface_geometry_hashes(cache.scene())==
        tetra_viewer::surface_geometry_hashes(monolithic()));

  REQUIRE(update(13U,tetra_viewer::SurfaceMethod::four_hexahedra));
  metrics=cache.surface_patch_metrics();
  CHECK(metrics.full_rebuild);
  CHECK(metrics.reused_field_samples==0U);
  CHECK(metrics.evaluated_field_samples==
        metrics.field_sample_records*tetra::four_hexahedra_field_samples_per_cell);
}

TEST_CASE("four-hexahedra remains headless after leaving the viewer selector") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=6,set-volume-connection=hierarchy-cells,"
      "set-surface-method=four-hexahedra,prepare-scene",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"surface_method\":\"four-hexahedra\"")!=std::string::npos);
  CHECK(text.find("\"four_hexahedra_triangles\":0")==std::string::npos);
  CHECK(text.find("\"surface_patch_active\":true")!=std::string::npos);
  CHECK(text.find("\"surface_patch_evaluated_field_samples\":0")==
        std::string::npos);
  CHECK(text.find("\"surface_patch_field_sample_records\":0")==
        std::string::npos);
}

TEST_CASE("headless surface statistics expose patch dependency contracts") {
  for(const auto method:tetra_viewer::surface_methods){
    CAPTURE(tetra_viewer::surface_method_key(method));
    const auto dependency=tetra_viewer::surface_patch_dependency(method);
    const auto neighbourhood=std::string(
        tetra_viewer::surface_patch_neighbourhood_key(dependency.neighbourhood));
    const auto script="set-volume-connection=hierarchy-cells,set-surface-method="+
        std::string(tetra_viewer::surface_method_key(method))+",stats";
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
    CHECK(errors.str().empty());
    const auto text=output.str();
    CHECK(text.find("\"event\":\"stats\"")!=std::string::npos);
    CHECK(text.find("\"surface_patch_neighbourhood\":\""+neighbourhood+"\"")!=
          std::string::npos);
    CHECK(text.find(std::string("\"surface_patchable\":")+
          (dependency.patchable()?"true":"false"))!=std::string::npos);
    const auto halo=dependency.patchable()?
        std::to_string(dependency.halo_steps):std::string("null");
    CHECK(text.find("\"surface_patch_halo_steps\":"+halo)!=std::string::npos);
    CHECK(text.find("\"surface_patch_reason\":\""+
          std::string(dependency.reason)+"\"")!=std::string::npos);
  }
}

TEST_CASE("marching tetrahedra is a directly selectable primal surface") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh, sphere, camera, 40.0, 4));
  const auto extracted = tetra::extract_isosurface(mesh, sphere);
  const auto scene = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside, true, false, true, false);
  REQUIRE_FALSE(extracted.empty());
  CHECK(scene.marching_tetrahedra_triangles == extracted.size());
  CHECK(scene.triangle_vertices.size() == extracted.size()*3);
  CHECK(scene.dual_contour_triangles == 0);
  CHECK(scene.surface_layer_tetrahedra == 0);
}

TEST_CASE("lattice cleaving replaces only sign-changing hierarchy leaves") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh, sphere, camera, 40.0, 4));
  const auto revision = mesh.revision();
  const auto leaves = mesh.active_leaves();
  std::size_t expected_cleaved = 0;
  for (const auto id : leaves) {
    std::size_t inside = 0;
    for (const auto vertex : mesh.tetrahedron(id).vertices)
      inside += sphere.signed_distance(mesh.vertices()[vertex]) < 0.0 ? 1U : 0U;
    if (inside == 1) ++expected_cleaved;
    else if (inside == 2 || inside == 3) expected_cleaved += 3;
  }
  const auto scene = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::SurfaceMethod::lattice_cleaving,
      tetra_viewer::MaterialRule::all_vertices_inside, true, false, true, false);
  CHECK(expected_cleaved > 0);
  CHECK(scene.cleaved_tetrahedra == expected_cleaved);
  CHECK(scene.cleaved_cells.size() == expected_cleaved);
  CHECK(scene.cleaved_volume > 0.0);
  CHECK(scene.cleaved_volume < 1.0);
  for(const auto& cell:scene.cleaved_cells){
    const auto ab=cell[1]-cell[0],ac=cell[2]-cell[0],ad=cell[3]-cell[0];
    const double determinant=ab.x*(ac.y*ad.z-ac.z*ad.y)-ab.y*(ac.x*ad.z-ac.z*ad.x)+ab.z*(ac.x*ad.y-ac.y*ad.x);
    CHECK(determinant>0.0);
  }
  using PointKey=std::array<long long,3>;
  using FaceKey=std::array<PointKey,3>;
  std::map<FaceKey,std::size_t> face_counts;
  const auto point_key=[](tetra::Vec3 point){
    constexpr double scale=1.0e10;
    return PointKey{{std::llround(point.x*scale),std::llround(point.y*scale),std::llround(point.z*scale)}};
  };
  constexpr std::array<std::array<std::size_t,3>,4> faces{{{{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  const auto add_faces=[&](const std::array<tetra::Vec3,4>& cell){
    for(const auto& face:faces){
      FaceKey key{{point_key(cell[face[0]]),point_key(cell[face[1]]),point_key(cell[face[2]])}};
      std::sort(key.begin(),key.end());
      ++face_counts[key];
    }
  };
  for(const auto id:mesh.active_leaves()){
    std::array<tetra::Vec3,4> cell{};
    bool all_inside=true;
    for(std::size_t vertex=0;vertex<4;++vertex){
      cell[vertex]=mesh.vertices()[mesh.tetrahedron(id).vertices[vertex]];
      all_inside&=sphere.signed_distance(cell[vertex])<0.0;
    }
    if(all_inside)add_faces(cell);
  }
  for(const auto& cell:scene.cleaved_cells)add_faces(cell);
  for(const auto& [face,count]:face_counts){
    CHECK(count<=2);
    if(count==1)for(const auto& point:face){
      const tetra::Vec3 position{point[0]/1.0e10,point[1]/1.0e10,point[2]/1.0e10};
      CHECK(std::abs(sphere.signed_distance(position))<1e-8);
    }
  }
  CHECK_FALSE(scene.triangle_vertices.empty());
  CHECK(mesh.revision() == revision);
  CHECK(mesh.active_leaves() == leaves);
}

TEST_CASE("surface optimization remains on the field and preserves orientation") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));
  const auto marching=tetra_viewer::prepare_scene(mesh,sphere,tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false);
  const auto optimized=tetra_viewer::prepare_scene(mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false);
  REQUIRE(optimized.optimized_surface_vertices>0);
  REQUIRE(optimized.triangle_vertices.size()==marching.triangle_vertices.size());
  bool changed=false;
  for(std::size_t triangle=0;triangle<optimized.triangle_vertices.size();triangle+=3){
    std::array<tetra::Vec3,3> points{};
    for(std::size_t corner=0;corner<3;++corner){
      const auto& vertex=optimized.triangle_vertices[triangle+corner];
      points[corner]={vertex.position[0],vertex.position[1],vertex.position[2]};
      CHECK(std::abs(sphere.signed_distance(points[corner]))<1e-6);
      const double normal_length=std::sqrt(
          vertex.normal[0]*vertex.normal[0]+vertex.normal[1]*vertex.normal[1]+vertex.normal[2]*vertex.normal[2]);
      CHECK(normal_length==doctest::Approx(1.0).epsilon(1e-6));
      const auto& original=marching.triangle_vertices[triangle+corner];
      changed|=vertex.position[0]!=original.position[0]||vertex.position[1]!=original.position[1]||vertex.position[2]!=original.position[2];
    }
    const auto ab=points[1]-points[0],ac=points[2]-points[0];
    const tetra::Vec3 normal{ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,ab.x*ac.y-ab.y*ac.x};
    const auto centre=(points[0]+points[1]+points[2])/3.0;
    const auto outward=centre-sphere.centre;
    CHECK(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z>0.0);
  }
  CHECK(changed);
}

TEST_CASE("bounded Jacobi is order independent and reproduces a five-ring core") {
  const auto make_positions=[](std::size_t count){
    std::vector<tetra::Vec3> positions(count);
    for(std::size_t vertex=0;vertex<count;++vertex)
      positions[vertex]={static_cast<double>(vertex*vertex+3U),0.0,0.0};
    return positions;
  };
  const auto pull_from_next=[](std::span<const tetra::Vec3> previous,
                               std::size_t vertex,std::uint32_t)
      ->std::optional<tetra::Vec3>{
    return previous[std::min(vertex+1U,previous.size()-1U)];
  };
  const auto same_point=[](tetra::Vec3 first,tetra::Vec3 second){
    return first.x==second.x&&first.y==second.y&&first.z==second.z;
  };

  auto monolithic=make_positions(8U),reverse=monolithic;
  std::vector<std::size_t> reverse_order(monolithic.size());
  std::iota(reverse_order.rbegin(),reverse_order.rend(),0U);
  const auto forward_metrics=tetra_viewer::run_bounded_jacobi(
      monolithic,tetra_viewer::surface_optimizer_passes,{},{},pull_from_next);
  const auto reverse_metrics=tetra_viewer::run_bounded_jacobi(
      reverse,tetra_viewer::surface_optimizer_passes,{},reverse_order,pull_from_next);
  REQUIRE(reverse.size()==monolithic.size());
  for(std::size_t vertex=0;vertex<reverse.size();++vertex)
    CHECK(same_point(reverse[vertex],monolithic[vertex]));
  CHECK(reverse_metrics.passes==forward_metrics.passes);
  CHECK(reverse_metrics.proposals==forward_metrics.proposals);
  CHECK(reverse_metrics.accepted==forward_metrics.accepted);

  auto five_ring=make_positions(8U);
  const std::vector<std::uint32_t> distances{0U,1U,2U,3U,4U,5U,6U,7U};
  const auto patch_metrics=tetra_viewer::run_bounded_jacobi(
      five_ring,tetra_viewer::surface_optimizer_passes,distances,{},pull_from_next);
  CHECK(same_point(five_ring.front(),monolithic.front()));
  CHECK(patch_metrics.passes==tetra_viewer::surface_optimizer_passes);
  CHECK(patch_metrics.proposals==15U);

  // A wider halo cannot affect the owned core in five passes, while omitting
  // the fifth-ring input changes it. The fifth ring is read on the first pass
  // but does not itself need to be updated.
  auto wider=make_positions(12U);
  std::vector<std::uint32_t> wider_distances(wider.size());
  std::iota(wider_distances.begin(),wider_distances.end(),0U);
  tetra_viewer::run_bounded_jacobi(
      wider,tetra_viewer::surface_optimizer_passes,wider_distances,{},pull_from_next);
  CHECK(same_point(wider.front(),monolithic.front()));

  auto four_ring_input=make_positions(8U);
  four_ring_input[5U].x=-1000.0;
  tetra_viewer::run_bounded_jacobi(four_ring_input,
      tetra_viewer::surface_optimizer_passes,distances,{},pull_from_next);
  CHECK_FALSE(same_point(four_ring_input.front(),monolithic.front()));
}

TEST_CASE("bounded surface optimization is globally keyed and worker invariant") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.44};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,42.0,9));
  tetra::GeometryExecutor one({.worker_count=1U,.blocks_per_worker=1U});
  tetra::GeometryExecutor four({.worker_count=4U,.blocks_per_worker=7U});
  const auto prepare=[&](tetra::GeometryExecutor& executor){
    return tetra_viewer::prepare_scene(mesh,sphere,
        tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
        false,false,0.5,tetra_viewer::VolumeConnectionMethod::adaptive_cleaving,
        tetra_viewer::StencilConstruction::fixed,
        tetra_viewer::StencilSelectionObjective::balanced,{}, {},false,&executor);
  };
  const auto serial=prepare(one),parallel=prepare(four);
  CHECK(serial.connected_surface_hash==parallel.connected_surface_hash);
  CHECK(serial.standalone_surface_hash==parallel.standalone_surface_hash);
  CHECK(serial.optimizer_passes==tetra_viewer::surface_optimizer_passes);
  CHECK(serial.optimizer_dependency_halo_rings==
        tetra_viewer::surface_optimizer_dependency_halo_rings);
  CHECK(serial.optimizer_dependency_halo_rings==serial.optimizer_passes);
  CHECK(serial.connected_volume_global_keys==parallel.connected_volume_global_keys);
  auto keys=serial.connected_volume_global_keys;
  std::ranges::sort(keys);
  CHECK(std::ranges::adjacent_find(keys)==keys.end());
}

TEST_CASE("blocked five-ring connected surface matches the production oracle") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.44};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,38.0,9));
  const auto checkpoint=tetra::make_world_cut_checkpoint(mesh,3U,1U);
  tetra::WorldCutDirectory directory(checkpoint);
  const auto blocked=tetra_viewer::build_blocked_derived_surface(
      mesh,directory,sphere);
  const auto monolithic=tetra_viewer::prepare_scene(mesh,sphere,
      tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,0.5,tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  CHECK(blocked.canonical_surface_hash==monolithic.connected_surface_hash);
  CHECK(blocked.vertices.size()==blocked.metrics.source_vertices);
  CHECK(blocked.triangles.size()==blocked.metrics.source_triangles);
  CHECK(blocked.snapshots.size()==blocked.metrics.surface_blocks);
  CHECK(blocked.metrics.halo_amplification>=1.0);
  CHECK(blocked.metrics.connected_volume_valid);
  CHECK(blocked.metrics.minimum_connected_tet_quality_after>0.0);
  CHECK(blocked.metrics.block_generations==3U);
  const auto render=tetra_viewer::prepare_blocked_derived_surface_scene(
      blocked,sphere);
  CHECK(render.connected_surface_hash==blocked.canonical_surface_hash);
  REQUIRE(render.triangle_vertices.size()==blocked.triangles.size()*3U);
  const auto render_hashes=tetra_viewer::surface_geometry_hashes(render);
  CHECK(render_hashes.edge_count==render_hashes.wire_edge_count);
  for(std::size_t triangle=0;triangle<render.triangle_vertices.size();triangle+=3U){
    const auto& first=render.triangle_vertices[triangle];
    const auto& second=render.triangle_vertices[triangle+1U];
    const auto& third=render.triangle_vertices[triangle+2U];
    const tetra::Vec3 a{first.position[0],first.position[1],first.position[2]};
    const tetra::Vec3 b{second.position[0],second.position[1],second.position[2]};
    const tetra::Vec3 c{third.position[0],third.position[1],third.position[2]};
    const auto ab=b-a,ac=c-a;
    const tetra::Vec3 geometric{
        ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,
        ab.x*ac.y-ab.y*ac.x};
    const double geometric_length=std::sqrt(
        geometric.x*geometric.x+geometric.y*geometric.y+
        geometric.z*geometric.z);
    const double normal_length=std::sqrt(
        first.normal[0]*first.normal[0]+first.normal[1]*first.normal[1]+
        first.normal[2]*first.normal[2]);
    if(geometric_length>1.0e-12){
      CHECK(normal_length==doctest::Approx(1.0).epsilon(1.0e-6));
      CHECK(normal_length>0.0001);
    }
    for(std::size_t corner=1;corner<3U;++corner)
      for(std::size_t axis=0;axis<3U;++axis)
        CHECK(render.triangle_vertices[triangle+corner].normal[axis]==
              render.triangle_vertices[triangle].normal[axis]);
    CHECK(render.triangle_vertices[triangle].edge_flags==7.0F);
  }
  CHECK(std::ranges::all_of(blocked.snapshots,[](const auto& snapshot){
    return snapshot.metrics.optimizer_passes==
               tetra_viewer::surface_optimizer_passes&&
           snapshot.metrics.dependency_halo_rings==
               tetra_viewer::surface_optimizer_dependency_halo_rings;
  }));
  auto staged=directory.stage_derived_surfaces(blocked.snapshots,2U);
  CHECK(directory.derived_surfaces().empty());
  directory.publish(staged);
  const auto published=tetra_viewer::assemble_blocked_derived_surface(directory);
  CHECK(published.canonical_surface_hash==blocked.canonical_surface_hash);
  CHECK(published.vertices.size()==blocked.vertices.size());
  CHECK(published.triangles.size()==blocked.triangles.size());
  tetra::WorldCutDirectory restored(directory.checkpoint());
  const auto reloaded=tetra_viewer::assemble_blocked_derived_surface(restored);
  CHECK(reloaded.canonical_surface_hash==blocked.canonical_surface_hash);
  for(const auto& triangle:reloaded.triangles){
    std::array<tetra::Vec3,3> points{};
    for(std::size_t corner=0;corner<3U;++corner){
      const auto vertex=std::ranges::lower_bound(
          reloaded.vertices,triangle.vertices[corner],{},
          &tetra::WorldSurfaceVertex::key);
      REQUIRE(vertex!=reloaded.vertices.end());points[corner]=vertex->position;
    }
    const auto ab=points[1]-points[0],ac=points[2]-points[0];
    const tetra::Vec3 normal{ab.y*ac.z-ab.z*ac.y,
                             ab.z*ac.x-ab.x*ac.z,
                             ab.x*ac.y-ab.y*ac.x};
    const auto centre=(points[0]+points[1]+points[2])/3.0;
    const auto outward=sphere.normal(centre);
    CHECK(normal.x*outward.x+normal.y*outward.y+normal.z*outward.z>0.0);
  }
}

TEST_CASE("blocked surface is invariant across widths phases budgets grouping and workers") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.44};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,42.0,12));
  const auto monolithic=tetra_viewer::prepare_scene(mesh,sphere,
      tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,0.5,tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  tetra::GeometryExecutor serial({.worker_count=1U,.blocks_per_worker=1U});
  tetra::GeometryExecutor parallel({.worker_count=4U,.blocks_per_worker=7U});
  std::uint64_t reference{};
  for(const unsigned int width:{3U,4U,5U}){
    CAPTURE(width);
    tetra::WorldCutDirectory directory(
        tetra::make_world_cut_checkpoint(mesh,width,1U));
    const auto narrow=tetra_viewer::build_blocked_derived_surface(
        mesh,directory,sphere,{.operation_budget=1U,.job_group_size=1U},
        &serial);
    const auto grouped=tetra_viewer::build_blocked_derived_surface(
        mesh,directory,sphere,{.operation_budget=7U,.job_group_size=3U,
                               .reverse_job_order=true},&parallel);
    CHECK(narrow.canonical_surface_hash==monolithic.connected_surface_hash);
    CHECK(grouped.canonical_surface_hash==narrow.canonical_surface_hash);
    REQUIRE(grouped.vertices.size()==narrow.vertices.size());
    for(std::size_t vertex=0;vertex<narrow.vertices.size();++vertex){
      CHECK(grouped.vertices[vertex].key==narrow.vertices[vertex].key);
      CHECK(grouped.vertices[vertex].position.x==narrow.vertices[vertex].position.x);
      CHECK(grouped.vertices[vertex].position.y==narrow.vertices[vertex].position.y);
      CHECK(grouped.vertices[vertex].position.z==narrow.vertices[vertex].position.z);
    }
    CHECK(grouped.triangles==narrow.triangles);
    CHECK(grouped.metrics.source_vertices==narrow.metrics.source_vertices);
    CHECK(grouped.metrics.source_triangles==narrow.metrics.source_triangles);
    CHECK(grouped.metrics.scheduling_batches<=narrow.metrics.scheduling_batches);
    CHECK(narrow.metrics.scheduling_batches==narrow.metrics.surface_blocks);
    CHECK(grouped.metrics.worker_count==4U);
    CHECK(narrow.metrics.connected_volume_valid);
    CHECK(grouped.metrics.connected_volume_valid);
    CHECK(narrow.metrics.minimum_connected_tet_quality_after>0.0);
    CHECK(grouped.metrics.minimum_connected_tet_quality_after>0.0);
    if(reference==0U)reference=narrow.canonical_surface_hash;
    CHECK(narrow.canonical_surface_hash==reference);

    std::set<unsigned int> phases;
    std::set<std::uint8_t> roots;
    for(const auto& triangle:narrow.triangles){
      phases.insert(triangle.owner.red_depth()%width);
      roots.insert(triangle.owner.root_id());
    }
    CHECK(phases.size()>1U);
    CHECK(roots.size()==tetra::bcc_root_tetrahedron_count);
  }
}

TEST_CASE("blocked surfaces refine coarsen cancel and reject stale publication atomically") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.44};
  const auto root=tetra::WorldTetAddress::root(0U);
  tetra::WorldCutDirectory directory(
      tetra::make_world_cut_checkpoint(mesh,2U,1U));
  const auto initial=tetra_viewer::build_blocked_derived_surface(
      mesh,directory,sphere);
  directory.publish(directory.stage_derived_surfaces(initial.snapshots,2U));
  REQUIRE_FALSE(directory.derived_surfaces().empty());

  const auto pending=tetra_viewer::build_blocked_derived_surface(
      mesh,directory,sphere);
  const auto stale=directory.stage_derived_surfaces(pending.snapshots,3U);
  const auto split=directory.stage_transaction(
      {{{root,tetra::WorldTopologyOperation::split}}},3U);
  CHECK_FALSE(split.manifest.removed_surfaces().empty());
  REQUIRE(mesh.refine_selected_binary({tetra::make_tet_id(0U,1U)}));
  directory.publish(split.manifest);
  CHECK(directory.derived_surfaces().empty());
  CHECK_THROWS_AS(directory.publish(stale),std::invalid_argument);
  const auto refined=tetra_viewer::build_blocked_derived_surface(
      mesh,directory,sphere);
  const auto refined_oracle=tetra_viewer::prepare_scene(mesh,sphere,
      tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,0.5,tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  CHECK(refined.canonical_surface_hash==refined_oracle.connected_surface_hash);
  directory.publish(directory.stage_derived_surfaces(refined.snapshots,4U));

  std::stop_source canceled;canceled.request_stop();
  const auto published_hash=directory.checkpoint().canonical_hash();
  CHECK_THROWS_AS(static_cast<void>(tetra_viewer::build_blocked_derived_surface(
      mesh,directory,sphere,{},nullptr,canceled.get_token())),std::runtime_error);
  CHECK(directory.checkpoint().canonical_hash()==published_hash);

  const auto merge=directory.stage_transaction(
      {{{root,tetra::WorldTopologyOperation::merge}}},5U);
  CHECK_FALSE(merge.manifest.removed_surfaces().empty());
  REQUIRE(mesh.coarsen_selected_red({tetra::make_tet_id(0U,1U)}));
  directory.publish(merge.manifest);
  const auto restored=tetra_viewer::build_blocked_derived_surface(
      mesh,directory,sphere);
  CHECK(restored.canonical_surface_hash==initial.canonical_surface_hash);
}

TEST_CASE("blocked surface eviction never retains a partial fine shell") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.44};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,42.0,12));
  const auto available=tetra::make_world_cut_checkpoint(mesh,1U,1U);
  tetra::WorldCutDirectory directory(available);
  const auto fine=tetra_viewer::build_blocked_derived_surface(
      mesh,directory,sphere);
  REQUIRE(fine.snapshots.size()>tetra::bcc_root_tetrahedron_count);
  const auto fine_owners=directory.logical_owner_count();
  directory.publish(directory.stage_derived_surfaces(fine.snapshots,2U));
  REQUIRE_FALSE(directory.derived_surfaces().empty());

  const auto update=directory.reconcile(
      available,{},tetra::bcc_root_tetrahedron_count,3U);
  CHECK(update.metrics.evicted_blocks>0U);
  CHECK(directory.logical_owner_count()<fine_owners);
  CHECK(directory.derived_surfaces().empty());
  CHECK(tetra_viewer::assemble_blocked_derived_surface(directory).triangles.empty());
  tetra::WorldCutDirectory reloaded(directory.checkpoint());
  CHECK(reloaded.derived_surfaces().empty());

  mesh.reset_active_hierarchy();
  mesh.refine_all_binary();
  CHECK(mesh.logical_red_owners().size()==directory.logical_owner_count());
  const auto coarse=tetra_viewer::build_blocked_derived_surface(
      mesh,directory,sphere);
  const auto oracle=tetra_viewer::prepare_scene(mesh,sphere,
      tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,0.5,tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  CHECK(coarse.canonical_surface_hash==oracle.connected_surface_hash);
  directory.publish(directory.stage_derived_surfaces(coarse.snapshots,4U));
  CHECK(tetra_viewer::assemble_blocked_derived_surface(directory).
        canonical_surface_hash==coarse.canonical_surface_hash);
}

TEST_CASE("diagnostic shading models and surface angle data are registered") {
  CHECK(tetra_viewer::shading_models.size() == 5);
  CHECK(tetra_viewer::shading_model_key(tetra_viewer::ShadingModel::studio_flat) == "studio-flat");
  CHECK(tetra_viewer::shading_model_key(tetra_viewer::ShadingModel::dihedral_angle) == "dihedral-angle");
  CHECK(tetra_viewer::shading_model_key(tetra_viewer::ShadingModel::normal_error) == "normal-error");
  CHECK(tetra_viewer::shading_model_key(tetra_viewer::ShadingModel::reflection_stripes) == "reflection-stripes");
  CHECK(tetra_viewer::shading_model_key(tetra_viewer::ShadingModel::stone_pbr) == "stone-pbr");

  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,40.0,5));
  const auto scene=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,false,false);
  CHECK_FALSE(scene.triangle_vertices.empty());
  CHECK(scene.mean_dihedral_degrees > 0.0);
  CHECK(scene.percentile95_dihedral_degrees >= scene.mean_dihedral_degrees);
  CHECK(scene.percentile99_dihedral_degrees >= scene.percentile95_dihedral_degrees);
  CHECK(scene.maximum_dihedral_degrees >= scene.percentile99_dihedral_degrees);
  CHECK(scene.maximum_dihedral_degrees >= scene.mean_dihedral_degrees);
  CHECK(scene.mean_normal_error_degrees >= 0.0);
  CHECK(scene.percentile99_normal_error_degrees >= scene.percentile95_normal_error_degrees);
  CHECK(scene.maximum_normal_error_degrees >= scene.mean_normal_error_degrees);
  CHECK(scene.minimum_surface_triangle_angle_degrees > 0.0);
  CHECK(scene.minimum_surface_triangle_angle_degrees <= 60.0);
  CHECK(scene.maximum_surface_triangle_edge_ratio >= 1.0);
  CHECK(scene.maximum_dihedral_degrees < 180.0);
  for(std::size_t triangle=0;triangle<scene.triangle_vertices.size();triangle+=3){
    const auto& first=scene.triangle_vertices[triangle];
    CHECK(std::isfinite(first.diagnostics[0]));
    CHECK(std::isfinite(first.diagnostics[1]));
    CHECK(first.edge_flags == doctest::Approx(7.0F));
    CHECK(scene.triangle_vertices[triangle+1].diagnostics[0] == first.diagnostics[0]);
    CHECK(scene.triangle_vertices[triangle+2].diagnostics[1] == first.diagnostics[1]);
    CHECK(scene.triangle_vertices[triangle+1].edge_flags == first.edge_flags);
    CHECK(scene.triangle_vertices[triangle+2].edge_flags == first.edge_flags);
  }
}

TEST_CASE("stone PBR keeps fixed-light terrain exposure stable while viewing angle changes") {
  const auto overhead=tetra_viewer::stone_pbr_colour(
      {0.0,1.0,0.0},{0.0,1.0,0.0});
  const auto oblique=tetra_viewer::stone_pbr_colour(
      {0.0,1.0,0.0},{0.8,0.6,0.0});
  for(std::size_t channel=0;channel<3U;++channel){
    CHECK(std::isfinite(overhead[channel]));
    CHECK(std::isfinite(oblique[channel]));
    CHECK(overhead[channel]>0.0);CHECK(overhead[channel]<1.0);
    CHECK(std::abs(overhead[channel]-oblique[channel])<0.08);
  }
  CHECK(std::abs(overhead[0]-overhead[2])<0.08);
}

TEST_CASE("world sun controls produce normalized sky directions") {
  const auto horizon=tetra_viewer::world_sun_direction(0.0,0.0);
  CHECK(horizon.x==doctest::Approx(1.0));
  CHECK(horizon.y==doctest::Approx(0.0));
  CHECK(horizon.z==doctest::Approx(0.0));
  const auto overhead=tetra_viewer::world_sun_direction(1.7,std::acos(-1.0)*0.5);
  CHECK(overhead.y==doctest::Approx(1.0));
  const auto initial=tetra_viewer::world_sun_direction(
      tetra_viewer::default_world_sun_azimuth_radians,
      tetra_viewer::default_world_sun_elevation_radians);
  const double length=std::sqrt(
      initial.x*initial.x+initial.y*initial.y+initial.z*initial.z);
  CHECK(length==doctest::Approx(1.0));
  CHECK(initial.x==doctest::Approx(-0.226338).epsilon(1.0e-5));
  CHECK(initial.y==doctest::Approx(0.0871558).epsilon(1.0e-5));
  CHECK(initial.z==doctest::Approx(-0.970142).epsilon(1.0e-5));
}

TEST_CASE("HDR scene colour uses the sampler2D-compatible image view") {
  CHECK(tetra_viewer::scene_colour_sample_dimension==
        tetra_viewer::SceneSampledImageDimension::two_d);
  CHECK(tetra_viewer::scene_colour_sample_dimension!=
        tetra_viewer::SceneSampledImageDimension::two_d_array);
}

TEST_CASE("camera-relative scene preparation preserves geometry at planet coordinates") {
  // Far beyond the point where a world-space float can preserve a unit cell.
  constexpr double world_offset=1.0e6;
  auto local_mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::maubach_diamond);
  auto world_mesh=tetra::TetMesh::make_cube(
      {world_offset,-world_offset,world_offset},1.0,
      tetra::SubdivisionMethod::maubach_diamond);
  local_mesh.refine_all_binary();
  world_mesh.refine_all_binary();

  tetra::Sphere local_field;
  tetra::Sphere world_field=local_field;
  world_field.centre=world_field.centre+
      tetra::Vec3{world_offset,-world_offset,world_offset};
  const auto local=tetra_viewer::prepare_scene(
      local_mesh,local_field,tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,true,true,true,false,
      false,false,0.5,tetra_viewer::VolumeConnectionMethod::quality_stencils,
      tetra_viewer::StencilConstruction::fixed,
      tetra_viewer::StencilSelectionObjective::balanced,
      {.surface_diagnostics=false,.summary_statistics=false});
  const tetra::Vec3 origin{world_offset,-world_offset,world_offset};
  const auto world=tetra_viewer::prepare_scene(
      world_mesh,world_field,tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,true,true,true,false,
      false,false,world_offset+0.5,
      tetra_viewer::VolumeConnectionMethod::quality_stencils,
      tetra_viewer::StencilConstruction::fixed,
      tetra_viewer::StencilSelectionObjective::balanced,
      {.surface_diagnostics=false,.summary_statistics=false,
       .render_origin=origin});

  CHECK(world.render_origin.x==origin.x);
  CHECK(world.render_origin.y==origin.y);
  CHECK(world.render_origin.z==origin.z);
  REQUIRE(world.triangle_vertices.size()==local.triangle_vertices.size());
  REQUIRE(world.hierarchy_line_vertices.size()==
          local.hierarchy_line_vertices.size());
  const auto compare=[](const tetra_viewer::SceneVertex& first,
                        const tetra_viewer::SceneVertex& second){
    for(std::size_t axis=0;axis<3U;++axis){
      CHECK(first.position[axis]==doctest::Approx(second.position[axis]).epsilon(1.0e-5));
      CHECK(first.normal[axis]==doctest::Approx(second.normal[axis]).epsilon(1.0e-5));
    }
  };
  for(std::size_t index=0;index<local.triangle_vertices.size();++index)
    compare(world.triangle_vertices[index],local.triangle_vertices[index]);
  for(std::size_t index=0;index<local.hierarchy_line_vertices.size();++index)
    compare(world.hierarchy_line_vertices[index],local.hierarchy_line_vertices[index]);
}

TEST_CASE("hierarchy and surface edges are independently selectable") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh, sphere, camera, 40.0, 4));

  for (const auto method : tetra_viewer::surface_methods) {
    CAPTURE(tetra_viewer::surface_method_name(method));
    const auto hierarchy_only = tetra_viewer::prepare_scene(
        mesh, sphere, method, tetra_viewer::MaterialRule::any_overlap,
        false, true, false, false);
    CHECK_FALSE(hierarchy_only.hierarchy_line_vertices.empty());
    CHECK(hierarchy_only.triangle_vertices.empty());

    const auto surface_only = tetra_viewer::prepare_scene(
        mesh, sphere, method, tetra_viewer::MaterialRule::any_overlap,
        false, false, true, false);
    CHECK(surface_only.hierarchy_line_vertices.empty());
    CHECK_FALSE(surface_only.triangle_vertices.empty());
    for (std::size_t triangle=0; triangle<surface_only.triangle_vertices.size(); triangle+=3) {
      CHECK(surface_only.triangle_vertices[triangle].barycentric[0] == 1.0F);
      CHECK(surface_only.triangle_vertices[triangle+1].barycentric[1] == 1.0F);
      CHECK(surface_only.triangle_vertices[triangle+2].barycentric[2] == 1.0F);
    }
  }
}

TEST_CASE("surface wireframe coverage is stable across orientation and scale") {
  constexpr double inverse_sqrt_two=0.7071067811865475244;
  const std::array<double,3> barycentric{{0.006,0.494,0.50}};
  const auto axis_aligned=tetra_viewer::wireframe_coverage(
      barycentric,{{0.010,0.0,0.0}},{{0.0,0.010,0.010}});
  const auto diagonal=tetra_viewer::wireframe_coverage(
      barycentric,
      {{0.010*inverse_sqrt_two,0.0,0.0}},
      {{0.010*inverse_sqrt_two,0.010,0.010}});
  CHECK(diagonal==doctest::Approx(axis_aligned).epsilon(1.0e-12));

  const auto twice_as_large=tetra_viewer::wireframe_coverage(
      {{0.003,0.497,0.50}},{{0.005,0.0,0.0}},{{0.0,0.005,0.005}});
  CHECK(twice_as_large==doctest::Approx(axis_aligned).epsilon(1.0e-12));
  CHECK(tetra_viewer::wireframe_coverage(
      {{0.001,0.499,0.50}},{{0.010,0.0,0.0}},{{0.0,0.010,0.010}})==doctest::Approx(0.9));
  CHECK(tetra_viewer::wireframe_coverage(
      {{0.016,0.484,0.50}},{{0.010,0.0,0.0}},{{0.0,0.010,0.010}})==doctest::Approx(0.0));
}

TEST_CASE("screen-space edge remains visible at the worst sub-pixel placement") {
  CHECK(tetra_viewer::screen_space_edge_coverage(0.0)==doctest::Approx(1.0));
  CHECK(tetra_viewer::screen_space_edge_coverage(0.5)>=0.5);
  CHECK(tetra_viewer::screen_space_edge_coverage(1.0)==doctest::Approx(0.0));
}

TEST_CASE("triangle wire remains visible halfway between pixel centres") {
  // Barycentric coordinate 0 is half a pixel from its edge when its
  // screen-space derivative has magnitude 0.01 per pixel.
  const double coverage=tetra_viewer::wireframe_coverage(
      {{0.005,0.495,0.5}},{{0.01,0.0,0.0}},{{0.0,0.01,0.01}},1);
  CHECK(coverage>=0.5);
}

TEST_CASE("screen-space edge tolerates adjacent face slope without showing hidden edges") {
  CHECK(tetra_viewer::screen_space_edge_depth_passes(0.500004,0.500000));
  CHECK_FALSE(tetra_viewer::screen_space_edge_depth_passes(0.50001,0.50000));
}

TEST_CASE("surface diagnostics cannot overwrite triangle edge selection") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,40.0,5));
  const auto scene=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false);
  REQUIRE_FALSE(scene.triangle_vertices.empty());
  // This proves the fixture contains diagnostic values that would have
  // disabled edges when the angle and mask incorrectly shared one float.
  CHECK(std::ranges::any_of(scene.triangle_vertices,[](const auto& vertex){
    return (static_cast<int>(vertex.diagnostics[1]+0.5F)&7)!=7;
  }));
  CHECK(std::ranges::all_of(scene.triangle_vertices,[](const auto& vertex){
    return (static_cast<int>(vertex.edge_flags+0.5F)&7)==7;
  }));
}

TEST_CASE("connected cleaved boundary is a single manifold triangle layer") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));
  const auto scene=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::full_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,
      true,false,true,false,true,true,1.0,
      tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  using Point=std::array<float,3>;
  using Edge=std::array<Point,2>;
  using Face=std::array<Point,3>;
  std::map<Face,std::size_t> faces;
  std::map<Edge,std::size_t> edges;
  for(std::size_t triangle=0;triangle+2<scene.triangle_vertices.size();triangle+=3){
    if(scene.triangle_vertices[triangle].diagnostics[0]>=-1.5F)continue;
    Face face{};
    for(std::size_t corner=0;corner<3;++corner){
      const auto& vertex=scene.triangle_vertices[triangle+corner];
      face[corner]={{vertex.position[0],vertex.position[1],vertex.position[2]}};
    }
    std::sort(face.begin(),face.end());
    ++faces[face];
    for(const auto pair:std::array<std::array<std::size_t,2>,3>{{{{0,1}},{{1,2}},{{2,0}}}}){
      Edge edge{{face[pair[0]],face[pair[1]]}};
      if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
      ++edges[edge];
    }
  }
  REQUIRE_FALSE(faces.empty());
  CHECK(std::ranges::all_of(faces,[](const auto& item){return item.second==1;}));
  CHECK(std::ranges::all_of(edges,[](const auto& item){return item.second==2;}));
}

TEST_CASE("scene cache follows repeated surface and subdivision method switches") {
  const tetra::Sphere sphere{};
  auto six = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  auto twenty_four = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(six, sphere, camera, 80.0, 3));
  static_cast<void>(tetra::refine_to_sphere(twenty_four, sphere, camera, 80.0, 3));
  tetra_viewer::SceneCache cache;

  CHECK(cache.update_scene(six, sphere, 0, tetra_viewer::SurfaceMethod::full_tetrahedra,
                           tetra_viewer::MaterialRule::all_vertices_inside, true, false, false));
  const auto first_generation = cache.scene_generation();
  CHECK(cache.scene().surface_layer_tetrahedra == 0);

  CHECK(cache.update_scene(twenty_four, sphere, 0, tetra_viewer::SurfaceMethod::tetrahedral_layer,
                           tetra_viewer::MaterialRule::all_vertices_inside, true, false, false));
  CHECK(cache.scene_generation() == first_generation + 1);
  CHECK(cache.scene().surface_layer_tetrahedra > 0);

  CHECK(cache.update_scene(six, sphere, 0, tetra_viewer::SurfaceMethod::full_tetrahedra,
                           tetra_viewer::MaterialRule::all_vertices_inside, true, false, false));
  CHECK(cache.scene_generation() == first_generation + 2);
  CHECK(cache.scene().surface_layer_tetrahedra == 0);

  CHECK(cache.update_scene(six, sphere, 0, tetra_viewer::SurfaceMethod::tetrahedral_layer,
                           tetra_viewer::MaterialRule::all_vertices_inside, true, false, false));
  CHECK(cache.scene_generation() == first_generation + 3);
  CHECK(cache.scene().surface_layer_tetrahedra > 0);
}

TEST_CASE("marching and lattice owner patches match monolithic surface hashes") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,48.0,6));
  constexpr std::array methods{
      tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::SurfaceMethod::lattice_cleaving};
  for(const auto method:methods){
    CAPTURE(tetra_viewer::surface_method_key(method));
    tetra_viewer::SceneCache cache;
    REQUIRE(cache.update_scene(
        mesh,sphere,7,method,tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells));
    const auto monolithic=tetra_viewer::prepare_scene(
        mesh,sphere,method,tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
    const auto patched_hashes=tetra_viewer::surface_geometry_hashes(cache.scene());
    const auto monolithic_hashes=tetra_viewer::surface_geometry_hashes(monolithic);
    CHECK(patched_hashes==monolithic_hashes);
    CHECK(patched_hashes.triangle_hash==monolithic_hashes.triangle_hash);
    CHECK(patched_hashes.edge_hash==monolithic_hashes.edge_hash);
    CHECK(patched_hashes.edge_incidence_hash==
          monolithic_hashes.edge_incidence_hash);
    CHECK(patched_hashes.material_boundary_hash==
          monolithic_hashes.material_boundary_hash);
    CHECK(patched_hashes.wire_edge_hash==patched_hashes.edge_hash);
    CHECK(patched_hashes.triangle_count==monolithic_hashes.triangle_count);
    CHECK(patched_hashes.edge_count==monolithic_hashes.edge_count);
    CHECK(patched_hashes.edge_incidence_count==patched_hashes.triangle_count*3U);
    CHECK(patched_hashes.wire_edge_count==patched_hashes.edge_count);
    for(std::size_t begin=0;begin+2U<cache.scene().triangle_vertices.size();begin+=3U){
      const auto* vertices=cache.scene().triangle_vertices.data()+begin;
      CHECK(static_cast<int>(vertices[0].edge_flags+0.5F)==7);
      for(std::size_t corner=0;corner<3U;++corner){
        CHECK(vertices[corner].barycentric[corner]==doctest::Approx(1.0F));
        CHECK(vertices[corner].barycentric[(corner+1U)%3U]==doctest::Approx(0.0F));
        CHECK(vertices[corner].barycentric[(corner+2U)%3U]==doctest::Approx(0.0F));
      }
    }
    const auto& metrics=cache.surface_patch_metrics();
    CHECK(metrics.active);
    CHECK_FALSE(metrics.global_fallback);
    CHECK(metrics.full_rebuild);
    CHECK(metrics.rebuilt_patches==mesh.logical_red_owners().size());
    CHECK(metrics.output_triangles==monolithic_hashes.triangle_count);
    CHECK(metrics.generated_triangles==metrics.output_triangles);
    CHECK(metrics.retained_bytes>0U);
    const auto records=cache.surface_patch_records();
    REQUIRE(records.size()==mesh.logical_red_owners().size());
    CHECK(std::ranges::is_sorted(records,{},&tetra_viewer::SurfacePatchRecord::logical_owner));
    std::vector<std::pair<std::size_t,std::size_t>> ranges;
    for(const auto& record:records){
      CHECK(record.mesh_revision==mesh.revision());
      CHECK(record.field_revision==7U);
      CHECK(record.triangle_count<=record.triangle_capacity);
      CHECK(std::isfinite(record.bounds_minimum.x));
      CHECK(std::isfinite(record.bounds_maximum.x));
      if(record.triangle_capacity!=0U)
        ranges.emplace_back(record.triangle_begin,
                            record.triangle_begin+record.triangle_capacity);
    }
    std::sort(ranges.begin(),ranges.end());
    for(std::size_t index=1;index<ranges.size();++index)
      CHECK(ranges[index-1].second<=ranges[index].first);
    if(method==tetra_viewer::SurfaceMethod::lattice_cleaving){
      CHECK(cache.scene().cleaved_tetrahedra==monolithic.cleaved_tetrahedra);
      CHECK(cache.scene().cleaved_volume==doctest::Approx(monolithic.cleaved_volume));
    }
  }
}

TEST_CASE("owner patch arena preserves unchanged ranges through split merge and fallback") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,48.0,3));
  tetra_viewer::SceneCache cache;
  const auto update=[&](std::uint64_t field_revision,
                        tetra_viewer::SurfaceMethod method){
    return cache.update_scene(
        mesh,sphere,field_revision,method,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  };
  REQUIRE(update(4,tetra_viewer::SurfaceMethod::marching_tetrahedra));
  struct Snapshot {
    std::size_t begin{},count{},capacity{};
    std::vector<unsigned char> bytes;
  };
  std::map<tetra::TetId,Snapshot> before;
  const auto capture=[&](auto& destination){
    const auto arena=cache.surface_patch_arena();
    for(const auto& record:cache.surface_patch_records()){
      Snapshot snapshot{record.triangle_begin,record.triangle_count,
                        record.triangle_capacity,{}};
      snapshot.bytes.resize(record.triangle_capacity*sizeof(tetra::Triangle));
      if(!snapshot.bytes.empty())std::memcpy(
          snapshot.bytes.data(),arena.data()+record.triangle_begin,
          snapshot.bytes.size());
      destination.emplace(record.logical_owner,std::move(snapshot));
    }
  };
  capture(before);
  const auto split=std::ranges::find_if(mesh.logical_red_owners(),[&](tetra::TetId owner){
    return tetra::classify_tetrahedron(mesh,owner,sphere)==
        tetra::SurfaceRelation::intersecting;
  });
  REQUIRE(split!=mesh.logical_red_owners().end());
  const auto parent=*split;
  REQUIRE(mesh.refine_selected_binary({parent}));
  REQUIRE(update(4,tetra_viewer::SurfaceMethod::marching_tetrahedra));
  const auto monolithic=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  CHECK(tetra_viewer::surface_geometry_hashes(cache.scene()).triangle_hash==
        tetra_viewer::surface_geometry_hashes(monolithic).triangle_hash);
  CHECK(tetra_viewer::surface_geometry_hashes(cache.scene()).edge_hash==
        tetra_viewer::surface_geometry_hashes(monolithic).edge_hash);
  CHECK(cache.surface_patch_metrics().rebuilt_patches>0U);
  CHECK(cache.surface_patch_metrics().reused_patches>0U);
  CHECK(cache.surface_patch_metrics().retired_patches>0U);
  std::size_t byte_stable{};
  const auto arena=cache.surface_patch_arena();
  for(const auto& record:cache.surface_patch_records()){
    const auto found=before.find(record.logical_owner);
    if(found==before.end()||std::binary_search(
         mesh.last_dirty_logical_owners().begin(),
         mesh.last_dirty_logical_owners().end(),record.logical_owner))continue;
    CHECK(record.triangle_begin==found->second.begin);
    CHECK(record.triangle_count==found->second.count);
    CHECK(record.triangle_capacity==found->second.capacity);
    std::vector<unsigned char> bytes(
        record.triangle_capacity*sizeof(tetra::Triangle));
    if(!bytes.empty())std::memcpy(bytes.data(),arena.data()+record.triangle_begin,
                                 bytes.size());
    CHECK(bytes==found->second.bytes);
    ++byte_stable;
  }
  CHECK(byte_stable>0U);

  std::vector<tetra::TetId> split_parents;
  for(const auto& [owner,snapshot]:before){
    static_cast<void>(snapshot);
    if(!std::binary_search(mesh.logical_red_owners().begin(),
                           mesh.logical_red_owners().end(),owner))
      split_parents.push_back(owner);
  }
  REQUIRE_FALSE(split_parents.empty());
  REQUIRE(mesh.coarsen_selected_red(split_parents));
  REQUIRE(update(4,tetra_viewer::SurfaceMethod::marching_tetrahedra));
  const auto merged_monolithic=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  CHECK(tetra_viewer::surface_geometry_hashes(cache.scene()).triangle_hash==
        tetra_viewer::surface_geometry_hashes(merged_monolithic).triangle_hash);
  CHECK(tetra_viewer::surface_geometry_hashes(cache.scene()).edge_hash==
        tetra_viewer::surface_geometry_hashes(merged_monolithic).edge_hash);
  CHECK(cache.surface_patch_metrics().retired_patches>0U);
  CHECK(cache.surface_patch_metrics().reused_patches>0U);

  CHECK(cache.update_scene(
      mesh,sphere,4,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells));
  CHECK_FALSE(cache.surface_patch_metrics().active);
  CHECK(cache.surface_patch_metrics().global_fallback);
  const auto retained_records=cache.surface_patch_records().size();
  REQUIRE(update(4,tetra_viewer::SurfaceMethod::lattice_cleaving));
  CHECK(cache.surface_patch_records().size()==retained_records);
  CHECK(cache.surface_patch_metrics().rebuilt_patches==0U);
  CHECK(cache.surface_patch_metrics().reused_patches==retained_records);
}

TEST_CASE("dual contour edge-star patches match monolithic mixed-depth topology") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,48.0,4));
  const auto split=std::ranges::find_if(
      mesh.logical_red_owners(),[&](tetra::TetId owner){
        return tetra::classify_tetrahedron(mesh,owner,sphere)==
            tetra::SurfaceRelation::intersecting;
      });
  REQUIRE(split!=mesh.logical_red_owners().end());
  REQUIRE(mesh.refine_selected_binary({*split}));

  tetra_viewer::SceneCache cache;
  REQUIRE(cache.update_scene(
      mesh,sphere,3,tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells));
  const auto monolithic=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  const auto patched_hashes=tetra_viewer::surface_geometry_hashes(cache.scene());
  const auto monolithic_hashes=tetra_viewer::surface_geometry_hashes(monolithic);
  CHECK(patched_hashes==monolithic_hashes);
  CHECK(patched_hashes.triangle_hash==monolithic_hashes.triangle_hash);
  CHECK(patched_hashes.edge_hash==monolithic_hashes.edge_hash);
  CHECK(patched_hashes.edge_incidence_hash==
        monolithic_hashes.edge_incidence_hash);
  CHECK(patched_hashes.material_boundary_hash==
        monolithic_hashes.material_boundary_hash);
  CHECK(patched_hashes.wire_edge_hash==patched_hashes.edge_hash);
  CHECK(patched_hashes.triangle_count==monolithic_hashes.triangle_count);
  CHECK(patched_hashes.edge_count==monolithic_hashes.edge_count);
  CHECK(patched_hashes.edge_incidence_count==patched_hashes.triangle_count*3U);
  CHECK(patched_hashes.wire_edge_count==patched_hashes.edge_count);
  CHECK(cache.scene().dual_contour_triangles==monolithic.dual_contour_triangles);
  CHECK(cache.surface_patch_metrics().active);
  CHECK_FALSE(cache.surface_patch_metrics().monolithic_fallback);
  CHECK_FALSE(cache.surface_patch_metrics().global_fallback);
  CHECK(cache.surface_patch_metrics().full_rebuild);
  CHECK(cache.surface_patch_metrics().rebuilt_patches==
        mesh.logical_red_owners().size());

  tetra::DualContourPatchBuilder builder;
  builder.rebuild_index(mesh,sphere);
  const auto dependencies=builder.dependencies();
  REQUIRE_FALSE(dependencies.empty());
  CHECK(std::ranges::is_sorted(dependencies));
  CHECK(std::ranges::adjacent_find(dependencies)==dependencies.end());
  for(const auto dependency:dependencies){
    CHECK(dependency.patch_owner<=dependency.incident_owner);
    CHECK(std::binary_search(mesh.logical_red_owners().begin(),
                             mesh.logical_red_owners().end(),
                             dependency.patch_owner));
  }
}

TEST_CASE("dual edge-star cache invalidates locally through split and inverse merge") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,48.0,4));
  tetra_viewer::SceneCache cache;
  const auto update=[&](std::uint64_t field_revision){
    return cache.update_scene(
        mesh,sphere,field_revision,
        tetra_viewer::SurfaceMethod::dual_contouring,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  };
  REQUIRE(update(8));
  struct Snapshot {
    std::size_t begin{},count{},capacity{};
    std::vector<unsigned char> bytes;
  };
  std::map<tetra::TetId,Snapshot> before;
  const auto capture=[&](auto& destination){
    const auto arena=cache.surface_patch_arena();
    for(const auto& record:cache.surface_patch_records()){
      Snapshot snapshot{record.triangle_begin,record.triangle_count,
                        record.triangle_capacity,{}};
      snapshot.bytes.resize(record.triangle_capacity*sizeof(tetra::Triangle));
      if(!snapshot.bytes.empty())std::memcpy(
          snapshot.bytes.data(),arena.data()+record.triangle_begin,
          snapshot.bytes.size());
      destination.emplace(record.logical_owner,std::move(snapshot));
    }
  };
  capture(before);
  const auto split=std::ranges::find_if(
      mesh.logical_red_owners(),[&](tetra::TetId owner){
        const auto record=before.find(owner);
        return record!=before.end()&&record->second.count!=0U;
      });
  REQUIRE(split!=mesh.logical_red_owners().end());
  const auto split_parent=*split;
  REQUIRE(mesh.refine_selected_binary({split_parent}));
  REQUIRE(update(8));
  const auto split_monolithic=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  CHECK(tetra_viewer::surface_geometry_hashes(cache.scene()).triangle_hash==
        tetra_viewer::surface_geometry_hashes(split_monolithic).triangle_hash);
  CHECK(tetra_viewer::surface_geometry_hashes(cache.scene()).edge_hash==
        tetra_viewer::surface_geometry_hashes(split_monolithic).edge_hash);
  const auto split_metrics=cache.surface_patch_metrics();
  CHECK_FALSE(split_metrics.full_rebuild);
  CHECK(split_metrics.rebuilt_patches>0U);
  CHECK(split_metrics.rebuilt_patches<cache.surface_patch_records().size());
  CHECK(split_metrics.reused_patches>0U);
  CHECK(split_metrics.retired_patches>0U);
  std::size_t stable_records{};
  const auto split_arena=cache.surface_patch_arena();
  for(const auto& record:cache.surface_patch_records()){
    const auto found=before.find(record.logical_owner);
    if(found==before.end()||
       record.triangle_begin!=found->second.begin||
       record.triangle_count!=found->second.count||
       record.triangle_capacity!=found->second.capacity)continue;
    std::vector<unsigned char> bytes(
        record.triangle_capacity*sizeof(tetra::Triangle));
    if(!bytes.empty())std::memcpy(
        bytes.data(),split_arena.data()+record.triangle_begin,bytes.size());
    if(bytes==found->second.bytes)++stable_records;
  }
  CHECK(stable_records>=split_metrics.reused_patches);

  std::vector<tetra::TetId> removed_parents;
  for(const auto& [owner,snapshot]:before){
    static_cast<void>(snapshot);
    if(!std::binary_search(mesh.logical_red_owners().begin(),
                           mesh.logical_red_owners().end(),owner))
      removed_parents.push_back(owner);
  }
  REQUIRE(std::binary_search(removed_parents.begin(),removed_parents.end(),
                             split_parent));
  REQUIRE(mesh.coarsen_selected_red(removed_parents));
  REQUIRE(update(8));
  const auto merged_monolithic=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  CHECK(tetra_viewer::surface_geometry_hashes(cache.scene()).triangle_hash==
        tetra_viewer::surface_geometry_hashes(merged_monolithic).triangle_hash);
  CHECK(tetra_viewer::surface_geometry_hashes(cache.scene()).edge_hash==
        tetra_viewer::surface_geometry_hashes(merged_monolithic).edge_hash);
  CHECK_FALSE(cache.surface_patch_metrics().full_rebuild);
  CHECK(cache.surface_patch_metrics().rebuilt_patches>0U);
  CHECK(cache.surface_patch_metrics().reused_patches>0U);
  CHECK(cache.surface_patch_metrics().retired_patches>0U);

  REQUIRE(update(9));
  CHECK(cache.surface_patch_metrics().full_rebuild);
  CHECK(cache.surface_patch_metrics().rebuilt_patches==
        cache.surface_patch_records().size());
}

TEST_CASE("dual edge-star patches cover bulk owner-set changes") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere sphere;
  tetra::Camera camera;
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,48.0,5));
  tetra_viewer::SceneCache cache;
  REQUIRE(cache.update_scene(
      mesh,sphere,11,tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells));
  const auto owners_before=mesh.logical_red_owners().size();
  std::vector<tetra::TetId> requests;
  for(const auto& record:cache.surface_patch_records()){
    if(record.triangle_count!=0U)requests.push_back(record.logical_owner);
    if(requests.size()==16U)break;
  }
  REQUIRE(requests.size()>=4U);
  REQUIRE(mesh.refine_selected_binary(requests));
  REQUIRE(mesh.logical_red_owners().size()!=owners_before);
  REQUIRE(cache.update_scene(
      mesh,sphere,11,tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells));
  const auto monolithic=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::dual_contouring,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  const auto patched_hashes=tetra_viewer::surface_geometry_hashes(cache.scene());
  const auto monolithic_hashes=tetra_viewer::surface_geometry_hashes(monolithic);
  CHECK(patched_hashes.triangle_hash==monolithic_hashes.triangle_hash);
  CHECK(patched_hashes.edge_hash==monolithic_hashes.edge_hash);
  CHECK(patched_hashes.triangle_count==monolithic_hashes.triangle_count);
  CHECK(patched_hashes.edge_count==monolithic_hashes.edge_count);
  CHECK_FALSE(cache.surface_patch_metrics().full_rebuild);
  CHECK(cache.surface_patch_metrics().generated_triangles>0U);
  CHECK(cache.surface_patch_metrics().reused_patches>0U);
}

TEST_CASE("owner patches retain locality across multiple unpublished revisions") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,48.0,4));
  constexpr std::array methods{
      tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::SurfaceMethod::lattice_cleaving,
      tetra_viewer::SurfaceMethod::dual_contouring};
  std::array<tetra_viewer::SceneCache,methods.size()> caches;
  for(std::size_t index=0;index<methods.size();++index)
    REQUIRE(caches[index].update_scene(
        mesh,sphere,13,methods[index],
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells));

  const auto initial_revision=mesh.revision();
  for(std::size_t transaction=0;transaction<2U;++transaction){
    std::vector<tetra::TetId> candidates;
    for(const auto owner:mesh.logical_red_owners())
      if(tetra::classify_tetrahedron(mesh,owner,sphere)==
         tetra::SurfaceRelation::intersecting)
        candidates.push_back(owner);
    bool committed{};
    for(const auto owner:candidates)
      if(mesh.refine_selected_binary({owner})){
        committed=true;
        break;
      }
    REQUIRE(committed);
  }
  REQUIRE(mesh.revision()>=initial_revision+2U);

  for(std::size_t index=0;index<methods.size();++index){
    CAPTURE(tetra_viewer::surface_method_key(methods[index]));
    REQUIRE(caches[index].update_scene(
        mesh,sphere,13,methods[index],
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells));
    const auto monolithic=tetra_viewer::prepare_scene(
        mesh,sphere,methods[index],
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
    const auto patched=tetra_viewer::surface_geometry_hashes(caches[index].scene());
    const auto direct=tetra_viewer::surface_geometry_hashes(monolithic);
    CHECK(patched==direct);
    CHECK(patched.wire_edge_hash==patched.edge_hash);
    CHECK(patched.wire_edge_count==patched.edge_count);
    CHECK_FALSE(caches[index].surface_patch_metrics().full_rebuild);
    CHECK(caches[index].surface_patch_metrics().rebuilt_patches>0U);
    CHECK(caches[index].surface_patch_metrics().reused_patches>0U);
  }
}

TEST_CASE("fixed surface draw chunks match direct packing for every local method") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,48.0,5));
  constexpr std::array methods{
      tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::SurfaceMethod::lattice_cleaving,
      tetra_viewer::SurfaceMethod::dual_contouring};
  const auto byte_equal=[](std::span<const tetra::Triangle> first,
                           std::span<const tetra::Triangle> second){
    return first.size()==second.size()&&
        (first.empty()||std::memcmp(first.data(),second.data(),
                                    first.size_bytes())==0);
  };
  for(const auto method:methods){
    CAPTURE(tetra_viewer::surface_method_key(method));
    tetra_viewer::SceneCache cache;
    REQUIRE(cache.update_scene(
        mesh,sphere,21,method,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells));
    const auto direct=tetra_viewer::direct_pack_surface_patches(
        cache.surface_patch_records(),cache.surface_patch_arena());
    tetra_viewer::SurfaceDrawChunkStorage chunks(3U);
    chunks.pack(cache.surface_patch_records(),cache.surface_patch_arena());
    const auto assembled=tetra_viewer::assemble_surface_draw_chunks(chunks);
    REQUIRE(byte_equal(assembled,direct));

    const auto packed_scene=tetra_viewer::prepare_scene(
        mesh,sphere,method,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::hierarchy_cells,
        tetra_viewer::StencilConstruction::fixed,
        tetra_viewer::StencilSelectionObjective::balanced,{},
        assembled,true);
    CHECK(tetra_viewer::surface_geometry_hashes(packed_scene)==
          tetra_viewer::surface_geometry_hashes(cache.scene()));

    const auto& metrics=chunks.metrics();
    CHECK(metrics.chunk_capacity==3U);
    CHECK(metrics.source_patches==cache.surface_patch_records().size());
    CHECK(metrics.triangles==direct.size());
    CHECK(metrics.active_chunks==(direct.size()+2U)/3U);
    CHECK(metrics.draw_calls==metrics.active_chunks);
    CHECK(metrics.fragmented_slots==metrics.active_chunks*3U-direct.size());
    CHECK(metrics.fragmentation_bytes==
          metrics.fragmented_slots*sizeof(tetra::Triangle));
    CHECK(metrics.copied_bytes==direct.size()*sizeof(tetra::Triangle));
    CHECK(metrics.global_compactions==1U);
    CHECK(metrics.occupancy>0.0);
    CHECK(metrics.occupancy<=1.0);
    CHECK(metrics.retained_bytes>0U);

    std::vector<std::size_t> slots;
    for(std::size_t chunk_index=0;chunk_index<chunks.chunks().size();++chunk_index){
      const auto& chunk=chunks.chunks()[chunk_index];
      CHECK(chunk.triangle_count>0U);
      CHECK(chunk.triangle_count<=chunks.chunk_capacity());
      CHECK(chunk.segment_begin+chunk.segment_count<=chunks.segments().size());
      slots.push_back(chunk.arena_slot);
      for(std::size_t segment_index=chunk.segment_begin;
          segment_index<chunk.segment_begin+chunk.segment_count;++segment_index){
        const auto& segment=chunks.segments()[segment_index];
        CHECK(segment.chunk_index==chunk_index);
        CHECK(segment.triangle_begin>=chunk.arena_slot*chunks.chunk_capacity());
        CHECK(segment.triangle_begin+segment.triangle_count<=
              chunk.arena_slot*chunks.chunk_capacity()+chunk.triangle_count);
      }
    }
    std::sort(slots.begin(),slots.end());
    CHECK(std::ranges::adjacent_find(slots)==slots.end());

    const auto retained_slots=metrics.retained_slots;
    chunks.pack({},cache.surface_patch_arena());
    CHECK(chunks.metrics().active_chunks==0U);
    CHECK(chunks.metrics().free_slots==retained_slots);
    REQUIRE(chunks.free_ranges().size()==1U);
    chunks.pack(cache.surface_patch_records(),cache.surface_patch_arena());
    CHECK(chunks.metrics().allocated_slots==0U);
    CHECK(chunks.metrics().reused_slots==chunks.metrics().active_chunks);
    CHECK(byte_equal(tetra_viewer::assemble_surface_draw_chunks(chunks),direct));
  }
}

TEST_CASE("surface draw chunks diagnose invalid ranges and report split merge packing") {
  CHECK_THROWS_AS(tetra_viewer::SurfaceDrawChunkStorage(0U),std::invalid_argument);
  CHECK_THROWS_AS(tetra_viewer::SurfaceDrawChunkStorage(
                      6U,tetra_viewer::SurfaceDrawChunkStrategy::
                          hybrid_large_patches,0U),
                  std::invalid_argument);
  CHECK_THROWS_AS(tetra_viewer::SurfaceDrawChunkStorage(
                      6U,tetra_viewer::SurfaceDrawChunkStrategy::
                          hybrid_large_patches,7U),
                  std::invalid_argument);
  std::array<tetra::Triangle,7> arena{};
  std::array patches{
      tetra_viewer::SurfacePatchRecord{
          .logical_owner=1U,.triangle_begin=0U,.triangle_count=5U,
          .triangle_capacity=5U},
      tetra_viewer::SurfacePatchRecord{
          .logical_owner=2U,.triangle_begin=5U,.triangle_count=2U,
          .triangle_capacity=2U}};
  tetra_viewer::SurfaceDrawChunkStorage chunks(3U);
  chunks.pack(patches,arena);
  CHECK(chunks.metrics().active_chunks==3U);
  CHECK(chunks.metrics().patch_segments==4U);
  CHECK(chunks.metrics().chunk_splits==2U);
  CHECK(chunks.metrics().chunk_merges==1U);
  CHECK(chunks.metrics().fragmented_slots==2U);
  CHECK(tetra_viewer::assemble_surface_draw_chunks(chunks).size()==arena.size());

  std::array unsorted{patches[1],patches[0]};
  CHECK_THROWS_AS(chunks.pack(unsorted,arena),std::invalid_argument);
  auto invalid=patches;
  invalid[1].triangle_begin=arena.size();
  invalid[1].triangle_count=1U;
  CHECK_THROWS_AS(chunks.pack(invalid,arena),std::out_of_range);
  CHECK_THROWS_AS(
      static_cast<void>(tetra_viewer::direct_pack_surface_patches(invalid,arena)),
      std::out_of_range);
}

TEST_CASE("hybrid draw chunks isolate large patches in packed retained slots") {
  CHECK(tetra_viewer::default_surface_draw_chunk_strategy==
        tetra_viewer::SurfaceDrawChunkStrategy::fixed_capacity);
  CHECK(tetra_viewer::SurfaceDrawChunkStorage{}.strategy()==
        tetra_viewer::SurfaceDrawChunkStrategy::fixed_capacity);
  std::array<tetra::Triangle,14> arena{};
  for(std::size_t index=0;index<arena.size();++index)
    arena[index].a.x=static_cast<double>(index+1U);
  std::array patches{
      tetra_viewer::SurfacePatchRecord{
          .logical_owner=1U,.field_revision=1U,.topology_hash=1U,
          .triangle_begin=0U,.triangle_count=2U,.triangle_capacity=2U},
      tetra_viewer::SurfacePatchRecord{
          .logical_owner=2U,.field_revision=1U,.topology_hash=2U,
          .triangle_begin=2U,.triangle_count=5U,.triangle_capacity=5U},
      tetra_viewer::SurfacePatchRecord{
          .logical_owner=3U,.field_revision=1U,.topology_hash=3U,
          .triangle_begin=7U,.triangle_count=2U,.triangle_capacity=2U},
      tetra_viewer::SurfacePatchRecord{
          .logical_owner=4U,.field_revision=1U,.topology_hash=4U,
          .triangle_begin=9U,.triangle_count=5U,.triangle_capacity=5U}};
  tetra_viewer::SurfaceDrawChunkStorage hybrid(
      6U,tetra_viewer::SurfaceDrawChunkStrategy::hybrid_large_patches,4U);
  hybrid.pack(patches,arena);
  const auto direct=tetra_viewer::direct_pack_surface_patches(patches,arena);
  const auto assembled=tetra_viewer::assemble_surface_draw_chunks(hybrid);
  REQUIRE(assembled.size()==direct.size());
  CHECK(std::memcmp(assembled.data(),direct.data(),
                    direct.size()*sizeof(tetra::Triangle))==0);
  CHECK(hybrid.metrics().strategy==
        tetra_viewer::SurfaceDrawChunkStrategy::hybrid_large_patches);
  CHECK(hybrid.metrics().large_patch_threshold==4U);
  CHECK(hybrid.metrics().large_patches==2U);
  CHECK(hybrid.metrics().large_patch_triangles==10U);
  CHECK(hybrid.metrics().active_chunks==4U);
  CHECK(hybrid.metrics().fragmented_slots==10U);
  for(const auto owner:{tetra::TetId{2U},tetra::TetId{4U}}){
    const auto segment=std::ranges::find_if(
        hybrid.segments(),[&](const auto& value){
          return value.logical_owner==owner;
        });
    REQUIRE(segment!=hybrid.segments().end());
    const auto& chunk=hybrid.chunks()[segment->chunk_index];
    CHECK(chunk.segment_count==1U);
    CHECK(segment->triangle_begin==
          chunk.arena_slot*hybrid.chunk_capacity());
  }

  arena[3].b.y=42.0;
  patches[1].field_revision=2U;
  hybrid.pack(patches,arena);
  CHECK(hybrid.metrics().dirty_patches==1U);
  CHECK(hybrid.metrics().dirty_chunks==1U);
  CHECK(hybrid.metrics().reused_chunks==3U);
  CHECK(hybrid.metrics().copied_bytes==5U*sizeof(tetra::Triangle));
  const auto changed=tetra_viewer::assemble_surface_draw_chunks(hybrid);
  const auto changed_direct=
      tetra_viewer::direct_pack_surface_patches(patches,arena);
  REQUIRE(changed.size()==changed_direct.size());
  CHECK(std::memcmp(changed.data(),changed_direct.data(),
                    changed.size()*sizeof(tetra::Triangle))==0);
}

TEST_CASE("surface draw chunks repack only bounded dirty owner neighbourhoods") {
  using Patch=tetra_viewer::SurfacePatchRecord;
  const auto make_state=[](const std::vector<tetra::TetId>& owners,
                           const std::vector<std::size_t>& counts,
                           std::uint64_t revision){
    std::pair<std::vector<Patch>,std::vector<tetra::Triangle>> state;
    REQUIRE(owners.size()==counts.size());
    for(std::size_t patch_index=0;patch_index<owners.size();++patch_index){
      const auto begin=state.second.size();
      for(std::size_t triangle_index=0;
          triangle_index<counts[patch_index];++triangle_index){
        const auto marker=static_cast<double>(
            owners[patch_index]*1000U+revision*100U+triangle_index);
        state.second.push_back({{marker,1.0,2.0},{3.0,marker,4.0},
                                {5.0,6.0,marker}});
      }
      state.first.push_back({
          .logical_owner=owners[patch_index],
          .mesh_revision=revision,
          .field_revision=revision,
          .topology_hash=owners[patch_index]*37U+revision,
          .triangle_begin=begin,
          .triangle_count=counts[patch_index],
          .triangle_capacity=counts[patch_index]});
    }
    return state;
  };
  const auto byte_equal=[](std::span<const tetra::Triangle> first,
                           std::span<const tetra::Triangle> second){
    return first.size()==second.size()&&
        (first.empty()||std::memcmp(first.data(),second.data(),
                                    first.size_bytes())==0);
  };
  const std::vector<tetra::TetId> owners{10,20,30,40,50,60,70,80,90,100};
  std::vector<std::size_t> counts(owners.size(),2U);
  auto state=make_state(owners,counts,1U);
  tetra_viewer::SurfaceDrawChunkStorage chunks(4U);
  chunks.pack(state.first,state.second);
  REQUIRE(byte_equal(tetra_viewer::assemble_surface_draw_chunks(chunks),
                     tetra_viewer::direct_pack_surface_patches(
                         state.first,state.second)));

  const std::vector initial_chunks(chunks.chunks().begin(),chunks.chunks().end());
  const std::vector initial_arena(chunks.arena().begin(),chunks.arena().end());
  auto same_layout=make_state(owners,counts,2U);
  for(std::size_t index=0;index<same_layout.first.size();++index){
    if(owners[index]==50U)continue;
    same_layout.first[index].field_revision=state.first[index].field_revision;
    same_layout.first[index].topology_hash=state.first[index].topology_hash;
    std::copy_n(state.second.begin()+static_cast<std::ptrdiff_t>(
                    state.first[index].triangle_begin),
                state.first[index].triangle_count,
                same_layout.second.begin()+static_cast<std::ptrdiff_t>(
                    same_layout.first[index].triangle_begin));
  }
  chunks.pack(same_layout.first,same_layout.second);
  CHECK(chunks.metrics().dirty_patches==1U);
  CHECK(chunks.metrics().dirty_chunks==1U);
  CHECK(chunks.metrics().copied_bytes==2U*sizeof(tetra::Triangle));
  CHECK(chunks.metrics().copied_bytes<
        chunks.metrics().triangles*sizeof(tetra::Triangle));
  CHECK(chunks.metrics().global_compactions==0U);
  REQUIRE(chunks.chunks().size()==initial_chunks.size());
  for(std::size_t index=0;index<chunks.chunks().size();++index){
    const auto& chunk=chunks.chunks()[index];
    if(index==2U)continue;
    CHECK(chunk.arena_slot==initial_chunks[index].arena_slot);
    CHECK(chunk.triangle_count==initial_chunks[index].triangle_count);
    const auto begin=chunk.arena_slot*chunks.chunk_capacity();
    CHECK(std::memcmp(chunks.arena().data()+begin,
                      initial_arena.data()+begin,
                      chunk.triangle_count*sizeof(tetra::Triangle))==0);
  }
  REQUIRE(byte_equal(tetra_viewer::assemble_surface_draw_chunks(chunks),
                     tetra_viewer::direct_pack_surface_patches(
                         same_layout.first,same_layout.second)));

  counts[4]=20U;
  auto grown=make_state(owners,counts,3U);
  for(std::size_t index=0;index<grown.first.size();++index){
    if(index==4U)continue;
    grown.first[index].field_revision=same_layout.first[index].field_revision;
    grown.first[index].topology_hash=same_layout.first[index].topology_hash;
    std::copy_n(same_layout.second.begin()+static_cast<std::ptrdiff_t>(
                    same_layout.first[index].triangle_begin),
                same_layout.first[index].triangle_count,
                grown.second.begin()+static_cast<std::ptrdiff_t>(
                    grown.first[index].triangle_begin));
  }
  chunks.pack(grown.first,grown.second);
  CHECK(chunks.metrics().local_repacks==1U);
  CHECK(chunks.metrics().global_compactions==0U);
  CHECK(chunks.metrics().overflow_splits>0U);
  CHECK(chunks.metrics().reused_chunks>0U);
  CHECK(chunks.metrics().copied_bytes<
        chunks.metrics().triangles*sizeof(tetra::Triangle));
  REQUIRE(byte_equal(tetra_viewer::assemble_surface_draw_chunks(chunks),
                     tetra_viewer::direct_pack_surface_patches(
                         grown.first,grown.second)));

  counts[4]=2U;
  auto shrunk=make_state(owners,counts,4U);
  for(std::size_t index=0;index<shrunk.first.size();++index){
    if(index==4U)continue;
    shrunk.first[index].field_revision=grown.first[index].field_revision;
    shrunk.first[index].topology_hash=grown.first[index].topology_hash;
    std::copy_n(grown.second.begin()+static_cast<std::ptrdiff_t>(
                    grown.first[index].triangle_begin),
                grown.first[index].triangle_count,
                shrunk.second.begin()+static_cast<std::ptrdiff_t>(
                    shrunk.first[index].triangle_begin));
  }
  chunks.pack(shrunk.first,shrunk.second);
  CHECK(chunks.metrics().underfull_merges>0U);
  CHECK(chunks.metrics().released_slots>0U);
  REQUIRE(byte_equal(tetra_viewer::assemble_surface_draw_chunks(chunks),
                     tetra_viewer::direct_pack_surface_patches(
                         shrunk.first,shrunk.second)));

  auto inserted_owners=owners;
  inserted_owners.insert(inserted_owners.begin()+5,55U);
  std::vector<std::size_t> inserted_counts(inserted_owners.size(),2U);
  auto inserted=make_state(inserted_owners,inserted_counts,5U);
  for(std::size_t index=0;index<inserted.first.size();++index){
    const auto found=std::ranges::find(owners,inserted_owners[index]);
    if(found==owners.end())continue;
    const auto old_index=static_cast<std::size_t>(std::distance(owners.begin(),found));
    inserted.first[index].field_revision=shrunk.first[old_index].field_revision;
    inserted.first[index].topology_hash=shrunk.first[old_index].topology_hash;
    std::copy_n(shrunk.second.begin()+static_cast<std::ptrdiff_t>(
                    shrunk.first[old_index].triangle_begin),
                shrunk.first[old_index].triangle_count,
                inserted.second.begin()+static_cast<std::ptrdiff_t>(
                    inserted.first[index].triangle_begin));
  }
  chunks.pack(inserted.first,inserted.second);
  CHECK(chunks.metrics().local_repacks==1U);
  REQUIRE(byte_equal(tetra_viewer::assemble_surface_draw_chunks(chunks),
                     tetra_viewer::direct_pack_surface_patches(
                         inserted.first,inserted.second)));
  chunks.pack(shrunk.first,shrunk.second);
  CHECK(chunks.metrics().local_repacks==1U);
  REQUIRE(byte_equal(tetra_viewer::assemble_surface_draw_chunks(chunks),
                     tetra_viewer::direct_pack_surface_patches(
                         shrunk.first,shrunk.second)));

  counts[4]=20U;
  auto regrown=make_state(owners,counts,6U);
  for(std::size_t index=0;index<regrown.first.size();++index){
    if(index==4U)continue;
    regrown.first[index].field_revision=shrunk.first[index].field_revision;
    regrown.first[index].topology_hash=shrunk.first[index].topology_hash;
    std::copy_n(shrunk.second.begin()+static_cast<std::ptrdiff_t>(
                    shrunk.first[index].triangle_begin),
                shrunk.first[index].triangle_count,
                regrown.second.begin()+static_cast<std::ptrdiff_t>(
                    regrown.first[index].triangle_begin));
  }
  chunks.pack(regrown.first,regrown.second);
  CHECK(chunks.metrics().reused_slots>0U);
  REQUIRE(byte_equal(tetra_viewer::assemble_surface_draw_chunks(chunks),
                     tetra_viewer::direct_pack_surface_patches(
                         regrown.first,regrown.second)));

  const auto valid_before=tetra_viewer::assemble_surface_draw_chunks(chunks);
  auto invalid=regrown.first;
  invalid[4].triangle_begin=regrown.second.size();
  invalid[4].triangle_count=1U;
  CHECK_THROWS_AS(chunks.pack(invalid,regrown.second),std::out_of_range);
  CHECK(byte_equal(tetra_viewer::assemble_surface_draw_chunks(chunks),valid_before));

  std::vector<tetra::TetId> replaced_owners{11,21,31,41,51,61,71,81,91,101};
  std::vector<std::size_t> replaced_counts(replaced_owners.size(),2U);
  auto replaced=make_state(replaced_owners,replaced_counts,7U);
  chunks.pack(replaced.first,replaced.second);
  CHECK(chunks.metrics().global_compactions==1U);
  CHECK(chunks.metrics().local_repacks==0U);
  REQUIRE(byte_equal(tetra_viewer::assemble_surface_draw_chunks(chunks),
                     tetra_viewer::direct_pack_surface_patches(
                         replaced.first,replaced.second)));
}

TEST_CASE("retained host staging atomically publishes solid and wire ranges") {
  const auto make_vertices=[](const tetra_viewer::SurfaceDrawChunkStorage& chunks){
    std::vector<tetra_viewer::SceneVertex> vertices;
    const auto triangles=tetra_viewer::assemble_surface_draw_chunks(chunks);
    vertices.reserve(triangles.size()*3U);
    for(const auto& triangle:triangles){
      for(const auto point:{triangle.a,triangle.b,triangle.c}){
        tetra_viewer::SceneVertex vertex{};
        vertex.position[0]=static_cast<float>(point.x);
        vertex.position[1]=static_cast<float>(point.y);
        vertex.position[2]=static_cast<float>(point.z);
        vertex.colour[0]=0.2F;vertex.colour[1]=0.7F;vertex.colour[2]=0.4F;
        vertex.edge_flags=7.0F;
        vertices.push_back(vertex);
      }
    }
    return vertices;
  };
  const auto byte_equal=[](std::span<const tetra_viewer::SceneVertex> first,
                           std::span<const tetra_viewer::SceneVertex> second){
    return first.size()==second.size()&&
        (first.empty()||std::memcmp(first.data(),second.data(),
                                    first.size_bytes())==0);
  };
  std::vector<tetra::Triangle> patch_arena;
  for(std::size_t index=0;index<10U;++index){
    const auto marker=static_cast<double>(index+1U);
    patch_arena.push_back({{marker,0.0,0.0},{0.0,marker,0.0},
                           {0.0,0.0,marker}});
  }
  std::array patches{
      tetra_viewer::SurfacePatchRecord{
          .logical_owner=10U,.field_revision=1U,.topology_hash=10U,
          .triangle_begin=0U,.triangle_count=2U,.triangle_capacity=2U},
      tetra_viewer::SurfacePatchRecord{
          .logical_owner=20U,.field_revision=1U,.topology_hash=20U,
          .triangle_begin=2U,.triangle_count=2U,.triangle_capacity=2U},
      tetra_viewer::SurfacePatchRecord{
          .logical_owner=30U,.field_revision=1U,.topology_hash=30U,
          .triangle_begin=4U,.triangle_count=2U,.triangle_capacity=2U},
      tetra_viewer::SurfacePatchRecord{
          .logical_owner=40U,.field_revision=1U,.topology_hash=40U,
          .triangle_begin=6U,.triangle_count=4U,.triangle_capacity=4U}};
  tetra_viewer::SurfaceDrawChunkStorage chunks(3U);
  chunks.pack(patches,patch_arena);
  const auto initial_vertices=make_vertices(chunks);
  tetra_viewer::SurfaceHostStagingStorage staging(3U);
  staging.stage(chunks,initial_vertices);
  REQUIRE(byte_equal(tetra_viewer::assemble_surface_host_staging(staging),
                     initial_vertices));
  tetra::GeometryExecutor staging_executor({.worker_count=4U});
  tetra_viewer::SurfaceHostStagingStorage parallel_staging(3U);
  parallel_staging.stage(chunks,initial_vertices,&staging_executor);
  REQUIRE(byte_equal(
      tetra_viewer::assemble_surface_host_staging(parallel_staging),
      initial_vertices));
  CHECK(parallel_staging.metrics().parallel_copy_tasks>1U);
  CHECK(parallel_staging.metrics().staged_triangle_bytes==
        staging.metrics().staged_triangle_bytes);
  CHECK(staging.metrics().dirty_ranges==chunks.chunks().size());
  CHECK(staging.metrics().reused_ranges==0U);
  CHECK(staging.metrics().staged_triangle_bytes==
        initial_vertices.size()*sizeof(tetra_viewer::SceneVertex));
  CHECK(staging.metrics().staged_wire_bytes==0U);
  CHECK(staging.metrics().aliased_wire_bytes==
        initial_vertices.size()*sizeof(tetra_viewer::SceneVertex));
  for(const auto& range:staging.ranges()){
    CHECK(range.triangle_vertex_begin==range.wire_vertex_begin);
    CHECK(range.triangle_vertex_count==range.wire_vertex_count);
  }

  const std::vector initial_ranges(staging.ranges().begin(),staging.ranges().end());
  const std::vector initial_host_arena(staging.arena().begin(),staging.arena().end());
  staging.stage(chunks,initial_vertices);
  CHECK(staging.metrics().dirty_ranges==0U);
  CHECK(staging.metrics().reused_ranges==chunks.chunks().size());
  CHECK(staging.metrics().staged_triangle_bytes==0U);
  CHECK(byte_equal(tetra_viewer::assemble_surface_host_staging(staging),
                   initial_vertices));

  for(std::size_t index=2U;index<4U;++index){
    const auto marker=static_cast<double>(100U+index);
    patch_arena[index]={{marker,0.0,0.0},{0.0,marker,0.0},
                        {0.0,0.0,marker}};
  }
  patches[1].field_revision=2U;
  patches[1].topology_hash=21U;
  chunks.pack(patches,patch_arena);
  const auto changed_vertices=make_vertices(chunks);
  staging.stage(chunks,changed_vertices);
  REQUIRE(byte_equal(tetra_viewer::assemble_surface_host_staging(staging),
                     changed_vertices));
  CHECK(staging.metrics().dirty_ranges==chunks.metrics().dirty_chunks);
  CHECK(staging.metrics().reused_ranges+staging.metrics().dirty_ranges==
        chunks.chunks().size());
  CHECK(staging.metrics().dirty_ranges>0U);
  CHECK(staging.metrics().reused_ranges>0U);
  CHECK(staging.metrics().staged_triangle_bytes<
        changed_vertices.size()*sizeof(tetra_viewer::SceneVertex));
  for(const auto& old_range:initial_ranges){
    const auto begin=old_range.host_slot*staging.vertex_slot_capacity();
    CHECK(std::memcmp(staging.arena().data()+begin,
                      initial_host_arena.data()+begin,
                      old_range.triangle_vertex_count*
                          sizeof(tetra_viewer::SceneVertex))==0);
  }

  const auto valid_generation=staging.metrics().publication_generation;
  const std::vector valid_ranges(staging.ranges().begin(),staging.ranges().end());
  const auto valid_assembled=tetra_viewer::assemble_surface_host_staging(staging);
  CHECK_THROWS_AS(
      staging.stage(chunks,std::span<const tetra_viewer::SceneVertex>(
          changed_vertices.data(),changed_vertices.size()-1U)),
      std::invalid_argument);
  CHECK(staging.metrics().publication_generation==valid_generation);
  CHECK(staging.ranges().size()==valid_ranges.size());
  CHECK(byte_equal(tetra_viewer::assemble_surface_host_staging(staging),
                   valid_assembled));

  tetra_viewer::SurfaceDeviceUploadPlanner device_planner;
  std::vector<tetra_viewer::SceneVertex> device_arena;
  device_planner.prepare(staging,device_arena.size());
  CHECK(device_planner.metrics().full_reallocation);
  CHECK(device_planner.metrics().upload_ranges==staging.ranges().size());
  CHECK(device_planner.metrics().uploaded_bytes==
        valid_assembled.size()*sizeof(tetra_viewer::SceneVertex));
  tetra_viewer::apply_surface_device_upload_plan(
      device_planner,staging,device_arena);
  CHECK(device_planner.published_generation()==
        staging.metrics().publication_generation);
  CHECK(device_planner.published_draws().size()==staging.ranges().size());
  for(const auto& draw:device_planner.published_draws())
    for(std::size_t vertex=0;vertex<draw.vertex_count;++vertex){
      const auto& position=device_arena[draw.first_vertex+vertex].position;
      CHECK(static_cast<double>(position[0])>=draw.minimum.x);
      CHECK(static_cast<double>(position[0])<=draw.maximum.x);
      CHECK(static_cast<double>(position[1])>=draw.minimum.y);
      CHECK(static_cast<double>(position[1])<=draw.maximum.y);
      CHECK(static_cast<double>(position[2])>=draw.minimum.z);
      CHECK(static_cast<double>(position[2])<=draw.maximum.z);
    }
  CHECK(byte_equal(tetra_viewer::assemble_surface_device_publication(
                       device_planner,device_arena),valid_assembled));

  patch_arena[0].a.x+=50.0;
  patches[0].field_revision=3U;
  patches[0].topology_hash=11U;
  chunks.pack(patches,patch_arena);
  const auto partial_vertices=make_vertices(chunks);
  staging.stage(chunks,partial_vertices);
  const auto previous_device=tetra_viewer::assemble_surface_device_publication(
      device_planner,device_arena);
  device_planner.prepare(staging,device_arena.size());
  CHECK_FALSE(device_planner.metrics().full_reallocation);
  CHECK(device_planner.metrics().upload_ranges==staging.metrics().dirty_ranges);
  CHECK(device_planner.metrics().uploaded_bytes==
        staging.metrics().staged_triangle_bytes);
  CHECK(device_planner.metrics().uploaded_bytes<
        partial_vertices.size()*sizeof(tetra_viewer::SceneVertex));
  CHECK(byte_equal(tetra_viewer::assemble_surface_device_publication(
                       device_planner,device_arena),previous_device));
  tetra_viewer::apply_surface_device_upload_plan(
      device_planner,staging,device_arena);
  CHECK(byte_equal(tetra_viewer::assemble_surface_device_publication(
                       device_planner,device_arena),partial_vertices));

  auto styled_vertices=partial_vertices;
  styled_vertices.front().colour[0]=0.95F;
  staging.stage(chunks,styled_vertices);
  CHECK(staging.metrics().dirty_ranges==1U);
  device_planner.prepare(staging,device_arena.size());
  CHECK(device_planner.metrics().upload_ranges==1U);
  tetra_viewer::apply_surface_device_upload_plan(
      device_planner,staging,device_arena);
  CHECK(byte_equal(tetra_viewer::assemble_surface_device_publication(
                       device_planner,device_arena),styled_vertices));

  auto superseded_vertices=styled_vertices;
  superseded_vertices[1].colour[1]=0.15F;
  staging.stage(chunks,superseded_vertices);
  device_planner.prepare(staging,device_arena.size());
  auto latest_vertices=superseded_vertices;
  latest_vertices[2].colour[2]=0.85F;
  staging.stage(chunks,latest_vertices);
  CHECK_THROWS_AS(tetra_viewer::apply_surface_device_upload_plan(
                      device_planner,staging,device_arena),
                  std::invalid_argument);
  CHECK(byte_equal(tetra_viewer::assemble_surface_device_publication(
                       device_planner,device_arena),styled_vertices));
  device_planner.cancel();
  device_planner.prepare(staging,device_arena.size());
  tetra_viewer::apply_surface_device_upload_plan(
      device_planner,staging,device_arena);
  CHECK(byte_equal(tetra_viewer::assemble_surface_device_publication(
                       device_planner,device_arena),latest_vertices));

  staging.stage(chunks,latest_vertices);
  device_planner.prepare(staging,device_arena.size());
  CHECK(device_planner.metrics().upload_ranges==0U);
  CHECK(device_planner.metrics().reused_ranges==staging.ranges().size());
  tetra_viewer::apply_surface_device_upload_plan(
      device_planner,staging,device_arena);
  CHECK(byte_equal(tetra_viewer::assemble_surface_device_publication(
                       device_planner,device_arena),latest_vertices));

  chunks.pack({},patch_arena);
  staging.stage(chunks,{});
  CHECK(staging.ranges().empty());
  CHECK(staging.metrics().released_slots==valid_ranges.size());
  chunks.pack(patches,patch_arena);
  const auto restored_vertices=make_vertices(chunks);
  staging.stage(chunks,restored_vertices);
  CHECK(staging.metrics().reused_slots>0U);
  CHECK(byte_equal(tetra_viewer::assemble_surface_host_staging(staging),
                   restored_vertices));
  device_planner.prepare(staging,device_arena.size());
  tetra_viewer::apply_surface_device_upload_plan(
      device_planner,staging,device_arena);
  CHECK(byte_equal(tetra_viewer::assemble_surface_device_publication(
                       device_planner,device_arena),restored_vertices));

  tetra_viewer::SurfaceHostStagingStorage wrong_capacity(4U);
  CHECK_THROWS_AS(wrong_capacity.stage(chunks,restored_vertices),
                  std::invalid_argument);
  CHECK_THROWS_AS(tetra_viewer::SurfaceHostStagingStorage(0U),
                  std::invalid_argument);
}

TEST_CASE("world render blocks retain local ranges and publish partial uploads") {
  using Block=tetra_viewer::SparseWorldSurfaceCache::RenderBlock;
  const auto make_block=[](std::uint8_t root,std::size_t triangles,
                           std::uint64_t revision,float marker){
    Block block;
    block.id=tetra::hierarchy_block_id(
        tetra::WorldTetAddress::root(root),2U);
    block.surface_payload_hash=revision;
    block.show_faces=true;block.show_edges=true;
    block.triangle_vertices.resize(triangles*3U);
    for(std::size_t index=0;index<block.triangle_vertices.size();++index){
      auto& vertex=block.triangle_vertices[index];
      vertex.position[0]=marker+static_cast<float>(index)*0.01F;
      vertex.position[1]=static_cast<float>(root);
      vertex.position[2]=static_cast<float>(index%3U);
      vertex.normal[1]=1.0F;vertex.colour[1]=0.75F;vertex.edge_flags=7.0F;
    }
    return block;
  };
  const auto direct=[](std::span<const Block> blocks){
    std::vector<tetra_viewer::SceneVertex> result;
    for(const auto& block:blocks)result.insert(result.end(),
        block.triangle_vertices.begin(),block.triangle_vertices.end());
    return result;
  };
  const auto equal=[](std::span<const tetra_viewer::SceneVertex> left,
                      std::span<const tetra_viewer::SceneVertex> right){
    return left.size()==right.size()&&
        (left.empty()||std::memcmp(
            left.data(),right.data(),left.size_bytes())==0);
  };

  std::vector<Block> blocks;
  blocks.push_back(make_block(0U,2U,11U,1.0F));
  blocks.push_back(make_block(1U,20U,12U,2.0F));
  blocks.push_back(make_block(2U,5U,13U,3.0F));
  tetra_viewer::SurfaceHostStagingStorage staging(4U);
  const auto initial_estimate=staging.estimate_world_render_blocks(blocks);
  CHECK(initial_estimate.dirty_ranges==8U);
  CHECK(initial_estimate.reused_ranges==0U);
  staging.stage_world_render_blocks(blocks);
  REQUIRE(equal(tetra_viewer::assemble_surface_host_staging(staging),
                direct(blocks)));
  CHECK(staging.metrics().dirty_ranges==8U);
  const std::vector first_ranges(staging.ranges().begin(),staging.ranges().end());

  staging.stage_world_render_blocks(blocks);
  CHECK(staging.metrics().dirty_ranges==0U);
  CHECK(staging.metrics().reused_ranges==first_ranges.size());
  for(std::size_t index=0;index<first_ranges.size();++index)
    CHECK(staging.ranges()[index].host_slot==first_ranges[index].host_slot);
  const auto retained_estimate=staging.estimate_world_render_blocks(blocks);
  CHECK(retained_estimate.dirty_ranges==0U);
  CHECK(retained_estimate.reused_ranges==first_ranges.size());

  tetra_viewer::SurfaceDeviceUploadPlanner planner;
  std::vector<tetra_viewer::SceneVertex> device;
  planner.prepare(staging,device.size());
  tetra_viewer::apply_surface_device_upload_plan(planner,staging,device);
  REQUIRE(equal(tetra_viewer::assemble_surface_device_publication(
                    planner,device),direct(blocks)));

  // One changed source block dirties only its own parts. Keep the revision to
  // exercise the collision guard: identity/hash equality is insufficient
  // unless the retained bytes also match.
  blocks[2].triangle_vertices[0].position[0]+=10.0F;
  const auto changed_estimate=staging.estimate_world_render_blocks(blocks);
  auto upload_budget=tetra_viewer::production_world_profile().budgets;
  upload_budget.maximum_upload_bytes=changed_estimate.staged_bytes;
  CHECK(tetra_viewer::evaluate_world_resource_budgets(upload_budget,
      {0U,0U,0U,changed_estimate.staged_bytes}).admitted());
  staging.stage_world_render_blocks(blocks);
  CHECK(changed_estimate.dirty_ranges==staging.metrics().dirty_ranges);
  CHECK(changed_estimate.staged_bytes==staging.metrics().staged_triangle_bytes);
  CHECK(staging.metrics().dirty_ranges==1U);
  CHECK(staging.metrics().reused_ranges==first_ranges.size()-1U);
  planner.prepare(staging,device.size());
  CHECK_FALSE(planner.metrics().full_reallocation);
  CHECK(planner.metrics().upload_ranges==1U);
  CHECK(planner.metrics().uploaded_bytes==
        staging.metrics().staged_triangle_bytes);
  CHECK(planner.metrics().uploaded_bytes<direct(blocks).size()*
        sizeof(tetra_viewer::SceneVertex));
  tetra_viewer::apply_surface_device_upload_plan(planner,staging,device);
  CHECK(equal(tetra_viewer::assemble_surface_device_publication(
                  planner,device),direct(blocks)));

  auto all_changed=blocks;
  for(auto& block:all_changed){
    ++block.surface_payload_hash;
    for(auto& vertex:block.triangle_vertices)vertex.position[1]+=20.0F;
  }
  const auto large_dirty=staging.estimate_world_render_blocks(all_changed);
  REQUIRE(large_dirty.staged_bytes>changed_estimate.staged_bytes);
  CHECK_FALSE(tetra_viewer::evaluate_world_resource_budgets(upload_budget,
      {0U,0U,0U,large_dirty.staged_bytes}).admitted());

  const auto valid_generation=staging.metrics().publication_generation;
  const auto valid= tetra_viewer::assemble_surface_host_staging(staging);
  auto invalid=blocks;invalid.front().surface_payload_hash=0U;
  CHECK_THROWS_AS(staging.stage_world_render_blocks(invalid),
                  std::invalid_argument);
  CHECK(staging.metrics().publication_generation==valid_generation);
  CHECK(equal(tetra_viewer::assemble_surface_host_staging(staging),valid));

  blocks.erase(blocks.begin()+1);
  staging.stage_world_render_blocks(blocks);
  CHECK(staging.metrics().released_slots==5U);
  CHECK(equal(tetra_viewer::assemble_surface_host_staging(staging),
              direct(blocks)));
  planner.prepare(staging,device.size());
  tetra_viewer::apply_surface_device_upload_plan(planner,staging,device);
  CHECK(equal(tetra_viewer::assemble_surface_device_publication(
                  planner,device),direct(blocks)));
}

TEST_CASE("world render block estimate predicts fragmentation compaction") {
  using Block=tetra_viewer::SparseWorldSurfaceCache::RenderBlock;
  Block block;
  block.id=tetra::hierarchy_block_id(
      tetra::WorldTetAddress::root(0U),2U);
  block.surface_payload_hash=17U;
  block.triangle_vertices.resize(4100U*3U);
  for(auto& vertex:block.triangle_vertices){
    vertex.normal[1]=1.0F;vertex.colour[1]=0.75F;vertex.edge_flags=7.0F;
  }
  tetra_viewer::SurfaceHostStagingStorage staging(1U);
  staging.stage_world_render_blocks(std::span<const Block>(&block,1U));
  block.triangle_vertices.resize(100U*3U);
  ++block.surface_payload_hash;
  const auto estimate=staging.estimate_world_render_blocks(
      std::span<const Block>(&block,1U));
  REQUIRE(estimate.compaction);
  CHECK(estimate.active_ranges==100U);
  CHECK(estimate.dirty_ranges==100U);
  CHECK(estimate.required_vertex_capacity==300U);
  staging.stage_world_render_blocks(std::span<const Block>(&block,1U));
  CHECK(staging.metrics().retained_slots==100U);
  CHECK(staging.metrics().dirty_ranges==estimate.dirty_ranges);
  CHECK(staging.metrics().staged_triangle_bytes==estimate.staged_bytes);
}

TEST_CASE("parallel surface draw packing is byte identical across update paths") {
  using Patch=tetra_viewer::SurfacePatchRecord;
  const auto make_state=[](std::span<const std::size_t> counts,
                           std::uint64_t revision){
    std::pair<std::vector<Patch>,std::vector<tetra::Triangle>> result;
    for(std::size_t owner=0;owner<counts.size();++owner){
      const auto begin=result.second.size();
      for(std::size_t index=0;index<counts[owner];++index){
        const double value=static_cast<double>(
            revision*10000U+owner*100U+index);
        result.second.push_back(
            {{value,0.0,0.0},{0.0,value,0.0},{0.0,0.0,value}});
      }
      result.first.push_back({
          .logical_owner=static_cast<tetra::TetId>(owner+1U),
          .field_revision=revision,
          .topology_hash=revision*31U+owner,
          .triangle_begin=begin,.triangle_count=counts[owner],
          .triangle_capacity=counts[owner]});
    }
    return result;
  };
  const auto equal=[](std::span<const tetra::Triangle> left,
                      std::span<const tetra::Triangle> right){
    return left.size()==right.size()&&
        (left.empty()||std::memcmp(left.data(),right.data(),
                                  left.size_bytes())==0);
  };
  tetra::GeometryExecutor executor({.worker_count=4U});
  tetra_viewer::SurfaceDrawChunkStorage serial(4U),parallel(4U);
  std::array<std::size_t,12U> counts;
  counts.fill(3U);
  for(std::uint64_t revision=1U;revision<=3U;++revision){
    if(revision==2U)counts[5U]=3U;  // same layout, new contents
    if(revision==3U)counts[5U]=17U; // local replacement and slot growth
    auto [patches,arena]=make_state(counts,revision);
    serial.pack(patches,arena);
    parallel.pack(patches,arena,&executor);
    const auto serial_output=
        tetra_viewer::assemble_surface_draw_chunks(serial);
    const auto parallel_output=
        tetra_viewer::assemble_surface_draw_chunks(parallel);
    CHECK(equal(serial_output,parallel_output));
    CHECK(equal(parallel_output,
                tetra_viewer::direct_pack_surface_patches(patches,arena)));
    CHECK(parallel.metrics().copied_bytes==serial.metrics().copied_bytes);
    CHECK(parallel.metrics().parallel_copy_tasks>1U);
  }
}

TEST_CASE("headless scene preparation reports local patch reuse and global fallback") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-volume-connection=hierarchy-cells,"
      "set-surface-method=marching-tetrahedra,prepare-scene,"
      "set-surface-method=lattice-cleaving,prepare-scene,"
      "set-surface-method=dual-contouring,prepare-scene,"
      "set-surface-method=surface-optimization,prepare-scene",
      output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  std::vector<std::string_view> scenes;
  std::size_t line_begin{};
  while(line_begin<text.size()){
    const auto line_end=text.find('\n',line_begin);
    const std::string_view line(text.data()+line_begin,
        (line_end==std::string::npos?text.size():line_end)-line_begin);
    if(line.find("\"event\":\"scene_preparation\"")!=std::string_view::npos)
      scenes.push_back(line);
    if(line_end==std::string::npos)break;
    line_begin=line_end+1U;
  }
  REQUIRE(scenes.size()==4U);
  const auto has=[](std::string_view event,std::string_view field){
    return event.find(field)!=std::string_view::npos;
  };
  CHECK(has(scenes[0],"\"surface_method\":\"marching-tetrahedra\""));
  CHECK(has(scenes[0],"\"surface_patch_active\":true"));
  CHECK(has(scenes[0],"\"surface_patch_full_rebuild\":true"));
  CHECK_FALSE(has(scenes[0],"\"surface_patches_rebuilt\":0"));
  CHECK(has(scenes[0],"\"surface_patches_reused\":0"));

  CHECK(has(scenes[1],"\"surface_method\":\"lattice-cleaving\""));
  CHECK(has(scenes[1],"\"surface_patch_active\":true"));
  CHECK(has(scenes[1],"\"surface_patch_full_rebuild\":false"));
  CHECK(has(scenes[1],"\"surface_patches_rebuilt\":0"));
  CHECK_FALSE(has(scenes[1],"\"surface_patches_reused\":0"));

  CHECK(has(scenes[2],"\"surface_method\":\"dual-contouring\""));
  CHECK(has(scenes[2],"\"surface_patch_active\":true"));
  CHECK(has(scenes[2],"\"surface_patch_full_rebuild\":true"));
  CHECK_FALSE(has(scenes[2],"\"surface_patch_monolithic_fallback\":true"));
  CHECK(has(scenes[2],"\"surface_patch_global_fallback\":false"));

  CHECK(has(scenes[3],"\"surface_method\":\"surface-optimization\""));
  CHECK(has(scenes[3],"\"surface_patch_monolithic_fallback\":true"));
  CHECK(has(scenes[3],"\"surface_patch_global_fallback\":true"));
  CHECK(has(scenes[3],"\"surface_patch_retained_bytes\":"));
}

TEST_CASE("background mesh updates publish only the latest converged snapshot") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera near_camera;
  near_camera.position={0.5,0.5,1.5};
  tetra::AdaptationConfiguration configuration;
  static_cast<void>(tetra::refine_to_sphere(
      mesh,terrain,near_camera,28.0*configuration.split_hysteresis,12));
  const auto source_revision=mesh.revision();
  const auto source_owners=mesh.logical_red_owners().size();

  tetra_viewer::MeshUpdateWorker worker;
  tetra_viewer::MeshUpdateParameters superseded{
      terrain,near_camera,12.0,12,configuration,0};
  const auto first=worker.submit(mesh,superseded);
  auto far_camera=near_camera;
  far_camera.position={0.5,0.5,10.0};
  tetra_viewer::MeshUpdateParameters latest{
      terrain,far_camera,28.0,12,configuration,0};
  const auto second=worker.submit(mesh,latest);
  REQUIRE(second>first);

  auto result=worker.wait_for_completed(std::chrono::seconds(5));
  REQUIRE(result.has_value());
  CHECK(result->request_id==second);
  CHECK(result->source_mesh_revision==source_revision);
  CHECK(result->converged);
  CHECK(tetra_viewer::same_mesh_update_parameters(result->parameters,latest));
  CHECK(result->mesh.revision()!=source_revision);
  CHECK(result->mesh.logical_red_owners().size()<source_owners);
  CHECK_FALSE(result->mesh.shares_storage_with(mesh));
  CHECK(result->mesh.has_positive_active_volumes());
  CHECK(result->mesh.has_conforming_active_faces());
  // The worker never mutates the render-thread snapshot.
  CHECK(mesh.revision()==source_revision);
  CHECK(mesh.logical_red_owners().size()==source_owners);

  auto coarse=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const auto coarse_revision=coarse.revision();
  const auto refine_request=worker.submit(
      coarse,latest,tetra_viewer::MeshUpdateOperation::refine_all_once);
  auto refined=worker.wait_for_completed(std::chrono::seconds(5));
  REQUIRE(refined.has_value());
  CHECK(refined->request_id==refine_request);
  CHECK(refined->operation==tetra_viewer::MeshUpdateOperation::refine_all_once);
  CHECK(refined->converged);
  CHECK(refined->mesh.revision()>coarse_revision);
  CHECK(coarse.revision()==coarse_revision);
}

TEST_CASE("interactive camera policy relaxes work and coalesces only at slice boundaries") {
  tetra_viewer::MeshUpdateParameters settled;
  settled.pixel_threshold=20.0;
  settled.budget.target_milliseconds=
      tetra_viewer::default_mesh_update_time_budget_milliseconds;
  const auto interactive=
      tetra_viewer::make_interactive_mesh_update_parameters(settled);
  CHECK(interactive.intent==
        tetra_viewer::MeshUpdateIntent::interactive_camera);
  CHECK(interactive.pixel_threshold==doctest::Approx(
      settled.pixel_threshold*
      tetra_viewer::interactive_mesh_update_pixel_threshold_scale));
  CHECK(interactive.budget.maximum_operations_per_transaction==
        tetra_viewer::interactive_mesh_update_operation_budget);
  CHECK(interactive.budget.target_milliseconds==doctest::Approx(
      tetra_viewer::interactive_mesh_update_time_budget_milliseconds));

  auto moved=interactive;
  moved.camera.position.x+=0.25;
  CHECK_FALSE(tetra_viewer::same_mesh_update_parameters(interactive,moved));
  auto changed_terrain=interactive;
  changed_terrain.surface.terrain.mountain_amplitude+=1.0;
  CHECK_FALSE(tetra_viewer::same_mesh_update_parameters(
      interactive,changed_terrain));
  CHECK(tetra_viewer::compatible_mesh_update_publication(interactive,moved));
  auto resized=moved;
  resized.camera.aspect_ratio+=0.25;
  CHECK_FALSE(tetra_viewer::compatible_mesh_update_publication(
      interactive,resized));
  moved.field_revision+=1U;
  CHECK_FALSE(tetra_viewer::compatible_mesh_update_publication(
      interactive,moved));
  CHECK_FALSE(tetra_viewer::compatible_mesh_update_publication(
      interactive,settled));

  CHECK(tetra_viewer::should_submit_mesh_update(
      false,true,true,false,false));
  CHECK_FALSE(tetra_viewer::should_submit_mesh_update(
      true,true,true,false,false));
  CHECK(tetra_viewer::should_submit_mesh_update(
      true,true,true,true,false));
  CHECK(tetra_viewer::should_submit_mesh_update(
      true,true,true,false,true));
  CHECK(tetra_viewer::should_submit_mesh_update(
      true,true,false,false,false));
  CHECK_FALSE(tetra_viewer::should_submit_mesh_update(
      true,false,false,true,true));
}

TEST_CASE("interactive camera movement publishes lagged conforming progress then retargets") {
  auto displayed_mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera camera;
  camera.position={0.5,0.5,1.25};
  camera.forward={0.0,0.0,-1.0};
  tetra::AdaptationConfiguration configuration;
  configuration.candidate_traversal=
      tetra::CandidateTraversal::hierarchy_bounds;
  tetra_viewer::MeshUpdateParameters settled{
      sphere,camera,4.0,9U,configuration,0U,
      {.target_milliseconds=
           tetra_viewer::default_mesh_update_time_budget_milliseconds}};
  auto submitted=
      tetra_viewer::make_interactive_mesh_update_parameters(settled);
  // Force a short first slice so the test observes an intermediate scene.
  submitted.budget.target_milliseconds=1.0e-9;

  tetra_viewer::MeshUpdateWorker worker;
  tetra::AdaptationPlanningCache planning_cache;
  const auto first_request=worker.submit(displayed_mesh,submitted);
  auto completed=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(completed.has_value());
  REQUIRE_FALSE(completed->converged);

  auto latest=submitted;
  latest.camera.position.x+=0.15;
  const auto publication=tetra_viewer::publish_mesh_update_result(
      worker,std::move(*completed),displayed_mesh,planning_cache,
      first_request,tetra_viewer::MeshUpdateOperation::reconcile_lod,latest);
  REQUIRE(publication.status==
          tetra_viewer::MeshPublicationStatus::intermediate);
  CHECK(displayed_mesh.has_positive_active_volumes());
  CHECK(displayed_mesh.has_conforming_active_faces());
  CHECK(tetra_viewer::should_submit_mesh_update(
      true,true,true,true,false));

  const auto latest_request=worker.submit(displayed_mesh,latest);
  auto retargeted=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(retargeted.has_value());
  CHECK(retargeted->request_id==latest_request);
  CHECK(tetra_viewer::same_mesh_update_parameters(
      retargeted->parameters,latest));
  CHECK(retargeted->mesh.has_positive_active_volumes());
  CHECK(retargeted->mesh.has_conforming_active_faces());
}

TEST_CASE("worker budgets stop only at complete transactions and preserve final hashes") {
  const auto source=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera camera;
  camera.position={0.5,0.5,1.25};
  camera.forward={0.0,0.0,-1.0};
  tetra::AdaptationConfiguration configuration;
  tetra_viewer::MeshUpdateParameters wide_parameters{
      sphere,camera,4.0,9U,configuration,0U,
      {.maximum_operations_per_transaction=4096U}};
  auto narrow_parameters=wide_parameters;
  narrow_parameters.budget.maximum_operations_per_transaction=64U;
  CHECK_FALSE(tetra_viewer::same_mesh_update_parameters(
      wide_parameters,narrow_parameters));
  auto low_yield_parameters=narrow_parameters;
  low_yield_parameters.budget.minimum_useful_operations_per_transaction=
      std::numeric_limits<std::uint32_t>::max();
  CHECK_FALSE(tetra_viewer::same_mesh_update_parameters(
      narrow_parameters,low_yield_parameters));
  auto low_yield_rate_parameters=narrow_parameters;
  low_yield_rate_parameters.budget.minimum_useful_operations_per_millisecond=
      std::numeric_limits<double>::max();
  CHECK_FALSE(tetra_viewer::same_mesh_update_parameters(
      narrow_parameters,low_yield_rate_parameters));

  tetra_viewer::MeshUpdateWorker worker;
  static_cast<void>(worker.submit(source,wide_parameters));
  auto wide=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(wide.has_value());
  REQUIRE(wide->converged);
  CHECK_FALSE(wide->time_budget_reached);
  CHECK(wide->transaction_operation_budget==4096U);
  CHECK(wide->cumulative_snapshot_copy_count==1U);
  CHECK(wide->cumulative_snapshot_copy_bytes==source.snapshot_copy_bytes());
  CHECK(wide->cumulative_snapshot_copy_milliseconds>=0.0);
  CHECK(wide->cumulative_worker_handoff_count==1U);
  CHECK(wide->cumulative_worker_handoff_milliseconds>=0.0);

  static_cast<void>(worker.submit(source,narrow_parameters));
  auto narrow=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(narrow.has_value());
  REQUIRE(narrow->converged);
  CHECK_FALSE(narrow->time_budget_reached);
  CHECK(narrow->transaction_operation_budget==64U);
  CHECK(narrow->adaptation.iterations>=wide->adaptation.iterations);
  CHECK(narrow->mesh.logical_cut().owners==wide->mesh.logical_cut().owners);
  CHECK(std::ranges::equal(narrow->mesh.conforming_volume().addresses(),
                           wide->mesh.conforming_volume().addresses()));

  auto mixed_threshold_parameters=narrow_parameters;
  mixed_threshold_parameters.budget.minimum_useful_operations_per_transaction=1U;
  mixed_threshold_parameters.budget.minimum_useful_operations_per_millisecond=
      std::numeric_limits<double>::max();
  static_cast<void>(worker.submit(source,mixed_threshold_parameters));
  auto mixed_threshold=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(mixed_threshold.has_value());
  CHECK(mixed_threshold->converged);
  CHECK_FALSE(mixed_threshold->low_yield_cutoff_reached);
  CHECK(mixed_threshold->low_yield_slices==0U);
  CHECK(mixed_threshold->mesh.logical_cut().owners==
        wide->mesh.logical_cut().owners);

  auto timed_parameters=narrow_parameters;
  timed_parameters.budget.target_milliseconds=1.0e-9;
  static_cast<void>(worker.submit(source,timed_parameters));
  auto timed=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(timed.has_value());
  CHECK_FALSE(timed->converged);
  CHECK(timed->time_budget_reached);
  CHECK(timed->adaptation.iterations==1U);
  CHECK(timed->admissible_operations>0U);
  CHECK(timed->admissible_operations<=64U);
  CHECK(timed->mesh.revision()!=source.revision());
  CHECK(timed->mesh.has_positive_active_volumes());
  CHECK(timed->mesh.has_conforming_active_faces());

  low_yield_parameters.budget.minimum_useful_operations_per_millisecond=
      std::numeric_limits<double>::max();
  static_cast<void>(worker.submit(source,low_yield_parameters));
  auto low_yield=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(low_yield.has_value());
  REQUIRE_FALSE(low_yield->converged);
  CHECK(low_yield->low_yield_cutoff_reached);
  CHECK_FALSE(low_yield->time_budget_reached);
  CHECK(low_yield->adaptation.iterations==1U);
  CHECK(low_yield->mesh.revision()!=source.revision());
  CHECK(low_yield->mesh.has_positive_active_volumes());
  CHECK(low_yield->mesh.has_conforming_active_faces());
  CHECK(low_yield->low_yield_slices==1U);
  CHECK(low_yield->last_transaction_useful_operations==
        low_yield->committed_useful_operations);
  CHECK(low_yield->last_transaction_useful_operations_per_millisecond>0.0);

  std::size_t useful_operations=low_yield->committed_useful_operations;
  std::size_t low_yield_slices=1U;
  for(std::size_t slice=1U;!low_yield->converged&&slice<64U;++slice){
    const auto continuation=worker.submit_continuation(std::move(*low_yield));
    REQUIRE(continuation.status==
            tetra_viewer::MeshContinuationStatus::accepted);
    low_yield=worker.wait_for_completed(std::chrono::seconds(10));
    REQUIRE(low_yield.has_value());
    CHECK(low_yield->mesh.has_positive_active_volumes());
    CHECK(low_yield->mesh.has_conforming_active_faces());
    useful_operations+=low_yield->committed_useful_operations;
    if(low_yield->low_yield_cutoff_reached)++low_yield_slices;
    CHECK(low_yield->cumulative_committed_useful_operations==useful_operations);
    CHECK(low_yield->low_yield_slices==low_yield_slices);
    if(!low_yield->converged){
      CHECK(low_yield->low_yield_cutoff_reached);
      CHECK(low_yield->adaptation.iterations==1U);
    }
  }
  REQUIRE(low_yield->converged);
  CHECK_FALSE(low_yield->low_yield_cutoff_reached);
  CHECK(low_yield_slices>1U);
  CHECK(low_yield->mesh.logical_cut().owners==wide->mesh.logical_cut().owners);
  CHECK(std::ranges::equal(low_yield->mesh.conforming_volume().addresses(),
                           wide->mesh.conforming_volume().addresses()));
}

TEST_CASE("unconverged worker revisions resume retained planning state") {
  const auto source=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera camera;
  camera.position={0.5,0.5,1.25};
  camera.forward={0.0,0.0,-1.0};
  tetra::AdaptationConfiguration configuration;
  configuration.candidate_traversal=
      tetra::CandidateTraversal::hierarchy_bounds;
  tetra_viewer::MeshUpdateParameters wide_parameters{
      sphere,camera,4.0,9U,configuration,0U,
      {.maximum_operations_per_transaction=4096U}};
  auto sliced_parameters=wide_parameters;
  sliced_parameters.budget.maximum_operations_per_transaction=64U;
  sliced_parameters.budget.target_milliseconds=1.0e-9;

  tetra_viewer::MeshUpdateWorker worker;
  static_cast<void>(worker.submit(source,wide_parameters));
  auto wide=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(wide.has_value());
  REQUIRE(wide->converged);

  const auto first_request=worker.submit(source,sliced_parameters);
  auto slice=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(slice.has_value());
  REQUIRE_FALSE(slice->converged);
  CHECK(slice->request_id==first_request);
  CHECK(slice->chain_id==first_request);
  CHECK(slice->slice_index==0U);
  CHECK(slice->source_mesh_revision==source.revision());
  CHECK(slice->slice_source_mesh_revision==source.revision());
  REQUIRE_FALSE(slice->planning_cache.layers.empty());
  CHECK(slice->cumulative_snapshot_copy_count==1U);
  CHECK(slice->cumulative_snapshot_copy_bytes==source.snapshot_copy_bytes());
  CHECK(slice->cumulative_worker_handoff_count==1U);

  const auto retained_capacity=[](const tetra::AdaptationPlanningCache& cache){
    std::size_t capacity=cache.layers.capacity()+
        cache.transaction_layers.capacity()+cache.spatial_runs.capacity()+
        cache.split_queue.capacity()+cache.merge_queue.capacity();
    for(const auto& layer:cache.layers)
      capacity+=layer.addresses.capacity()+layer.spatial_minimum.capacity()+
          layer.spatial_maximum.capacity()+layer.field_minimum.capacity()+
          layer.field_maximum.capacity()+
          layer.deepest_resident_depth.capacity()+
          layer.deepest_active_depth.capacity()+
          layer.pinned_descendant_words.capacity();
    for(const auto& layer:cache.transaction_layers)
      capacity+=layer.addresses.capacity()+
          layer.current_status_words.capacity()+
          layer.desired_mark_words.capacity()+layer.command_words.capacity();
    return capacity;
  };

  auto reused_first=*slice;
  const auto chain_id=slice->chain_id;
  auto previous_request=slice->request_id;
  auto previous_revision=slice->mesh.revision();
  auto previous_capacity=retained_capacity(slice->planning_cache);
  std::size_t summed_transactions=slice->adaptation.iterations;
  std::size_t summed_admissible=slice->admissible_operations;
  double summed_duration=slice->duration_milliseconds;
  std::size_t completed_slices=1U;
  while(!slice->converged&&completed_slices<64U){
    const auto submission=worker.submit_continuation(std::move(*slice));
    REQUIRE(submission.status==
            tetra_viewer::MeshContinuationStatus::accepted);
    CHECK(submission.request_id>previous_request);
    if(completed_slices==1U){
      const auto reused=worker.submit_continuation(std::move(reused_first));
      CHECK(reused.status==tetra_viewer::MeshContinuationStatus::superseded);
      CHECK(reused.request_id==0U);
    }
    slice=worker.wait_for_completed(std::chrono::seconds(10));
    REQUIRE(slice.has_value());
    ++completed_slices;
    CHECK(slice->chain_id==chain_id);
    CHECK(slice->slice_index==completed_slices-1U);
    CHECK(slice->request_id==submission.request_id);
    CHECK(slice->slice_source_mesh_revision==previous_revision);
    CHECK(slice->source_mesh_revision==source.revision());
    CHECK(slice->mesh.has_positive_active_volumes());
    CHECK(slice->mesh.has_conforming_active_faces());
    CHECK(slice->planning_cache.field_revision==0U);
    CHECK(retained_capacity(slice->planning_cache)>=previous_capacity);
    summed_transactions+=slice->adaptation.iterations;
    summed_admissible+=slice->admissible_operations;
    summed_duration+=slice->duration_milliseconds;
    CHECK(slice->cumulative_adaptation.iterations==summed_transactions);
    CHECK(slice->cumulative_admissible_operations==summed_admissible);
    CHECK(slice->cumulative_duration_milliseconds==
          doctest::Approx(summed_duration));
    CHECK(slice->cumulative_snapshot_copy_count==1U);
    CHECK(slice->cumulative_snapshot_copy_bytes==source.snapshot_copy_bytes());
    CHECK(slice->cumulative_worker_handoff_count==completed_slices);
    previous_request=slice->request_id;
    previous_revision=slice->mesh.revision();
    previous_capacity=retained_capacity(slice->planning_cache);
  }
  REQUIRE(slice->converged);
  CHECK(completed_slices>1U);
  CHECK(slice->mesh.logical_cut().owners==wide->mesh.logical_cut().owners);
  CHECK(std::ranges::equal(slice->mesh.conforming_volume().addresses(),
                           wide->mesh.conforming_volume().addresses()));

  const auto finished=worker.submit_continuation(std::move(*slice));
  CHECK(finished.status==
        tetra_viewer::MeshContinuationStatus::already_converged);
  CHECK(finished.request_id==0U);
}

TEST_CASE("viewer publishes every complete worker slice before convergence") {
  const auto source=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  tetra::Camera camera;
  camera.position={0.5,0.5,1.25};
  camera.forward={0.0,0.0,-1.0};
  tetra::AdaptationConfiguration configuration;
  configuration.candidate_traversal=
      tetra::CandidateTraversal::hierarchy_bounds;
  tetra_viewer::MeshUpdateParameters wide_parameters{
      sphere,camera,4.0,9U,configuration,0U,
      {.maximum_operations_per_transaction=4096U}};
  auto sliced_parameters=wide_parameters;
  sliced_parameters.budget.maximum_operations_per_transaction=64U;
  sliced_parameters.budget.target_milliseconds=1.0e-9;

  tetra_viewer::MeshUpdateWorker worker;
  static_cast<void>(worker.submit(source,wide_parameters));
  auto wide=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(wide.has_value());
  REQUIRE(wide->converged);

  auto published_mesh=source;
  tetra::AdaptationPlanningCache published_planning_cache;
  tetra_viewer::SceneCache scene_cache;
  REQUIRE(scene_cache.update_scene(
      published_mesh,sphere,0U,
      tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::variational_smooth,true,false,true,false));
  const auto initial_scene_generation=scene_cache.scene_generation();
  auto expected_request=worker.submit(source,sliced_parameters);
  std::size_t intermediate_revisions{};
  std::size_t previous_logical_owners=
      published_mesh.logical_cut().owners.size();
  std::uint64_t chain_id{};
  tetra_viewer::MeshPublicationResult final_publication;
  for(std::size_t slice_index=0;slice_index<64U;++slice_index){
    auto completed=worker.wait_for_completed(std::chrono::seconds(10));
    REQUIRE(completed.has_value());
    const auto publication=tetra_viewer::publish_mesh_update_result(
        worker,std::move(*completed),published_mesh,
        published_planning_cache,expected_request,
        tetra_viewer::MeshUpdateOperation::reconcile_lod,sliced_parameters);
    REQUIRE(publication.published());
    CHECK(publication.slice_index==slice_index);
    if(slice_index==0U)chain_id=publication.chain_id;
    CHECK(publication.chain_id==chain_id);
    CHECK(published_mesh.has_positive_active_volumes());
    CHECK(published_mesh.has_conforming_active_faces());
    const auto logical_owners=published_mesh.logical_cut().owners.size();
    CHECK(logical_owners>=previous_logical_owners);
    if(publication.status==
       tetra_viewer::MeshPublicationStatus::intermediate){
      CHECK(logical_owners>previous_logical_owners);
      CHECK(publication.snapshot_copy_bytes>0U);
      CHECK(publication.snapshot_copy_milliseconds>=0.0);
      CHECK(publication.worker_handoff_milliseconds>=0.0);
      ++intermediate_revisions;
      CHECK(publication.cumulative_snapshot_copy_count==
            intermediate_revisions+1U);
      CHECK(publication.cumulative_worker_handoff_count==
            intermediate_revisions+1U);
      expected_request=publication.request_id;
      REQUIRE(scene_cache.update_scene(
          published_mesh,sphere,0U,
          tetra_viewer::SurfaceMethod::surface_optimization,
          tetra_viewer::MaterialRule::variational_smooth,
          true,false,true,false));
      CHECK(scene_cache.mesh_revision()==published_mesh.revision());
      CHECK(scene_cache.scene_generation()==
            initial_scene_generation+intermediate_revisions);
      previous_logical_owners=logical_owners;
      continue;
    }
    REQUIRE(publication.status==
            tetra_viewer::MeshPublicationStatus::converged);
    final_publication=publication;
    break;
  }
  REQUIRE(final_publication.status==
          tetra_viewer::MeshPublicationStatus::converged);
  CHECK(intermediate_revisions>1U);
  CHECK(final_publication.adaptation.iterations==intermediate_revisions);
  CHECK(final_publication.snapshot_copy_bytes==0U);
  CHECK(final_publication.snapshot_copy_milliseconds==0.0);
  CHECK(final_publication.worker_handoff_milliseconds==0.0);
  CHECK(final_publication.cumulative_snapshot_copy_count==
        intermediate_revisions+1U);
  CHECK(final_publication.cumulative_snapshot_copy_bytes>
        source.snapshot_copy_bytes());
  CHECK(final_publication.cumulative_worker_handoff_count==
        intermediate_revisions+1U);
  CHECK_FALSE(published_planning_cache.layers.empty());
  CHECK(published_mesh.logical_cut().owners==wide->mesh.logical_cut().owners);
  CHECK(std::ranges::equal(published_mesh.conforming_volume().addresses(),
                           wide->mesh.conforming_volume().addresses()));

  const auto final_revision=published_mesh.revision();
  static_cast<void>(worker.submit(source,sliced_parameters));
  auto stale=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(stale.has_value());
  const auto wrong_request_id=stale->request_id+1U;
  const auto rejected=tetra_viewer::publish_mesh_update_result(
      worker,std::move(*stale),published_mesh,published_planning_cache,
      wrong_request_id,tetra_viewer::MeshUpdateOperation::reconcile_lod,
      sliced_parameters);
  CHECK(rejected.status==tetra_viewer::MeshPublicationStatus::stale);
  CHECK_FALSE(rejected.published());
  CHECK(published_mesh.revision()==final_revision);
}

TEST_CASE("production terrain LOD publishes visible progress before convergence") {
  const auto source=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.35,0.71,0.35};
  camera.forward={0.71,-0.55,0.44};
  tetra::AdaptationConfiguration configuration;
  configuration.candidate_traversal=
      tetra::CandidateTraversal::hierarchy_bounds;
  tetra_viewer::MeshUpdateParameters parameters{
      terrain,camera,13.0,21U,configuration,0U,
      {.target_milliseconds=
           tetra_viewer::default_mesh_update_time_budget_milliseconds}};

  tetra_viewer::MeshUpdateWorker worker;
  auto displayed_mesh=source;
  tetra::AdaptationPlanningCache planning_cache;
  const auto displayed_revision=displayed_mesh.revision();
  const auto expected_request=worker.submit(source,parameters);
  auto completed=worker.wait_for_completed(std::chrono::seconds(20));
  REQUIRE(completed.has_value());
  REQUIRE_FALSE(completed->converged);
  const auto publication=tetra_viewer::publish_mesh_update_result(
      worker,std::move(*completed),displayed_mesh,planning_cache,
      expected_request,tetra_viewer::MeshUpdateOperation::reconcile_lod,
      parameters);
  REQUIRE(publication.status==
          tetra_viewer::MeshPublicationStatus::intermediate);
  CHECK(displayed_mesh.revision()!=displayed_revision);
  CHECK(displayed_mesh.logical_cut().owners.size()>
        source.logical_cut().owners.size());
  CHECK(worker.busy());
}

TEST_CASE("progressive terrain publications stay field aligned and manifold") {
  auto displayed_mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  camera.position={0.35,0.71,0.35};
  camera.forward={0.71,-0.55,0.44};
  tetra::AdaptationConfiguration configuration;
  tetra_viewer::MeshUpdateParameters parameters{
      terrain,camera,13.0,21U,configuration,0U,
      {.maximum_operations_per_transaction=256U,
       .target_milliseconds=1.0e-9}};
  tetra_viewer::MeshUpdateWorker worker;
  tetra::AdaptationPlanningCache planning_cache;
  auto expected_request=worker.submit(displayed_mesh,parameters);
  for(std::size_t slice=0;slice<4U;++slice){
    auto completed=worker.wait_for_completed(std::chrono::seconds(20));
    REQUIRE(completed.has_value());
    const auto publication=tetra_viewer::publish_mesh_update_result(
        worker,std::move(*completed),displayed_mesh,planning_cache,
        expected_request,tetra_viewer::MeshUpdateOperation::reconcile_lod,
        parameters);
    REQUIRE(publication.status==
            tetra_viewer::MeshPublicationStatus::intermediate);
    expected_request=publication.request_id;
    REQUIRE(displayed_mesh.has_positive_active_volumes());
    REQUIRE(displayed_mesh.has_conforming_active_faces());

    const auto scene=tetra_viewer::prepare_scene(
        displayed_mesh,terrain,
        tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::variational_smooth,
        true,false,false,false,false,false,1.0,
        tetra_viewer::VolumeConnectionMethod::adaptive_cleaving,
        tetra_viewer::StencilConstruction::fixed,
        tetra_viewer::StencilSelectionObjective::balanced,
        {.surface_diagnostics=false,.summary_statistics=false});
    CAPTURE(slice);
    REQUIRE_FALSE(scene.triangle_vertices.empty());
    const auto topology=tetra_viewer::validate_connected_complex(
        scene,&displayed_mesh);
    // Terrain material intentionally reaches the unit-domain floor and side
    // walls, so those unmatched domain faces are not implicit-surface faces.
    CHECK(topology.positive_volumes);
    CHECK(topology.manifold_face_incidence);
    CHECK(topology.regions_aligned);
    CHECK(topology.graded_parent_band);
    CHECK(topology.nonmanifold_faces==0U);
    for(const auto& vertex:scene.triangle_vertices){
      const tetra::Vec3 point{
          vertex.position[0],vertex.position[1],vertex.position[2]};
      CHECK(std::abs(terrain.signed_distance(point))<1.0e-6);
      const double normal_length=std::sqrt(
          vertex.normal[0]*vertex.normal[0]+
          vertex.normal[1]*vertex.normal[1]+
          vertex.normal[2]*vertex.normal[2]);
      CHECK(normal_length==doctest::Approx(1.0).epsilon(1.0e-5));
    }
  }
}

TEST_CASE("scene preparation worker publishes only the latest complete scene") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere first_surface;
  tetra::Sphere latest_surface;
  latest_surface.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::ScenePreparationParameters first;
  first.surface=first_surface;
  first.surface_method=tetra_viewer::SurfaceMethod::marching_tetrahedra;
  first.show_volume_edges=false;
  first.show_volume_faces=false;
  first.preparation={.surface_diagnostics=false,.summary_statistics=false};
  auto latest=first;
  latest.surface=latest_surface;
  latest.surface_revision=1U;
  auto changed_terrain=first;
  changed_terrain.surface.terrain.mountain_range_frequency*=0.5;
  CHECK_FALSE(tetra_viewer::same_scene_preparation_parameters(
      first,changed_terrain));

  tetra_viewer::ScenePreparationWorker worker;
  const auto first_request=worker.submit(mesh,first);
  const auto latest_request=worker.submit(mesh,latest);
  CHECK(latest_request>first_request);
  auto result=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(result.has_value());
  CHECK(result->request_id==latest_request);
  CHECK(result->mesh_revision==mesh.revision());
  CHECK(tetra_viewer::same_scene_preparation_parameters(
      result->parameters,latest));
  CHECK_FALSE(result->scene.triangle_vertices.empty());
  CHECK(result->duration_milliseconds>=0.0);
  CHECK_FALSE(worker.busy());
}

TEST_CASE("interactive scene preparation publishes completed lagged geometry instead of starving") {
  auto displayed_mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra_viewer::ScenePreparationParameters parameters;
  parameters.surface_method=
      tetra_viewer::SurfaceMethod::marching_tetrahedra;
  parameters.show_volume_edges=false;
  parameters.show_volume_faces=false;
  parameters.preparation={
      .surface_diagnostics=false,.summary_statistics=false};

  tetra_viewer::ScenePreparationWorker worker;
  const auto request=worker.submit(displayed_mesh,parameters);
  displayed_mesh.refine_all_binary();
  auto completed=worker.wait_for_completed(std::chrono::seconds(10));
  REQUIRE(completed.has_value());
  REQUIRE(completed->mesh_revision!=displayed_mesh.revision());
  REQUIRE_FALSE(completed->scene.triangle_vertices.empty());

  CHECK_FALSE(tetra_viewer::compatible_scene_preparation_publication(
      *completed,request,displayed_mesh.revision(),parameters,false));
  CHECK(tetra_viewer::compatible_scene_preparation_publication(
      *completed,request,displayed_mesh.revision(),parameters,true));
  auto changed=parameters;
  changed.show_surface_edges=!changed.show_surface_edges;
  CHECK_FALSE(tetra_viewer::compatible_scene_preparation_publication(
      *completed,request,displayed_mesh.revision(),changed,true));
  CHECK_FALSE(tetra_viewer::compatible_scene_preparation_publication(
      *completed,request+1U,displayed_mesh.revision(),parameters,true));

  // This is the starvation regression: rapid mesh revisions must not cancel
  // the only renderable in-flight scene during a drag.
  CHECK_FALSE(tetra_viewer::should_submit_scene_preparation(
      true,true,true));
  CHECK(tetra_viewer::should_submit_scene_preparation(
      true,false,true));
  CHECK(tetra_viewer::should_submit_scene_preparation(
      true,true,false));
  CHECK_FALSE(tetra_viewer::should_submit_scene_preparation(
      false,false,true));
}

TEST_CASE("headless worker budget benchmark reports bounded hash-equivalent policies") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "benchmark-cpu-worker-budgets",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"variant\":\"wide\"")!=std::string::npos);
  CHECK(text.find("\"variant\":\"bounded\"")!=std::string::npos);
  CHECK(text.find("\"variant\":\"timed-slice\"")!=std::string::npos);
  CHECK(text.find("\"variant\":\"resumed-slices\"")!=std::string::npos);
  CHECK(text.find("\"variant\":\"low-yield-slices\"")!=std::string::npos);
  CHECK(text.find("\"variant\":\"production-shared-snapshot\"")!=
        std::string::npos);
  CHECK(text.find("\"transaction_operation_budget\":64")!=std::string::npos);
  CHECK(text.find("\"time_budget_reached\":true,\"converged\":false,\"valid\":true")
        !=std::string::npos);
  CHECK(text.find("\"resumed_without_rebuild\":true")!=std::string::npos);
  CHECK(text.find("\"published_revisions\":5")!=std::string::npos);
  CHECK(text.find("\"intermediate_revisions\":4")!=std::string::npos);
  CHECK(text.find("\"low_yield_cutoff_reached\":true")!=std::string::npos);
  CHECK(text.find("\"low_yield_slices\":")!=std::string::npos);
  CHECK(text.find("\"committed_useful_operations\":")!=std::string::npos);
  CHECK(text.find("\"last_useful_operations_per_ms\":")!=std::string::npos);
  CHECK(text.find("\"snapshot_copy_count\":5")!=std::string::npos);
  CHECK(text.find("\"snapshot_copy_bytes\":")!=std::string::npos);
  CHECK(text.find("\"resident_storage_bytes\":")!=std::string::npos);
  CHECK(text.find("\"snapshot_copy_ms\":")!=std::string::npos);
  CHECK(text.find("\"worker_handoff_count\":5")!=std::string::npos);
  CHECK(text.find("\"worker_handoff_ms\":")!=std::string::npos);
  CHECK(text.find("\"transfer_fraction_of_measured_cpu\":")!=
        std::string::npos);
  CHECK(text.find("\"stationary_shared_storage\":true")!=std::string::npos);
  CHECK(text.find("\"moved_private_storage\":true")!=std::string::npos);
  CHECK(text.find("\"source_unchanged\":true,\"valid\":true")!=
        std::string::npos);
}

TEST_CASE("headless worker supersession keeps only the latest camera request") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "benchmark-cpu-worker-supersession",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"event\":\"cpu_worker_supersession_benchmark\"")!=
        std::string::npos);
  CHECK(text.find("\"rapid_requests\":8")!=std::string::npos);
  CHECK(text.find("\"latest_wins\":true")!=std::string::npos);
  CHECK(text.find("\"prompt_boundary\":true")!=std::string::npos);
  CHECK(text.find("\"stale_publications\":0")!=std::string::npos);
}

TEST_CASE("lightweight scene preparation preserves render geometry without research diagnostics") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,80.0,4));
  const auto full=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,0.5,tetra_viewer::VolumeConnectionMethod::quality_stencils,
      tetra_viewer::StencilConstruction::fixed,
      tetra_viewer::StencilSelectionObjective::balanced,{});
  const auto lightweight=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,0.5,tetra_viewer::VolumeConnectionMethod::quality_stencils,
      tetra_viewer::StencilConstruction::fixed,
      tetra_viewer::StencilSelectionObjective::balanced,
      {.surface_diagnostics=false,.summary_statistics=false});
  REQUIRE(lightweight.triangle_vertices.size()==full.triangle_vertices.size());
  CHECK(lightweight.surface_line_vertices.size()==full.surface_line_vertices.size());
  CHECK(lightweight.relations==full.relations);
  CHECK_FALSE(lightweight.surface_diagnostics_available);
  CHECK_FALSE(lightweight.summary_statistics_available);
  CHECK(full.surface_diagnostics_available);
  CHECK(full.summary_statistics_available);
  CHECK(lightweight.depth_counts.empty());
  CHECK(lightweight.total_volume==0.0);
  for(std::size_t index=0;index<lightweight.triangle_vertices.size();++index){
    const auto& actual=lightweight.triangle_vertices[index];
    const auto& expected=full.triangle_vertices[index];
    CHECK(std::equal(std::begin(actual.position),std::end(actual.position),std::begin(expected.position)));
    CHECK(std::equal(std::begin(actual.colour),std::end(actual.colour),std::begin(expected.colour)));
    CHECK(std::equal(std::begin(actual.normal),std::end(actual.normal),std::begin(expected.normal)));
    CHECK(std::equal(std::begin(actual.barycentric),std::end(actual.barycentric),std::begin(expected.barycentric)));
  }
}

TEST_CASE("scene cache enriches lightweight geometry only when requested") {
  auto mesh=tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  tetra_viewer::SceneCache cache;
  const auto update=[&](tetra_viewer::ScenePreparationOptions preparation){
    return cache.update_scene(
        mesh,sphere,0,tetra_viewer::SurfaceMethod::marching_tetrahedra,
        tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
        false,false,0.5,tetra_viewer::VolumeConnectionMethod::quality_stencils,
        tetra_viewer::StencilConstruction::fixed,
        tetra_viewer::StencilSelectionObjective::balanced,preparation);
  };
  const tetra_viewer::ScenePreparationOptions lightweight{
      .surface_diagnostics=false,.summary_statistics=false};
  REQUIRE(update(lightweight));
  CHECK_FALSE(cache.scene().surface_diagnostics_available);
  const auto lightweight_generation=cache.scene_generation();
  CHECK_FALSE(update(lightweight));
  CHECK(update({}));
  CHECK(cache.scene_generation()==lightweight_generation+1);
  CHECK(cache.scene().surface_diagnostics_available);
  CHECK(cache.scene().summary_statistics_available);
  // A fully measured scene satisfies a later lightweight request without a
  // rebuild or loss of measurements.
  CHECK_FALSE(update(lightweight));
  CHECK(cache.scene_generation()==lightweight_generation+1);
}

TEST_CASE("viewer scene cache rebuilds only for relevant revisions") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  const tetra::Sphere sphere{};
  tetra::Camera camera{};
  tetra_viewer::SceneCache cache;

  CHECK(cache.update_scene(mesh, sphere, 0, tetra_viewer::MaterialRule::all_vertices_inside, true, true, true));
  const auto first_scene_generation = cache.scene_generation();
  CHECK_FALSE(cache.update_scene(mesh, sphere, 0, tetra_viewer::MaterialRule::all_vertices_inside, true, true, true));
  CHECK(cache.scene_generation() == first_scene_generation);
  CHECK(cache.update_scene(mesh, sphere, 0, tetra_viewer::MaterialRule::centroid_inside, true, true, true));
  CHECK(cache.scene_generation() == first_scene_generation + 1);
  CHECK_FALSE(cache.update_scene(mesh, sphere, 0, tetra_viewer::MaterialRule::centroid_inside, true, true, true));

  CHECK(cache.update_projection(mesh, camera, 28.0));
  const auto first_projection_generation = cache.projection_generation();
  CHECK_FALSE(cache.update_projection(mesh, camera, 28.0));
  camera.position.z += 1.0;
  CHECK(cache.update_projection(mesh, camera, 28.0));
  CHECK(cache.projection_generation() == first_projection_generation + 1);
  CHECK(cache.scene_generation() == first_scene_generation + 1);

  mesh.refine_all_binary();
  CHECK(cache.update_scene(mesh, sphere, 0, tetra_viewer::MaterialRule::all_vertices_inside, true, true, true));
  CHECK(cache.scene_generation() == first_scene_generation + 2);
  CHECK(cache.update_projection(mesh, camera, 28.0));

  const auto other_method = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  CHECK(cache.update_scene(other_method, sphere, 0, tetra_viewer::MaterialRule::all_vertices_inside, true, true, true));
}

TEST_CASE("viewer scene cache distinguishes methods at the same mesh revision") {
  const auto six = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  const auto twenty_four = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_halfedge_24);
  const tetra::Sphere sphere{};
  tetra_viewer::SceneCache cache;
  CHECK(six.revision() == twenty_four.revision());
  CHECK(cache.update_scene(six, sphere, 0, tetra_viewer::MaterialRule::all_vertices_inside, true, true, true));
  const auto generation = cache.scene_generation();
  CHECK(cache.update_scene(twenty_four, sphere, 0, tetra_viewer::MaterialRule::all_vertices_inside, true, true, true));
  CHECK(cache.scene_generation() == generation + 1);
}

TEST_CASE("viewer scene cache distinguishes stencil construction and objective") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,80.0,6));
  tetra_viewer::SceneCache cache;
  const auto update=[&](tetra_viewer::StencilConstruction construction,
                        tetra_viewer::StencilSelectionObjective objective){
    return cache.update_scene(
        mesh,sphere,0,tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,0.5,
        tetra_viewer::VolumeConnectionMethod::quality_stencils,
        construction,objective);
  };
  CHECK(update(tetra_viewer::StencilConstruction::fixed,
               tetra_viewer::StencilSelectionObjective::balanced));
  const auto generation=cache.scene_generation();
  CHECK_FALSE(update(tetra_viewer::StencilConstruction::fixed,
                     tetra_viewer::StencilSelectionObjective::balanced));
  CHECK(update(tetra_viewer::StencilConstruction::selected,
               tetra_viewer::StencilSelectionObjective::balanced));
  CHECK(cache.scene_generation()==generation+1);
  CHECK(update(tetra_viewer::StencilConstruction::selected,
               tetra_viewer::StencilSelectionObjective::surface));
  CHECK(cache.scene_generation()==generation+2);
}

TEST_CASE("mesh revision changes once per public refinement operation") {
  auto mesh = tetra::TetMesh::make_unit_cube();
  CHECK(mesh.revision() == 0);
  mesh.refine_all_binary();
  CHECK(mesh.revision() == 1);
  mesh.refine_selected_binary({});
  CHECK(mesh.revision() == 1);
  mesh.refine_all_binary();
  CHECK(mesh.revision() == 2);
}

TEST_CASE("headless refinement benchmark reports every increasing pass") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script("benchmark-refinement=3,validate", output, errors) == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("\"event\":\"refinement_benchmark\",\"pass\":1") != std::string::npos);
  CHECK(text.find("\"event\":\"refinement_benchmark\",\"pass\":2") != std::string::npos);
  CHECK(text.find("\"event\":\"refinement_benchmark\",\"pass\":3") != std::string::npos);
  CHECK(text.find("\"event\":\"validation\",\"valid\":true") != std::string::npos);
}

TEST_CASE("headless named owner diagnostic exposes projection demand and decision") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script("diagnose-owner=first",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"event\":\"camera_lod_owner_diagnostic\"")!=
        std::string::npos);
  CHECK(text.find("\"selected_depth\":")!=std::string::npos);
  CHECK(text.find("\"projected_diameter\":")!=std::string::npos);
  CHECK(text.find("\"intersects_exact_frustum\":")!=std::string::npos);
  CHECK(text.find("\"instantaneous_zone\":")!=std::string::npos);
  CHECK(text.find("\"effective_target\":")!=std::string::npos);
  CHECK((text.find("\"decision\":\"keep\"")!=std::string::npos||
         text.find("\"decision\":\"split\"")!=std::string::npos||
         text.find("\"decision\":\"merge\"")!=std::string::npos));
}

TEST_CASE("headless CPU camera benchmark covers every deterministic motion path") {
  const auto run=[](std::string_view script="benchmark-cpu-camera-paths") {
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
    CHECK(errors.str().empty());
    return output.str();
  };
  const auto first=run();
  const auto second=run();
  const auto persistent=run(
      "set-update-scheduler=persistent-split-merge-queues,"
      "benchmark-cpu-camera-paths");
  const auto persistent_second=run(
      "set-update-scheduler=persistent-split-merge-queues,"
      "benchmark-cpu-camera-paths");
  constexpr std::array paths{
      "stationary","slow-orbit","rapid-orbit","near-to-far",
      "far-to-near","teleport","reversal","repeated-pose"};
  std::size_t event_count{};
  std::size_t offset{};
  while((offset=first.find("\"event\":\"cpu_camera_path_benchmark\"",offset))!=
        std::string::npos){
    ++event_count;
    ++offset;
  }
  CHECK(event_count==paths.size());
  const auto event_for=[](const std::string& text,std::string_view path){
    const std::string marker="\"path\":\""+std::string(path)+"\"";
    const auto marker_position=text.find(marker);
    REQUIRE(marker_position!=std::string::npos);
    const auto begin=text.rfind('{',marker_position);
    const auto end=text.find('\n',marker_position);
    REQUIRE(begin!=std::string::npos);
    REQUIRE(end!=std::string::npos);
    return text.substr(begin,end-begin);
  };
  const auto field=[](const std::string& event,std::string_view key){
    const auto begin=event.find(key);
    REQUIRE(begin!=std::string::npos);
    const auto value=begin+key.size();
    return event.substr(value,event.find_first_of(",}",value)-value);
  };
  const auto number=[&](const std::string& event,std::string_view key){
    return std::stoull(field(event,key));
  };
  for(const auto path:paths){
    const auto first_event=event_for(first,path);
    const auto second_event=event_for(second,path);
    const auto persistent_event=event_for(persistent,path);
    const auto persistent_second_event=event_for(persistent_second,path);
    CHECK(first_event.find("\"shape\":\"perlin-terrain\"")!=std::string::npos);
    CHECK(first_event.find("\"valid\":true")!=std::string::npos);
    CHECK(first_event.find("\"upload_backend\":\"host-mirror\"")!=std::string::npos);
    for(const auto key:{"\"turn_readiness\":","\"visible_error_max\":",
                        "\"visible_error_p95\":","\"visible_error_p99\":",
                        "\"active_owner_cv\":","\"surface_triangle_cv\":"})
      CHECK(std::stod(field(first_event,key))>=0.0);
    CHECK(std::stod(field(first_event,"\"turn_readiness\":"))<=1.0);
    CHECK(number(first_event,"\"minimum_active_owners\":")<=
          number(first_event,"\"maximum_active_owners\":"));
    CHECK(number(first_event,"\"minimum_surface_triangles\":")<=
          number(first_event,"\"maximum_surface_triangles\":"));
    for(const auto key:{"\"adaptation_ms\":","\"scene_preparation_ms\":",
                        "\"scene_statistics_ms\":","\"scene_geometry_ms\":",
                        "\"upload_ms\":","\"publish_commit_ms\":",
                        "\"publication_ms\":",
                        "\"first_complete_revision_ms\":",
                        "\"final_convergence_ms\":"})
      CHECK(field(first_event,key).find('-')==std::string::npos);
    CHECK(std::stod(field(first_event,"\"publication_ms\":"))>=
          std::stod(field(first_event,"\"adaptation_ms\":")));
    CHECK(std::stod(field(first_event,"\"final_convergence_ms\":"))>=
          std::stod(field(first_event,"\"publication_ms\":")));
    const bool first_revision_observed=
        field(first_event,"\"first_complete_revision_observed\":")=="true";
    CHECK(first_revision_observed==
          (number(first_event,"\"published_revisions\":")>0U));
    CHECK(first_revision_observed==
          (number(first_event,"\"first_complete_revision_update\":")>0U));
    if(first_revision_observed){
      CHECK(std::stod(field(first_event,"\"first_complete_revision_ms\":"))>0.0);
      CHECK(std::stod(field(first_event,"\"final_convergence_ms\":"))>=
            std::stod(field(first_event,"\"first_complete_revision_ms\":")));
      CHECK(number(first_event,"\"first_complete_revision_update\":")<=
            number(first_event,"\"updates\":"));
    }
    CHECK(number(first_event,"\"active_logical_owners\":")<=
          number(first_event,"\"resident_logical_owners\":"));
    CHECK(number(first_event,"\"requested_splits\":")>=
          number(first_event,"\"admissible_splits\":"));
    CHECK(number(first_event,"\"requested_merges\":")>=
          number(first_event,"\"admissible_merges\":"));
    CHECK(number(first_event,"\"mesh_snapshot_copied_bytes\":")>0U);
    CHECK(number(first_event,"\"copied_bytes\":")==
          number(first_event,"\"mesh_snapshot_copied_bytes\":")+
          number(first_event,"\"uploaded_bytes\":"));
    CHECK(number(first_event,"\"uploaded_bytes\":")>=
          number(first_event,"\"generated_surface_bytes\":"));
    CHECK(first_event.find("\"exact_field_evaluations\":")!=std::string::npos);
    CHECK(first_event.find("\"dirty_owners\":")!=std::string::npos);
    CHECK(first_event.find("\"rejected_split_operations\":")!=std::string::npos);
    CHECK(first_event.find("\"rejected_merge_operations\":")!=std::string::npos);
    for(const auto kind:{"splits","merges"}){
      const auto requested=number(first_event,
          "\"requested_"+std::string(kind)+"\":");
      const auto admissible=number(first_event,
          "\"admissible_"+std::string(kind)+"\":");
      const auto committed=number(first_event,
          "\"committed_"+std::string(kind)+"\":");
      const auto rejected=number(first_event,
          "\"rejected_"+std::string(kind==std::string_view{"splits"}
              ?"split":"merge")+"_operations\":");
      const auto stale=number(first_event,
          "\"stale_"+std::string(kind==std::string_view{"splits"}
              ?"split":"merge")+"_operations\":");
      const auto expanded=number(first_event,
          "\"conformity_expanded_"+std::string(kind)+"\":");
      const auto conformity_rejected=number(first_event,
          "\"conformity_rejected_"+std::string(kind)+"\":");
      const auto deferred=number(first_event,
          "\"deferred_"+std::string(kind)+"\":");
      CHECK(requested==admissible+conformity_rejected+deferred);
      CHECK(committed+rejected+stale==
            admissible+expanded+conformity_rejected);
    }
    CHECK(field(first_event,"\"logical_cut_hash\":")==
          field(second_event,"\"logical_cut_hash\":"));
    CHECK(field(first_event,"\"conforming_volume_hash\":")==
          field(second_event,"\"conforming_volume_hash\":"));
    CHECK(field(persistent_event,"\"logical_cut_hash\":")==
          field(persistent_second_event,"\"logical_cut_hash\":"));
    CHECK(field(persistent_event,"\"conforming_volume_hash\":")==
          field(persistent_second_event,"\"conforming_volume_hash\":"));
    CHECK(field(persistent_event,"\"update_scheduler\":")==
          "\"persistent-split-merge-queues\"");
    CHECK(number(persistent_event,"\"scheduler_candidates_avoided\":")>0U);
  }
  const auto stationary=event_for(first,"stationary");
  CHECK(field(stationary,"\"updates\":")==field(stationary,"\"zero_work_updates\":"));
  CHECK(field(stationary,"\"published_revisions\":")=="0");
  CHECK(field(stationary,"\"first_complete_revision_observed\":")=="false");
  CHECK(field(stationary,"\"first_complete_revision_update\":")=="0");
  CHECK(std::stod(field(stationary,"\"first_complete_revision_ms\":"))==0.0);
  CHECK(field(stationary,"\"generated_surface_bytes\":")=="0");
  CHECK(field(stationary,"\"uploaded_bytes\":")=="0");
  CHECK(field(stationary,"\"dirty_owners\":")=="0");
  const auto repeated=event_for(first,"repeated-pose");
  CHECK(std::stoul(field(repeated,"\"zero_work_updates\":"))>=1U);
  CHECK(std::stoul(field(repeated,"\"published_revisions\":"))>=1U);
  CHECK(std::stod(field(repeated,"\"turn_readiness\":"))>0.0);
  CHECK(std::stod(field(repeated,"\"turn_readiness\":"))<1.0);
  CHECK(field(repeated,"\"first_complete_revision_observed\":")=="true");
  CHECK(field(repeated,"\"first_complete_revision_update\":")=="1");
  CHECK(number(event_for(persistent,"slow-orbit"),
               "\"scheduler_fallbacks\":")==0U);
  CHECK(number(event_for(persistent,"teleport"),
               "\"scheduler_fallbacks\":")>0U);
  CHECK(std::stoull(field(repeated,"\"generated_surface_bytes\":"))>0U);
  CHECK(std::stoul(field(repeated,"\"uploaded_bytes\":"))>0U);
  CHECK(std::stoull(field(repeated,"\"dirty_owners\":"))>0U);
}

TEST_CASE("headless surface patch benchmark proves locality and exact output") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "benchmark-cpu-surface-patches=6",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  constexpr std::array paths{
      "stationary","slow-orbit","rapid-orbit","near-to-far",
      "far-to-near","teleport","reversal","repeated-pose"};
  constexpr std::array local_methods{
      "marching-tetrahedra","lattice-cleaving","dual-contouring",
      "four-hexahedra","mixed-depth-dual"};
  const auto event_for=[&](std::string_view path,std::string_view method){
    const std::string marker="\"path\":\""+std::string(path)+
        "\",\"method\":\""+std::string(method)+"\"";
    const auto marker_position=text.find(marker);
    REQUIRE(marker_position!=std::string::npos);
    const auto begin=text.rfind('{',marker_position);
    const auto end=text.find('\n',marker_position);
    REQUIRE(begin!=std::string::npos);
    REQUIRE(end!=std::string::npos);
    return text.substr(begin,end-begin);
  };
  const auto field=[](const std::string& event,std::string_view key){
    const auto begin=event.find(key);
    REQUIRE(begin!=std::string::npos);
    const auto value=begin+key.size();
    return event.substr(value,event.find_first_of(",}",value)-value);
  };
  for(const auto path:paths){
    for(const auto method:local_methods){
      const auto event=event_for(path,method);
      const auto revisions=std::stoull(field(event,"\"revisions\":"));
      CHECK(event.find("\"valid\":true")!=std::string::npos);
      CHECK(revisions>0U);
      CHECK(std::stoull(field(event,"\"exact_matches\":"))==revisions);
      CHECK(std::stoull(field(event,"\"full_rebuilds\":"))==1U);
      CHECK(std::stoull(field(event,"\"global_fallbacks\":"))==0U);
      CHECK(std::stod(field(event,"\"patch_update_ms\":"))>=0.0);
      CHECK(std::stod(field(event,"\"monolithic_reference_ms\":"))>=0.0);
    }
    const auto fallback=event_for(path,"surface-optimization");
    const auto revisions=std::stoull(field(fallback,"\"revisions\":"));
    CHECK(std::stoull(field(fallback,"\"exact_matches\":"))==revisions);
    CHECK(std::stoull(field(fallback,"\"global_fallbacks\":"))==revisions);
  }

  std::ostringstream invalid_output,invalid_errors;
  CHECK(tetra_viewer::run_script(
      "benchmark-cpu-surface-patches=33",invalid_output,invalid_errors)==2);
  CHECK(invalid_errors.str().find("depth outside the supported range")!=
        std::string::npos);
}

TEST_CASE("surface quality metrics normalize face normals and reject coarse grids") {
  tetra_viewer::PreparedScene scene;
  const auto vertex=[](tetra::Vec3 point){
    tetra_viewer::SceneVertex result{};
    result.position[0]=static_cast<float>(point.x);
    result.position[1]=static_cast<float>(point.y);
    result.position[2]=static_cast<float>(point.z);
    return result;
  };
  // This small tangent triangle has an area normal of length 0.04. Passing
  // that unnormalised vector to acos would report about 88 degrees.
  scene.triangle_vertices={
      vertex({0.85,0.4,0.4}),vertex({0.85,0.6,0.4}),
      vertex({0.85,0.5,0.7})};
  const auto quality=tetra_viewer::evaluate_surface_quality(
      scene,tetra::Sphere{},8U);
  CHECK(quality.valid);
  CHECK(quality.triangle_count==1U);
  CHECK(quality.degenerate_triangle_count==0U);
  CHECK(quality.implicit_reference_samples>0U);
  CHECK(quality.mean_normal_error_degrees==doctest::Approx(0.0).scale(1.0));
  CHECK(quality.maximum_normal_error_degrees==doctest::Approx(0.0).scale(1.0));
  CHECK(std::isfinite(quality.sampled_hausdorff_distance));
  CHECK(std::isfinite(quality.mean_triangle_edge_aspect_ratio));
  CHECK(quality.mean_triangle_edge_aspect_ratio>1.0);
  CHECK_THROWS_AS(static_cast<void>(tetra_viewer::evaluate_surface_quality(
      scene,tetra::Sphere{},1U)),std::invalid_argument);
}

TEST_CASE("four-hexahedra quality benchmark covers a deterministic matrix") {
  const auto run=[] {
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script(
        "benchmark-cpu-four-hexahedra-quality=3:4",output,errors)==0);
    CHECK(errors.str().empty());
    return output.str();
  };
  const auto first=run(),second=run();
  const auto rows=[](const std::string& text){
    std::vector<std::string> result;
    std::istringstream lines(text);
    for(std::string line;std::getline(lines,line);)
      if(line.find("\"event\":\"cpu_four_hexahedra_quality_benchmark\"")!=
         std::string::npos)
        result.push_back(std::move(line));
    return result;
  };
  const auto first_rows=rows(first),second_rows=rows(second);
  REQUIRE(first_rows.size()==25U);
  REQUIRE(second_rows.size()==first_rows.size());
  constexpr std::array<std::string_view,4> timing_fields{
      "\"cold_patch_update_ms\":","\"cold_end_to_end_update_ms\":",
      "\"update_patch_ms\":","\"end_to_end_update_ms\":"};
  const auto without_timings=[&](std::string row){
    for(const auto key:timing_fields){
      const auto begin=row.find(key);
      REQUIRE(begin!=std::string::npos);
      const auto value=begin+key.size();
      row.replace(value,row.find_first_of(",}",value)-value,"<timing>");
    }
    return row;
  };
  std::set<std::string> shapes,methods;
  for(std::size_t index=0;index<first_rows.size();++index){
    const auto& row=first_rows[index];
    CHECK(row.find("\"valid\":true")!=std::string::npos);
    const auto field=[&](std::string_view key){
      const auto begin=row.find(key);
      REQUIRE(begin!=std::string::npos);
      const auto value=begin+key.size();
      return row.substr(value,row.find_first_of(",}",value)-value);
    };
    shapes.insert(field("\"shape\":"));
    methods.insert(field("\"method\":"));
    CHECK(without_timings(row)==without_timings(second_rows[index]));
  }
  CHECK(shapes.size()==5U);
  CHECK(methods.size()==5U);
  CHECK(methods.contains("\"four-hexahedra\""));

  std::ostringstream deep_output,deep_errors;
  REQUIRE(tetra_viewer::run_script(
      "benchmark-cpu-four-hexahedra-quality=10:2",
      deep_output,deep_errors)==0);
  CHECK(deep_errors.str().empty());
  CHECK(rows(deep_output.str()).size()==25U);

  for(const auto command:{"benchmark-cpu-four-hexahedra-quality=33:4",
                          "benchmark-cpu-four-hexahedra-quality=3:1",
                          "benchmark-cpu-four-hexahedra-quality=nope"}){
    std::ostringstream output,errors;
    CHECK(tetra_viewer::run_script(command,output,errors)==2);
    CHECK(errors.str().find("parameters outside the supported range")!=
          std::string::npos);
  }
}

TEST_CASE("mixed-depth dual benchmark compares required reference methods") {
  const auto run=[](std::string_view command){
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script(command,output,errors)==0);
    CHECK(errors.str().empty());
    return output.str();
  };
  const auto rows=[](const std::string& text){
    std::vector<std::string> result;
    std::istringstream lines(text);
    for(std::string line;std::getline(lines,line);)
      if(line.find("\"event\":\"cpu_mixed_depth_dual_benchmark\"")!=
         std::string::npos)
        result.push_back(std::move(line));
    return result;
  };
  const auto first_rows=rows(run("benchmark-cpu-mixed-depth-dual=3:4"));
  const auto second_rows=rows(run("benchmark-cpu-mixed-depth-dual=3:4"));
  REQUIRE(first_rows.size()==20U);
  REQUIRE(second_rows.size()==first_rows.size());
  constexpr std::array<std::string_view,4> timing_fields{
      "\"cold_patch_update_ms\":","\"cold_end_to_end_update_ms\":",
      "\"update_patch_ms\":","\"end_to_end_update_ms\":"};
  const auto without_timings=[&](std::string row){
    for(const auto key:timing_fields){
      const auto begin=row.find(key);
      REQUIRE(begin!=std::string::npos);
      const auto value=begin+key.size();
      row.replace(value,row.find_first_of(",}",value)-value,"<timing>");
    }
    return row;
  };
  std::set<std::string> shapes,methods;
  for(std::size_t index=0;index<first_rows.size();++index){
    const auto& row=first_rows[index];
    CHECK(row.find("\"valid\":true")!=std::string::npos);
    const auto field=[&](std::string_view key){
      const auto begin=row.find(key);
      REQUIRE(begin!=std::string::npos);
      const auto value=begin+key.size();
      return row.substr(value,row.find_first_of(",}",value)-value);
    };
    shapes.insert(field("\"shape\":"));
    methods.insert(field("\"method\":"));
    CHECK(without_timings(row)==without_timings(second_rows[index]));
    if(field("\"method\":")=="\"mixed-depth-dual\""){
      CHECK(std::stoull(field("\"cold_field_samples\":"))>0U);
      CHECK(std::stoull(field("\"update_field_samples\":"))>0U);
      CHECK(field("\"patchable\":")=="true");
    }
  }
  CHECK(shapes.size()==5U);
  CHECK(methods==std::set<std::string>{
      "\"four-hexahedra\"","\"full-tetrahedra\"",
      "\"marching-tetrahedra\"","\"mixed-depth-dual\""});
  CHECK(rows(run("benchmark-cpu-mixed-depth-dual=10:2")).size()==20U);

  for(const auto command:{"benchmark-cpu-mixed-depth-dual=33:4",
                          "benchmark-cpu-mixed-depth-dual=3:1",
                          "benchmark-cpu-mixed-depth-dual=nope"}){
    std::ostringstream output,errors;
    CHECK(tetra_viewer::run_script(command,output,errors)==2);
    CHECK(errors.str().find("parameters outside the supported range")!=
          std::string::npos);
  }
}

TEST_CASE("headless draw chunk benchmark matches direct packing on every path") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "benchmark-cpu-draw-chunks=6",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  constexpr std::array paths{
      "stationary","slow-orbit","rapid-orbit","near-to-far",
      "far-to-near","teleport","reversal","repeated-pose"};
  constexpr std::array methods{
      "marching-tetrahedra","lattice-cleaving","dual-contouring",
      "four-hexahedra","mixed-depth-dual"};
  const auto event_for=[&](std::string_view path,std::string_view method,
                           std::string_view strategy="fixed-capacity"){
    const std::string marker="\"path\":\""+std::string(path)+
        "\",\"method\":\""+std::string(method)+
        "\",\"strategy\":\""+std::string(strategy)+"\"";
    const auto marker_position=text.find(marker);
    REQUIRE(marker_position!=std::string::npos);
    const auto begin=text.rfind('{',marker_position);
    const auto end=text.find('\n',marker_position);
    REQUIRE(begin!=std::string::npos);
    REQUIRE(end!=std::string::npos);
    return text.substr(begin,end-begin);
  };
  const auto field=[](const std::string& event,std::string_view key){
    const auto begin=event.find(key);
    REQUIRE(begin!=std::string::npos);
    const auto value=begin+key.size();
    return event.substr(value,event.find_first_of(",}",value)-value);
  };
  std::size_t total_local_repacks{};
  std::size_t total_hybrid_large_patches{};
  for(const auto path:paths){
    for(const auto method:methods){
      const auto event=event_for(path,method);
      const auto monolithic=event_for(path,method,"direct-monolithic");
      const auto hybrid=event_for(path,method,"hybrid-large-patches");
      const auto triangles=std::stoull(field(event,"\"triangles\":"));
      const auto chunks=std::stoull(field(event,"\"active_chunks\":"));
      const auto revisions=std::stoull(field(event,"\"revisions\":"));
      const auto full_pack_bytes=
          std::stoull(field(event,"\"full_pack_bytes\":"));
      const auto copied_bytes=std::stoull(field(event,"\"copied_bytes\":"));
      const auto full_host_stage_bytes=
          std::stoull(field(event,"\"full_host_stage_bytes\":"));
      const auto host_staged_bytes=
          std::stoull(field(event,"\"host_staged_bytes\":"));
      const auto full_device_upload_bytes=
          std::stoull(field(event,"\"full_device_upload_bytes\":"));
      const auto device_uploaded_bytes=
          std::stoull(field(event,"\"device_uploaded_bytes\":"));
      CHECK(event.find("\"byte_match\":true")!=std::string::npos);
      CHECK(event.find("\"layout_valid\":true")!=std::string::npos);
      CHECK(event.find("\"host_byte_match\":true")!=std::string::npos);
      CHECK(event.find("\"device_byte_match\":true")!=std::string::npos);
      CHECK(event.find("\"exact\":true")!=std::string::npos);
      CHECK(monolithic.find("\"exact\":true")!=std::string::npos);
      CHECK(hybrid.find("\"exact\":true")!=std::string::npos);
      CHECK(field(event,"\"chunk_capacity\":")=="256");
      CHECK(field(event,"\"large_patch_threshold\":")=="16");
      CHECK(field(monolithic,"\"chunk_capacity\":")=="0");
      CHECK(field(monolithic,"\"draw_calls\":")=="1");
      CHECK(field(monolithic,"\"occupancy\":")=="1.000000");
      CHECK(field(hybrid,"\"chunk_capacity\":")=="256");
      CHECK(field(hybrid,"\"large_patch_threshold\":")=="16");
      CHECK(field(monolithic,"\"triangle_hash\":")==
            field(event,"\"triangle_hash\":"));
      CHECK(field(hybrid,"\"triangle_hash\":")==
            field(event,"\"triangle_hash\":"));
      CHECK(field(monolithic,"\"wire_edge_hash\":")==
            field(event,"\"wire_edge_hash\":"));
      CHECK(field(hybrid,"\"wire_edge_hash\":")==
            field(event,"\"wire_edge_hash\":"));
      CHECK(triangles>0U);
      CHECK(chunks>0U);
      CHECK(std::stoull(field(event,"\"retained_slots\":"))>=chunks);
      CHECK(std::stoull(field(event,"\"allocated_slots\":"))>=chunks);
      CHECK(std::stoull(field(event,"\"global_compactions\":"))>=1U);
      CHECK(std::stoull(field(event,"\"exact_matches\":"))==revisions);
      CHECK(revisions>1U);
      CHECK(std::stoull(field(event,"\"draw_calls\":"))==chunks);
      CHECK(full_pack_bytes>=revisions*triangles*sizeof(tetra::Triangle)/2U);
      CHECK(copied_bytes<full_pack_bytes);
      CHECK(std::stoull(field(event,"\"host_publications\":"))==revisions);
      CHECK(host_staged_bytes<full_host_stage_bytes);
      CHECK(field(event,"\"host_staged_wire_bytes\":")=="0");
      CHECK(std::stoull(field(event,"\"host_aliased_wire_bytes\":"))==
            host_staged_bytes);
      CHECK(std::stoull(field(event,"\"host_retained_slots\":"))>=chunks);
      CHECK(std::stoull(field(event,"\"host_dirty_ranges\":"))>0U);
      CHECK(std::stoull(field(event,"\"host_reused_ranges\":"))>0U);
      CHECK(std::stoull(field(event,"\"device_publications\":"))==revisions);
      CHECK(full_device_upload_bytes==full_host_stage_bytes);
      CHECK(device_uploaded_bytes<full_device_upload_bytes);
      CHECK(std::stoull(field(monolithic,"\"copied_bytes\":"))==
            std::stoull(field(monolithic,"\"full_pack_bytes\":")));
      CHECK(std::stoull(field(monolithic,"\"device_uploaded_bytes\":"))==
            std::stoull(field(monolithic,"\"full_device_upload_bytes\":")));
      CHECK(copied_bytes<
            std::stoull(field(monolithic,"\"copied_bytes\":")));
      CHECK(device_uploaded_bytes<
            std::stoull(field(monolithic,"\"device_uploaded_bytes\":")));
      CHECK(std::stoull(field(event,"\"device_upload_ranges\":"))>0U);
      CHECK(std::stoull(field(event,"\"device_reused_ranges\":"))>0U);
      CHECK(std::stoull(field(event,"\"device_draw_calls\":"))>=revisions);
      total_local_repacks+=
          std::stoull(field(event,"\"local_repacks\":"));
      total_hybrid_large_patches+=
          std::stoull(field(hybrid,"\"large_patches\":"));
      CHECK(std::stod(field(event,"\"occupancy\":"))>0.0);
      CHECK(std::stod(field(event,"\"occupancy\":"))<=1.0);
      CHECK(std::stod(field(event,"\"direct_pack_ms\":"))>=0.0);
      CHECK(std::stod(field(event,"\"chunk_pack_ms\":"))>=0.0);
      CHECK(std::stod(field(event,"\"host_stage_ms\":"))>=0.0);
    }
  }
  CHECK(total_local_repacks>0U);
  CHECK(total_hybrid_large_patches>0U);
  const auto selection_position=text.find(
      "\"event\":\"cpu_draw_strategy_selection\"");
  REQUIRE(selection_position!=std::string::npos);
  const auto selection_end=text.find('\n',selection_position);
  REQUIRE(selection_end!=std::string::npos);
  const auto selection=text.substr(
      selection_position,selection_end-selection_position);
  CHECK(selection.find("\"selected\":\"fixed-capacity\"")!=
        std::string::npos);
  CHECK(selection.find("\"selection_applicable\":false")!=
        std::string::npos);
  CHECK(selection.find("\"fixed_qualified\":true")!=std::string::npos);
  CHECK(std::stoull(field(selection,"\"fixed_uploaded_bytes\":"))<
        std::stoull(field(selection,"\"fixed_full_upload_bytes\":")));

  std::ostringstream invalid_output,invalid_errors;
  CHECK(tetra_viewer::run_script(
      "benchmark-cpu-draw-chunks=33",invalid_output,invalid_errors)==2);
  CHECK(invalid_errors.str().find("depth outside the supported range")!=
        std::string::npos);
  invalid_output.str({});
  invalid_errors.str({});
  CHECK(tetra_viewer::run_script(
      "benchmark-cpu-draw-chunks=6:0",invalid_output,invalid_errors)==2);
  CHECK(invalid_errors.str().find(
            "hybrid threshold outside the supported range")!=
        std::string::npos);
}

TEST_CASE("headless shape hash matrix covers every shape and camera path deterministically") {
  const auto run=[] {
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script(
        "benchmark-cpu-shape-hashes=all:6",output,errors)==0);
    CHECK(errors.str().empty());
    const auto initialized_end=output.str().find('\n');
    REQUIRE(initialized_end!=std::string::npos);
    return output.str().substr(initialized_end+1U);
  };
  const auto first=run();
  const auto second=run();
  CHECK(first==second);
  constexpr std::array paths{
      "stationary","slow-orbit","rapid-orbit","near-to-far",
      "far-to-near","teleport","reversal","repeated-pose"};
  std::size_t events{};
  std::size_t offset{};
  while((offset=first.find("\"event\":\"cpu_shape_path_hash\"",offset))!=
        std::string::npos){++events;++offset;}
  CHECK(events==tetra::implicit_shape_kinds.size()*paths.size());
  for(const auto shape:tetra::implicit_shape_kinds){
    for(const auto path:paths){
      const std::string marker="\"shape\":\""+
          std::string(tetra::implicit_shape_key(shape))+"\",\"path\":\""+path+"\"";
      const auto begin=first.find(marker);
      REQUIRE(begin!=std::string::npos);
      const auto end=first.find('\n',begin);
      REQUIRE(end!=std::string::npos);
      const auto event=first.substr(begin,end-begin);
      CHECK(event.find("\"maximum_depth\":6")!=std::string::npos);
      CHECK(event.find("\"valid\":true")!=std::string::npos);
      CHECK(event.find("\"logical_cut_hash\":")!=std::string::npos);
      CHECK(event.find("\"conforming_volume_hash\":")!=std::string::npos);
      CHECK(event.find("\"surface_triangle_hash\":")!=std::string::npos);
      CHECK(event.find("\"surface_edge_hash\":")!=std::string::npos);
      CHECK(event.find("\"surface_triangles\":0") == std::string::npos);
      CHECK(event.find("\"surface_edges\":0") == std::string::npos);
    }
  }
}

TEST_CASE("headless shape hash benchmark rejects unknown shapes and depths") {
  for(const auto command:{"benchmark-cpu-shape-hashes=unknown",
                          "benchmark-cpu-shape-hashes=all:33",
                          "benchmark-cpu-shape-hashes=sphere:nope"}){
    std::ostringstream output,errors;
    CHECK(tetra_viewer::run_script(command,output,errors)==2);
    CHECK_FALSE(errors.str().empty());
  }
}

TEST_CASE("headless camera stress path is deterministic and conforming") {
  const auto run=[] {
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script(
        "set-shape=perlin-terrain,set-maximum-depth=3,stress-camera=1000",
        output,errors)==0);
    CHECK(errors.str().empty());
    CHECK(output.str().find("\"event\":\"camera_stress\",\"updates\":1000")!=
          std::string::npos);
    return output.str().substr(output.str().rfind('{'));
  };
  const auto first=run();
  const auto second=run();
  const auto field=[](const std::string& text,std::string_view key){
    const auto begin=text.find(key);
    REQUIRE(begin!=std::string::npos);
    const auto value=begin+key.size();
    return text.substr(value,text.find_first_of(",}",value)-value);
  };
  CHECK(field(first,"\"logical_cut_hash\":")==field(second,"\"logical_cut_hash\":"));
  CHECK(field(first,"\"conforming_volume_hash\":")==
        field(second,"\"conforming_volume_hash\":"));
}

TEST_CASE("deterministic randomized camera budgets and checkpoint copies remain identical") {
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  auto first=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::AdaptationPlanningCache first_cache;
  std::optional<tetra::TetMesh> restored;
  std::optional<tetra::AdaptationPlanningCache> restored_cache;
  std::uint64_t random=0x6a09e667f3bcc909ULL;
  const auto next=[&]{random=random*6364136223846793005ULL+1442695040888963407ULL;
                      return random;};
  tetra::Camera final_camera;
  tetra::AdaptationConfiguration final_configuration;
  unsigned int final_depth{};
  for(std::size_t update=0;update<96U;++update){
    const double x=static_cast<double>(next()%2001U)/1000.0-0.5;
    const double y=static_cast<double>(next()%1001U)/1000.0+0.2;
    const double z=static_cast<double>(next()%2001U)/1000.0-0.5;
    tetra::Camera camera;
    camera.position={x,y,z};
    const auto direction=terrain.centre-camera.position;
    const double length=std::sqrt(direction.x*direction.x+direction.y*direction.y+
                                  direction.z*direction.z);
    camera.forward=length>1.0e-12?direction/length:tetra::Vec3{0.0,0.0,-1.0};
    tetra::AdaptationConfiguration configuration;
    configuration.operation_budget=1U+static_cast<std::uint32_t>(next()%64U);
    const unsigned int maximum_depth=(next()&1U)!=0U?6U:9U;
    const auto apply=[&](tetra::TetMesh& mesh,tetra::AdaptationPlanningCache& cache){
      const auto owners_before=mesh.logical_cut().owners;
      const auto revision_before=mesh.revision();
      const auto resident_revision_before=mesh.resident_revision();
      const auto plan=tetra::plan_adaptation(
          mesh,terrain,camera,32.0,maximum_depth,configuration,11,&cache);
      const auto result=tetra::commit_adaptation(mesh,plan,configuration,11);
      INFO("update="<<update<<" depth="<<maximum_depth<<" budget="
           <<configuration.operation_budget<<" status="
           <<static_cast<unsigned int>(result.status)<<" commands="<<plan.commands.size()
           <<" splits="<<plan.planned_splits<<" merges="<<plan.planned_merges
           <<" over_budget="<<plan.over_budget<<" supported="<<plan.supported);
      REQUIRE(result.status!=tetra::AdaptationCommitStatus::stale_plan);
      if(result.status==tetra::AdaptationCommitStatus::rejected){
        CHECK(mesh.logical_cut().owners==owners_before);
        CHECK(mesh.revision()==revision_before);
        CHECK(mesh.resident_revision()==resident_revision_before);
      }
      CHECK(mesh.has_positive_active_volumes());
      CHECK(mesh.has_conforming_active_faces());
      return result.status;
    };
    const auto first_status=apply(first,first_cache);
    if(update==31U){restored=first;restored_cache=first_cache;}
    if(restored&&update>31U){
      CHECK(apply(*restored,*restored_cache)==first_status);
      CHECK(restored->logical_cut().owners==first.logical_cut().owners);
      CHECK(std::ranges::equal(restored->conforming_volume().addresses(),
                               first.conforming_volume().addresses()));
    }
    final_camera=camera;final_configuration=configuration;final_depth=maximum_depth;
  }
  REQUIRE(restored.has_value());REQUIRE(restored_cache.has_value());
  for(std::size_t transaction=0;transaction<256U;++transaction){
    const auto a=tetra::adapt_to_surface(first,terrain,final_camera,32.0,final_depth,
                                         final_configuration,11,&first_cache);
    const auto b=tetra::adapt_to_surface(*restored,terrain,final_camera,32.0,final_depth,
                                         final_configuration,11,&*restored_cache);
    CHECK(a.status==b.status);
    CHECK(first.logical_cut().owners==restored->logical_cut().owners);
    if(a.status==tetra::AdaptationCommitStatus::no_change)break;
    REQUIRE(a.status==tetra::AdaptationCommitStatus::committed);
  }
  CHECK(std::ranges::equal(first.conforming_volume().addresses(),
                           restored->conforming_volume().addresses()));
}

TEST_CASE("headless method selection resets to the registered 24-tet hierarchy") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-maximum-depth=3,set-method=maubach-halfedge-24,validate,stats", output, errors) == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("\"command\":\"set-method=maubach-halfedge-24\"") != std::string::npos);
  CHECK(text.find("\"subdivision_method\":\"maubach-halfedge-24\"") != std::string::npos);
  CHECK(text.find("\"event\":\"validation\",\"valid\":true") != std::string::npos);
}

TEST_CASE("headless method selection supports every registered hierarchy") {
  for(const auto method:tetra::subdivision_methods){
    std::ostringstream output;
    std::ostringstream errors;
    const std::string key(tetra::subdivision_method_key(method));
    CHECK(tetra_viewer::run_script("set-maximum-depth=3,set-method="+key+",validate,stats",output,errors)==0);
    CHECK(errors.str().empty());
    CHECK(output.str().find("\"subdivision_method\":\""+key+"\"")!=std::string::npos);
    CHECK(output.str().find("\"event\":\"validation\",\"valid\":true")!=std::string::npos);
  }
}

TEST_CASE("headless longest-edge method converges to a valid face-to-face mesh") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-maximum-depth=12,set-method=longest-edge,validate,prepare-scene", output, errors) == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("\"subdivision_method\":\"longest-edge\"") != std::string::npos);
  CHECK(text.find("\"event\":\"validation\",\"valid\":true") != std::string::npos);
}

TEST_CASE("headless material-rule selection is independent of subdivision") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-method=maubach-halfedge-24,set-material-rule=centroid,prepare-scene", output, errors) == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("\"subdivision_method\":\"maubach-halfedge-24\"") != std::string::npos);
  CHECK(text.find("\"material_rule\":\"centroid\"") != std::string::npos);
  CHECK(text.find("\"selected\":") != std::string::npos);
}

TEST_CASE("headless surface-method selection prepares and renders the tetrahedral layer") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-maximum-depth=3,set-volume-connection=hierarchy-cells,"
      "set-surface-method=tetrahedral-layer,prepare-scene,stats", output, errors) == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("\"surface_method\":\"tetrahedral-layer\"") != std::string::npos);
  CHECK(text.find("\"surface_layer_tetrahedra\":0") == std::string::npos);
}

TEST_CASE("headless surface-method selection prepares the dual contour") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-maximum-depth=3,set-volume-connection=hierarchy-cells,"
      "set-surface-method=dual-contouring,prepare-scene,stats", output, errors) == 0);
  CHECK(errors.str().empty());
  const auto text = output.str();
  CHECK(text.find("\"surface_method\":\"dual-contouring\"") != std::string::npos);
  CHECK(text.find("\"dual_contour_triangles\":0") == std::string::npos);
  CHECK(text.find("\"surface_layer_tetrahedra\":0") != std::string::npos);
}

TEST_CASE("every implicit shape prepares geometry with every surface method") {
  for(const auto kind:tetra::implicit_shape_kinds){
    CAPTURE(tetra::implicit_shape_name(kind));
    std::string script="set-maximum-depth=6,set-volume-connection=hierarchy-cells,set-shape="+
        std::string(tetra::implicit_shape_key(kind));
    for(const auto method:tetra_viewer::surface_methods){
      script+=",set-surface-method="+std::string(tetra_viewer::surface_method_key(method));
      script+=",prepare-scene";
    }
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
    CHECK(errors.str().empty());
    const auto text=output.str();
    CHECK(text.find("\"shape\":\""+std::string(tetra::implicit_shape_key(kind))+"\"")!=
          std::string::npos);
    CHECK(text.find("\"triangle_vertices\":0,")==std::string::npos);
    std::size_t scenes=0,position=0;
    while((position=text.find("\"event\":\"scene_preparation\"",position))!=
          std::string::npos){++scenes;++position;}
    CHECK(scenes==tetra_viewer::surface_methods.size());
  }
}

TEST_CASE("headless shading-model selection supports every diagnostic view") {
  for(const auto model:tetra_viewer::shading_models){
    std::ostringstream output;
    std::ostringstream errors;
    const std::string command="set-shading-model="+std::string(tetra_viewer::shading_model_key(model))+",stats";
    CHECK(tetra_viewer::run_script(command,output,errors)==0);
    CHECK(errors.str().empty());
    CHECK(output.str().find("\"shading_model\":\""+std::string(tetra_viewer::shading_model_key(model))+"\"")!=std::string::npos);
  }
}

TEST_CASE("headless surface controls support anti-aliased wire-only geometry") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-volume-connection=quality-stencils,set-surface-method=dual-contouring,"
      "set-solid-faces=off,set-surface-edges=on,set-x-cut=0.5,prepare-scene",
      output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"solid_faces\":false")!=std::string::npos);
  CHECK(text.find("\"surface_edges\":true")!=std::string::npos);
  CHECK(text.find("\"x_cutaway\":true")!=std::string::npos);
  CHECK(text.find("\"x_cut_position\":0.5")!=std::string::npos);
  CHECK(text.find("\"volume_edges\":true")!=std::string::npos);
  CHECK(text.find("\"solid_volume\":true")!=std::string::npos);
  CHECK(text.find("\"volume_internal_edges\":0")==std::string::npos);
  CHECK(text.find("\"volume_boundary_edges\":0")==std::string::npos);
  CHECK(text.find("\"visible_volume_face_triangles\":0")==std::string::npos);
  CHECK(text.find("\"volume_connection\":\"quality-stencils\"")!=std::string::npos);
  CHECK(text.find("\"connected_volume_tetrahedra\":0")==std::string::npos);
  CHECK(text.find("\"triangle_vertices\":0")==std::string::npos);
}

TEST_CASE("headless TetWeave-inspired solid cutaway optimizes its connected volume") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script(
      "set-maximum-depth=9,set-method=bcc-red-green,"
      "set-surface-method=surface-optimization,set-volume-connection=adaptive-cleaving,"
      "set-solid-volume=on,set-volume-edges=on,set-x-cut=0.5,prepare-scene",
      output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"surface_method\":\"surface-optimization\"")!=std::string::npos);
  CHECK(text.find("\"volume_connection\":\"adaptive-cleaving\"")!=std::string::npos);
  CHECK(text.find("\"optimized_volume_boundary_vertices\":0")==std::string::npos);
  CHECK(text.find("\"connected_volume_tetrahedra\":0")==std::string::npos);
  CHECK(text.find("\"minimum_connected_tet_quality_before\":0.000000000")==std::string::npos);
  CHECK(text.find("\"minimum_connected_tet_quality_after\":0.000000000")==std::string::npos);
}

TEST_CASE("cutaway keeps whole volume tetrahedra on the visible side and distinguishes the material boundary") {
  auto mesh = tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::maubach_diamond);
  for (int pass = 0; pass < 8; ++pass) mesh.refine_all_binary();
  const tetra::Sphere sphere{{0.5, 0.5, 0.5}, 0.48};
  const auto scene = tetra_viewer::prepare_scene(
      mesh, sphere, tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,
      true, false, true, false, true, true, 0.58,
      tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  CHECK(scene.volume_internal_edges > 0);
  CHECK(scene.volume_boundary_edges > 0);
  CHECK(scene.visible_volume_face_triangles > 0);
  // Cut-volume edges use their own screen-space strip buffer. The hierarchy
  // buffer remains hierarchy-only.
  CHECK(scene.hierarchy_line_vertices.empty());
  bool found_internal = false;
  bool found_boundary = false;
  for (const auto& vertex : scene.triangle_vertices) {
    found_internal |= vertex.colour[0] == doctest::Approx(0.18F) &&
                      vertex.colour[1] == doctest::Approx(0.62F);
    found_boundary |= vertex.colour[0] == doctest::Approx(0.94F) &&
                      vertex.colour[1] == doctest::Approx(0.43F);
    if (vertex.diagnostics[0] < -0.5F && vertex.diagnostics[0] > -1.5F)
      CHECK((static_cast<int>(vertex.edge_flags + 0.5F) & 7) == 7);
  }
  CHECK(found_internal);
  CHECK(found_boundary);
  CHECK(std::ranges::any_of(scene.triangle_vertices, [](const tetra_viewer::SceneVertex& vertex) {
    return vertex.diagnostics[0] < 0.0F;
  }));
  bool retained_cell_crosses_plane = false;
  for (const auto& vertex : scene.triangle_vertices) {
    if (vertex.diagnostics[0] >= 0.0F) continue;
    retained_cell_crosses_plane |= vertex.position[0] > 0.58F;
    const bool original_mesh_vertex = std::ranges::any_of(mesh.vertices(), [&vertex](tetra::Vec3 point) {
      return static_cast<float>(point.x) == vertex.position[0] &&
             static_cast<float>(point.y) == vertex.position[1] &&
             static_cast<float>(point.z) == vertex.position[2];
    });
    CHECK(original_mesh_vertex);
  }
  CHECK_FALSE(retained_cell_crosses_plane);
}

TEST_CASE("adaptive mesh cleaving forms a packed conforming surface-to-volume connection") {
  CHECK(tetra_viewer::volume_connection_methods.size()==5);
  CHECK(tetra_viewer::volume_connection_method_key(
      tetra_viewer::VolumeConnectionMethod::hierarchy_cells)=="hierarchy-cells");
  CHECK(tetra_viewer::volume_connection_method_key(
      tetra_viewer::VolumeConnectionMethod::fixed_surface_shell)=="fixed-surface-shell");
  CHECK(tetra_viewer::volume_connection_method_key(
      tetra_viewer::VolumeConnectionMethod::coned_prototype)=="coned-prototype");
  CHECK(tetra_viewer::volume_connection_method_key(
      tetra_viewer::VolumeConnectionMethod::quality_stencils)=="quality-stencils");
  CHECK(tetra_viewer::volume_connection_method_key(
      tetra_viewer::VolumeConnectionMethod::adaptive_cleaving)=="adaptive-cleaving");
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.48};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,9));
  const auto scene=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::marching_tetrahedra,
      tetra_viewer::MaterialRule::all_vertices_inside,
      false,false,true,false,true,true,1.0,
      tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  REQUIRE_FALSE(scene.connected_volume_tetrahedra.empty());
  CHECK(scene.connected_volume_tetrahedra.size()==scene.connected_volume_parents.size());
  CHECK(scene.connected_volume_tetrahedra.size()==scene.connected_volume_boundary.size());
  REQUIRE(scene.connected_volume_vertex_kinds.size()==scene.connected_volume_vertices.size());
  REQUIRE(scene.connected_volume_source_edges.size()==scene.connected_volume_vertices.size());
  REQUIRE(scene.connected_volume_surface_vertices.size()==scene.connected_volume_vertices.size());
  std::size_t intersection_vertices{};
  for(std::size_t vertex=0;vertex<scene.connected_volume_vertices.size();++vertex){
    if(scene.connected_volume_vertex_kinds[vertex]!=
       tetra_viewer::ConnectedVertexKind::surface_intersection)continue;
    ++intersection_vertices;
    CHECK(scene.connected_volume_surface_vertices[vertex]!=0U);
    const auto edge=scene.connected_volume_source_edges[vertex];
    CHECK(edge[0]<mesh.vertices().size());
    CHECK(edge[1]<mesh.vertices().size());
  }
  CHECK(intersection_vertices>0);
  CHECK(scene.connected_volume_vertices.size()>mesh.vertices().size());
  CHECK(std::ranges::any_of(scene.connected_volume_boundary,[](std::uint8_t value){return value==0;}));
  CHECK(std::ranges::any_of(scene.connected_volume_boundary,[](std::uint8_t value){return value!=0;}));
  CHECK(std::ranges::all_of(scene.triangle_vertices,[](const tetra_viewer::SceneVertex& vertex){
    return vertex.diagnostics[0]<-0.5F;
  }));
  const auto edge_key=[](const tetra_viewer::SceneVertex& first,
                         const tetra_viewer::SceneVertex& second){
    std::array<float,3> a{{first.position[0],first.position[1],first.position[2]}};
    std::array<float,3> b{{second.position[0],second.position[1],second.position[2]}};
    if(b<a)std::swap(a,b);
    return std::array<float,6>{{a[0],a[1],a[2],b[0],b[1],b[2]}};
  };
  std::set<std::array<float,6>> triangle_edges,submitted_edges;
  for(std::size_t triangle=0;triangle+2<scene.triangle_vertices.size();triangle+=3){
    const auto& a=scene.triangle_vertices[triangle];
    const auto& b=scene.triangle_vertices[triangle+1];
    const auto& c=scene.triangle_vertices[triangle+2];
    triangle_edges.insert(edge_key(a,b));
    triangle_edges.insert(edge_key(b,c));
    triangle_edges.insert(edge_key(c,a));
  }
  for(std::size_t edge=0;edge+1<scene.surface_line_vertices.size();edge+=2)
    submitted_edges.insert(edge_key(scene.surface_line_vertices[edge],
                                    scene.surface_line_vertices[edge+1]));
  CHECK(submitted_edges==triangle_edges);
  CHECK(std::ranges::any_of(scene.triangle_vertices,[](const tetra_viewer::SceneVertex& vertex){
    return vertex.diagnostics[0]<-1.5F;
  }));
  for(const auto& vertex:scene.triangle_vertices)if(vertex.diagnostics[0]<-1.5F)
    CHECK(std::abs(sphere.signed_distance({vertex.position[0],vertex.position[1],vertex.position[2]}))<1.0e-6);
  std::map<std::array<std::size_t,3>,std::size_t> face_counts;
  constexpr std::array<std::array<std::size_t,3>,4> faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  for(const auto& cell:scene.connected_volume_tetrahedra){
    const auto a=scene.connected_volume_vertices[cell[0]];
    const auto b=scene.connected_volume_vertices[cell[1]];
    const auto c=scene.connected_volume_vertices[cell[2]];
    const auto d=scene.connected_volume_vertices[cell[3]];
    const auto ab=b-a,ac=c-a,ad=d-a;
    const double determinant=ab.x*(ac.y*ad.z-ac.z*ad.y)-
        ab.y*(ac.x*ad.z-ac.z*ad.x)+ab.z*(ac.x*ad.y-ac.y*ad.x);
    CHECK(determinant>0.0);
    for(const auto face:faces){
      std::array<std::size_t,3> key{{cell[face[0]],cell[face[1]],cell[face[2]]}};
      std::sort(key.begin(),key.end());
      ++face_counts[key];
    }
  }
  std::size_t surface_faces{};
  for(const auto& [face,count]:face_counts){
    CHECK(count<=2);
    if(count!=1)continue;
    ++surface_faces;
    for(const auto vertex:face)
      CHECK(std::abs(sphere.signed_distance(scene.connected_volume_vertices[vertex]))<1.0e-8);
  }
  CHECK(surface_faces>0);
  const auto displayed_surface_triangles=static_cast<std::size_t>(std::ranges::count_if(
      scene.triangle_vertices,[](const tetra_viewer::SceneVertex& vertex){
        return vertex.diagnostics[0]<-1.5F;
      })/3);
  CHECK(displayed_surface_triangles==surface_faces);
  CHECK(scene.connected_surface_edges*2==surface_faces*3);
  // The complete wireframe is a unique-edge screen-space ribbon overlay;
  // opaque surface triangles remain the authoritative visibility buffer.
  CHECK_FALSE(scene.surface_line_vertices.empty());
  for(std::size_t index=0;index<scene.triangle_vertices.size();index+=3){
    if(scene.triangle_vertices[index].diagnostics[0]>=-1.5F)continue;
    CHECK(scene.triangle_vertices[index+0].barycentric[0]==doctest::Approx(1.0F));
    CHECK(scene.triangle_vertices[index+1].barycentric[1]==doctest::Approx(1.0F));
    CHECK(scene.triangle_vertices[index+2].barycentric[2]==doctest::Approx(1.0F));
  }
}

TEST_CASE("fixed optimized shell preserves the standalone surface and whole hierarchy core") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.42};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,42.0,9));
  const auto standalone=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  const auto uncut=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      false,false,1.0,tetra_viewer::VolumeConnectionMethod::fixed_surface_shell);
  const auto cut=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,true,false,true,false,
      true,true,0.5,tetra_viewer::VolumeConnectionMethod::fixed_surface_shell);
  REQUIRE(standalone.standalone_surface_hash!=0);
  CHECK(uncut.hybrid_volume_valid);
  CHECK(cut.hybrid_volume_valid);
  CHECK(uncut.hybrid_failed_prisms==0);
  CHECK(uncut.connected_surface_hash==standalone.standalone_surface_hash);
  CHECK(cut.connected_surface_hash==uncut.connected_surface_hash);
  const auto topology=tetra_viewer::validate_connected_complex(uncut,&mesh);
  CHECK(topology.valid);
  CHECK(topology.nonmanifold_faces==0);
  CHECK(topology.unmatched_non_surface_faces==0);
  CHECK(topology.exterior_faces==uncut.hybrid_shell_tetrahedra/3);
  // BCC octasection stores one logical level as three address bits. Closure
  // must never skip a logical grading layer across a shared face.
  CHECK(topology.maximum_adjacent_parent_depth_difference<=3);
  CHECK(uncut.hybrid_shell_tetrahedra>0);
  REQUIRE(uncut.connected_volume_regions.size()==uncut.connected_volume_tetrahedra.size());
  CHECK(std::ranges::any_of(uncut.connected_volume_regions,[](auto region){
    return region==tetra_viewer::ConnectedCellRegion::hierarchy_core;
  }));
  CHECK(std::ranges::any_of(uncut.connected_volume_regions,[](auto region){
    return region==tetra_viewer::ConnectedCellRegion::boundary_connector;
  }));
  CHECK(std::ranges::any_of(uncut.connected_volume_regions,[](auto region){
    return region==tetra_viewer::ConnectedCellRegion::outer_shell;
  }));
  constexpr std::array<std::array<std::size_t,3>,4> face_corners{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  std::map<std::array<std::size_t,3>,std::size_t> face_incidence;
  for(const auto& tet:uncut.connected_volume_tetrahedra){
    std::array<tetra::Vec3,4> points{};
    for(std::size_t corner=0;corner<4;++corner)points[corner]=uncut.connected_volume_vertices[tet[corner]];
    CHECK(tetra_viewer::evaluate_tetrahedron_quality(points).signed_six_volume>0.0);
    for(const auto face:face_corners){
      std::array<std::size_t,3> key{{tet[face[0]],tet[face[1]],tet[face[2]]}};
      std::sort(key.begin(),key.end());++face_incidence[key];
    }
  }
  std::size_t exterior_faces{};
  for(const auto& [face,count]:face_incidence){
    CHECK(count<=2);
    if(count!=1)continue;
    ++exterior_faces;
    for(const auto vertex:face)CHECK(uncut.connected_volume_surface_vertices[vertex]!=0U);
  }
  CHECK(exterior_faces*3==uncut.hybrid_shell_tetrahedra);
  for(std::size_t index=0;index<uncut.connected_volume_tetrahedra.size();++index){
    if(uncut.connected_volume_regions[index]!=tetra_viewer::ConnectedCellRegion::hierarchy_core)continue;
    auto source=mesh.tetrahedron(uncut.connected_volume_parents[index]).vertices;
    auto retained=uncut.connected_volume_tetrahedra[index];
    std::sort(source.begin(),source.end());std::sort(retained.begin(),retained.end());
    for(std::size_t corner=0;corner<4;++corner)CHECK(retained[corner]==source[corner]);
  }
  std::ostringstream output,errors;
  CHECK(tetra_viewer::run_script(
      "set-method=bcc-red-green,set-surface-method=surface-optimization,"
      "set-volume-connection=fixed-surface-shell,validate-volume",output,errors)==0);
  CHECK(errors.str().empty());
  CHECK(output.str().find("\"surface_hash_match\":true")!=std::string::npos);
  std::ostringstream invalid_output,invalid_errors;
  CHECK(tetra_viewer::run_script("set-surface-method=full-tetrahedra,"
                                 "set-volume-connection=fixed-surface-shell",
                                 invalid_output,invalid_errors)==2);
  CHECK(invalid_errors.str().find("requires surface optimization")!=std::string::npos);
}

TEST_CASE("default connected cutaway keeps unit geometric normals through refinement") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra_viewer::default_subdivision_method);
  tetra::Sphere terrain;
  terrain.kind=tetra_viewer::default_implicit_shape;
  // This regression needs sub-cell terrain faces near the unit-domain test
  // camera; the production safe-spawn disc intentionally removes them.
  terrain.terrain.spawn_flat_radius=0.0;
  terrain.terrain.spawn_blend_radius=0.0;
  tetra::Camera camera;
  const auto check_scene=[&] {
    const auto scene=tetra_viewer::prepare_scene(
        mesh,terrain,tetra_viewer::default_surface_method,
        tetra_viewer::MaterialRule::variational_smooth,
        true,false,true,false,true,true,1.0,
        tetra_viewer::default_volume_connection_for_shape(terrain.kind));
    REQUIRE_FALSE(scene.triangle_vertices.empty());
    REQUIRE(scene.triangle_vertices.size()%3U==0U);
    bool found_smooth_variation=false;
    for(std::size_t triangle=0;triangle<scene.triangle_vertices.size();triangle+=3U){
      const auto& first=scene.triangle_vertices[triangle];
      const auto& second=scene.triangle_vertices[triangle+1U];
      const auto& third=scene.triangle_vertices[triangle+2U];
      const tetra::Vec3 a{first.position[0],first.position[1],first.position[2]};
      const tetra::Vec3 b{second.position[0],second.position[1],second.position[2]};
      const tetra::Vec3 c{third.position[0],third.position[1],third.position[2]};
      const auto ab=b-a,ac=c-a;
      const tetra::Vec3 geometric{
          ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,
          ab.x*ac.y-ab.y*ac.x};
      const double geometric_length=std::sqrt(
          geometric.x*geometric.x+geometric.y*geometric.y+
          geometric.z*geometric.z);
      REQUIRE(geometric_length>1.0e-15);
      for(std::size_t corner=0;corner<3U;++corner){
        const auto& vertex=scene.triangle_vertices[triangle+corner];
        const double normal_length=std::sqrt(
            vertex.normal[0]*vertex.normal[0]+vertex.normal[1]*vertex.normal[1]+
            vertex.normal[2]*vertex.normal[2]);
        CHECK(normal_length==doctest::Approx(1.0).epsilon(1.0e-5));
        const double alignment=std::abs(
            vertex.normal[0]*geometric.x+vertex.normal[1]*geometric.y+
            vertex.normal[2]*geometric.z)/geometric_length;
        CHECK(alignment==doctest::Approx(1.0).epsilon(1.0e-5));
        CHECK(vertex.normal[0]==doctest::Approx(first.normal[0]));
        CHECK(vertex.normal[1]==doctest::Approx(first.normal[1]));
        CHECK(vertex.normal[2]==doctest::Approx(first.normal[2]));
        const double smooth_length=std::sqrt(
            vertex.smooth_normal[0]*vertex.smooth_normal[0]+
            vertex.smooth_normal[1]*vertex.smooth_normal[1]+
            vertex.smooth_normal[2]*vertex.smooth_normal[2]);
        CHECK(smooth_length==doctest::Approx(1.0).epsilon(1.0e-5));
        const double normal_delta=
            std::abs(vertex.smooth_normal[0]-vertex.normal[0])+
            std::abs(vertex.smooth_normal[1]-vertex.normal[1])+
            std::abs(vertex.smooth_normal[2]-vertex.normal[2]);
        found_smooth_variation|=normal_delta>1.0e-4;
      }
      if(first.diagnostics[0]<-1.5F){
        const auto analytic=terrain.normal((a+b+c)/3.0);
        CHECK(first.normal[0]*analytic.x+first.normal[1]*analytic.y+
              first.normal[2]*analytic.z>0.0);
      }
    }
    CHECK(found_smooth_variation);
  };

  static_cast<void>(tetra::refine_to_sphere(mesh,terrain,camera,28.0,9U));
  check_scene();
  static_cast<void>(tetra::refine_to_sphere(mesh,terrain,camera,14.0,12U));
  check_scene();

  tetra_viewer::SceneCache cache;
  REQUIRE(cache.update_scene(
      mesh,terrain,0U,tetra_viewer::default_surface_method,
      tetra_viewer::MaterialRule::variational_smooth,
      false,false,true,false,true,true,1.0,
      tetra_viewer::default_volume_connection_for_shape(terrain.kind)));
  for(std::size_t triangle=0;
      triangle+2U<cache.scene().triangle_vertices.size();triangle+=3U){
    const auto* vertices=cache.scene().triangle_vertices.data()+triangle;
    const tetra::Vec3 a{vertices[0].position[0],vertices[0].position[1],
                        vertices[0].position[2]};
    const tetra::Vec3 b{vertices[1].position[0],vertices[1].position[1],
                        vertices[1].position[2]};
    const tetra::Vec3 c{vertices[2].position[0],vertices[2].position[1],
                        vertices[2].position[2]};
    const auto ab=b-a,ac=c-a;
    const tetra::Vec3 area_normal{
        ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,
        ab.x*ac.y-ab.y*ac.x};
    const double area_normal_length=std::sqrt(
        area_normal.x*area_normal.x+area_normal.y*area_normal.y+
        area_normal.z*area_normal.z);
    CHECK(area_normal_length>1.0e-15);
    for(std::size_t corner=0;corner<3U;++corner){
      const double normal_length=std::sqrt(
          vertices[corner].normal[0]*vertices[corner].normal[0]+
          vertices[corner].normal[1]*vertices[corner].normal[1]+
          vertices[corner].normal[2]*vertices[corner].normal[2]);
      CHECK(normal_length==doctest::Approx(1.0).epsilon(1.0e-5));
    }
  }
}

TEST_CASE("fixed optimized shell validates across every packed hierarchy family") {
  const tetra::Sphere sphere{{0.47,0.52,0.49},0.31};
  const tetra::Camera camera{};
  for(const auto method:tetra::subdivision_methods){
    auto mesh=tetra::TetMesh::make_unit_cube(method);
    static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,72.0,6));
    const auto scene=tetra_viewer::prepare_scene(
        mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::all_vertices_inside,false,false,false,false,
        false,false,1.0,tetra_viewer::VolumeConnectionMethod::fixed_surface_shell);
    CAPTURE(tetra::subdivision_method_key(method));
    CHECK(scene.hybrid_volume_valid);
    CHECK(scene.hybrid_failed_prisms==0);
    CHECK(scene.connected_surface_hash==scene.standalone_surface_hash);
  }
}

TEST_CASE("connected hierarchy-core cutaways preserve one authoritative complex") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.48,0.51,0.46},0.36};
  tetra::Camera camera{{1.2,0.8,1.3}};
  const auto direction=sphere.centre-camera.position;
  const double direction_length=std::sqrt(
      direction.x*direction.x+direction.y*direction.y+direction.z*direction.z);
  camera.forward=direction/direction_length;
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,36.0,9));
  std::uint64_t surface_hash{};
  for(const double cut_position:{0.34,0.50,0.66}){
    const auto scene=tetra_viewer::prepare_scene(
        mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::variational_smooth,true,false,true,false,
        true,true,cut_position,tetra_viewer::VolumeConnectionMethod::fixed_surface_shell);
    const auto topology=tetra_viewer::validate_connected_complex(scene,&mesh);
    CAPTURE(cut_position);
    CHECK(scene.hybrid_volume_valid);
    CHECK(topology.valid);
    CHECK(topology.unmatched_non_surface_faces==0);
    CHECK(topology.nonmanifold_faces==0);
    CHECK(topology.graded_parent_band);
    CHECK(scene.connected_surface_hash==scene.standalone_surface_hash);
    CHECK(scene.visible_volume_face_triangles==scene.triangle_vertices.size()/3);
    CHECK(scene.visible_volume_face_triangles>0);
    if(surface_hash==0)surface_hash=scene.connected_surface_hash;
    CHECK(scene.connected_surface_hash==surface_hash);
  }
}

TEST_CASE("TetWeave-inspired cutaway optimizes the authoritative volume boundary") {
  CHECK(tetra_viewer::supports_connected_volume(
      tetra_viewer::SurfaceMethod::surface_optimization));
  CHECK_FALSE(tetra_viewer::supports_connected_volume(
      tetra_viewer::SurfaceMethod::dual_contouring));
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.48};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,36.0,9));
  const auto prepare=[&](tetra_viewer::SurfaceMethod method){
    return tetra_viewer::prepare_scene(
        mesh,sphere,method,tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,true,true,1.0,
        tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  };
  const auto baseline=prepare(tetra_viewer::SurfaceMethod::marching_tetrahedra);
  const auto optimized=prepare(tetra_viewer::SurfaceMethod::surface_optimization);
  const auto repeated=prepare(tetra_viewer::SurfaceMethod::surface_optimization);
  const auto coned=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,
      true,false,true,false,true,true,1.0,
      tetra_viewer::VolumeConnectionMethod::coned_prototype);
  REQUIRE(optimized.connected_volume_tetrahedra==baseline.connected_volume_tetrahedra);
  REQUIRE(optimized.connected_volume_vertices.size()==baseline.connected_volume_vertices.size());
  REQUIRE(repeated.connected_volume_vertices.size()==optimized.connected_volume_vertices.size());
  CHECK(optimized.optimized_volume_boundary_vertices>0);
  CHECK(optimized.minimum_connected_tet_quality_before>0.0);
  CHECK(optimized.minimum_connected_tet_quality_after>0.0);
  CHECK(optimized.connected_volume_tetrahedra.size()<coned.connected_volume_tetrahedra.size());
  CHECK(optimized.connected_volume_vertices.size()<coned.connected_volume_vertices.size());
  CHECK(optimized.rejected_volume_boundary_moves<coned.rejected_volume_boundary_moves);
  CHECK(optimized.minimum_connected_tet_quality_after>coned.minimum_connected_tet_quality_after);
  CHECK(optimized.maximum_dihedral_degrees<coned.maximum_dihedral_degrees);
  bool changed=false;
  for(std::size_t vertex=0;vertex<optimized.connected_volume_vertices.size();++vertex){
    const auto& before=baseline.connected_volume_vertices[vertex];
    const auto& after=optimized.connected_volume_vertices[vertex];
    const auto& again=repeated.connected_volume_vertices[vertex];
    changed|=before.x!=after.x||before.y!=after.y||before.z!=after.z;
    CHECK(after.x==again.x);
    CHECK(after.y==again.y);
    CHECK(after.z==again.z);
  }
  CHECK(changed);
  const auto quality=[](const tetra_viewer::PreparedScene& scene,std::size_t tet_index){
    const auto& ids=scene.connected_volume_tetrahedra[tet_index];
    std::array<tetra::Vec3,4> points{};
    for(std::size_t corner=0;corner<4;++corner)points[corner]=scene.connected_volume_vertices[ids[corner]];
    const auto ab=points[1]-points[0],ac=points[2]-points[0],ad=points[3]-points[0];
    const double determinant=ab.x*(ac.y*ad.z-ac.z*ad.y)-
        ab.y*(ac.x*ad.z-ac.z*ad.x)+ab.z*(ac.x*ad.y-ac.y*ad.x);
    double edge_squared_sum{};
    for(std::size_t first=0;first<4;++first)for(std::size_t second=first+1;second<4;++second){
      const auto edge=points[second]-points[first];
      edge_squared_sum+=edge.x*edge.x+edge.y*edge.y+edge.z*edge.z;
    }
    return std::pair{determinant,12.0*std::pow(determinant*0.5,2.0/3.0)/edge_squared_sum};
  };
  for(std::size_t tet_index=0;tet_index<optimized.connected_volume_tetrahedra.size();++tet_index){
    const auto [baseline_determinant,baseline_quality]=quality(baseline,tet_index);
    const auto [optimized_determinant,optimized_quality]=quality(optimized,tet_index);
    CHECK(baseline_determinant>0.0);
    CHECK(optimized_determinant>0.0);
    CHECK(optimized_quality+1.0e-12>=std::max(1.0e-5,baseline_quality*0.5));
  }
  CHECK(std::ranges::all_of(optimized.triangle_vertices,[](const tetra_viewer::SceneVertex& vertex){
    return vertex.diagnostics[0]<-0.5F;
  }));
  for(const auto& vertex:optimized.triangle_vertices)if(vertex.diagnostics[0]<-1.5F)
    CHECK(std::abs(sphere.signed_distance(
        {vertex.position[0],vertex.position[1],vertex.position[2]}))<1.0e-6);
}

TEST_CASE("TetWeave-inspired cutaway preserves the uncut optimized surface") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.48};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,36.0,9));
  const auto uncut=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,
      true,false,true,false,false,false,1.0,
      tetra_viewer::VolumeConnectionMethod::adaptive_cleaving,
      tetra_viewer::StencilConstruction::selected,
      tetra_viewer::StencilSelectionObjective::balanced);
  const auto cut=tetra_viewer::prepare_scene(
      mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
      tetra_viewer::MaterialRule::all_vertices_inside,
      true,false,true,false,true,true,0.5,
      tetra_viewer::VolumeConnectionMethod::adaptive_cleaving,
      tetra_viewer::StencilConstruction::selected,
      tetra_viewer::StencilSelectionObjective::balanced);
  CHECK(uncut.connected_surface_hash!=0);
  CHECK(cut.connected_surface_hash==uncut.connected_surface_hash);
  using Point=std::array<float,3>;
  using Face=std::array<Point,3>;
  const auto face_key=[](const tetra_viewer::SceneVertex* vertices){
    Face face{{Point{{vertices[0].position[0],vertices[0].position[1],vertices[0].position[2]}},
               Point{{vertices[1].position[0],vertices[1].position[1],vertices[1].position[2]}},
               Point{{vertices[2].position[0],vertices[2].position[1],vertices[2].position[2]}}}};
    std::sort(face.begin(),face.end());
    return face;
  };
  std::set<Face> uncut_faces;
  for(std::size_t triangle=0;triangle+2<uncut.triangle_vertices.size();triangle+=3)
    uncut_faces.insert(face_key(uncut.triangle_vertices.data()+triangle));
  std::size_t compared{};
  for(std::size_t triangle=0;triangle+2<cut.triangle_vertices.size();triangle+=3){
    if(cut.triangle_vertices[triangle].diagnostics[0]>=-1.5F)continue;
    ++compared;
    CHECK(uncut_faces.contains(face_key(cut.triangle_vertices.data()+triangle)));
  }
  CHECK(compared>0);
  CHECK(compared<uncut_faces.size());
}

TEST_CASE("quality cleaving recovers standalone surface fairness and safe warping improves elements") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.48};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,36.0,9));
  const auto prepare=[&](tetra_viewer::VolumeConnectionMethod connection){
    return tetra_viewer::prepare_scene(
        mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,0.5,connection);
  };
  const auto standalone=prepare(tetra_viewer::VolumeConnectionMethod::hierarchy_cells);
  const auto quality=prepare(tetra_viewer::VolumeConnectionMethod::quality_stencils);
  const auto warped=prepare(tetra_viewer::VolumeConnectionMethod::adaptive_cleaving);
  REQUIRE(standalone.maximum_dihedral_degrees>0.0);
  CHECK(quality.mean_dihedral_degrees<=standalone.mean_dihedral_degrees*1.10);
  CHECK(quality.percentile99_dihedral_degrees<=standalone.percentile99_dihedral_degrees*1.20);
  CHECK(quality.maximum_dihedral_degrees<=standalone.maximum_dihedral_degrees*1.15);
  CHECK(quality.minimum_connected_tet_quality_after>
        quality.minimum_connected_tet_quality_before*2.0);
  CHECK(quality.rejected_volume_boundary_moves<quality.optimized_surface_vertices*2);
  CHECK(warped.minimum_connected_tet_quality_after>quality.minimum_connected_tet_quality_after);
  CHECK(warped.minimum_surface_triangle_angle_degrees>quality.minimum_surface_triangle_angle_degrees);
  CHECK(warped.maximum_surface_triangle_edge_ratio<quality.maximum_surface_triangle_edge_ratio);
}

TEST_CASE("quality-selected prism atlas improves the connected surface without changing conformity") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  const tetra::Sphere sphere{{0.5,0.5,0.5},0.35};
  const tetra::Camera camera{};
  static_cast<void>(tetra::refine_to_sphere(mesh,sphere,camera,28.0,16));
  const auto prepare=[&](tetra_viewer::StencilConstruction construction,
                         tetra_viewer::StencilSelectionObjective objective){
    return tetra_viewer::prepare_scene(
        mesh,sphere,tetra_viewer::SurfaceMethod::surface_optimization,
        tetra_viewer::MaterialRule::all_vertices_inside,
        true,false,true,false,false,false,0.5,
        tetra_viewer::VolumeConnectionMethod::quality_stencils,
        construction,objective);
  };
  const auto fixed=prepare(tetra_viewer::StencilConstruction::fixed,
                           tetra_viewer::StencilSelectionObjective::balanced);
  const auto selected=prepare(tetra_viewer::StencilConstruction::selected,
                              tetra_viewer::StencilSelectionObjective::balanced);
  const auto repeated=prepare(tetra_viewer::StencilConstruction::selected,
                              tetra_viewer::StencilSelectionObjective::balanced);
  CHECK(selected.connected_volume_tetrahedra.size()==fixed.connected_volume_tetrahedra.size());
  CHECK(selected.selected_stencil_cells>0);
  CHECK(selected.alternate_stencil_cells>0);
  CHECK(selected.connected_surface_hash==repeated.connected_surface_hash);
  CHECK(selected.maximum_dihedral_degrees<fixed.maximum_dihedral_degrees);
  CHECK(selected.minimum_connected_tet_quality_after>
        fixed.minimum_connected_tet_quality_after);
  CHECK(selected.minimum_connected_tet_volume_surface_quality_before>
        fixed.minimum_connected_tet_volume_surface_quality_before);

  constexpr std::array<std::array<std::size_t,3>,4> local_faces{{
      {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};
  std::map<std::array<std::size_t,3>,std::size_t> incidents;
  for(const auto& tet:selected.connected_volume_tetrahedra){
    for(const auto face:local_faces){
      std::array<std::size_t,3> key{{tet[face[0]],tet[face[1]],tet[face[2]]}};
      std::sort(key.begin(),key.end());
      ++incidents[key];
    }
  }
  std::size_t exterior_faces{};
  for(const auto& [face,count]:incidents){
    CHECK(count<=2);
    if(count==1){
      ++exterior_faces;
      CHECK(selected.connected_volume_surface_vertices[face[0]]!=0U);
      CHECK(selected.connected_volume_surface_vertices[face[1]]!=0U);
      CHECK(selected.connected_volume_surface_vertices[face[2]]!=0U);
    }
  }
  CHECK(exterior_faces>0);
}

TEST_CASE("headless stencil atlas controls select every research objective") {
  for(const auto objective:tetra_viewer::stencil_selection_objectives){
    std::ostringstream output;
    std::ostringstream errors;
    const std::string script="set-stencil-construction=selected,set-stencil-objective="+
        std::string(tetra_viewer::stencil_selection_objective_key(objective))+",prepare-scene";
    CHECK(tetra_viewer::run_script(script,output,errors)==0);
    CHECK(errors.str().empty());
    CHECK(output.str().find("\"stencil_construction\":\"selected\"")!=std::string::npos);
    CHECK(output.str().find("\"stencil_objective\":\""+
          std::string(tetra_viewer::stencil_selection_objective_key(objective))+"\"")!=
          std::string::npos);
  }
}

TEST_CASE("headless LOD camera direction is scriptable") {
  std::ostringstream output,errors;
  CHECK(tetra_viewer::run_script(
      "set-camera=0.5:0.5:3,set-camera-direction=0:1:0,stats",output,errors)==0);
  CHECK(errors.str().empty());
  CHECK(output.str().find("\"lod_camera\":[0.500,0.500,3.000]")!=std::string::npos);
  CHECK(output.str().find("\"lod_direction\":[0.000,1.000,0.000]")!=std::string::npos);
}

TEST_CASE("headless guarded camera LOD reports temporal demand classes") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-camera-lod-policy=guarded,set-camera-direction=0:0:1,"
      "adapt-once,stats",output,errors)==0);
  CHECK(errors.str().empty());
  CHECK(output.str().find("\"camera_lod_policy\":\"guarded\"")!=
        std::string::npos);
  CHECK(output.str().find("\"camera_demand_evaluations\":{")!=
        std::string::npos);
  CHECK(output.str().find("\"cold\":")!=std::string::npos);

  std::ostringstream configured_output,configured_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-camera-lod-policy=guarded-predicted,"
      "set-camera-lod-metric=geometric-error,set-guard-scale=1.5,"
      "set-near-lod-radius=1.25,set-recent-lod-epochs=4,"
      "set-prediction-factor=0.5,set-complexity-target=1000,stats",
      configured_output,configured_errors)==0);
  CHECK(configured_errors.str().empty());
  CHECK(configured_output.str().find(
      "\"camera_lod_policy\":\"guarded-predicted\"")!=std::string::npos);
  CHECK(configured_output.str().find(
      "\"camera_lod_metric\":\"geometric-error\"")!=std::string::npos);
  CHECK(configured_output.str().find(
      "\"camera_complexity_target_owners\":1000")!=std::string::npos);

  std::ostringstream bad_output,bad_errors;
  CHECK(tetra_viewer::run_script(
      "set-camera-lod-policy=unknown",bad_output,bad_errors)==2);
  CHECK(bad_errors.str().find("unknown camera LOD policy")!=std::string::npos);
}

TEST_CASE("headless events identify adaptation schemas strategies and both cut views") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script("stats",output,errors)==0);
  const auto text=output.str();
  CHECK(text.find("\"adaptation_configuration_schema\":2")!=std::string::npos);
  CHECK(text.find("\"benchmark_schema\":2")!=std::string::npos);
  CHECK(text.find("\"lod_update\":\"transactional-active-cut\"")!=std::string::npos);
  CHECK(text.find("\"update_scheduler\":\"classify-and-stream\"")!=std::string::npos);
  CHECK(text.find("\"candidate_traversal\":\"active-cut-scan\"")!=std::string::npos);
  CHECK(text.find("\"closure_execution\":\"sparse-frontier\"")!=std::string::npos);
  CHECK(text.find("\"layer_storage\":\"flat-packed\"")!=std::string::npos);
  CHECK(text.find("\"adjacency\":\"logical-face-table\"")!=std::string::npos);
  CHECK(text.find("\"kernel_order\":\"address-order\"")!=std::string::npos);
  CHECK(text.find("\"transition_strategy\":\"crystalline-restricted\"")!=std::string::npos);
  CHECK(text.find("\"x_cutaway\":true")!=std::string::npos);
  CHECK(text.find("\"x_cut_position\":1.000")!=std::string::npos);
  CHECK(text.find("\"logical_owners\":")!=std::string::npos);
  CHECK(text.find("\"conforming_cells\":")!=std::string::npos);
  CHECK(text.find("\"logical_candidates\":")!=std::string::npos);
  CHECK(text.find("\"field_classifications\":")!=std::string::npos);
  CHECK(text.find("\"exact_field_evaluations\":")!=std::string::npos);
  CHECK(text.find("\"scheduler_seed_scans\":")!=std::string::npos);
  CHECK(text.find("\"scheduler_seed_candidates\":")!=std::string::npos);
  CHECK(text.find("\"scheduler_incremental_candidates\":")!=std::string::npos);
  CHECK(text.find("\"scheduler_conformity_candidates\":")!=std::string::npos);
  CHECK(text.find("\"scheduler_candidates_avoided\":")!=std::string::npos);
  CHECK(text.find("\"plan_ms\":")!=std::string::npos);
  CHECK(text.find("\"commit_ms\":")!=std::string::npos);
  CHECK(text.find("\"logical_cut_hash\":")!=std::string::npos);
  CHECK(text.find("\"conforming_volume_hash\":")!=std::string::npos);
  CHECK(text.find("\"minimum_conforming_mean_ratio\":")!=std::string::npos);
  CHECK(text.find("\"minimum_conforming_dihedral_degrees\":")!=std::string::npos);
  CHECK(text.find("\"maximum_conforming_dihedral_degrees\":")!=std::string::npos);
  CHECK(text.find("\"retained_layer_bytes\":")!=std::string::npos);
  CHECK(text.find("\"bcc_cut_scan_ms\":")!=std::string::npos);
  CHECK(text.find("\"bcc_face_repair_ms\":")!=std::string::npos);
  CHECK(errors.str().empty());
}

TEST_CASE("headless BCC transition strategy selects complete minimal closure") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=6,set-transition-strategy=complete-minimal,validate,stats",
      output,errors)==0);
  CHECK(errors.str().empty());
  CHECK(output.str().find("\"transition_strategy\":\"complete-minimal\"")!=
        std::string::npos);
  CHECK(output.str().find("\"event\":\"validation\",\"valid\":true")!=
        std::string::npos);

  std::ostringstream invalid_output,invalid_errors;
  CHECK(tetra_viewer::run_script(
      "set-method=maubach-diamond,set-transition-strategy=complete-minimal",
      invalid_output,invalid_errors)==2);
  CHECK(invalid_errors.str().find("requires BCC")!=std::string::npos);
}

TEST_CASE("headless LOD strategy selection is explicit and rejects unavailable research paths") {
  std::ostringstream oracle_output,oracle_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-lod-update=full-rebuild-oracle,stats",oracle_output,oracle_errors)==0);
  CHECK(oracle_errors.str().empty());
  CHECK(oracle_output.str().find("\"lod_update\":\"full-rebuild-oracle\"")!=
        std::string::npos);

  std::ostringstream saturated_output,saturated_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-lod-update=saturated-clusters,validate,stats",
      saturated_output,saturated_errors)==0);
  CHECK(saturated_errors.str().empty());
  CHECK(saturated_output.str().find("\"lod_update\":\"saturated-clusters\"")!=
        std::string::npos);
  CHECK(saturated_output.str().find("\"event\":\"validation\",\"valid\":true")!=
        std::string::npos);

  std::ostringstream hierarchy_output,hierarchy_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-x-cut=off,set-lod-update=relevant-surface-hierarchy,"
      "set-camera=0.2:0.7:2.0,stats",hierarchy_output,hierarchy_errors)==0);
  CHECK(hierarchy_errors.str().empty());
  CHECK(hierarchy_output.str().find(
      "\"lod_update\":\"relevant-surface-hierarchy\"")!=std::string::npos);
  CHECK(hierarchy_output.str().find("\"surface_hierarchy_rebuilds\":1")!=
        std::string::npos);

  std::ostringstream minimal_output,minimal_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-x-cut=off,set-lod-update=minimal-surface-hierarchy,stats",
      minimal_output,minimal_errors)==0);
  CHECK(minimal_errors.str().empty());
  CHECK(minimal_output.str().find(
      "\"lod_update\":\"minimal-surface-hierarchy\"")!=std::string::npos);

  std::ostringstream preorder_output,preorder_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-x-cut=off,set-lod-update=on-demand-render-traversal,prepare-scene,stats",
      preorder_output,preorder_errors)==0);
  CHECK(preorder_errors.str().empty());
  CHECK(preorder_output.str().find(
      "\"lod_update\":\"on-demand-render-traversal\"")!=std::string::npos);
  CHECK(preorder_output.str().find("\"preorder_rebuilds\":1")!=std::string::npos);
  const auto preorder_stats=preorder_output.str().rfind("\"event\":\"stats\"");
  REQUIRE(preorder_stats!=std::string::npos);
  CHECK(preorder_output.str().substr(preorder_stats).find(
      "\"preorder_generated_triangles\":0")==std::string::npos);

  std::ostringstream spatial_output,spatial_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-candidate-traversal=spatial-runs,set-camera=0.3:0.8:2.0,stats",
      spatial_output,spatial_errors)==0);
  CHECK(spatial_errors.str().empty());
  CHECK(spatial_output.str().find("\"candidate_traversal\":\"spatial-runs\"")!=
        std::string::npos);
  const auto spatial_stats=spatial_output.str().rfind("\"event\":\"stats\"");
  REQUIRE(spatial_stats!=std::string::npos);
  CHECK(spatial_output.str().substr(spatial_stats).find("\"spatial_run_count\":0")==
        std::string::npos);

  std::ostringstream rejected_output,rejected_errors;
  CHECK(tetra_viewer::run_script(
      "set-x-cut=0.5,set-lod-update=minimal-surface-hierarchy,stats",
      rejected_output,rejected_errors)==2);
  CHECK(rejected_errors.str().find("does not support volume cutaway")!=std::string::npos);
  CHECK(rejected_output.str().find("minimal-surface-hierarchy")==std::string::npos);
}

TEST_CASE("headless adaptation configuration and accepted-command replay are scriptable") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-split-hysteresis=1.25,set-merge-hysteresis=0.65,"
      "set-operation-budget=8192,set-closure-execution=hybrid,"
      "set-hybrid-threshold=0.20,set-camera-direction=0:0:1,"
      "reverse-last-adaptation,replay-last-adaptation,stats",output,errors)==0);
  CHECK(errors.str().empty());
  const auto text=output.str();
  CHECK(text.find("\"split_hysteresis\":1.250")!=std::string::npos);
  CHECK(text.find("\"merge_hysteresis\":0.650")!=std::string::npos);
  CHECK(text.find("\"operation_budget\":8192")!=std::string::npos);
  CHECK(text.find("\"closure_execution\":\"hybrid\"")!=std::string::npos);
  CHECK(text.find("\"hybrid_frontier_ratio\":0.200")!=std::string::npos);
  CHECK(text.find("\"replay_schema\":2")!=std::string::npos);
  const auto stats=text.rfind("\"event\":\"stats\"");
  REQUIRE(stats!=std::string::npos);
  CHECK(text.substr(stats).find("\"last_replay_commands\":0")==std::string::npos);
  CHECK(text.substr(stats).find("\"bcc_full_cut_cells_scanned\":0")==std::string::npos);
  CHECK(text.substr(stats).find("\"bcc_logical_owners_changed\":0")==std::string::npos);
  CHECK(text.find("\"bcc_green_generation_ms\":")!=std::string::npos);
  CHECK(text.find("\"bcc_incidence_update_ms\":")!=std::string::npos);
  CHECK(text.find("\"command\":\"reverse-last-adaptation\"")!=std::string::npos);
  CHECK(text.find("\"command\":\"replay-last-adaptation\"")!=std::string::npos);

  std::ostringstream invalid_output,invalid_errors;
  CHECK(tetra_viewer::run_script(
      "set-merge-hysteresis=2.0",invalid_output,invalid_errors)==2);
  CHECK(invalid_errors.str().find("below split hysteresis")!=std::string::npos);
}

TEST_CASE("headless experiment controls build packed layouts and reject incompatible combinations") {
  std::optional<std::string> topology_hash,classification_hash,
      multiplicity_hash,oriented_hash;
  for(const auto storage:{tetra::LayerStorage::flat_packed,
                          tetra::LayerStorage::mutable_macro_blocks,
                          tetra::LayerStorage::occupancy_bit_macro_blocks,
                          tetra::LayerStorage::address_runs}){
    for(const auto order:{tetra::KernelOrder::address_order,
                          tetra::KernelOrder::orientation_buckets,
                          tetra::KernelOrder::fused_macro_blocks}){
      std::ostringstream output,errors;
      const std::string script="set-layer-storage="+
          std::string(tetra::strategy_key(storage))+",set-kernel-order="+
          std::string(tetra::strategy_key(order))+",stats";
      REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
      CHECK(errors.str().empty());
      const auto text=output.str();
      CHECK(text.find("\"layer_storage\":\""+std::string(tetra::strategy_key(storage))+
                      "\"")!=std::string::npos);
      CHECK(text.find("\"kernel_order\":\""+std::string(tetra::strategy_key(order))+
                      "\"")!=std::string::npos);
      const auto field=[&](std::string_view name){
        const auto position=text.rfind(std::string{"\""}+std::string{name}+"\":");
        if(position==std::string::npos)return std::string{};
        const auto begin=text.find(':',position)+1U;
        const auto end=text.find_first_of(",}",begin);
        return text.substr(begin,end-begin);
      };
      if(topology_hash){
        CHECK(field("storage_topology_hash")==*topology_hash);
        CHECK(field("storage_classification_hash")==*classification_hash);
      }else{
        topology_hash=field("storage_topology_hash");
        classification_hash=field("storage_classification_hash");
      }
    }
  }
  for(const auto adjacency:{tetra::AdjacencyRepresentation::path_arithmetic,
                            tetra::AdjacencyRepresentation::packed_half_facets,
                            tetra::AdjacencyRepresentation::logical_face_table,
                            tetra::AdjacencyRepresentation::reconstruction_oracle}){
    std::ostringstream output,errors;
    REQUIRE(tetra_viewer::run_script(
        "set-adjacency="+std::string(tetra::strategy_key(adjacency))+",stats",
        output,errors)==0);
    CHECK(errors.str().empty());
    const auto text=output.str();
    const auto field=[&](std::string_view name){
      const auto position=text.rfind(std::string{"\""}+std::string{name}+"\":");
      if(position==std::string::npos)return std::string{};
      const auto begin=text.find(':',position)+1U;
      return text.substr(begin,text.find_first_of(",}",begin)-begin);
    };
    if(multiplicity_hash){
      CHECK(field("adjacency_multiplicity_hash")==*multiplicity_hash);
      CHECK(field("adjacency_oriented_hash")==*oriented_hash);
    }else{
      multiplicity_hash=field("adjacency_multiplicity_hash");
      oriented_hash=field("adjacency_oriented_hash");
    }
  }

  std::ostringstream invalid_output,invalid_errors;
  CHECK(tetra_viewer::run_script(
      "set-update-scheduler=persistent-split-merge-queues,set-x-cut=off,"
      "set-lod-update=minimal-surface-hierarchy",
      invalid_output,invalid_errors)==2);
  CHECK(invalid_errors.str().find("incompatible")!=std::string::npos);

  std::ostringstream reverse_output,reverse_errors;
  CHECK(tetra_viewer::run_script(
      "set-x-cut=off,set-lod-update=minimal-surface-hierarchy,"
      "set-candidate-traversal=spatial-runs",
      reverse_output,reverse_errors)==2);
  CHECK(reverse_errors.str().find("incompatible")!=std::string::npos);
}

TEST_CASE("controlled five-shape adaptation matrix preserves conforming hashes") {
  const auto final_field=[](const std::string& text,std::string_view name){
    const auto stats=text.rfind("\"event\":\"stats\"");
    REQUIRE(stats!=std::string::npos);
    const auto position=text.find(std::string{"\""}+std::string{name}+"\":",stats);
    REQUIRE(position!=std::string::npos);
    const auto begin=text.find(':',position)+1U;
    return text.substr(begin,text.find_first_of(",}",begin)-begin);
  };
  for(const auto shape:tetra::implicit_shape_kinds){
    const std::string path="set-maximum-depth=6,set-shape="+
        std::string(tetra::implicit_shape_key(shape))+
        ",set-camera=0.45:0.55:1.8,set-camera=0.65:0.55:1.4,"
        "set-camera=0.35:0.65:2.1,validate,stats";
    std::ostringstream baseline_output,baseline_errors;
    REQUIRE(tetra_viewer::run_script(path,baseline_output,baseline_errors)==0);
    REQUIRE(baseline_errors.str().empty());

    const std::string experimental=
        "set-maximum-depth=6,set-update-scheduler=persistent-split-merge-queues,"
        "set-candidate-traversal=spatial-runs,set-closure-execution=hybrid,"
        "set-hybrid-threshold=0.10,set-layer-storage=occupancy-bit-macro-blocks,"
        "set-adjacency=packed-half-facets,set-kernel-order=orientation-buckets,"
        "set-shape="+std::string(tetra::implicit_shape_key(shape))+
        ",set-camera=0.45:0.55:1.8,set-camera=0.65:0.55:1.4,"
        "set-camera=0.35:0.65:2.1,validate,stats";
    std::ostringstream experimental_output,experimental_errors;
    REQUIRE(tetra_viewer::run_script(
        experimental,experimental_output,experimental_errors)==0);
    REQUIRE(experimental_errors.str().empty());
    CAPTURE(tetra::implicit_shape_key(shape));
    CHECK(final_field(experimental_output.str(),"logical_cut_hash")==
          final_field(baseline_output.str(),"logical_cut_hash"));
    CHECK(final_field(experimental_output.str(),"conforming_volume_hash")==
          final_field(baseline_output.str(),"conforming_volume_hash"));
    CHECK(experimental_output.str().find("\"event\":\"validation\",\"valid\":true")!=
          std::string::npos);
  }
}

TEST_CASE("headless camera commands reconcile terrain LOD in both directions") {
  std::ostringstream away_output,away_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-camera-lod-policy=exact-frustum,"
      "set-camera-lod-metric=projected-diameter,"
      "set-maximum-depth=6,set-shape=perlin-terrain,set-camera=-1:0.7:0.5,"
      "set-camera-direction=-1:0:0,validate,stats",away_output,away_errors)==0);
  CHECK(away_errors.str().empty());
  CHECK(away_output.str().find("\"active_leaves\":12")!=std::string::npos);
  CHECK(away_output.str().find("\"maximum_active_depth\":0")!=std::string::npos);

  std::ostringstream toward_output,toward_errors;
  REQUIRE(tetra_viewer::run_script(
      "set-camera-lod-policy=exact-frustum,"
      "set-camera-lod-metric=projected-diameter,"
      "set-maximum-depth=6,set-shape=perlin-terrain,set-camera=-1:0.7:0.5,"
      "set-camera-direction=1:0:0,validate,stats",toward_output,toward_errors)==0);
  CHECK(toward_errors.str().empty());
  const auto stats=toward_output.str().rfind("\"event\":\"stats\"");
  REQUIRE(stats!=std::string::npos);
  const auto final=toward_output.str().substr(stats);
  CHECK(final.find("\"maximum_active_depth\":6")!=std::string::npos);
  CHECK(final.find("\"active_leaves\":12,")==std::string::npos);
}

TEST_CASE("default terrain LOD coarsens the same detailed cut when camera moves away") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-shape=perlin-terrain,set-camera=0.5:0.5:1.5,"
      "set-camera=0.5:0.5:10,validate,stats",output,errors)==0);
  REQUIRE(errors.str().empty());
  const auto text=output.str();
  const auto near_event=text.find("\"command\":\"set-camera=0.5:0.5:1.5\"");
  const auto far_event=text.find("\"command\":\"set-camera=0.5:0.5:10\"");
  REQUIRE(near_event!=std::string::npos);
  REQUIRE(far_event!=std::string::npos);
  const auto field=[&](std::size_t event,std::string_view name){
    const auto position=text.find(std::string{"\""}+std::string{name}+"\":",event);
    REQUIRE(position!=std::string::npos);
    const auto begin=text.find(':',position)+1U;
    return std::stoull(text.substr(begin,text.find_first_of(",}",begin)-begin));
  };
  const auto near_owners=field(near_event,"logical_owners");
  const auto far_owners=field(far_event,"logical_owners");
  CHECK(far_owners<near_owners);
  CHECK(field(far_event,"accepted_merges")>0U);
  CHECK(text.find("\"event\":\"validation\",\"valid\":true")!=std::string::npos);
}

TEST_CASE("lateral LOD camera movement finishes pending coarsening after refinement") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-camera-lod-policy=exact-frustum,"
      "set-camera-lod-metric=projected-diameter,"
      "set-maximum-depth=12,set-shape=sphere,"
      "set-camera=0:1:0.5,set-camera=1:0:0.5,set-camera=1:0:0.5,stats",
      output,errors)==0);
  REQUIRE(errors.str().empty());
  const auto text=output.str();
  const auto first=text.find("\"command\":\"set-camera=0:1:0.5\"");
  const auto moved=text.find("\"command\":\"set-camera=1:0:0.5\"");
  const auto continued=text.find("\"command\":\"set-camera=1:0:0.5\"",moved+1U);
  REQUIRE(first!=std::string::npos);
  REQUIRE(moved!=std::string::npos);
  REQUIRE(continued!=std::string::npos);
  const auto field=[&](std::size_t event,std::string_view name){
    const auto position=text.find(std::string{"\""}+std::string{name}+"\":",event);
    REQUIRE(position!=std::string::npos);
    const auto begin=text.find(':',position)+1U;
    return std::stoull(text.substr(begin,text.find_first_of(",}",begin)-begin));
  };
  const auto splits_before_move=field(first,"accepted_splits");
  const auto merges_before_move=field(first,"accepted_merges");
  const auto owners_before_move=field(first,"logical_owners");
  const auto owners_after_move=field(moved,"logical_owners");
  CHECK(field(moved,"accepted_splits")>splits_before_move);
  CHECK(field(moved,"accepted_merges")>merges_before_move);
  CHECK(owners_after_move<owners_before_move);
  CHECK(field(continued,"logical_owners")==owners_after_move);
}

TEST_CASE("a no-op LOD camera continuation does not retain detail released by a tiny nudge") {
  const auto final_owner_count=[](std::string_view final_camera){
    std::ostringstream output,errors;
    const std::string script=
        "set-maximum-depth=12,set-shape=perlin-terrain,"
        "set-camera=0:1:0.5,set-camera=1:0:0.5,set-camera="+
        std::string(final_camera)+",stats";
    REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
    REQUIRE(errors.str().empty());
    const auto text=output.str();
    const auto stats=text.rfind("\"event\":\"stats\"");
    REQUIRE(stats!=std::string::npos);
    const auto position=text.find("\"logical_owners\":",stats);
    REQUIRE(position!=std::string::npos);
    const auto begin=text.find(':',position)+1U;
    return std::stoull(text.substr(begin,text.find_first_of(",}",begin)-begin));
  };
  const auto continued=final_owner_count("1:0:0.5");
  const auto nudged=final_owner_count("1.000001:0:0.5");
  CHECK(continued<=nudged+nudged/100U);
}

TEST_CASE("a converged camera pose does not commit zero-delta merge transactions forever") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=12,set-shape=perlin-terrain,"
      "set-camera=0:1:0.5,set-camera=0:1:0.5,set-camera=0:1:0.5",
      output,errors)==0);
  REQUIRE(errors.str().empty());
  const auto text=output.str();
  std::array<std::size_t,3> events{};
  std::size_t search{};
  for(auto& event:events){
    event=text.find("\"command\":\"set-camera=0:1:0.5\"",search);
    REQUIRE(event!=std::string::npos);
    search=event+1U;
  }
  const auto field=[&](std::size_t event,std::string_view name){
    const auto position=text.find(std::string{"\""}+std::string{name}+"\":",event);
    REQUIRE(position!=std::string::npos);
    const auto begin=text.find(':',position)+1U;
    return std::stoull(text.substr(begin,text.find_first_of(",}",begin)-begin));
  };
  const auto transactions=field(events[0],"adaptation_transactions");
  const auto owners=field(events[0],"logical_owners");
  CHECK(field(events[1],"adaptation_transactions")==transactions);
  CHECK(field(events[2],"adaptation_transactions")==transactions);
  CHECK(field(events[1],"logical_owners")==owners);
  CHECK(field(events[2],"logical_owners")==owners);
}

TEST_CASE("pixel threshold reconciles detail in both directions") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-maximum-depth=12,set-shape=perlin-terrain,set-camera=0:1:0.5,"
      "set-pixel-threshold=8,set-pixel-threshold=1000,set-pixel-threshold=8",
      output,errors)==0);
  REQUIRE(errors.str().empty());
  const auto text=output.str();
  const auto fine=text.find("\"command\":\"set-pixel-threshold=8\"");
  const auto coarse=text.find("\"command\":\"set-pixel-threshold=1000\"");
  const auto restored=text.find("\"command\":\"set-pixel-threshold=8\"",fine+1U);
  REQUIRE(fine!=std::string::npos);REQUIRE(coarse!=std::string::npos);
  REQUIRE(restored!=std::string::npos);
  const auto field=[&](std::size_t event,std::string_view name){
    const auto position=text.find(std::string{"\""}+std::string{name}+"\":",event);
    REQUIRE(position!=std::string::npos);
    const auto begin=text.find(':',position)+1U;
    return std::stoull(text.substr(begin,text.find_first_of(",}",begin)-begin));
  };
  CHECK(field(coarse,"logical_owners")<field(fine,"logical_owners"));
  CHECK(field(coarse,"accepted_merges")>field(fine,"accepted_merges"));
  CHECK(field(restored,"logical_owners")>field(coarse,"logical_owners"));
  CHECK(field(restored,"accepted_splits")>field(coarse,"accepted_splits"));
}

TEST_CASE("large and stepped camera moves converge within the LOD hysteresis band") {
  const auto run=[](std::string_view path){
    std::ostringstream output,errors;
    const std::string script="set-maximum-depth=9,set-shape=perlin-terrain,"
        "set-camera=0:1:0.5,"+std::string(path)+",validate,stats";
    REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
    REQUIRE(errors.str().empty());
    CHECK(output.str().find("\"event\":\"validation\",\"valid\":true")!=
          std::string::npos);
    const auto stats=output.str().rfind("\"event\":\"stats\"");
    REQUIRE(stats!=std::string::npos);
    const auto field=[&](std::string_view name){
      const auto position=output.str().find(
          std::string{"\""}+std::string{name}+"\":",stats);
      REQUIRE(position!=std::string::npos);
      const auto begin=output.str().find(':',position)+1U;
      return output.str().substr(
          begin,output.str().find_first_of(",}",begin)-begin);
    };
    return std::tuple{
        std::stoull(field("logical_owners")),
        std::stoull(field("maximum_active_depth")),
        field("logical_cut_hash"),field("conforming_volume_hash")};
  };
  const auto direct=run("set-camera=1:0:0.5");
  const auto stepped=run(
      "set-camera=0.25:0.75:0.5,set-camera=0.5:0.5:0.5,"
      "set-camera=0.75:0.25:0.5,set-camera=1:0:0.5");
  // Split and merge thresholds deliberately define a history-dependent
  // hysteresis band. Require equivalent depth and a bounded population,
  // rather than accidentally demanding one canonical topology inside it.
  CHECK(run("set-camera=1:0:0.5")==direct);
  CHECK(run("set-camera=0.25:0.75:0.5,set-camera=0.5:0.5:0.5,"
            "set-camera=0.75:0.25:0.5,set-camera=1:0:0.5")==stepped);
  CHECK(std::get<1>(stepped)==std::get<1>(direct));
  const auto larger=std::max(std::get<0>(stepped),std::get<0>(direct));
  const auto smaller=std::min(std::get<0>(stepped),std::get<0>(direct));
  CHECK(smaller*10U>=larger*9U);
}

TEST_CASE("gizmo translation simplifies LOD without retargeting its camera direction") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,1.0,0.5};
  pose.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::Camera camera;
  pose.apply(camera);
  tetra::ImplicitValueCache field_cache;
  tetra::AdaptationPlanningCache planning_cache;
  tetra::AdaptationConfiguration configuration;
  static_cast<void>(tetra::refine_to_sphere(
      mesh,terrain,camera,28.0*configuration.split_hysteresis,12,&field_cache));
  const auto detailed_owners=mesh.logical_cut().owners.size();
  const auto detailed_depth=std::ranges::max(
      mesh.logical_cut().owners|std::views::transform(tetra::tet_depth));
  const auto original_forward=camera.forward;

  pose.translate(tetra_viewer::CameraGizmoAxis::z,3.0);
  pose.apply(camera);
  CHECK(camera.forward.x==doctest::Approx(original_forward.x));
  CHECK(camera.forward.y==doctest::Approx(original_forward.y));
  CHECK(camera.forward.z==doctest::Approx(original_forward.z));
  bool merged=false,converged=false;
  for(std::size_t frame=0;frame<256U;++frame){
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,12,configuration,0,&planning_cache);
    if(result.status==tetra::AdaptationCommitStatus::no_change){converged=true;break;}
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    merged|=result.accepted_merges>0U;
  }
  CHECK(converged);
  CHECK(merged);
  CHECK(mesh.logical_cut().owners.size()<detailed_owners);
  CHECK(std::ranges::max(mesh.logical_cut().owners|
        std::views::transform(tetra::tet_depth))<detailed_depth);
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("successive gizmo drag frames still finish the final translation merge phase") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,1.0,0.5};
  pose.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::Camera camera;
  pose.apply(camera);
  tetra::AdaptationPlanningCache planning_cache;
  tetra::AdaptationConfiguration configuration;
  static_cast<void>(tetra::refine_to_sphere(
      mesh,terrain,camera,28.0*configuration.split_hysteresis,12));
  const auto detailed_owners=mesh.logical_cut().owners.size();
  const auto original_forward=camera.forward;
  std::size_t accepted_merges{};
  for(std::size_t drag_frame=0;drag_frame<8U;++drag_frame){
    pose.translate(tetra_viewer::CameraGizmoAxis::z,0.375);
    pose.apply(camera);
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,12,configuration,0,&planning_cache);
    REQUIRE((result.status==tetra::AdaptationCommitStatus::committed||
             result.status==tetra::AdaptationCommitStatus::no_change));
    accepted_merges+=result.accepted_merges;
  }
  bool converged=false;
  for(std::size_t frame=0;frame<256U;++frame){
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,12,configuration,0,&planning_cache);
    if(result.status==tetra::AdaptationCommitStatus::no_change){converged=true;break;}
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    accepted_merges+=result.accepted_merges;
  }
  CHECK(converged);
  CHECK(accepted_merges>0U);
  CHECK(mesh.logical_cut().owners.size()<detailed_owners);
  CHECK(camera.forward.x==doctest::Approx(original_forward.x));
  CHECK(camera.forward.y==doctest::Approx(original_forward.y));
  CHECK(camera.forward.z==doctest::Approx(original_forward.z));
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("scene cache publishes the simplified cut after gizmo translation") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,1.0,0.5};
  pose.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::Camera camera;
  pose.apply(camera);
  tetra::AdaptationPlanningCache planning_cache;
  tetra::AdaptationConfiguration configuration;
  static_cast<void>(tetra::refine_to_sphere(
      mesh,terrain,camera,28.0*configuration.split_hysteresis,9));
  tetra_viewer::SceneCache scene_cache;
  REQUIRE(scene_cache.update_scene(
      mesh,terrain,0,tetra_viewer::MaterialRule::all_vertices_inside,
      false,false,false));
  const auto detailed_cells=scene_cache.scene().relations.size();
  const auto detailed_generation=scene_cache.scene_generation();

  pose.translate(tetra_viewer::CameraGizmoAxis::z,3.0);
  pose.apply(camera);
  bool converged=false;
  for(std::size_t frame=0;frame<128U;++frame){
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,9,configuration,0,&planning_cache);
    if(result.status==tetra::AdaptationCommitStatus::no_change){converged=true;break;}
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
  }
  REQUIRE(converged);
  REQUIRE(scene_cache.update_scene(
      mesh,terrain,0,tetra_viewer::MaterialRule::all_vertices_inside,
      false,false,false));
  CHECK(scene_cache.scene_generation()==detailed_generation+1U);
  CHECK(scene_cache.scene().relations.size()==mesh.conforming_volume().size());
  CHECK(scene_cache.scene().relations.size()<detailed_cells);
}

TEST_CASE("gizmo rotation away from the terrain simplifies LOD at a fixed origin") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,1.0,0.5};
  pose.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::Camera camera;
  pose.apply(camera);
  tetra::AdaptationPlanningCache planning_cache;
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::exact_frustum;
  configuration.camera_lod_metric=tetra::CameraLodMetric::projected_diameter;
  static_cast<void>(tetra::refine_to_sphere(
      mesh,terrain,camera,28.0*configuration.split_hysteresis,12));
  const auto detailed_owners=mesh.logical_cut().owners.size();
  const auto fixed_position=camera.position;

  pose.rotate(tetra_viewer::CameraGizmoAxis::z,std::acos(-1.0));
  pose.apply(camera);
  bool merged=false,converged=false;
  for(std::size_t frame=0;frame<256U;++frame){
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,12,configuration,0,&planning_cache);
    if(result.status==tetra::AdaptationCommitStatus::no_change){converged=true;break;}
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    merged|=result.accepted_merges>0U;
  }
  CHECK(converged);
  CHECK(merged);
  CHECK(mesh.logical_cut().owners.size()<detailed_owners);
  CHECK(camera.position.x==doctest::Approx(fixed_position.x));
  CHECK(camera.position.y==doctest::Approx(fixed_position.y));
  CHECK(camera.position.z==doctest::Approx(fixed_position.z));
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("guarded camera policy retains a bounded standby cut through a turn") {
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,1.0,0.5};
  pose.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::Camera initial_camera;
  pose.apply(initial_camera);

  auto initial=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra::AdaptationConfiguration initial_configuration;
  initial_configuration.camera_lod_policy=tetra::CameraLodPolicy::exact_frustum;
  initial_configuration.camera_lod_metric=
      tetra::CameraLodMetric::projected_diameter;
  static_cast<void>(tetra::refine_to_sphere(
      initial,terrain,initial_camera,
      28.0*initial_configuration.split_hysteresis,9));
  const auto initial_owners=initial.logical_cut().owners.size();
  REQUIRE(initial_owners>initial.layers().front().tetrahedra.size());

  pose.rotate(tetra_viewer::CameraGizmoAxis::z,std::acos(-1.0));
  tetra::Camera turned_camera;
  pose.apply(turned_camera);
  const auto converge=[&](tetra::TetMesh mesh,
                          tetra::AdaptationConfiguration configuration){
    tetra::AdaptationPlanningCache cache;
    for(std::size_t frame=0;frame<256U;++frame){
      const auto result=tetra::adapt_to_surface(
          mesh,terrain,turned_camera,28.0,9,configuration,0,&cache);
      if(result.status==tetra::AdaptationCommitStatus::no_change)return mesh;
      REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    }
    FAIL("camera policy did not converge");
    return mesh;
  };

  auto exact=converge(initial,initial_configuration);
  auto guarded_configuration=initial_configuration;
  guarded_configuration.camera_lod_policy=tetra::CameraLodPolicy::guarded;
  auto guarded=converge(initial,guarded_configuration);
  CHECK(exact.logical_cut().owners.size()<initial_owners);
  CHECK(guarded.logical_cut().owners.size()>exact.logical_cut().owners.size());
  CHECK(guarded.has_conforming_active_faces());
}

TEST_CASE("recent camera demand is revisioned and stale plans cannot publish it") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::guarded_recent;
  tetra::AdaptationPlanningCache cache;

  const auto first_plan=tetra::plan_adaptation(
      mesh,terrain,camera,28.0,9,configuration,0,&cache);
  const auto first=tetra::commit_adaptation(mesh,first_plan,configuration,0,&cache);
  REQUIRE((first.status==tetra::AdaptationCommitStatus::committed||
           first.status==tetra::AdaptationCommitStatus::no_change));
  REQUIRE(cache.has_committed_camera);
  const auto first_epoch=cache.camera_demand_epoch;
  CHECK(first_epoch==1U);
  CHECK(std::ranges::any_of(cache.camera_temporal_layers,[](const auto& layer){
    return !layer.addresses.empty();
  }));

  camera.forward={0.0,0.0,1.0};
  const auto turned_plan=tetra::plan_adaptation(
      mesh,terrain,camera,28.0,9,configuration,0,&cache);
  CHECK(turned_plan.camera_demand_evaluations[
        static_cast<std::size_t>(tetra::CameraLodZone::recent)]>0U);
  CHECK(turned_plan.camera_demand_pose_changed);
  CHECK(turned_plan.camera_demand_epoch==first_epoch+1U);

  auto changed=configuration;
  changed.near_radius+=0.25;
  const auto stale=tetra::commit_adaptation(
      mesh,turned_plan,changed,0,&cache);
  CHECK(stale.status==tetra::AdaptationCommitStatus::stale_plan);
  CHECK(cache.camera_demand_epoch==first_epoch);
  CHECK(cache.committed_camera.forward.z<0.0);

  const auto accepted=tetra::commit_adaptation(
      mesh,turned_plan,configuration,0,&cache);
  REQUIRE((accepted.status==tetra::AdaptationCommitStatus::committed||
           accepted.status==tetra::AdaptationCommitStatus::no_change));
  CHECK(cache.camera_demand_epoch==first_epoch+1U);
  CHECK(cache.committed_camera.forward.z>0.0);
}

TEST_CASE("predicted camera demand prepares cells beyond the current guard") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::guarded_predicted;
  configuration.guard_frustum_scale=1.5;
  configuration.prediction_factor=1.0;
  tetra::AdaptationPlanningCache cache;
  tetra::Camera camera;
  const auto direction=[](double degrees){
    const double radians=degrees*std::acos(-1.0)/180.0;
    return tetra::Vec3{std::sin(radians),0.0,-std::cos(radians)};
  };

  camera.forward=direction(120.0);
  const auto first_plan=tetra::plan_adaptation(
      mesh,terrain,camera,28.0,9,configuration,0,&cache);
  const auto first=tetra::commit_adaptation(mesh,first_plan,configuration,0,&cache);
  REQUIRE((first.status==tetra::AdaptationCommitStatus::committed||
           first.status==tetra::AdaptationCommitStatus::no_change));

  camera.forward=direction(70.0);
  auto translation_configuration=configuration;
  translation_configuration.prediction_mode=
      tetra::CameraPredictionMode::translation;
  auto translation_cache=cache;
  const auto translation_only=tetra::plan_adaptation(
      mesh,terrain,camera,28.0,9,translation_configuration,0,
      &translation_cache);
  CHECK(translation_only.camera_demand_evaluations[
        static_cast<std::size_t>(tetra::CameraLodZone::predicted)]==0U);
  const auto predicted=tetra::plan_adaptation(
      mesh,terrain,camera,28.0,9,configuration,0,&cache);
  CHECK(predicted.camera_demand_evaluations[
        static_cast<std::size_t>(tetra::CameraLodZone::predicted)]>0U);
  CHECK(predicted.camera_demand_pose_changed);
}

TEST_CASE("recent demand expires deterministically and releases packed metadata") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::guarded_recent;
  configuration.recent_retention_epochs=2U;
  tetra::AdaptationPlanningCache cache;
  tetra::Camera camera;
  const auto apply=[&]{
    const auto plan=tetra::plan_adaptation(
        mesh,terrain,camera,28.0,9,configuration,0,&cache);
    const auto result=tetra::commit_adaptation(
        mesh,plan,configuration,0,&cache);
    REQUIRE((result.status==tetra::AdaptationCommitStatus::committed||
             result.status==tetra::AdaptationCommitStatus::no_change));
  };
  apply();
  REQUIRE(std::ranges::any_of(cache.camera_temporal_layers,[](const auto& layer){
    return !layer.addresses.empty();
  }));
  camera.forward={0.0,0.0,1.0};
  for(std::size_t step=0;step<3U;++step){
    camera.position.x+=0.01;
    apply();
  }
  CHECK(cache.camera_demand_epoch==4U);
  CHECK(std::ranges::all_of(cache.camera_temporal_layers,[](const auto& layer){
    return layer.addresses.empty()&&layer.last_demand_epochs.empty();
  }));
}

TEST_CASE("geometric camera error summaries are conservative and distance ordered") {
  auto near_mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  auto far_mesh=near_mesh;
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::guarded;
  configuration.camera_lod_metric=tetra::CameraLodMetric::geometric_error;
  configuration.candidate_traversal=tetra::CandidateTraversal::hierarchy_bounds;
  tetra::Camera near_camera;
  tetra::Camera far_camera=near_camera;
  far_camera.position.z=12.0;
  tetra::AdaptationPlanningCache near_cache,far_cache;
  const auto near=tetra::plan_adaptation(
      near_mesh,terrain,near_camera,28.0,9,configuration,0,&near_cache);
  const auto far=tetra::plan_adaptation(
      far_mesh,terrain,far_camera,28.0,9,configuration,0,&far_cache);
  CHECK(near.requested_splits>=far.requested_splits);
  REQUIRE_FALSE(near_cache.layers.empty());
  for(const auto& layer:near_cache.layers){
    CHECK(layer.geometric_error_bound.size()==layer.addresses.size());
    CHECK(std::ranges::all_of(layer.geometric_error_bound,[](double error){
      return std::isfinite(error)&&error>0.0;
    }));
  }
}

TEST_CASE("camera complexity controller changes only bounded soft-zone quality") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::guarded_recent;
  configuration.complexity_target_owners=1U;
  configuration.complexity_adjustment=0.5;
  configuration.maximum_soft_quality_multiplier=2.0;
  tetra::AdaptationPlanningCache cache;
  tetra::Camera camera;

  auto plan=tetra::plan_adaptation(
      mesh,terrain,camera,28.0,9,configuration,0,&cache);
  CHECK(plan.camera_soft_quality_multiplier==doctest::Approx(1.5));
  auto result=tetra::commit_adaptation(mesh,plan,configuration,0,&cache);
  REQUIRE((result.status==tetra::AdaptationCommitStatus::committed||
           result.status==tetra::AdaptationCommitStatus::no_change));
  CHECK(cache.camera_soft_quality_multiplier==doctest::Approx(1.5));

  camera.position.x+=0.1;
  plan=tetra::plan_adaptation(
      mesh,terrain,camera,28.0,9,configuration,0,&cache);
  CHECK(plan.camera_soft_quality_multiplier==doctest::Approx(2.0));
  result=tetra::commit_adaptation(mesh,plan,configuration,0,&cache);
  REQUIRE((result.status==tetra::AdaptationCommitStatus::committed||
           result.status==tetra::AdaptationCommitStatus::no_change));
  CHECK(cache.camera_soft_quality_multiplier<=
        configuration.maximum_soft_quality_multiplier);

  configuration.complexity_target_owners=1000000U;
  camera.position.x+=0.1;
  plan=tetra::plan_adaptation(
      mesh,terrain,camera,28.0,9,configuration,0,&cache);
  CHECK(plan.camera_soft_quality_multiplier<
        cache.camera_soft_quality_multiplier);
  CHECK(plan.camera_soft_quality_multiplier>=1.0);
}

TEST_CASE("camera complexity controller remains bounded through adversarial demand changes") {
  auto mesh=tetra::TetMesh::make_unit_cube(
      tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra::Camera camera;
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::guarded_recent;
  configuration.complexity_target_owners=1U;
  configuration.complexity_adjustment=0.1;
  configuration.maximum_soft_quality_multiplier=1.5;
  tetra::AdaptationPlanningCache cache;
  std::uint64_t field_revision{};
  const auto apply=[&]{
    const double previous=cache.camera_soft_quality_multiplier;
    const auto plan=tetra::plan_adaptation(
        mesh,terrain,camera,28.0,9,configuration,field_revision,&cache);
    CHECK(std::isfinite(plan.camera_soft_quality_multiplier));
    CHECK(plan.camera_soft_quality_multiplier>=1.0);
    CHECK(plan.camera_soft_quality_multiplier<=
          configuration.maximum_soft_quality_multiplier);
    CHECK(plan.camera_soft_quality_multiplier<=
          previous*(1.0+configuration.complexity_adjustment)+1.0e-12);
    const auto result=tetra::commit_adaptation(
        mesh,plan,configuration,field_revision,&cache);
    REQUIRE((result.status==tetra::AdaptationCommitStatus::committed||
             result.status==tetra::AdaptationCommitStatus::no_change));
  };

  apply();
  terrain.kind=tetra::ImplicitShapeKind::sphere; // rough to smooth
  ++field_revision;
  camera.position.x+=0.02;
  apply();
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain; // smooth to rough
  ++field_revision;
  camera.forward={1.0,0.0,0.0}; // rapid turn
  apply();
  camera.position={20.0,20.0,20.0}; // teleport
  camera.forward={-1.0,-1.0,-1.0};
  apply();
  const double after_motion=cache.camera_soft_quality_multiplier;

  // An unchanged pose is inside the dead band in time: it cannot chatter the
  // controller even if topology work remains queued.
  apply();
  CHECK(cache.camera_soft_quality_multiplier==doctest::Approx(after_motion));
}

TEST_CASE("successive rotation drag frames finish coarsening after release") {
  auto mesh=tetra::TetMesh::make_unit_cube(tetra::SubdivisionMethod::bcc_red_green);
  tetra::Sphere terrain;
  terrain.kind=tetra::ImplicitShapeKind::perlin_terrain;
  tetra_viewer::LodCameraPose pose;
  pose.position={0.0,1.0,0.5};
  pose.forward={0.7071067811865475,-0.7071067811865475,0.0};
  tetra::Camera camera;
  pose.apply(camera);
  tetra::AdaptationPlanningCache planning_cache;
  tetra::AdaptationConfiguration configuration;
  configuration.camera_lod_policy=tetra::CameraLodPolicy::exact_frustum;
  configuration.camera_lod_metric=tetra::CameraLodMetric::projected_diameter;
  static_cast<void>(tetra::refine_to_sphere(
      mesh,terrain,camera,28.0*configuration.split_hysteresis,9));
  const auto detailed_owners=mesh.logical_cut().owners.size();
  std::size_t accepted_merges{};
  for(std::size_t drag_frame=0;drag_frame<8U;++drag_frame){
    pose.rotate(tetra_viewer::CameraGizmoAxis::z,std::acos(-1.0)/8.0);
    pose.apply(camera);
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,9,configuration,0,&planning_cache);
    REQUIRE((result.status==tetra::AdaptationCommitStatus::committed||
             result.status==tetra::AdaptationCommitStatus::no_change));
    accepted_merges+=result.accepted_merges;
  }
  bool converged=false;
  for(std::size_t frame=0;frame<128U;++frame){
    const auto result=tetra::adapt_to_surface(
        mesh,terrain,camera,28.0,9,configuration,0,&planning_cache);
    if(result.status==tetra::AdaptationCommitStatus::no_change){converged=true;break;}
    REQUIRE(result.status==tetra::AdaptationCommitStatus::committed);
    accepted_merges+=result.accepted_merges;
  }
  CHECK(converged);
  CHECK(accepted_merges>0U);
  CHECK(mesh.logical_cut().owners.size()<detailed_owners);
  CHECK(mesh.has_conforming_active_faces());
}

TEST_CASE("successive terrain camera rotations terminate with a conforming BCC cut") {
  std::ostringstream output,errors;
  REQUIRE(tetra_viewer::run_script(
      "set-shape=perlin-terrain,set-camera=0:1:0.5,"
      "set-camera-direction=-0.60439:0.79502:0.05149,"
      "set-camera-direction=-0.75710:0.21744:-0.61605,"
      "set-camera-direction=-0.88569:0.14620:-0.44065,"
      "set-camera-direction=-0.85249:-0.16423:-0.49628,"
      "set-camera-direction=-0.51411:-0.63082:0.58117,"
      "set-camera-direction=-0.32578:0.67589:0.66109,"
      "set-camera-direction=0.11090:0.98514:0.13118,"
      "set-camera-direction=0.28419:-0.43034:0.85677,validate,stats",
      output,errors)==0);
  CHECK(errors.str().empty());
  CHECK(output.str().find("\"event\":\"validation\",\"valid\":true")!=
        std::string::npos);
  CHECK(output.str().find("\"lod_direction\":[0.284,-0.430,0.857]")!=
        std::string::npos);
}

TEST_CASE("headless renderer writes a deterministic comparison image") {
  const auto path = std::filesystem::temp_directory_path() / "tetra-viewer-headless-test.ppm";
  std::filesystem::remove(path);
  std::ostringstream output;
  std::ostringstream errors;
  const std::string script = "set-camera=1.9:1.6:2.25,set-maximum-depth=3,"
      "set-method=maubach-halfedge-24,render-image=" + path.string();
  CHECK(tetra_viewer::run_script(script, output, errors) == 0);
  CHECK(errors.str().empty());
  CHECK(output.str().find("\"event\":\"image\"") != std::string::npos);
  CHECK(std::filesystem::file_size(path) > 800 * 800 * 3);
  std::ifstream image(path, std::ios::binary);
  std::array<char, 2> magic{};
  image.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  CHECK(magic == std::array<char, 2>{{'P', '6'}});
  image.close();
  std::filesystem::remove(path);
}

TEST_CASE("GPU capture conversion canonicalizes channel order and row direction") {
  // Top row is red/green and bottom row is blue/white in canonical RGB order.
  const std::array<std::uint8_t,16> rgba{
      255,0,0,9, 0,255,0,9, 0,0,255,9, 255,255,255,9};
  const std::array<std::uint8_t,16> bgra_bottom_up{
      255,0,0,9, 255,255,255,9, 0,0,255,9, 0,255,0,9};
  tetra_viewer::Rgb8Image rgba_image,bgra_image;
  std::string error;
  REQUIRE(tetra_viewer::make_rgb8_image(
      rgba,2U,2U,false,false,rgba_image,error));
  REQUIRE(tetra_viewer::make_rgb8_image(
      bgra_bottom_up,2U,2U,true,true,bgra_image,error));
  CHECK(rgba_image.pixels==bgra_image.pixels);
  CHECK(rgba_image.pixels==std::vector<std::uint8_t>{
      255,0,0,0,255,0,0,0,255,255,255,255});
  CHECK(tetra_viewer::rgb8_hash(rgba_image)==
        tetra_viewer::rgb8_hash(bgra_image));

  tetra_viewer::Rgb8Image invalid;
  CHECK_FALSE(tetra_viewer::make_rgb8_image(
      std::span<const std::uint8_t>{rgba}.first(15),2U,2U,false,false,
      invalid,error));
  CHECK(error.find("source size")!=std::string::npos);

  CHECK(tetra_viewer::parse_pixel_extent("960x540")==
        std::optional{std::array<std::uint32_t,2>{960U,540U}});
  CHECK_FALSE(tetra_viewer::parse_pixel_extent("960X540"));
  CHECK_FALSE(tetra_viewer::parse_pixel_extent("0x540"));
  CHECK_FALSE(tetra_viewer::parse_pixel_extent("960x9000"));

  const std::array<double,5> samples{9.0,1.0,5.0,3.0,7.0};
  const auto summary=tetra_viewer::summarize_samples(samples);
  REQUIRE(summary);
  CHECK(summary->minimum==1.0);
  CHECK(summary->median==5.0);
  CHECK(summary->percentile_95==doctest::Approx(8.6));
  CHECK(summary->maximum==9.0);
  CHECK_FALSE(tetra_viewer::summarize_samples(
      std::array<double,1>{std::numeric_limits<double>::infinity()}));
}

TEST_CASE("RGB image oracle round trips PPM and supports masked comparisons") {
  tetra_viewer::Rgb8Image reference{
      2U,2U,{0,0,0, 32,64,96, 128,160,192, 255,255,255}};
  const auto path=std::filesystem::temp_directory_path()/
      "tetra-atmosphere-image-oracle.ppm";
  std::filesystem::remove(path);
  std::string error;
  REQUIRE(tetra_viewer::write_ppm(path.string(),reference,error));
  tetra_viewer::Rgb8Image loaded;
  REQUIRE(tetra_viewer::read_ppm(path.string(),loaded,error));
  CHECK(loaded.width==reference.width);
  CHECK(loaded.height==reference.height);
  CHECK(loaded.pixels==reference.pixels);
  const auto analysis=tetra_viewer::analyse_rgb8_image(loaded);
  CHECK(analysis.sampled_pixels==4U);
  CHECK(analysis.minimum==std::array<std::uint8_t,3>{0,0,0});
  CHECK(analysis.maximum==std::array<std::uint8_t,3>{255,255,255});
  CHECK(analysis.black_fraction==doctest::Approx(0.25));
  CHECK(analysis.clipped_fraction==doctest::Approx(0.25));
  CHECK(analysis.luminance_standard_deviation>0.3);

  auto candidate=reference;
  candidate.pixels[0]=255U;
  tetra_viewer::Rgb8ImageComparison comparison;
  const std::array<std::uint8_t,4> exclude_first{0,1,1,1};
  REQUIRE(tetra_viewer::compare_rgb8_images(
      reference,candidate,comparison,error,exclude_first));
  CHECK(comparison.changed_fraction==0.0);
  const std::array<std::uint8_t,4> only_first{1,0,0,0};
  REQUIRE(tetra_viewer::compare_rgb8_images(
      reference,candidate,comparison,error,only_first));
  CHECK(comparison.changed_fraction==1.0);
  CHECK(comparison.maximum_absolute_error==
        std::array<std::uint8_t,3>{255,0,0});
  CHECK(comparison.mean_absolute_error[0]==doctest::Approx(1.0));
  std::vector<std::uint8_t> depth_mask;
  REQUIRE(tetra_viewer::make_reversed_depth_mask(
      std::array<float,4>{0.0F,1.0F,0.5F,1.0e-9F},2U,2U,
      depth_mask,error));
  CHECK(depth_mask==std::vector<std::uint8_t>{0,255,255,0});
  std::vector<std::uint8_t> clear_mask;
  REQUIRE(tetra_viewer::make_complement_mask(
      depth_mask,clear_mask,error));
  CHECK(clear_mask==std::vector<std::uint8_t>{255,0,0,255});
  std::vector<std::uint8_t> isolated_geometry(25U,0U),silhouette;
  isolated_geometry[12U]=255U;
  REQUIRE(tetra_viewer::make_silhouette_band_mask(
      isolated_geometry,5U,5U,1U,silhouette,error));
  CHECK(std::ranges::count(silhouette,255U)==9U);
  CHECK(silhouette[12U]==255U);
  CHECK(silhouette.front()==0U);
  std::vector<std::uint8_t> horizon;
  REQUIRE(tetra_viewer::make_horizontal_band_mask(
      5U,10U,0.5,0.2,horizon,error));
  CHECK(std::ranges::count(horizon,255U)==10U);
  CHECK(horizon[3U*5U]==0U);
  CHECK(horizon[4U*5U]==255U);
  CHECK(horizon[5U*5U]==255U);
  CHECK(horizon[6U*5U]==0U);
  CHECK_FALSE(tetra_viewer::make_silhouette_band_mask(
      isolated_geometry,5U,5U,0U,silhouette,error));
  CHECK_FALSE(tetra_viewer::make_horizontal_band_mask(
      5U,10U,0.5,0.0,horizon,error));
  auto invalid_depth=std::array<float,4>{0.0F,1.0F,0.5F,2.0F};
  CHECK_FALSE(tetra_viewer::make_reversed_depth_mask(
      invalid_depth,2U,2U,depth_mask,error));
  CHECK(error.find("out-of-range")!=std::string::npos);
  const auto mask_path=std::filesystem::temp_directory_path()/
      "tetra-atmosphere-image-oracle.pgm";
  REQUIRE(tetra_viewer::make_reversed_depth_mask(
      std::array<float,4>{0.0F,1.0F,0.5F,1.0e-9F},2U,2U,
      depth_mask,error));
  REQUIRE(tetra_viewer::write_pgm(
      mask_path.string(),2U,2U,depth_mask,error));
  CHECK(std::filesystem::file_size(mask_path)==15U);
  std::filesystem::remove(mask_path);
  std::filesystem::remove(path);
}

TEST_CASE("X cut at one is pixel-identical to disabled and never changes topology") {
  const auto render=[&](std::string_view cut,std::string_view suffix){
    const auto path=std::filesystem::temp_directory_path()/
        ("tetra-x-cut-equivalence-"+std::string(suffix)+".ppm");
    std::filesystem::remove(path);
    std::ostringstream output,errors;
    const std::string script="set-maximum-depth=6,set-shape=perlin-terrain,"
        "set-camera=1.9:1.6:2.25,set-volume-connection=hierarchy-cells,"
        "set-x-cut="+std::string(cut)+",render-image="+path.string()+",stats";
    REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
    REQUIRE(errors.str().empty());
    std::ifstream image(path,std::ios::binary);
    REQUIRE(image.good());
    std::uint64_t image_hash=1469598103934665603ULL;
    for(char byte{};image.get(byte);){
      image_hash^=static_cast<unsigned char>(byte);
      image_hash*=1099511628211ULL;
    }
    image.close();std::filesystem::remove(path);
    const auto stats=output.str().rfind("\"event\":\"stats\"");
    REQUIRE(stats!=std::string::npos);
    const auto field=[&](std::string_view name){
      const auto position=output.str().find(
          std::string{"\""}+std::string{name}+"\":",stats);
      REQUIRE(position!=std::string::npos);
      const auto begin=output.str().find(':',position)+1U;
      return output.str().substr(
          begin,output.str().find_first_of(",}",begin)-begin);
    };
    return std::tuple{image_hash,field("logical_cut_hash"),
                      field("conforming_volume_hash")};
  };
  const auto disabled=render("off","off");
  const auto complete=render("1.0","one");
  CHECK(complete==disabled);

  for(const auto cut:{"0.0","0.2","0.5","0.8"}){
    const auto sample=render(cut,cut);
    CHECK(std::get<1>(sample)==std::get<1>(disabled));
    CHECK(std::get<2>(sample)==std::get<2>(disabled));
  }
}

TEST_CASE("default terrain cutaway visual baselines remain stable for both transitions") {
  const auto render_hash=[](std::string_view strategy){
    const auto path=std::filesystem::temp_directory_path()/
        ("tetra-terrain-cutaway-"+std::string(strategy)+".ppm");
    std::filesystem::remove(path);
    std::ostringstream output,errors;
    const std::string script="set-maximum-depth=8,set-shape=perlin-terrain,"
        "set-camera=1.9:1.6:2.25,set-volume-connection=hierarchy-cells,"
        "set-x-cut=0.5,set-transition-strategy="+std::string(strategy)+
        ",render-image="+path.string();
    REQUIRE(tetra_viewer::run_script(script,output,errors)==0);
    REQUIRE(errors.str().empty());
    std::ifstream image(path,std::ios::binary);
    REQUIRE(image.good());
    std::uint64_t hash=1469598103934665603ULL;
    for(char byte{};image.get(byte);){
      hash^=static_cast<unsigned char>(byte);
      hash*=1099511628211ULL;
    }
    image.close();std::filesystem::remove(path);
    return hash;
  };
  // Exact-on-surface hierarchy endpoints remain at the endpoint instead of
  // being walked to the opposite side of the edge by generic bisection.
  // This baseline uses the same positive-height Vulkan screen basis as the
  // interactive renderer; the previous CPU-only image was horizontally
  // mirrored relative to Vulkan.
  constexpr std::uint64_t expected=9289684079062712499ULL;
  CHECK(render_hash("crystalline-restricted")==expected);
  CHECK(render_hash("complete-minimal")==expected);
  CHECK(std::filesystem::exists(
      "tests/visual_baselines/terrain-cutaway-crystalline.png"));
  CHECK(std::filesystem::exists(
      "tests/visual_baselines/terrain-cutaway-complete.png"));
  CHECK(std::filesystem::exists(
      "tests/visual_baselines/terrain-lod-strategy-comparison.png"));
}

TEST_CASE("headless viewer script rejects malformed and unknown commands") {
  std::ostringstream output;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_script("set-radius=not-a-number", output, errors) == 2);
  CHECK(errors.str().find("value outside the supported range") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("refine-once,,stats", output, errors) == 2);
  CHECK(errors.str().find("empty command") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("explode", output, errors) == 2);
  CHECK(errors.str().find("unknown command") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-method=made-up", output, errors) == 2);
  CHECK(errors.str().find("unknown subdivision method") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-shape=made-up",output,errors)==2);
  CHECK(errors.str().find("unknown implicit shape")!=std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-material-rule=made-up", output, errors) == 2);
  CHECK(errors.str().find("unknown material rule") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-surface-method=made-up", output, errors) == 2);
  CHECK(errors.str().find("unknown surface method") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-volume-connection=made-up", output, errors) == 2);
  CHECK(errors.str().find("unknown volume connection method") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-stencil-construction=made-up",output,errors)==2);
  CHECK(errors.str().find("unknown stencil construction")!=std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-stencil-objective=made-up",output,errors)==2);
  CHECK(errors.str().find("unknown stencil objective")!=std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-camera=1:2", output, errors) == 2);
  CHECK(errors.str().find("three finite colon-separated values") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-camera-direction=0:0:0",output,errors)==2);
  CHECK(errors.str().find("direction must be nonzero")!=std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-maximum-depth=33", output, errors) == 2);
  CHECK(errors.str().find("outside the supported range") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-x-cut=2", output, errors) == 2);
  CHECK(errors.str().find("outside the supported range") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-volume-edges=maybe", output, errors) == 2);
  CHECK(errors.str().find("volume edges must be on or off") != std::string::npos);

  output.str({});
  output.clear();
  errors.str({});
  errors.clear();
  CHECK(tetra_viewer::run_script("set-solid-volume=maybe", output, errors) == 2);
  CHECK(errors.str().find("solid volume must be on or off") != std::string::npos);
}

TEST_CASE("atmosphere quality profiles are ordered and default stays budgeted") {
  const auto low=tetra_viewer::atmosphere_quality_settings(
      tetra_viewer::AtmosphereQuality::low);
  const auto standard=tetra_viewer::atmosphere_quality_settings(
      tetra_viewer::AtmosphereQuality::standard);
  const auto high=tetra_viewer::atmosphere_quality_settings(
      tetra_viewer::AtmosphereQuality::high);
  CHECK(low.transmittance_width<standard.transmittance_width);
  CHECK(standard.transmittance_width<high.transmittance_width);
  CHECK(low.aerial_depth<standard.aerial_depth);
  CHECK(standard.aerial_depth<high.aerial_depth);
  CHECK(low.sky_width==standard.sky_width);
  CHECK(low.sky_height==standard.sky_height);
  CHECK(standard.sky_width<high.sky_width);
  CHECK(low.shadow_resolution<standard.shadow_resolution);
  CHECK(standard.shadow_resolution<high.shadow_resolution);
  CHECK(low.atmosphere_shadow_resolution<
        standard.atmosphere_shadow_resolution);
  CHECK(standard.atmosphere_shadow_resolution<
        high.atmosphere_shadow_resolution);
  CHECK(standard.atmosphere_shadow_resolution<standard.shadow_resolution);
  constexpr std::size_t buffered_frames=3U;
  const std::size_t default_shadow_bytes=buffered_frames*
      tetra_viewer::shadow_map_layer_count*standard.shadow_resolution*
      standard.shadow_resolution*sizeof(float);
  CHECK(default_shadow_bytes<64U*1024U*1024U);
}

TEST_CASE("atmosphere transport defaults to faithful with baseline available") {
  using tetra_viewer::AtmosphereTransport;
  CHECK(tetra_viewer::default_atmosphere_transport==
        AtmosphereTransport::faithful_hillaire);
  CHECK(tetra_viewer::parse_atmosphere_transport("qualified-baseline")==
        AtmosphereTransport::qualified_baseline);
  CHECK(tetra_viewer::parse_atmosphere_transport("faithful-hillaire")==
        AtmosphereTransport::faithful_hillaire);
  CHECK_FALSE(tetra_viewer::parse_atmosphere_transport("approximately-blue"));
  CHECK(tetra_viewer::atmosphere_transport_name(
            AtmosphereTransport::qualified_baseline)=="qualified-baseline");
  CHECK(tetra_viewer::atmosphere_transport_name(
            AtmosphereTransport::faithful_hillaire)=="faithful-hillaire");
}

TEST_CASE("atmosphere lookup revisions dispatch only their dependencies") {
  using namespace tetra_viewer;
  AtmosphereLookupRevisions state{
      .optical={1}, .scattering={2}, .sun={3}, .camera_position={4},
      .sky_position={8}, .camera_orientation={5}, .shadow={6},
      .render_origin={7}};
  auto plan=atmosphere_dispatch_plan(std::nullopt,state,
      AtmosphereTransport::qualified_baseline);
  CHECK(plan.transmittance);
  CHECK(plan.multiple_scattering);
  CHECK(plan.sky_view);
  CHECK(plan.sky_irradiance);
  CHECK(plan.aerial_perspective);
  CHECK_FALSE(plan.long_shadow);

  plan=atmosphere_dispatch_plan(state,state,
      AtmosphereTransport::qualified_baseline);
  CHECK_FALSE(plan.transmittance);
  CHECK_FALSE(plan.multiple_scattering);
  CHECK_FALSE(plan.sky_view);
  CHECK_FALSE(plan.sky_irradiance);
  CHECK_FALSE(plan.aerial_perspective);
  CHECK_FALSE(plan.long_shadow);

  auto rotated=state;
  rotated.camera_orientation.value++;
  const auto baseline_rotation=atmosphere_dispatch_plan(state,rotated,
      AtmosphereTransport::qualified_baseline);
  CHECK(baseline_rotation.sky_view);
  CHECK(baseline_rotation.aerial_perspective);
  const auto faithful_rotation=atmosphere_dispatch_plan(state,rotated,
      AtmosphereTransport::faithful_hillaire);
  CHECK_FALSE(faithful_rotation.transmittance);
  CHECK_FALSE(faithful_rotation.multiple_scattering);
  CHECK_FALSE(faithful_rotation.sky_view);
  CHECK_FALSE(faithful_rotation.sky_irradiance);
  CHECK(faithful_rotation.aerial_perspective);
  CHECK(faithful_rotation.long_shadow);

  auto moved=state;
  moved.camera_position.value++;
  plan=atmosphere_dispatch_plan(state,moved,
      AtmosphereTransport::faithful_hillaire);
  CHECK_FALSE(plan.transmittance);
  CHECK_FALSE(plan.multiple_scattering);
  CHECK_FALSE(plan.sky_view);
  CHECK_FALSE(plan.sky_irradiance);
  CHECK(plan.aerial_perspective);
  CHECK(plan.long_shadow);

  auto sky_moved=state;
  sky_moved.sky_position.value++;
  plan=atmosphere_dispatch_plan(state,sky_moved,
      AtmosphereTransport::faithful_hillaire);
  CHECK(plan.sky_view);
  CHECK(plan.sky_irradiance);
  CHECK_FALSE(plan.aerial_perspective);
  CHECK_FALSE(plan.long_shadow);

  auto relit=state;
  relit.sun.value++;
  plan=atmosphere_dispatch_plan(state,relit,
      AtmosphereTransport::faithful_hillaire);
  CHECK_FALSE(plan.transmittance);
  CHECK_FALSE(plan.multiple_scattering);
  CHECK(plan.sky_view);
  CHECK(plan.aerial_perspective);
  CHECK(plan.long_shadow);

  auto reshaded=state;
  reshaded.shadow.value++;
  plan=atmosphere_dispatch_plan(state,reshaded,
      AtmosphereTransport::faithful_hillaire);
  CHECK_FALSE(plan.transmittance);
  CHECK_FALSE(plan.multiple_scattering);
  CHECK_FALSE(plan.sky_view);
  CHECK(plan.aerial_perspective);
  CHECK(plan.long_shadow);

  auto rebased=state;
  rebased.render_origin.value++;
  plan=atmosphere_dispatch_plan(state,rebased,
      AtmosphereTransport::faithful_hillaire);
  CHECK_FALSE(plan.transmittance);
  CHECK_FALSE(plan.multiple_scattering);
  CHECK_FALSE(plan.sky_view);
  CHECK_FALSE(plan.sky_irradiance);
  CHECK(plan.aerial_perspective);
  CHECK(plan.long_shadow);

  auto material=state;
  material.optical.value++;
  plan=atmosphere_dispatch_plan(state,material,
      AtmosphereTransport::faithful_hillaire);
  CHECK(plan.transmittance);
  CHECK(plan.multiple_scattering);
  CHECK(plan.sky_view);
  CHECK(plan.aerial_perspective);
  CHECK(plan.long_shadow);

  auto scattering=state;
  scattering.scattering.value++;
  plan=atmosphere_dispatch_plan(state,scattering,
      AtmosphereTransport::faithful_hillaire);
  CHECK_FALSE(plan.transmittance);
  CHECK(plan.multiple_scattering);
  CHECK(plan.sky_view);
  CHECK(plan.aerial_perspective);
  CHECK(plan.long_shadow);
}

TEST_CASE("atmosphere lookup snapshots reject incompatible generations") {
  using namespace tetra_viewer;
  const auto parameters=atmosphere_preset(AtmospherePreset::gameplay_planet);
  const auto material=atmosphere_material_snapshot(
      parameters,AtmosphereTransport::faithful_hillaire);
  AtmosphereLookupRevisions frame{
      .optical=material.optical,
      .scattering=material.scattering,
      .sun={3},
      .camera_position={4},
      .sky_position={5},
      .camera_orientation={6},
      .shadow={7},
      .render_origin={8}};
  const auto initial_plan=atmosphere_dispatch_plan(
      std::nullopt,frame,material.transport);
  const auto initial=advance_atmosphere_lookup_snapshots(
      std::nullopt,material,frame,initial_plan);
  CHECK(atmosphere_validation_snapshot(material,initial,frame).compatible());

  auto rotated=frame;
  ++rotated.camera_orientation.value;
  const auto rotation_plan=atmosphere_dispatch_plan(
      frame,rotated,material.transport);
  CHECK_FALSE(rotation_plan.sky_view);
  CHECK(rotation_plan.aerial_perspective);
  CHECK(rotation_plan.long_shadow);
  const auto after_rotation=advance_atmosphere_lookup_snapshots(
      initial,material,rotated,rotation_plan);
  CHECK(atmosphere_validation_snapshot(
            material,after_rotation,rotated).compatible());
  REQUIRE(initial.lighting);
  REQUIRE(after_rotation.lighting);
  REQUIRE(initial.view);
  REQUIRE(after_rotation.view);
  CHECK(after_rotation.lighting->camera_orientation==
        initial.lighting->camera_orientation);
  CHECK(after_rotation.view->camera_orientation==rotated.camera_orientation);
  CHECK(initial.view->camera_orientation==frame.camera_orientation);

  auto relit=rotated;
  ++relit.sun.value;
  const AtmosphereDispatchPlan broken_plan{};
  const auto stale=advance_atmosphere_lookup_snapshots(
      after_rotation,material,relit,broken_plan);
  const auto stale_validation=atmosphere_validation_snapshot(
      material,stale,relit);
  CHECK_FALSE(stale_validation.compatible());
  REQUIRE(stale_validation.incompatibility);
  CHECK(stale_validation.incompatibility->find("lighting")!=
        std::string::npos);

  const auto relight_plan=atmosphere_dispatch_plan(
      rotated,relit,material.transport);
  const auto relighted=advance_atmosphere_lookup_snapshots(
      after_rotation,material,relit,relight_plan);
  CHECK(atmosphere_validation_snapshot(material,relighted,relit).compatible());

  const auto baseline_material=atmosphere_material_snapshot(
      parameters,AtmosphereTransport::qualified_baseline);
  CHECK_FALSE(atmosphere_validation_snapshot(
      baseline_material,relighted,relit).compatible());
  const auto switched=advance_atmosphere_lookup_snapshots(
      std::nullopt,baseline_material,relit,
      atmosphere_dispatch_plan(std::nullopt,relit,
          baseline_material.transport));
  CHECK(atmosphere_validation_snapshot(
            baseline_material,switched,relit).compatible());
}

TEST_CASE("faithful full sky ignores sub-resolution camera motion") {
  const auto parameters=tetra_viewer::atmosphere_preset(
      tetra_viewer::AtmospherePreset::gameplay_planet);
  const double radius=parameters.ground_radius_metres+1.8;
  const tetra::Vec3 origin{0.0,radius,0.0};
  const auto baseline=tetra_viewer::atmosphere_sky_position_revision(
      origin,parameters);
  CHECK(tetra_viewer::atmosphere_sky_position_revision(
            {0.05,radius+0.05,0.0},parameters)==baseline);
  CHECK(tetra_viewer::atmosphere_sky_position_revision(
            {1.0,radius,0.0},parameters)==baseline);
  CHECK(tetra_viewer::atmosphere_sky_position_revision(
            {0.0,radius+2.0,0.0},parameters)!=baseline);
  CHECK(tetra_viewer::atmosphere_sky_position_revision(
            {100.0,radius,0.0},parameters)!=baseline);
}

TEST_CASE("atmosphere optical and scattering hashes separate dependencies") {
  auto parameters=tetra_viewer::atmosphere_preset(
      tetra_viewer::AtmospherePreset::earth);
  const auto optical=tetra_viewer::atmosphere_optical_hash(parameters);
  const auto scattering=tetra_viewer::atmosphere_scattering_hash(parameters);

  auto exposure=parameters;
  exposure.solar_irradiance[0]*=1.1;
  CHECK(tetra_viewer::atmosphere_optical_hash(exposure)==optical);
  CHECK(tetra_viewer::atmosphere_scattering_hash(exposure)!=scattering);

  auto albedo=parameters;
  albedo.ground_albedo[1]+=0.1;
  CHECK(tetra_viewer::atmosphere_optical_hash(albedo)==optical);
  CHECK(tetra_viewer::atmosphere_scattering_hash(albedo)!=scattering);

  auto density=parameters;
  density.rayleigh_scale_height_metres+=1.0;
  CHECK(tetra_viewer::atmosphere_optical_hash(density)!=optical);
  CHECK(tetra_viewer::atmosphere_scattering_hash(density)!=scattering);
}

TEST_CASE("atmosphere presets are valid deterministic physical snapshots") {
  using tetra_viewer::AtmospherePreset;
  const std::array presets{
      AtmospherePreset::gameplay_planet, AtmospherePreset::earth,
      AtmospherePreset::mars_like,
      AtmospherePreset::dense_haze, AtmospherePreset::nearly_airless};
  std::set<std::uint64_t> hashes;
  for (const auto preset : presets) {
    const auto parameters = tetra_viewer::atmosphere_preset(preset);
    CHECK_FALSE(tetra_viewer::validate_atmosphere(parameters).has_value());
    CHECK(hashes.insert(
        tetra_viewer::atmosphere_parameter_hash(parameters)).second);
    CHECK(tetra_viewer::serialize_atmosphere_parameters(parameters) ==
          tetra_viewer::serialize_atmosphere_parameters(parameters));
    CHECK(tetra_viewer::parse_atmosphere_preset(
              tetra_viewer::atmosphere_preset_name(preset)) == preset);
  }
  CHECK_FALSE(tetra_viewer::parse_atmosphere_preset("cloud-city"));
}

TEST_CASE("gameplay planet preserves Earth-like vertical optical depth") {
  using tetra_viewer::AtmospherePreset;
  const auto earth=tetra_viewer::atmosphere_preset(AtmospherePreset::earth);
  const auto game=tetra_viewer::atmosphere_preset(
      AtmospherePreset::gameplay_planet);
  CHECK(game.ground_radius_metres==doctest::Approx(200'000.0));
  CHECK(game.atmosphere_height_metres==doctest::Approx(20'000.0));
  CHECK(tetra_viewer::default_world_atmosphere_preset==
        AtmospherePreset::gameplay_planet);
  CHECK(tetra_viewer::default_world_aerial_distance_metres==
        doctest::Approx(game.ground_radius_metres));
  CHECK(tetra_viewer::planetary_horizon_distance(
            game.ground_radius_metres,18.0)>2'000.0);
  CHECK(tetra_viewer::planetary_horizon_distance(
            game.ground_radius_metres,18.0)<3'000.0);
  CHECK(game.mie_scale_height_metres==doctest::Approx(30.0));
  CHECK(game.ground_albedo[0]==doctest::Approx(0.32));
  for(std::size_t channel=0;channel<3U;++channel){
    CHECK(game.rayleigh_scattering_per_metre[channel]*
              game.rayleigh_scale_height_metres==
          doctest::Approx(earth.rayleigh_scattering_per_metre[channel]*
              earth.rayleigh_scale_height_metres).epsilon(1.0e-12));
    CHECK(game.mie_scattering_per_metre[channel]*
              game.mie_scale_height_metres==
          doctest::Approx(earth.mie_scattering_per_metre[channel]*
              earth.mie_scale_height_metres).epsilon(1.0e-12));
    CHECK(game.mie_absorption_per_metre[channel]*
              game.mie_scale_height_metres==
          doctest::Approx(earth.mie_absorption_per_metre[channel]*
              earth.mie_scale_height_metres).epsilon(1.0e-12));
    CHECK(game.absorption_per_metre[channel]*
              game.absorption_half_width_metres==
          doctest::Approx(earth.absorption_per_metre[channel]*
              earth.absorption_half_width_metres).epsilon(1.0e-12));
    const double earth_mie_extinction=earth.mie_scattering_per_metre[channel]+
        earth.mie_absorption_per_metre[channel];
    CHECK(earth_mie_extinction==doctest::Approx(4.4e-6).epsilon(1.0e-12));
    CHECK(game.mie_scattering_per_metre[channel]/
              (game.mie_scattering_per_metre[channel]+
               game.mie_absorption_per_metre[channel])>0.90);
  }
}

TEST_CASE("atmosphere validation rejects unphysical snapshots transactionally") {
  auto parameters =
      tetra_viewer::atmosphere_preset(tetra_viewer::AtmospherePreset::earth);
  parameters.ground_radius_metres = 0.0;
  CHECK(tetra_viewer::validate_atmosphere(parameters));
  parameters = tetra_viewer::atmosphere_preset(
      tetra_viewer::AtmospherePreset::earth);
  parameters.mie_anisotropy = 1.0;
  CHECK(tetra_viewer::validate_atmosphere(parameters));
  parameters = tetra_viewer::atmosphere_preset(
      tetra_viewer::AtmospherePreset::earth);
  parameters.ground_albedo[1] = 1.01;
  CHECK(tetra_viewer::validate_atmosphere(parameters));
  parameters = tetra_viewer::atmosphere_preset(
      tetra_viewer::AtmospherePreset::earth);
  parameters.rayleigh_scattering_per_metre[0] =
      std::numeric_limits<double>::quiet_NaN();
  CHECK(tetra_viewer::validate_atmosphere(parameters));
}

TEST_CASE("atmosphere boundary rays remain stable from ground through space") {
  const auto parameters =
      tetra_viewer::atmosphere_preset(tetra_viewer::AtmospherePreset::earth);
  const double ground = parameters.ground_radius_metres;
  const double top = ground + parameters.atmosphere_height_metres;
  const auto upward = tetra_viewer::atmosphere_ray_segment(
      {0.0, ground, 0.0}, {0.0, 1.0, 0.0}, parameters);
  REQUIRE(upward);
  CHECK(upward->begin_metres == doctest::Approx(0.0));
  CHECK(upward->end_metres ==
        doctest::Approx(parameters.atmosphere_height_metres).epsilon(1.0e-11));

  const auto downward_from_space = tetra_viewer::atmosphere_ray_segment(
      {0.0, top + 20'000.0, 0.0}, {0.0, -1.0, 0.0}, parameters);
  REQUIRE(downward_from_space);
  CHECK(downward_from_space->begin_metres == doctest::Approx(20'000.0));
  CHECK(downward_from_space->end_metres ==
        doctest::Approx(120'000.0).epsilon(1.0e-11));
  CHECK_FALSE(tetra_viewer::atmosphere_ray_segment(
      {0.0, top + 1.0, 0.0}, {0.0, 1.0, 0.0}, parameters));

  const auto tangent = tetra_viewer::atmosphere_ray_segment(
      {top, 0.0, 0.0}, {0.0, 1.0, 0.0}, parameters);
  CHECK_FALSE(tangent);  // A zero-length tangent contributes no medium.
}

TEST_CASE("ground tangent contact remains medium while strict crossings stop") {
  const auto parameters=tetra_viewer::atmosphere_preset(
      tetra_viewer::AtmospherePreset::gameplay_planet);
  constexpr double altitude=1.0;
  const double radius=parameters.ground_radius_metres+altitude;
  const double horizon_cosine=-std::sqrt(
      altitude*(radius+parameters.ground_radius_metres)/(radius*radius));
  const auto direction=[](double cosine){
    return tetra::Vec3{std::sqrt(std::max(0.0,1.0-cosine*cosine)),
                       cosine,0.0};
  };
  const tetra::Vec3 origin{0.0,radius,0.0};
  const auto tangent=tetra_viewer::atmosphere_ray_segment(
      origin,direction(horizon_cosine),parameters);
  const auto crossing=tetra_viewer::atmosphere_ray_segment(
      origin,direction(horizon_cosine-0.001),parameters);
  REQUIRE(tangent);
  REQUIRE(crossing);
  CHECK(tangent->end_metres>50'000.0);
  CHECK(crossing->end_metres<1'000.0);
  const auto tangent_end=origin+direction(horizon_cosine)*tangent->end_metres;
  const auto crossing_end=
      origin+direction(horizon_cosine-0.001)*crossing->end_metres;
  const auto tangent_transmittance=tetra_viewer::atmosphere_transmittance(
      origin,tangent_end,parameters,512U);
  const auto crossing_transmittance=tetra_viewer::atmosphere_transmittance(
      origin,crossing_end,parameters,512U);
  for(std::size_t channel=0;channel<3U;++channel)
    CHECK(tangent_transmittance[channel]<crossing_transmittance[channel]);
}

TEST_CASE("compact planet shaders preserve metre-scale horizon precision") {
  constexpr float radius=200'000.0F;
  constexpr float altitude=1.0F;
  const float radial_distance=radius+altitude;
  const float exact=altitude*(2.0F*radius+altitude);
  const float stable=(radial_distance-radius)*(radial_distance+radius);
  const float cancelled=radial_distance*radial_distance-radius*radius;
  CHECK(stable==doctest::Approx(exact).epsilon(1.0e-6));
  CHECK(std::abs(cancelled-exact)>100.0F);
}

TEST_CASE("atmosphere densities phases and transmittance obey analytic limits") {
  const auto parameters =
      tetra_viewer::atmosphere_preset(tetra_viewer::AtmospherePreset::earth);
  CHECK(tetra_viewer::atmosphere_rayleigh_density(0.0, parameters) ==
        doctest::Approx(1.0));
  CHECK(tetra_viewer::atmosphere_mie_density(0.0, parameters) ==
        doctest::Approx(1.0));
  CHECK(tetra_viewer::atmosphere_rayleigh_density(
            parameters.rayleigh_scale_height_metres, parameters) ==
        doctest::Approx(std::exp(-1.0)));
  CHECK(tetra_viewer::atmosphere_absorption_density(
            parameters.absorption_peak_altitude_metres, parameters) ==
        doctest::Approx(1.0));
  CHECK(tetra_viewer::atmosphere_rayleigh_density(-1.0, parameters) == 0.0);
  CHECK(tetra_viewer::rayleigh_phase(-0.4) ==
        doctest::Approx(tetra_viewer::rayleigh_phase(0.4)));
  CHECK(tetra_viewer::mie_henyey_greenstein_phase(1.0, 0.8) >
        tetra_viewer::mie_henyey_greenstein_phase(-1.0, 0.8));

  const tetra::Vec3 start{0.0, parameters.ground_radius_metres, 0.0};
  const tetra::Vec3 middle{0.0, parameters.ground_radius_metres + 10'000.0,
                           0.0};
  const tetra::Vec3 end{0.0, parameters.ground_radius_metres + 100'000.0,
                        0.0};
  const auto short_path =
      tetra_viewer::atmosphere_transmittance(start, middle, parameters, 256U);
  const auto long_path =
      tetra_viewer::atmosphere_transmittance(start, end, parameters, 256U);
  const auto reverse =
      tetra_viewer::atmosphere_transmittance(end, start, parameters, 256U);
  for (std::size_t channel = 0; channel < 3U; ++channel) {
    CHECK(long_path[channel] > 0.0);
    CHECK(long_path[channel] <= short_path[channel]);
    CHECK(long_path[channel] ==
          doctest::Approx(reverse[channel]).epsilon(1.0e-12));
  }
  CHECK(long_path[2] < long_path[0]);
}

TEST_CASE("horizon-aware transmittance coordinates are invertible and bounded") {
  using tetra_viewer::AtmosphereLookupCoordinates;
  for(const auto preset:{tetra_viewer::AtmospherePreset::gameplay_planet,
                         tetra_viewer::AtmospherePreset::earth,
                         tetra_viewer::AtmospherePreset::mars_like}){
    const auto parameters=tetra_viewer::atmosphere_preset(preset);
    for(const double v:{0.0,0.01,0.1,0.5,0.9,0.999,1.0}){
      for(const double u:{0.0,0.01,0.1,0.5,0.9,0.999,1.0}){
        const AtmosphereLookupCoordinates uv{u,v};
        const auto physical=tetra_viewer::atmosphere_transmittance_parameters(
            uv,parameters);
        CHECK(std::isfinite(physical.altitude_metres));
        CHECK(std::isfinite(physical.zenith_cosine));
        CHECK(physical.altitude_metres>=0.0);
        CHECK(physical.altitude_metres<=parameters.atmosphere_height_metres);
        CHECK(physical.zenith_cosine>=-1.0);
        CHECK(physical.zenith_cosine<=1.0);
        const auto round_trip=tetra_viewer::atmosphere_transmittance_uv(
            physical.altitude_metres,physical.zenith_cosine,parameters);
        CHECK(round_trip.u==doctest::Approx(u).epsilon(2.0e-9).scale(1.0));
        CHECK(round_trip.v==doctest::Approx(v).epsilon(2.0e-9).scale(1.0));
      }
    }

    const double altitude=std::min(1000.0,
        parameters.atmosphere_height_metres*0.1);
    const double radius=parameters.ground_radius_metres+altitude;
    const double horizon_cosine=-std::sqrt(std::max(0.0,
        (radius-parameters.ground_radius_metres)*
        (radius+parameters.ground_radius_metres)/(radius*radius)));
    const auto horizon=tetra_viewer::atmosphere_transmittance_uv(
        altitude,horizon_cosine,parameters);
    CHECK(horizon.u==doctest::Approx(1.0).epsilon(1.0e-10));
    const auto below=tetra_viewer::atmosphere_transmittance_uv(
        altitude,-1.0,parameters);
    CHECK(below.u==doctest::Approx(1.0));
  }
}

TEST_CASE("full sky coordinates round trip and concentrate samples at the horizon") {
  const tetra::Vec3 up{0.2,0.95,-0.1};
  const tetra::Vec3 sun{-0.7,0.3,0.6};
  for(const double v:{0.01,0.1,0.25,0.5,0.75,0.9,0.99}){
    for(const double u:{0.01,0.1,0.25,0.5,0.75,0.9,0.99}){
      const tetra_viewer::AtmosphereLookupCoordinates uv{u,v};
      const auto direction=tetra_viewer::atmosphere_full_sky_direction(
          uv,up,sun);
      const double magnitude=std::sqrt(direction.x*direction.x+
          direction.y*direction.y+direction.z*direction.z);
      CHECK(magnitude==doctest::Approx(1.0).epsilon(1.0e-12));
      const auto round_trip=tetra_viewer::atmosphere_full_sky_uv(
          direction,up,sun);
      CHECK(round_trip.u==doctest::Approx(u).epsilon(2.0e-12).scale(1.0));
      CHECK(round_trip.v==doctest::Approx(v).epsilon(2.0e-12).scale(1.0));
    }
  }

  const auto horizon=tetra_viewer::atmosphere_full_sky_direction(
      {0.37,0.5},up,sun);
  const double up_length=std::sqrt(up.x*up.x+up.y*up.y+up.z*up.z);
  CHECK(std::abs((horizon.x*up.x+horizon.y*up.y+horizon.z*up.z)/up_length)<
        1.0e-12);
  const double one_degree=std::numbers::pi/180.0;
  constexpr double latitude_shape=std::numbers::pi/4.0-1.0;
  const double one_degree_vertical=std::sin(one_degree);
  const double one_degree_root=std::sqrt(1.0-one_degree_vertical);
  const double mapped_offset=0.5*std::sqrt(
      (1.0-one_degree_root)/(1.0+latitude_shape*one_degree_root));
  const double linear_offset=0.5*one_degree/(std::numbers::pi/2.0);
  CHECK(mapped_offset>linear_offset*5.0);

  // Near-nadir orbital rays must not collapse into the pole texel.  At the
  // Default 216-row resolution this retains several samples across the last
  // two degrees, matching the former angular mapping closely enough for the
  // full-sky transport oracle.
  const tetra::Vec3 near_nadir=up*std::cos(one_degree*2.0)*-1.0+
      sun*std::sin(one_degree*2.0);
  const auto near_nadir_uv=tetra_viewer::atmosphere_full_sky_uv(
      near_nadir,up,sun);
  CHECK(near_nadir_uv.v>1.0/216.0);

  // A zenith sun has no projected azimuth; the documented planet-fixed
  // fallback must still be deterministic and invertible.
  const auto zenith_direction=tetra_viewer::atmosphere_full_sky_direction(
      {0.2,0.6},up,up);
  const auto zenith_uv=tetra_viewer::atmosphere_full_sky_uv(
      zenith_direction,up,up);
  CHECK(zenith_uv.u==doctest::Approx(0.2).epsilon(2.0e-12));
  CHECK(zenith_uv.v==doctest::Approx(0.6).epsilon(2.0e-12));

  for(const tetra::Vec3 pole_up:{tetra::Vec3{1.0,0.0,0.0},
                                  tetra::Vec3{0.0,1.0,0.0},
                                  tetra::Vec3{0.0,0.0,1.0},
                                  tetra::Vec3{-1.0,0.0,0.0}}){
    const auto pole_direction=tetra_viewer::atmosphere_full_sky_direction(
        {0.73,0.42},pole_up,{0.0,1.0,0.0});
    const auto pole_uv=tetra_viewer::atmosphere_full_sky_uv(
        pole_direction,pole_up,{0.0,1.0,0.0});
    CHECK(pole_uv.u==doctest::Approx(0.73).epsilon(2.0e-12));
    CHECK(pole_uv.v==doctest::Approx(0.42).epsilon(2.0e-12));
  }
}

TEST_CASE("Hillaire multiple scattering closure is finite energy bounded and local") {
  using tetra_viewer::AtmosphereSpectrum;
  const auto vacuum=tetra_viewer::atmosphere_multiple_scattering_closure(
      AtmosphereSpectrum{0.0,0.0,0.0},AtmosphereSpectrum{0.0,0.0,0.0});
  CHECK((vacuum==AtmosphereSpectrum{0.0,0.0,0.0}));

  const AtmosphereSpectrum second_order{0.2,0.4,0.8};
  const auto no_transfer=tetra_viewer::atmosphere_multiple_scattering_closure(
      second_order,AtmosphereSpectrum{0.0,0.0,0.0});
  CHECK(no_transfer==second_order);
  const auto varying=tetra_viewer::atmosphere_multiple_scattering_closure(
      second_order,AtmosphereSpectrum{0.25,0.5,0.75});
  CHECK(varying[0]==doctest::Approx(0.2/0.75));
  CHECK(varying[1]==doctest::Approx(0.4/0.5));
  CHECK(varying[2]==doctest::Approx(0.8/0.25));
  CHECK(varying[0]>second_order[0]);
  CHECK(varying[1]>varying[0]);
  CHECK(varying[2]>varying[1]);

  const auto bounded=tetra_viewer::atmosphere_multiple_scattering_closure(
      AtmosphereSpectrum{1.0,1.0,1.0},
      AtmosphereSpectrum{1.0,2.0,std::numeric_limits<double>::infinity()});
  CHECK(std::isfinite(bounded[0]));
  CHECK(std::isfinite(bounded[1]));
  CHECK(std::isfinite(bounded[2]));
  CHECK(bounded[0]==doctest::Approx(1000.0));
  CHECK(bounded[1]==doctest::Approx(1000.0));
  CHECK(bounded[2]==doctest::Approx(1.0));
}

TEST_CASE("double precision multiple scattering oracle covers physical regimes") {
  using tetra_viewer::AtmospherePreset;
  const auto finite_nonnegative=[](const tetra_viewer::AtmosphereSpectrum& v){
    return std::ranges::all_of(v,[](double x){
      return std::isfinite(x)&&x>=0.0;
    });
  };

  auto vacuum=tetra_viewer::atmosphere_preset(AtmospherePreset::earth);
  vacuum.rayleigh_scattering_per_metre={0.0,0.0,0.0};
  vacuum.mie_scattering_per_metre={0.0,0.0,0.0};
  vacuum.mie_absorption_per_metre={0.0,0.0,0.0};
  vacuum.absorption_per_metre={0.0,0.0,0.0};
  vacuum.ground_albedo={0.0,0.0,0.0};
  const auto empty=tetra_viewer::atmosphere_multiple_scattering_reference(
      vacuum,1000.0,0.5,16U,8U);
  CHECK((empty.second_order==tetra_viewer::AtmosphereSpectrum{}));
  CHECK((empty.transfer_factor==tetra_viewer::AtmosphereSpectrum{}));
  CHECK((empty.closed_contribution==tetra_viewer::AtmosphereSpectrum{}));

  auto absorption_only=vacuum;
  absorption_only.mie_absorption_per_metre={1.0e-4,2.0e-4,3.0e-4};
  const auto absorbed=tetra_viewer::atmosphere_multiple_scattering_reference(
      absorption_only,1000.0,0.5,16U,8U);
  CHECK((absorbed.second_order==tetra_viewer::AtmosphereSpectrum{}));
  CHECK((absorbed.transfer_factor==tetra_viewer::AtmosphereSpectrum{}));

  auto conservative=vacuum;
  conservative.rayleigh_scattering_per_metre={8.0e-6,12.0e-6,20.0e-6};
  conservative.mie_scattering_per_metre={4.0e-6,4.0e-6,4.0e-6};
  const auto conservative_reference=
      tetra_viewer::atmosphere_multiple_scattering_reference(
          conservative,1000.0,0.5,64U,20U);
  CHECK(finite_nonnegative(conservative_reference.closed_contribution));
  for(std::size_t channel=0;channel<3U;++channel){
    CHECK(conservative_reference.transfer_factor[channel]>=0.0);
    CHECK(conservative_reference.transfer_factor[channel]<1.0);
    CHECK(conservative_reference.closed_contribution[channel]>=
          conservative_reference.second_order[channel]);
  }

  const auto earth=tetra_viewer::atmosphere_preset(AtmospherePreset::earth);
  const auto reference=tetra_viewer::atmosphere_multiple_scattering_reference(
      earth,1000.0,0.5,64U,20U);
  CHECK(finite_nonnegative(reference.second_order));
  CHECK(finite_nonnegative(reference.transfer_factor));
  CHECK(finite_nonnegative(reference.closed_contribution));
  for(std::size_t channel=0;channel<3U;++channel){
    CHECK(reference.transfer_factor[channel]<1.0);
    CHECK(reference.closed_contribution[channel]>=reference.second_order[channel]);
  }

  auto thick=earth;
  for(auto& value:thick.rayleigh_scattering_per_metre)value*=20.0;
  for(auto& value:thick.mie_scattering_per_metre)value*=20.0;
  const auto thick_reference=
      tetra_viewer::atmosphere_multiple_scattering_reference(
          thick,1000.0,0.1,32U,12U);
  CHECK(finite_nonnegative(thick_reference.closed_contribution));
  CHECK(*std::max_element(thick_reference.closed_contribution.begin(),
                          thick_reference.closed_contribution.end())<1000.0);

  for(const auto preset:{AtmospherePreset::gameplay_planet,
                         AtmospherePreset::mars_like,
                         AtmospherePreset::dense_haze,
                         AtmospherePreset::nearly_airless}){
    const auto parameters=tetra_viewer::atmosphere_preset(preset);
    const auto low=tetra_viewer::atmosphere_multiple_scattering_reference(
        parameters,parameters.atmosphere_height_metres*0.05,0.25,16U,8U);
    const auto high=tetra_viewer::atmosphere_multiple_scattering_reference(
        parameters,parameters.atmosphere_height_metres*0.9,0.25,16U,8U);
    CHECK(finite_nonnegative(low.closed_contribution));
    CHECK(finite_nonnegative(high.closed_contribution));
    for(const double transfer:low.transfer_factor)CHECK(transfer<1.0);
    for(const double transfer:high.transfer_factor)CHECK(transfer<1.0);
  }
}

TEST_CASE("double precision atmosphere transport oracle covers ground to orbit") {
  using tetra_viewer::AtmospherePreset;
  using tetra_viewer::AtmosphereSpectrum;
  const auto finite_nonnegative=[](const AtmosphereSpectrum& value){
    return std::ranges::all_of(value,[](double component){
      return std::isfinite(component)&&component>=0.0;
    });
  };

  auto vacuum=tetra_viewer::atmosphere_preset(AtmospherePreset::earth);
  vacuum.rayleigh_scattering_per_metre={0.0,0.0,0.0};
  vacuum.mie_scattering_per_metre={0.0,0.0,0.0};
  vacuum.mie_absorption_per_metre={0.0,0.0,0.0};
  vacuum.absorption_per_metre={0.0,0.0,0.0};
  vacuum.ground_albedo={0.0,0.0,0.0};
  const tetra::Vec3 upward{0.0,1.0,0.0};
  const auto empty=tetra_viewer::atmosphere_scattering_reference(
      vacuum,{0.0,vacuum.ground_radius_metres+1.0,0.0},upward,upward,
      vacuum.atmosphere_height_metres,16U,8U,4U);
  CHECK((empty.radiance==AtmosphereSpectrum{}));
  CHECK((empty.transmittance==AtmosphereSpectrum{1.0,1.0,1.0}));

  auto absorption_only=vacuum;
  absorption_only.mie_absorption_per_metre={1.0e-5,2.0e-5,3.0e-5};
  const auto absorbed=tetra_viewer::atmosphere_scattering_reference(
      absorption_only,
      {0.0,absorption_only.ground_radius_metres+1.0,0.0},upward,upward,
      absorption_only.atmosphere_height_metres,32U,8U,4U);
  CHECK((absorbed.radiance==AtmosphereSpectrum{}));
  for(const double value:absorbed.transmittance){
    CHECK(value>0.0);
    CHECK(value<1.0);
  }

  for(const auto preset:{AtmospherePreset::gameplay_planet,
                         AtmospherePreset::earth,
                         AtmospherePreset::mars_like,
                         AtmospherePreset::dense_haze,
                         AtmospherePreset::nearly_airless}){
    const auto parameters=tetra_viewer::atmosphere_preset(preset);
    const double radius=parameters.ground_radius_metres;
    for(const auto probe:std::array{
            std::pair{tetra::Vec3{0.0,radius+2.0,0.0},
                      tetra::Vec3{0.999,0.0447,0.0}},
            std::pair{tetra::Vec3{0.0,radius+
                                      parameters.atmosphere_height_metres*0.5,
                                  0.0},tetra::Vec3{0.7,0.7,0.0}},
            std::pair{tetra::Vec3{0.0,radius+
                                      parameters.atmosphere_height_metres+
                                      radius*0.2,0.0},
                      tetra::Vec3{0.0,-1.0,0.0}}}){
      const auto reference=tetra_viewer::atmosphere_scattering_reference(
          parameters,probe.first,probe.second,{0.6,0.7,0.2},
          radius*3.0,24U,12U,6U);
      CHECK(finite_nonnegative(reference.radiance));
      CHECK(finite_nonnegative(reference.transmittance));
      for(const double value:reference.transmittance)CHECK(value<=1.0);
    }
  }
}

TEST_CASE("atmosphere transport oracle composes split paths and distance") {
  const auto parameters=tetra_viewer::atmosphere_preset(
      tetra_viewer::AtmospherePreset::earth);
  const tetra::Vec3 origin{0.0,parameters.ground_radius_metres+100.0,0.0};
  const tetra::Vec3 direction{0.0,1.0,0.0};
  const tetra::Vec3 sun{0.3,0.95,0.0};
  constexpr double split_distance=5'000.0;
  constexpr double complete_distance=20'000.0;
  const auto short_path=tetra_viewer::atmosphere_scattering_reference(
      parameters,origin,direction,sun,split_distance,64U,16U,8U);
  const auto complete=tetra_viewer::atmosphere_scattering_reference(
      parameters,origin,direction,sun,complete_distance,128U,16U,8U);
  const auto remainder=tetra_viewer::atmosphere_scattering_reference(
      parameters,origin+direction*split_distance,direction,sun,
      complete_distance-split_distance,96U,16U,8U);
  for(std::size_t channel=0;channel<3U;++channel){
    CHECK(complete.transmittance[channel]<=short_path.transmittance[channel]);
    CHECK(complete.radiance[channel]>=short_path.radiance[channel]);
    CHECK(short_path.radiance[channel]+short_path.transmittance[channel]*
              remainder.radiance[channel]==
          doctest::Approx(complete.radiance[channel]).epsilon(0.03)
              .scale(1.0e-8));
    CHECK(short_path.transmittance[channel]*remainder.transmittance[channel]==
          doctest::Approx(complete.transmittance[channel]).epsilon(0.01));
  }
}

TEST_CASE("numeric atmosphere probe reference detects every perturbed stage") {
  const auto parameters=tetra_viewer::atmosphere_preset(
      tetra_viewer::AtmospherePreset::gameplay_planet);
  const tetra_viewer::AtmosphereNumericProbeInput input{
      .parameters=parameters,
      .camera_position_from_planet_centre_metres=
          {0.0,parameters.ground_radius_metres+18.0,0.0},
      .camera_right={1.0,0.0,0.0},
      .camera_down={0.0,1.0,0.0},
      .camera_forward={0.0,0.0,-1.0},
      .sun_direction={0.3,0.8,-0.5},
      .vertical_tangent=0.7,
      .aspect_ratio=16.0/9.0,
      .maximum_aerial_distance_metres=5'000.0,
      .quality=tetra_viewer::AtmosphereQuality::low};
  const auto reference=tetra_viewer::atmosphere_numeric_probe_reference(input);
  for(std::size_t index=0;index<reference.size();++index)
    for(const double value:reference[index])CHECK(std::isfinite(value));
  const auto exact=tetra_viewer::evaluate_atmosphere_numeric_probe(
      reference,input);
  CHECK(exact.passed);
  REQUIRE(exact.comparisons.size()==reference.size());
  for(const auto& comparison:exact.comparisons)CHECK(comparison.passed);

  auto perturbed=reference;
  for(auto& stage:perturbed)stage[0]+=10.0;
  const auto rejected=tetra_viewer::evaluate_atmosphere_numeric_probe(
      perturbed,input);
  CHECK_FALSE(rejected.passed);
  REQUIRE(rejected.comparisons.size()==reference.size());
  for(const auto& comparison:rejected.comparisons)
    CHECK_FALSE(comparison.passed);
}

TEST_CASE("double precision sky irradiance oracle preserves Lambertian energy") {
  using tetra_viewer::AtmosphereSpectrum;
  const tetra::Vec3 up{0.0,1.0,0.0};
  const AtmosphereSpectrum constant{0.25,1.0,4.0};
  const auto constant_sky=[&](tetra::Vec3){return constant;};
  const auto zenith=tetra_viewer::atmosphere_sky_irradiance_reference(
      up,up,constant_sky,65'536U);
  const auto horizon=tetra_viewer::atmosphere_sky_irradiance_reference(
      {1.0,0.0,0.0},up,constant_sky,65'536U);
  const auto nadir=tetra_viewer::atmosphere_sky_irradiance_reference(
      {0.0,-1.0,0.0},up,constant_sky,65'536U);
  for(std::size_t channel=0;channel<3U;++channel){
    CHECK(zenith[channel]==doctest::Approx(constant[channel]).epsilon(2.0e-4));
    CHECK(horizon[channel]==doctest::Approx(constant[channel]*0.5)
        .epsilon(8.0e-4));
    CHECK(nadir[channel]==doctest::Approx(0.0).epsilon(1.0e-12));
  }
}

TEST_CASE("sky irradiance oracle responds to surface normal at noon and sunset") {
  using tetra_viewer::AtmosphereSpectrum;
  const tetra::Vec3 up{0.0,1.0,0.0};
  const auto directional_sky=[](tetra::Vec3 sun){
    return [sun](tetra::Vec3 direction){
      const double cosine=direction.x*sun.x+direction.y*sun.y+
          direction.z*sun.z;
      const double lobe=std::pow(std::max(0.0,cosine),16.0);
      return AtmosphereSpectrum{0.02+lobe,0.03+0.8*lobe,0.05+0.5*lobe};
    };
  };
  const auto noon_sky=directional_sky(up);
  const auto noon_up=tetra_viewer::atmosphere_sky_irradiance_reference(
      up,up,noon_sky,32'768U);
  const auto noon_side=tetra_viewer::atmosphere_sky_irradiance_reference(
      {1.0,0.0,0.0},up,noon_sky,32'768U);
  CHECK(noon_up[0]>noon_side[0]*1.5);

  const tetra::Vec3 sunset_direction{1.0,0.0,0.0};
  const auto sunset_sky=directional_sky(sunset_direction);
  const auto toward_sun=tetra_viewer::atmosphere_sky_irradiance_reference(
      sunset_direction,up,sunset_sky,32'768U);
  const auto away_from_sun=tetra_viewer::atmosphere_sky_irradiance_reference(
      {-1.0,0.0,0.0},up,sunset_sky,32'768U);
  CHECK(toward_sun[0]>away_from_sun[0]*4.0);
  for(const auto& irradiance:{noon_up,noon_side,toward_sun,away_from_sun})
    for(const double channel:irradiance){
      CHECK(std::isfinite(channel));
      CHECK(channel>=0.0);
    }
}

TEST_CASE("aerial LUT cubic depth resolves gameplay range through orbit") {
  constexpr double maximum_distance=200'000.0;
  constexpr double default_depth_slices=16.0;
  const double first_sample=tetra_viewer::aerial_lut_distance(
      1.0/(default_depth_slices-1.0),maximum_distance);
  CHECK(first_sample<100.0);
  CHECK(first_sample>50.0);
  for(const double distance:std::array{0.0,50.0,500.0,10'000.0,
                                       maximum_distance}){
    const double slice=tetra_viewer::aerial_lut_slice(
        distance,maximum_distance);
    CHECK(tetra_viewer::aerial_lut_distance(slice,maximum_distance)==
          doctest::Approx(distance).epsilon(1.0e-12));
  }
}

TEST_CASE("local aerial range follows density altitude and visible bounds") {
  const auto gameplay=tetra_viewer::atmosphere_preset(
      tetra_viewer::AtmospherePreset::gameplay_planet);
  const auto earth=tetra_viewer::atmosphere_preset(
      tetra_viewer::AtmospherePreset::earth);
  CHECK(tetra_viewer::atmosphere_local_aerial_distance(
            gameplay,1.8,200'000.0)==doctest::Approx(24'001.8));
  CHECK(tetra_viewer::atmosphere_local_aerial_distance(
            earth,1.8,200'000.0)==doctest::Approx(64'000.0));
  CHECK(tetra_viewer::atmosphere_local_aerial_distance(
            gameplay,10'000.0,200'000.0)==doctest::Approx(34'000.0));
  CHECK(tetra_viewer::atmosphere_local_aerial_distance(
            gameplay,1.8,5'000.0)==doctest::Approx(5'000.0));
  CHECK(tetra_viewer::atmosphere_local_aerial_distance(
            gameplay,1.8,0.0)==0.0);
}

TEST_CASE("local and long aerial regimes share multiplicative transmittance") {
  const auto parameters=tetra_viewer::atmosphere_preset(
      tetra_viewer::AtmospherePreset::gameplay_planet);
  const double radius=parameters.ground_radius_metres;
  const tetra::Vec3 start{0.0,radius+100'000.0,0.0};
  const tetra::Vec3 end{0.0,radius+1.0,0.0};
  const double local=tetra_viewer::atmosphere_local_aerial_distance(
      parameters,100'000.0,200'000.0);
  const double total_distance=99'999.0;
  const tetra::Vec3 split=start+(end-start)*(local/total_distance);
  const auto complete=tetra_viewer::atmosphere_transmittance(
      start,end,parameters,1024U);
  const auto first=tetra_viewer::atmosphere_transmittance(
      start,split,parameters,512U);
  const auto second=tetra_viewer::atmosphere_transmittance(
      split,end,parameters,512U);
  for(std::size_t channel=0;channel<3U;++channel){
    CHECK(std::isfinite(complete[channel]));
    CHECK(complete[channel]>=0.0);
    CHECK(complete[channel]<=1.0);
    CHECK(first[channel]*second[channel]==
          doctest::Approx(complete[channel]).epsilon(2.0e-3));
  }
}

TEST_CASE("atmosphere LUT invalidation follows the documented dependency graph") {
  const auto original =
      tetra_viewer::atmosphere_preset(tetra_viewer::AtmospherePreset::earth);
  CHECK_FALSE(tetra_viewer::atmosphere_invalidation(original, original)
                  .transmittance);

  auto exposure_independent = original;
  exposure_independent.solar_irradiance[0] *= 1.1;
  auto invalidation = tetra_viewer::atmosphere_invalidation(
      original, exposure_independent);
  CHECK_FALSE(invalidation.transmittance);
  CHECK(invalidation.multiple_scattering);
  CHECK(invalidation.sky_view);
  CHECK(invalidation.aerial_perspective);

  auto albedo = original;
  albedo.ground_albedo[0] += 0.01;
  invalidation = tetra_viewer::atmosphere_invalidation(original, albedo);
  CHECK_FALSE(invalidation.transmittance);
  CHECK(invalidation.multiple_scattering);

  auto extinction = original;
  extinction.rayleigh_scale_height_metres += 1.0;
  invalidation = tetra_viewer::atmosphere_invalidation(original, extinction);
  CHECK(invalidation.transmittance);
  CHECK(invalidation.multiple_scattering);
  CHECK(invalidation.sky_view);
  CHECK(invalidation.aerial_perspective);

  auto phase = original;
  phase.mie_anisotropy -= 0.1;
  invalidation = tetra_viewer::atmosphere_invalidation(original, phase);
  CHECK_FALSE(invalidation.transmittance);
  CHECK(invalidation.multiple_scattering);
  CHECK(invalidation.sky_view);
  CHECK(invalidation.aerial_perspective);
}

TEST_CASE("headless atmosphere check exposes deterministic camera sun and preset probes") {
  std::ostringstream first;
  std::ostringstream second;
  std::ostringstream errors;
  CHECK(tetra_viewer::run_atmosphere_check(
            tetra_viewer::AtmospherePreset::earth, 1'000.0, 70.0, 85.0,
            first, errors) == 0);
  CHECK(tetra_viewer::run_atmosphere_check(
            tetra_viewer::AtmospherePreset::earth, 1'000.0, 70.0, 85.0,
            second, errors) == 0);
  CHECK(first.str() == second.str());
  CHECK(first.str().find("\"event\":\"atmosphere_check\"") !=
        std::string::npos);
  CHECK(first.str().find("\"transmittance\":[") != std::string::npos);

  std::ostringstream gameplay;
  CHECK(tetra_viewer::run_atmosphere_check(
            tetra_viewer::AtmospherePreset::gameplay_planet, 1.8, 89.0, 65.0,
            gameplay, errors) == 0);
  CHECK(gameplay.str().find("\"preset\":\"gameplay-planet\"") !=
        std::string::npos);

  std::ostringstream invalid_output;
  std::ostringstream invalid_errors;
  CHECK(tetra_viewer::run_atmosphere_check(
            tetra_viewer::AtmospherePreset::earth, -1.0, 0.0, 0.0,
            invalid_output, invalid_errors) == 2);
  CHECK(invalid_errors.str().find("nonnegative") != std::string::npos);
}

TEST_CASE("stable shadow cascades are nested snapped and centred in clip space") {
  const auto cascades=tetra_viewer::make_stable_shadow_cascades(
      {0.5,0.78,0.5},{0.0,0.0,-1.0},{-0.2,0.3,-0.9},2048U);
  double previous_split{};
  for(const auto& cascade:cascades.cascades){
    CHECK(cascade.split_distance>previous_split);
    CHECK(cascade.depth_half_range>=cascade.half_width);
    CHECK(cascade.texel_world_size==
          doctest::Approx(2.0*cascade.half_width/2048.0));
    const auto centre=tetra_viewer::transform_shadow_point(
        cascade.matrix,cascade.snapped_centre);
    CHECK(centre.x==doctest::Approx(0.0).epsilon(1.0e-5));
    CHECK(centre.y==doctest::Approx(0.0).epsilon(1.0e-5));
    CHECK(centre.z==doctest::Approx(0.5).epsilon(1.0e-5));
    const auto right_edge=tetra_viewer::transform_shadow_point(
        cascade.matrix,cascade.snapped_centre+
            cascades.light_right*cascade.half_width);
    CHECK(right_edge.x==doctest::Approx(1.0).epsilon(1.0e-5));
    previous_split=cascade.split_distance;
  }
}

TEST_CASE("atmospheric shadow cascades cover the gameplay horizon without a light jump") {
  const auto game=tetra_viewer::atmosphere_preset(
      tetra_viewer::AtmospherePreset::gameplay_planet);
  constexpr double eye_height_metres=18.0;
  constexpr double metres_per_world_unit=10.0;
  const double horizon_world=tetra_viewer::planetary_horizon_distance(
      game.ground_radius_metres,eye_height_metres)/metres_per_world_unit;
  const auto cascades=tetra_viewer::make_stable_shadow_cascades(
      {0.5,2.3,0.5},{0.0,0.0,-1.0},{-0.2,0.3,-0.9},1024U);
  CHECK(cascades.cascades.back().split_distance>horizon_world);
  CHECK(cascades.cascades.back().half_width==
        doctest::Approx(tetra_viewer::default_shadow_cascade_half_widths.back()));
  CHECK(cascades.cascades.back().texel_world_size<=1.0);

  CHECK(tetra_viewer::atmosphere_shadow_filter_visibility(4U,4U,1.0)==1.0);
  CHECK(tetra_viewer::atmosphere_shadow_filter_visibility(0U,4U,1.0)==0.0);
  CHECK(tetra_viewer::atmosphere_shadow_filter_visibility(2U,4U,1.0)==0.5);
  CHECK(tetra_viewer::atmosphere_shadow_filter_visibility(0U,4U,0.25)==0.75);
  CHECK(tetra_viewer::atmosphere_shadow_filter_visibility(9U,4U,1.0)==1.0);
}

TEST_CASE("shadow cascade motion is quantized to texels and deterministic") {
  const tetra::Vec3 camera{17.25,2.5,-9.75};
  const tetra::Vec3 forward{0.2,-0.1,-0.9};
  const tetra::Vec3 sun{-0.7,0.4,0.3};
  const auto first=tetra_viewer::make_stable_shadow_cascades(
      camera,forward,sun,1024U);
  const auto repeated=tetra_viewer::make_stable_shadow_cascades(
      camera,forward,sun,1024U);
  const auto moved=tetra_viewer::make_stable_shadow_cascades(
      camera+tetra::Vec3{0.001,0.0,0.0},forward,sun,1024U);
  for(std::size_t index=0;index<tetra_viewer::shadow_cascade_count;++index){
    CHECK(first.cascades[index].matrix==repeated.cascades[index].matrix);
    const auto delta=moved.cascades[index].snapped_centre-
        first.cascades[index].snapped_centre;
    for(const auto axis:{first.light_right,first.light_up,first.sun_direction}){
      const double projected=delta.x*axis.x+delta.y*axis.y+delta.z*axis.z;
      const double texel=first.cascades[index].texel_world_size;
      CHECK(projected/texel==
            doctest::Approx(std::round(projected/texel)).epsilon(1.0e-8));
    }
  }
  CHECK_THROWS_AS(([&]{
    const auto unused=tetra_viewer::make_stable_shadow_cascades(
        camera,forward,sun,0U);
    static_cast<void>(unused);
  }()),std::invalid_argument);
  CHECK_THROWS_AS(([&]{
    const auto unused=tetra_viewer::make_stable_shadow_cascades(
        camera,forward,sun,1024U,{2.0,2.0,8.0,32.0});
    static_cast<void>(unused);
  }()),std::invalid_argument);
}

TEST_CASE("atmospheric shadow cascade policy blends continuously to unshadowed space") {
  const auto cascades=tetra_viewer::make_stable_shadow_cascades(
      {0.5,2.3,0.5},{0.0,0.0,-1.0},{-0.2,0.3,-0.9},1024U);
  for(std::size_t index=0;index<tetra_viewer::shadow_cascade_count;++index){
    const double split=cascades.cascades[index].split_distance;
    const double previous=index==0U?0.0:
        cascades.cascades[index-1U].split_distance;
    const double begin=index+1U==tetra_viewer::shadow_cascade_count?
        split*0.80:previous+(split-previous)*0.85;
    const auto inside=tetra_viewer::atmosphere_shadow_cascade_blend(
        std::nextafter(begin,0.0),cascades);
    const auto middle=tetra_viewer::atmosphere_shadow_cascade_blend(
        0.5*(begin+split),cascades);
    const auto boundary=tetra_viewer::atmosphere_shadow_cascade_blend(
        std::nextafter(split,0.0),cascades);
    CHECK(inside.primary==index);
    CHECK(inside.secondary_weight==doctest::Approx(0.0).epsilon(1.0e-10));
    CHECK(middle.secondary_weight==doctest::Approx(0.5).epsilon(1.0e-10));
    CHECK(boundary.secondary_weight==doctest::Approx(1.0).epsilon(1.0e-10));
    if(index+1U<tetra_viewer::shadow_cascade_count){
      REQUIRE(middle.secondary);
      CHECK(*middle.secondary==index+1U);
      const auto outside=tetra_viewer::atmosphere_shadow_cascade_blend(
          std::nextafter(split,std::numeric_limits<double>::infinity()),
          cascades);
      CHECK(outside.primary==index+1U);
      CHECK(outside.secondary_weight==doctest::Approx(0.0).epsilon(1.0e-10));
    }else{
      CHECK_FALSE(middle.secondary);
      const auto beyond=tetra_viewer::atmosphere_shadow_cascade_blend(
          split*1.01,cascades);
      CHECK(beyond.primary==index);
      CHECK_FALSE(beyond.secondary);
      CHECK(beyond.secondary_weight==doctest::Approx(1.0));
    }
  }
}

TEST_CASE("atmospheric shadow bias and footprint stay bounded across cascades") {
  double previous_bias{};
  for(std::size_t cascade=0;cascade<tetra_viewer::shadow_cascade_count;
      ++cascade){
    const double bias=tetra_viewer::atmosphere_shadow_depth_bias(cascade);
    CHECK(bias>previous_bias);
    CHECK(bias<0.0011);
    previous_bias=bias;
  }
  CHECK(tetra_viewer::atmosphere_shadow_footprint_fade(0.0,0.0)==1.0);
  CHECK(tetra_viewer::atmosphere_shadow_footprint_fade(0.88,0.5)==1.0);
  CHECK(tetra_viewer::atmosphere_shadow_footprint_fade(0.93,0.5)==
        doctest::Approx(0.5));
  CHECK(tetra_viewer::atmosphere_shadow_footprint_fade(0.98,0.5)==0.0);
  CHECK(tetra_viewer::atmosphere_shadow_footprint_fade(1.2,0.5)==0.0);
}

TEST_CASE("atmospheric receiver projection mirrors shader clip and depth policy") {
  const auto cascades=tetra_viewer::make_stable_shadow_cascades(
      {0.5,0.72,0.78},{0.0,0.0,1.0},{-0.864838,0.052336,-0.499315},1024U);
  for(std::size_t index=0;index<tetra_viewer::shadow_cascade_count;++index){
    const auto& cascade=cascades.cascades[index];
    const auto centre=tetra_viewer::project_atmosphere_shadow_point(
        cascade,cascade.snapped_centre);
    CHECK(centre.sampleable());
    CHECK(centre.u==doctest::Approx(0.5).epsilon(1.0e-6));
    CHECK(centre.v==doctest::Approx(0.5).epsilon(1.0e-6));
    CHECK(centre.clip.z==doctest::Approx(0.5).epsilon(1.0e-6));

    const auto footprint_edge=tetra_viewer::project_atmosphere_shadow_point(
        cascade,cascade.snapped_centre+
            cascades.light_right*cascade.half_width);
    CHECK(footprint_edge.sampleable());
    CHECK(footprint_edge.u==doctest::Approx(1.0).epsilon(1.0e-6));
    const auto footprint_outside=tetra_viewer::project_atmosphere_shadow_point(
        cascade,cascade.snapped_centre+
            cascades.light_right*(cascade.half_width*1.001));
    CHECK_FALSE(footprint_outside.inside_footprint);

    const auto toward_sun=tetra_viewer::project_atmosphere_shadow_point(
        cascade,cascade.snapped_centre+
            cascades.sun_direction*cascade.depth_half_range);
    const auto away_from_sun=tetra_viewer::project_atmosphere_shadow_point(
        cascade,cascade.snapped_centre-
            cascades.sun_direction*cascade.depth_half_range);
    CHECK(toward_sun.clip.z==doctest::Approx(0.0).epsilon(1.0e-5));
    CHECK(away_from_sun.clip.z==doctest::Approx(1.0).epsilon(1.0e-5));
  }
  const auto probe_cases=
      tetra_viewer::make_atmosphere_shadow_projection_probe_cases(cascades);
  CHECK(probe_cases.size()==
        tetra_viewer::atmosphere_shadow_projection_probe_count);
  for(const auto& probe:probe_cases){
    const auto projected=tetra_viewer::project_atmosphere_shadow_point(
        cascades.cascades[probe.cascade],probe.point);
    CHECK(projected.clip.x==
          doctest::Approx(probe.expected_clip.x).epsilon(2.0e-5));
    CHECK(projected.clip.y==
          doctest::Approx(probe.expected_clip.y).epsilon(2.0e-5));
    CHECK(projected.clip.z==
          doctest::Approx(probe.expected_clip.z).epsilon(2.0e-5));
    CHECK(projected.sampleable()==probe.expected_sampleable);
  }
}

TEST_CASE("atmospheric receiver depth oracle preserves lit borders and partial filtering") {
  tetra_viewer::AtmosphereShadowProjection projection{
      .clip={0.0,0.0,0.6},.u=0.5,.v=0.5,
      .inside_footprint=true,.inside_depth=true};
  CHECK(tetra_viewer::atmosphere_shadow_depth_visibility(0.6,0.7,0.001)==1.0);
  CHECK(tetra_viewer::atmosphere_shadow_depth_visibility(0.6,0.5,0.001)==0.0);
  CHECK(tetra_viewer::atmosphere_shadow_filtered_visibility(
      projection,{0.5,0.5,0.7,0.7},0.001)==doctest::Approx(0.5));
  projection.clip.x=0.93;
  CHECK(tetra_viewer::atmosphere_shadow_filtered_visibility(
      projection,{0.5,0.5,0.5,0.5},0.001)==doctest::Approx(0.5));
  projection.clip.x=1.01;
  projection.inside_footprint=false;
  CHECK(tetra_viewer::atmosphere_shadow_filtered_visibility(
      projection,{0.0,0.0,0.0,0.0},0.001)==1.0);
  projection.clip.x=0.0;
  projection.inside_footprint=true;
  projection.clip.z=1.01;
  projection.inside_depth=false;
  CHECK(tetra_viewer::atmosphere_shadow_filtered_visibility(
      projection,{0.0,0.0,0.0,0.0},0.001)==1.0);
}
