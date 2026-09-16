#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "tetra_probes/nonmatching_red_frustum_control.hpp"
TEST_CASE("sequential outer-edge bisection rejects a noncanonical red-facet boundary") {
  const double s3=std::sqrt(3.0),s23=std::sqrt(2.0/3.0); const std::array<tetra::Vec3,4> inner{{{0,0,0},{1,0,0},{.5,s3/2,0},{.5,s3/6,s23}}}; tetra::Vec3 c{};for(auto p:inner)c=c+p;c=c/4.; tetra::probes::NonmatchingRedFrustumInput input;input.inner=inner;for(unsigned i=0;i<4;++i)input.outer[i]=c+(inner[i]-c)*3.;input.scale=3.;const auto r=tetra::probes::construct_nonmatching_red_frustum_control(input);INFO("failure="<<static_cast<int>(r.failure)<<" valid="<<r.validation.valid<<" vf="<<static_cast<int>(r.validation.failure)<<" missing="<<r.validation.missing_outer_faces<<" unexp="<<r.validation.unexpected_boundary_faces);CHECK_FALSE(r.accepted());CHECK(r.failure==tetra::probes::NonmatchingRedFrustumFailure::geometry_rejected);CHECK_FALSE(r.validation.valid);CHECK(r.output.tetrahedra.empty());
}
