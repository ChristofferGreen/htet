#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <limits>

#include "tetra_probes/surface_core_contract.hpp"
#include "tetra_probes/canonical_delaunay_seed.hpp"

namespace {

tetra::probes::SurfaceCoreTransitionInput nested_tetrahedra() {
  using tetra::Vec3;
  tetra::probes::SurfaceCoreTransitionInput input;
  input.vertices={
      {-2.0,-2.0,-2.0}, {2.0,-2.0,-2.0}, {-2.0,2.0,-2.0}, {-2.0,-2.0,2.0},
      {-1.25,-1.25,-1.25}, {-0.25,-1.25,-1.25},
      {-1.25,-0.25,-1.25}, {-1.25,-1.25,-0.25}};
  input.outer_faces={{{0,2,1}},{{0,1,3}},{{0,3,2}},{{1,2,3}}};
  input.retained_core_tetrahedra={{{4,5,6,7}}};
  input.coordinate_scale=4.0;
  return input;
}

tetra::probes::SurfaceCoreTransitionInput concave_pit_with_core(double core_z) {
  using tetra::Vec3;
  tetra::probes::SurfaceCoreTransitionInput input;
  // A closed terrain volume whose first interior terrain diagonal descends
  // from (-1,-1,2) to (-.5,-.5,-2).  A tetrahedron can have all vertices in
  // the volume while this diagonal still crosses its interior.
  for(unsigned y=0U;y<5U;++y)for(unsigned x=0U;x<5U;++x)
    input.vertices.push_back({-1.0+0.5*x,-1.0+0.5*y,
                              x>0U&&x<4U&&y>0U&&y<4U?-2.0:2.0});
  for(unsigned y=0U;y<4U;++y)for(unsigned x=0U;x<4U;++x) {
    const auto lower=y*5U+x;
    input.outer_faces.push_back({{lower,lower+1U,lower+6U}});
    input.outer_faces.push_back({{lower,lower+6U,lower+5U}});
  }
  const std::array<std::uint32_t,16> loop{{0U,1U,2U,3U,4U,9U,14U,19U,
                                             24U,23U,22U,21U,20U,15U,10U,5U}};
  const auto bottom_start=static_cast<std::uint32_t>(input.vertices.size());
  for(const auto top:loop) {
    const auto point=input.vertices[top];
    input.vertices.push_back({point.x,point.y,-2.5});
  }
  for(unsigned i=0U;i<loop.size();++i) {
    const auto next=(i+1U)%loop.size();
    const auto bottom_i=bottom_start+i;
    const auto bottom_next=bottom_start+next;
    input.outer_faces.push_back({{loop[i],bottom_next,loop[next]}});
    input.outer_faces.push_back({{loop[i],bottom_i,bottom_next}});
  }
  const auto cap=static_cast<std::uint32_t>(input.vertices.size());
  input.vertices.push_back({0.0,0.0,-2.5});
  for(unsigned i=0U;i<loop.size();++i) {
    const auto bottom_i=bottom_start+i;
    const auto bottom_next=bottom_start+(i+1U)%loop.size();
    input.outer_faces.push_back({{cap,bottom_next,bottom_i}});
  }
  const auto core_start=static_cast<std::uint32_t>(input.vertices.size());
  input.vertices.insert(input.vertices.end(),{{-0.8,-0.8,core_z},
      {-0.55,-0.8,core_z},{-0.8,-0.55,core_z},{-0.8,-0.8,core_z+0.25}});
  input.retained_core_tetrahedra.push_back(
      {{core_start,core_start+1U,core_start+2U,core_start+3U}});
  input.coordinate_scale=5.0;
  input.minimum_outer_triangle_angle_degrees=0.0;
  return input;
}

tetra::probes::SurfaceCoreTransitionOutput matching_tetrahedral_shell() {
  tetra::probes::SurfaceCoreTransitionOutput output;
  // Every outer/inner face pair is a triangular prism. The same globally
  // ordered 012/345 split is used on all four prisms, so shared side quads
  // agree. This is a controlled output-validator success fixture, not the
  // proposed arbitrary DC-to-grid construction.
  for (const auto face:std::array<std::array<std::uint32_t,3>,4>{{
      {{0,2,1}},{{0,1,3}},{{0,3,2}},{{1,2,3}}}}) {
    auto top=face; auto bottom=face;
    for (auto& id:bottom) id+=4U;
    std::sort(top.begin(),top.end());
    std::sort(bottom.begin(),bottom.end());
    output.tetrahedra.push_back({{top[0],top[1],top[2],bottom[0]}});
    output.tetrahedra.push_back({{top[1],top[2],bottom[0],bottom[1]}});
    output.tetrahedra.push_back({{top[2],bottom[0],bottom[1],bottom[2]}});
  }
  output.tetrahedra.push_back({{4,5,6,7}});
  return output;
}

}  // namespace

TEST_CASE("generic surface/core contract accepts a closed DC-like outer front and a core") {
  const auto report=tetra::probes::validate_surface_core_transition_input(nested_tetrahedra());
  CHECK(report.accepted);
  CHECK(report.failure==tetra::probes::SurfaceCoreInputFailure::none);
  CHECK(report.outer_boundary_edges==0U);
  CHECK(report.outer_nonmanifold_edges==0U);
  CHECK(report.core_boundary_faces==4U);
}

TEST_CASE("frozen geometric facet split is exact, transformed, and seam-owned deterministically") {
  using namespace tetra::probes;
  const FrozenFacetIdentity parent{{{91U,17U,44U}}};
  const auto literal=split_frozen_facet(parent,FacetPreservationMode::literal,9U,3U);
  const auto left=split_frozen_facet(parent,FacetPreservationMode::geometric,3U,9U);
  const auto right=split_frozen_facet(parent,FacetPreservationMode::geometric,9U,3U);
  REQUIRE(validate_frozen_facet_split(literal));
  REQUIRE(validate_frozen_facet_split(left));
  CHECK(literal.subfaces.size()==1U);
  CHECK(left.subfaces.size()==4U);
  // Both independent chunk calls derive precisely the same parent-keyed
  // subfaces and agree that the lower stable chunk id owns the seam facet.
  CHECK(left.parent==right.parent);
  CHECK(left.mode==right.mode);
  CHECK(left.owner_chunk==right.owner_chunk);
  for(std::size_t i=0;i<left.subfaces.size();++i) {
    CHECK(left.subfaces[i].parent==right.subfaces[i].parent);
    CHECK(left.subfaces[i].corners==right.subfaces[i].corners);
    CHECK(left.subfaces[i].ordinal==right.subfaces[i].ordinal);
    CHECK(left.subfaces[i].owner_chunk==right.subfaces[i].owner_chunk);
  }
  CHECK(left.owner_chunk==3U);
  for(const auto& face:left.subfaces) { CHECK(face.owner_chunk==3U); CHECK(face.emitted_by_local_chunk); }
  for(const auto& face:right.subfaces) CHECK_FALSE(face.emitted_by_local_chunk);

  const std::array<tetra::Vec3,3> triangle{{{2.0,-1.0,4.0},{5.0,2.0,3.0},{1.0,6.0,7.0}}};
  const auto transform=[](tetra::Vec3 p) { return tetra::Vec3{-p.y+11.0,p.x-5.0,p.z*2.0+1.0}; };
  const std::array<FrozenFacetVertex,3> local_parent{{{91U,triangle[0]},{17U,triangle[1]},{44U,triangle[2]}}};
  const std::array<FrozenFacetVertex,3> reversed_parent{{{44U,triangle[2]},{17U,triangle[1]},{91U,triangle[0]}}};
  for(const auto& face:left.subfaces) for(const auto& corner:face.corners) {
    const auto original=evaluate_facet_barycentric(local_parent,corner);
    auto transformed=local_parent; for(auto& vertex:transformed) vertex.position=transform(vertex.position);
    const auto remapped=evaluate_facet_barycentric(transformed,corner);
    const auto reversed=evaluate_facet_barycentric(reversed_parent,corner);
    const auto expected=transform(original);
    CHECK(remapped.x==doctest::Approx(expected.x));
    CHECK(remapped.y==doctest::Approx(expected.y));
    CHECK(remapped.z==doctest::Approx(expected.z));
    CHECK(reversed.x==doctest::Approx(original.x));
    CHECK(reversed.y==doctest::Approx(original.y));
    CHECK(reversed.z==doctest::Approx(original.z));
  }
  SUBCASE("gap or positive-area overlap is rejected exactly") {
    auto gap=left; gap.subfaces.pop_back();
    CHECK_FALSE(validate_frozen_facet_split(gap));
    auto overlap=left; overlap.subfaces[3]=overlap.subfaces[0]; overlap.subfaces[3].ordinal=3U;
    CHECK_FALSE(validate_frozen_facet_split(overlap));
  }
}

TEST_CASE("transition-output gate requires frozen outer facets and the exact retained core") {
  const auto input=nested_tetrahedra();
  const auto valid=tetra::probes::validate_surface_core_transition_output(input,matching_tetrahedral_shell());
  CHECK(valid.valid);
  CHECK(valid.frozen_outer_faces_preserved);
  CHECK(valid.retained_core_preserved);
  CHECK(valid.closed_two_manifold);
  SUBCASE("core substitution is rejected") {
    auto output=matching_tetrahedral_shell(); output.tetrahedra.pop_back();
    const auto report=tetra::probes::validate_surface_core_transition_output(input,output);
    CHECK_FALSE(report.valid);
    CHECK_FALSE(report.retained_core_preserved);
  }
  SUBCASE("unapproved outer boundary is rejected") {
    auto output=matching_tetrahedral_shell(); output.tetrahedra.pop_back();
    output.tetrahedra.push_back({{0,4,5,6}});
    const auto report=tetra::probes::validate_surface_core_transition_output(input,output);
    CHECK_FALSE(report.valid);
    CHECK(report.unexpected_boundary_faces>0U);
  }
  SUBCASE("strictly overlapping tetrahedra are rejected") {
    auto output=matching_tetrahedral_shell();
    output.tetrahedra.push_back({{0,1,2,4}});
    const auto report=tetra::probes::validate_surface_core_transition_output(input,output);
    CHECK_FALSE(report.valid);
    CHECK_FALSE(report.no_strict_tetrahedron_overlap);
    CHECK(report.tetrahedron_overlap_pairs>0U);
  }
  SUBCASE("same-sided shared face is rejected independently of overlap") {
    auto output=matching_tetrahedral_shell();
    // Both 4 and 5 lie on the same side of the outer 012 plane.
    output.tetrahedra.push_back({{0,1,2,5}});
    const auto report=tetra::probes::validate_surface_core_transition_output(input,output);
    CHECK_FALSE(report.valid);
    CHECK_FALSE(report.consistently_oriented_shared_faces);
    CHECK(report.same_sided_shared_faces>0U);
  }
}

TEST_CASE("prevalidated output audit matches the complete public audit") {
  using namespace tetra::probes;
  const auto input=nested_tetrahedra();
  REQUIRE(validate_surface_core_transition_input(input).accepted);
  const auto complete=validate_surface_core_transition_output(
      input,matching_tetrahedral_shell());
  const auto prevalidated=
      validate_surface_core_transition_output_assuming_valid_input(
          input,matching_tetrahedral_shell());
  CHECK(prevalidated.valid==complete.valid);
  CHECK(prevalidated.failure==complete.failure);
  CHECK(prevalidated.positive_tetrahedra==complete.positive_tetrahedra);
  CHECK(prevalidated.unique_tetrahedra==complete.unique_tetrahedra);
  CHECK(prevalidated.no_strict_tetrahedron_overlap==
        complete.no_strict_tetrahedron_overlap);
  CHECK(prevalidated.closed_two_manifold==complete.closed_two_manifold);
  CHECK(prevalidated.consistently_oriented_shared_faces==
        complete.consistently_oriented_shared_faces);
  CHECK(prevalidated.frozen_outer_faces_preserved==
        complete.frozen_outer_faces_preserved);
  CHECK(prevalidated.retained_core_preserved==complete.retained_core_preserved);
}

TEST_CASE("surface/core output contract rejects unproven geometric parent facets") {
  using namespace tetra::probes;
  auto input=nested_tetrahedra();
  input.stable_vertex_ids={101U,102U,103U,104U,105U,106U,107U,108U};
  for(const auto triangle:input.outer_faces) {
    FrozenFacetIdentity identity{{input.stable_vertex_ids[triangle[0]],input.stable_vertex_ids[triangle[1]],input.stable_vertex_ids[triangle[2]]}};
    std::sort(identity.vertex_ids.begin(),identity.vertex_ids.end());
    input.outer_parent_facets.push_back({identity,FacetPreservationMode::literal});
  }
  input.outer_parent_facets[0].mode=FacetPreservationMode::geometric;
  SUBCASE("missing geometric coverage is a specific failure") {
    const auto report=validate_surface_core_transition_output(input,matching_tetrahedral_shell());
    CHECK_FALSE(report.valid);
    CHECK(report.failure==SurfaceCoreOutputFailure::missing_facet_preservation);
  }
  SUBCASE("off-plane owned vertex is rejected before topology can bless it") {
    auto output=matching_tetrahedral_shell();
    output.owned_vertices={{100.0,100.0,100.0}};
    output.owned_vertex_ids={9001U};
    const auto split=split_frozen_facet(input.outer_parent_facets[0].identity,FacetPreservationMode::geometric,1U,2U);
    for(const auto& exact:split.subfaces)
      output.outer_preserved_facets.push_back({exact,{{8U,8U,8U}}});
    const auto report=validate_surface_core_transition_output(input,output);
    CHECK_FALSE(report.valid);
    CHECK(report.failure==SurfaceCoreOutputFailure::invalid_facet_preservation);
  }
  SUBCASE("non-owner chunk cannot emit an otherwise exact seam report") {
    auto output=matching_tetrahedral_shell();
    // This is the adjacent chunk's independently derived split. Its exact
    // barycentrics match, but the owner bit is false and must fail closed.
    const auto split=split_frozen_facet(input.outer_parent_facets[0].identity,FacetPreservationMode::geometric,2U,1U);
    const std::array<FrozenFacetVertex,3> parent{{
      {101U,input.vertices[0]}, {102U,input.vertices[1]}, {103U,input.vertices[2]}}};
    for(const auto& exact:split.subfaces) {
      OutputFacetSubface reported; reported.exact=exact;
      for(std::size_t corner=0;corner<3U;++corner) {
        reported.vertices[corner]=static_cast<std::uint32_t>(input.vertices.size()+output.owned_vertices.size());
        output.owned_vertices.push_back(evaluate_facet_barycentric(parent,exact.corners[corner]));
        output.owned_vertex_ids.push_back(9100U+output.owned_vertex_ids.size());
      }
      output.outer_preserved_facets.push_back(reported);
    }
    const auto report=validate_surface_core_transition_output(input,output);
    CHECK_FALSE(report.valid);
    CHECK(report.failure==SurfaceCoreOutputFailure::geometric_facet_not_emitted);
  }
  SUBCASE("retained explicit core rejects a refined core interface") {
    auto literal=nested_tetrahedra();
    literal.core_parent_facets.push_back({FrozenFacetIdentity{{4U,5U,6U}},FacetPreservationMode::geometric});
    literal.core_parent_facets.push_back({FrozenFacetIdentity{{4U,5U,7U}},FacetPreservationMode::literal});
    literal.core_parent_facets.push_back({FrozenFacetIdentity{{4U,6U,7U}},FacetPreservationMode::literal});
    literal.core_parent_facets.push_back({FrozenFacetIdentity{{5U,6U,7U}},FacetPreservationMode::literal});
    const auto report=validate_surface_core_transition_output(literal,matching_tetrahedral_shell());
    CHECK_FALSE(report.valid);
    CHECK(report.failure==SurfaceCoreOutputFailure::unsupported_geometric_core);
  }
}

TEST_CASE("homologous-front constructor is deterministic and fails closed outside its topology contract") {
  const auto input=nested_tetrahedra();
  tetra::probes::SurfaceCoreConstructionOptions options;
  options.outer_to_core_vertex={4U,5U,6U,7U,0U,1U,2U,3U};
  const auto first=tetra::probes::construct_homologous_surface_core_transition(input,options);
  const auto second=tetra::probes::construct_homologous_surface_core_transition(input,options);
  CHECK(first.succeeded);
  CHECK(first.failure==tetra::probes::SurfaceCoreConstructionFailure::none);
  CHECK(first.validation.valid);
  CHECK(first.output.tetrahedra==second.output.tetrahedra);
  CHECK(first.minimum_dihedral_degrees>=5.0);
  SUBCASE("rigidly transformed input preserves topology and quality") {
    auto transformed=input;
    for (auto& p:transformed.vertices) {
      const auto x=p.x, y=p.y, z=p.z;
      p={-y+7.0,x-3.0,z+2.0};
    }
    const auto report=tetra::probes::construct_homologous_surface_core_transition(transformed,options);
    CHECK(report.succeeded);
    CHECK(report.output.tetrahedra==first.output.tetrahedra);
    CHECK(report.minimum_dihedral_degrees==doctest::Approx(first.minimum_dihedral_degrees));
  }
  SUBCASE("missing correspondence fails without a partial mesh") {
    options.outer_to_core_vertex.pop_back();
    const auto report=tetra::probes::construct_homologous_surface_core_transition(input,options);
    CHECK_FALSE(report.succeeded);
    CHECK(report.failure==tetra::probes::SurfaceCoreConstructionFailure::missing_or_invalid_correspondence);
    CHECK(report.output.tetrahedra.empty());
  }
  SUBCASE("non-homologous regular-core boundary fails without a partial mesh") {
    options.outer_to_core_vertex={4U,5U,6U,0U,1U,2U,3U,7U};
    const auto report=tetra::probes::construct_homologous_surface_core_transition(input,options);
    CHECK_FALSE(report.succeeded);
    CHECK(report.failure==tetra::probes::SurfaceCoreConstructionFailure::core_front_not_homologous);
    CHECK(report.output.tetrahedra.empty());
  }
}

TEST_CASE("generic surface/core contract rejects invalid input before transition construction") {
  SUBCASE("open outer front") {
    auto input=nested_tetrahedra(); input.outer_faces.pop_back();
    const auto report=tetra::probes::validate_surface_core_transition_input(input);
    CHECK_FALSE(report.accepted);
    CHECK(report.failure==tetra::probes::SurfaceCoreInputFailure::outer_not_closed_two_manifold);
  }
  SUBCASE("duplicate frozen outer facet") {
    auto input=nested_tetrahedra(); input.outer_faces.push_back(input.outer_faces.front());
    const auto report=tetra::probes::validate_surface_core_transition_input(input);
    CHECK_FALSE(report.accepted);
    CHECK(report.failure==tetra::probes::SurfaceCoreInputFailure::duplicate_outer_face);
  }
  SUBCASE("inconsistently wound outer surface") {
    auto input=nested_tetrahedra(); std::swap(input.outer_faces[0][1],input.outer_faces[0][2]);
    const auto report=tetra::probes::validate_surface_core_transition_input(input);
    CHECK_FALSE(report.accepted);
    CHECK(report.failure==tetra::probes::SurfaceCoreInputFailure::outer_inconsistent_orientation);
  }
  SUBCASE("self-intersecting outer components") {
    auto input=nested_tetrahedra();
    input.vertices.insert(input.vertices.end(),{{-1.5,-1.5,-1.5},{2.5,-1.5,-1.5},{-1.5,2.5,-1.5},{-1.5,-1.5,2.5}});
    input.outer_faces.insert(input.outer_faces.end(),{{{8,10,9}},{{8,9,11}},{{8,11,10}},{{9,10,11}}});
    const auto report=tetra::probes::validate_surface_core_transition_input(input);
    CHECK_FALSE(report.accepted);
    CHECK(report.failure==tetra::probes::SurfaceCoreInputFailure::outer_self_intersection);
  }
  SUBCASE("non-finite position") {
    auto input=nested_tetrahedra(); input.vertices[0].x=std::numeric_limits<double>::infinity();
    const auto report=tetra::probes::validate_surface_core_transition_input(input);
    CHECK_FALSE(report.accepted);
    CHECK(report.failure==tetra::probes::SurfaceCoreInputFailure::non_finite_vertex);
  }
  SUBCASE("non-manifold core") {
    auto input=nested_tetrahedra(); input.retained_core_tetrahedra.push_back({{4,5,6,0}});
    input.retained_core_tetrahedra.push_back({{4,5,6,1}});
    const auto report=tetra::probes::validate_surface_core_transition_input(input);
    CHECK_FALSE(report.accepted);
    CHECK(report.failure==tetra::probes::SurfaceCoreInputFailure::core_not_two_manifold);
  }
  SUBCASE("bounded input") {
    auto input=nested_tetrahedra(); input.maximum_outer_faces=3U;
    const auto report=tetra::probes::validate_surface_core_transition_input(input);
    CHECK_FALSE(report.accepted);
    CHECK(report.failure==tetra::probes::SurfaceCoreInputFailure::resource_limit);
  }
  SUBCASE("frozen-facet quality is diagnostic, not an input gate") {
    auto input=nested_tetrahedra(); input.minimum_outer_triangle_angle_degrees=50.0;
    const auto report=tetra::probes::validate_surface_core_transition_input(input);
    CHECK(report.accepted);
    CHECK(report.failure==tetra::probes::SurfaceCoreInputFailure::none);
    CHECK(report.minimum_outer_triangle_angle_degrees<50.0);
  }
  SUBCASE("core outside the frozen outer boundary") {
    auto input=nested_tetrahedra(); input.vertices[4]={3.0,0.0,0.0};
    const auto report=tetra::probes::validate_surface_core_transition_input(input);
    CHECK_FALSE(report.accepted);
    CHECK(report.failure==tetra::probes::SurfaceCoreInputFailure::core_not_strictly_nested);
  }
  SUBCASE("concave outer surface crossing an all-inside core is rejected") {
    const auto report=tetra::probes::validate_surface_core_transition_input(
        concave_pit_with_core(-0.3));
    CHECK_FALSE(report.accepted);
    CHECK(report.failure==
          tetra::probes::SurfaceCoreInputFailure::core_touches_or_intersects_outer);
    CHECK(report.minimum_core_outer_clearance==doctest::Approx(0.0));
  }
  SUBCASE("core contact is rejected by the clearance contract") {
    const auto report=tetra::probes::validate_surface_core_transition_input(
        concave_pit_with_core(-0.6));
    CHECK_FALSE(report.accepted);
    CHECK(report.failure==
          tetra::probes::SurfaceCoreInputFailure::core_touches_or_intersects_outer);
    CHECK(report.minimum_core_outer_clearance<=5.0e-10);
  }
  SUBCASE("a clear concave-core gap is accepted") {
    const auto report=tetra::probes::validate_surface_core_transition_input(
        concave_pit_with_core(-0.61));
    CHECK(report.accepted);
    CHECK(report.minimum_core_outer_clearance>5.0e-10);
  }
}

TEST_CASE("surface/core PLC adapter preserves explicit rejection reasons") {
  using namespace tetra::probes;
  SUBCASE("open input") {
    auto input=nested_tetrahedra();input.outer_faces.pop_back();
    const auto result=materialize_canonical_plc_constraints(input);
    CHECK_FALSE(result.accepted());
    CHECK(result.surface_core_failure==
          SurfaceCoreInputFailure::outer_not_closed_two_manifold);
  }
  SUBCASE("degenerate input") {
    auto input=nested_tetrahedra();input.outer_faces[0][2]=input.outer_faces[0][1];
    const auto result=materialize_canonical_plc_constraints(input);
    CHECK_FALSE(result.accepted());
    CHECK(result.surface_core_failure==SurfaceCoreInputFailure::repeated_vertex);
  }
  SUBCASE("self-inconsistent orientation") {
    auto input=nested_tetrahedra();std::swap(input.outer_faces[0][1],input.outer_faces[0][2]);
    const auto result=materialize_canonical_plc_constraints(input);
    CHECK_FALSE(result.accepted());
    CHECK(result.surface_core_failure==
          SurfaceCoreInputFailure::outer_inconsistent_orientation);
  }
  SUBCASE("self-intersection") {
    auto input=nested_tetrahedra();
    input.vertices.insert(input.vertices.end(),{{-1.5,-1.5,-1.5},{2.5,-1.5,-1.5},
        {-1.5,2.5,-1.5},{-1.5,-1.5,2.5}});
    input.outer_faces.insert(input.outer_faces.end(),
        {{{8,10,9}},{{8,9,11}},{{8,11,10}},{{9,10,11}}});
    const auto result=materialize_canonical_plc_constraints(input);
    CHECK_FALSE(result.accepted());
    CHECK(result.surface_core_failure==SurfaceCoreInputFailure::outer_self_intersection);
    CHECK(result.related_element>result.failing_element);
  }
}

TEST_CASE("prevalidated surface/core PLC materialization is byte-for-byte equivalent") {
  using namespace tetra::probes;
  const auto input=nested_tetrahedra();
  REQUIRE(validate_surface_core_transition_input(input).accepted);
  const auto checked=materialize_canonical_plc_constraints(input);
  const auto prevalidated=
      materialize_canonical_plc_constraints_assuming_valid_input(input);
  REQUIRE(checked.accepted());
  REQUIRE(prevalidated.accepted());
  REQUIRE(prevalidated.constraints.vertices.size()==
          checked.constraints.vertices.size());
  for(std::size_t index=0U;index<checked.constraints.vertices.size();++index) {
    const auto& left=prevalidated.constraints.vertices[index];
    const auto& right=checked.constraints.vertices[index];
    CHECK(left.id==right.id);
    CHECK(left.position.x==right.position.x);
    CHECK(left.position.y==right.position.y);
    CHECK(left.position.z==right.position.z);
  }
  CHECK(prevalidated.constraints.facets==checked.constraints.facets);
  CHECK(prevalidated.constraints.exact_affine_planes.empty());
  CHECK(checked.constraints.exact_affine_planes.empty());
  CHECK(prevalidated.constraints.split_vertices.empty());
  CHECK(checked.constraints.split_vertices.empty());
  CHECK(prevalidated.constraints.recovery_journal.empty());
  CHECK(checked.constraints.recovery_journal.empty());
}
