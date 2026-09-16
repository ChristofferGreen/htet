#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define N6_FINAL_THREE_REGIONS_TEST
#include "../../artifacts/dc-viability-2026-09-09/n6_final_three_regions_probe.cpp"
#undef N6_FINAL_THREE_REGIONS_TEST

namespace {
std::size_t shell_count() {
  std::ifstream in("artifacts/dc-viability-2026-09-09/shell-n6.1.ele");
  std::size_t shell{}; unsigned corners{},attributes{}; in>>shell>>corners>>attributes; return shell;
}
std::vector<Tet> canonical_tets(std::vector<Tet> tets) {
  for(auto& t:tets) std::sort(t.begin(),t.end());
  std::sort(tets.begin(),tets.end()); return tets;
}
void remap_domain_ids(Domain& d,const std::map<std::uint64_t,std::uint64_t>& ids) {
  const auto remap=[&](std::uint64_t id) { const auto it=ids.find(id); return it==ids.end()?id:it->second; };
  const auto remap_vertices=[&](const auto& source) { std::decay_t<decltype(source)> result; for(const auto& [id,p]:source) result.emplace(remap(id),p); return result; };
  d.vertices=remap_vertices(d.vertices); d.input_vertices=remap_vertices(d.input_vertices); d.frozen_vertices=remap_vertices(d.frozen_vertices);
  std::set<std::uint64_t> interfaces; for(const auto id:d.interface_vertices) interfaces.insert(remap(id)); d.interface_vertices=std::move(interfaces);
  for(auto& t:d.tets) for(auto& id:t) id=remap(id);
  std::map<Face,int> prescribed; for(const auto& [f,kind]:d.prescribed) prescribed.emplace(key({{remap(f[0]),remap(f[1]),remap(f[2])}}),kind); d.prescribed=std::move(prescribed);
}
}

TEST_CASE("N6 data-driven bounded repair preserves the passing witness contract") {
  CHECK(n6_final_three_regions_main()==0);
  const auto shell_input=shell_count();
  const auto witness=build_final_three_regions_witness();
  const auto candidate=build_final_three_regions_candidate();
  const auto shell=candidate.domain.tets.size()-96U;
  const auto report=audit(candidate.domain,shell);
  const auto quality=quality_of(candidate.domain);
  CHECK(audit(witness.domain,witness.domain.tets.size()-96U).quality);
  CHECK(witness.domain.tets.size()-96U==670U);
  CHECK(report.geometry);
  CHECK(report.quality);
  CHECK(quality.below_five==0U);
  CHECK(quality.minimum>5.13);
  CHECK(exact_interface(candidate.domain));
  CHECK(exact_frozen_vertices(candidate.domain));
  CHECK(exact_retained_core(read_domain("artifacts/dc-viability-2026-09-09/shell-n6",6U),shell_input,candidate.domain));
  CHECK(candidate.search_points>0U);
  CHECK(candidate.search_subcavities>0U);
  CHECK(candidate.retained_bytes<=65536U);
  CHECK(candidate.temporary_bytes<=131072U);

  SUBCASE("tetrahedron record order does not change the constructed complex") {
    auto reordered=read_domain("artifacts/dc-viability-2026-09-09/shell-n6",6U);
    std::reverse(reordered.tets.begin(),reordered.tets.begin()+static_cast<std::ptrdiff_t>(shell_input));
    const auto rebuilt=build_data_driven_n6_candidate(std::move(reordered),shell_input);
    CHECK(audit(rebuilt.domain,rebuilt.domain.tets.size()-96U).geometry);
    CHECK(exact_frozen_vertices(rebuilt.domain));
    CHECK(exact_retained_core(read_domain("artifacts/dc-viability-2026-09-09/shell-n6",6U),shell_input,rebuilt.domain));
    CHECK(canonical_tets(rebuilt.domain.tets)==canonical_tets(candidate.domain.tets));
  }

  SUBCASE("rigid transform and small input perturbation remain valid") {
    auto transformed=read_domain("artifacts/dc-viability-2026-09-09/shell-n6",6U);
    const auto transform=[](Vec3 p) { return Vec3{-p.y+3.0,p.x-2.0,p.z+1.0}; };
    for(auto* vertices:{&transformed.vertices,&transformed.input_vertices,&transformed.frozen_vertices})
      for(auto& [id,p]:*vertices) p=transform(p);
    const auto rotated=build_data_driven_n6_candidate(std::move(transformed),shell_input);
    CHECK(audit(rotated.domain,rotated.domain.tets.size()-96U).geometry);
    CHECK(quality_of(rotated.domain).below_five==0U);
    CHECK(exact_frozen_vertices(rotated.domain));
    CHECK(canonical_tets(rotated.domain.tets)==canonical_tets(candidate.domain.tets));

    auto perturbed=read_domain("artifacts/dc-viability-2026-09-09/shell-n6",6U);
    const auto id=perturbed.vertices.begin()->first;
    perturbed.vertices.at(id).x+=1e-9; perturbed.input_vertices.at(id).x+=1e-9; perturbed.frozen_vertices.at(id).x+=1e-9;
    const auto rebuilt=build_data_driven_n6_candidate(std::move(perturbed),shell_input);
    CHECK(audit(rebuilt.domain,rebuilt.domain.tets.size()-96U).geometry);
    CHECK(quality_of(rebuilt.domain).below_five==0U);
    CHECK(exact_frozen_vertices(rebuilt.domain));
  }

  SUBCASE("vertex renumbering changes no geometric construction decision") {
    auto renumbered=read_domain("artifacts/dc-viability-2026-09-09/shell-n6",6U);
    std::map<std::uint64_t,std::uint64_t> forward,reverse;
    for(const auto& [id,p]:renumbered.vertices) { const auto replacement=id+0x1000000ULL; forward.emplace(id,replacement); reverse.emplace(replacement,id); }
    remap_domain_ids(renumbered,forward);
    auto rebuilt=build_data_driven_n6_candidate(std::move(renumbered),shell_input);
    CHECK(audit(rebuilt.domain,rebuilt.domain.tets.size()-96U).geometry);
    CHECK(exact_frozen_vertices(rebuilt.domain));
    remap_domain_ids(rebuilt.domain,reverse);
    CHECK(canonical_tets(rebuilt.domain.tets)==canonical_tets(candidate.domain.tets));
  }
}
