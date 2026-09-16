#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <string>

// Exercise the retained external-oracle outputs with the exact verifier used
// by the reproduction runner.  The verifier is intentionally built outside
// the application; including it here makes its geometry-valid / quality-fail
// distinction a deterministic regression rather than a prose-only claim.
#define main dc_shell_reference_verifier_main
#include "../../artifacts/dc-viability-2026-09-09/verify_shell.cpp"
#undef main

namespace {

int verify(const char* prefix,const char* resolution,bool require_quality) {
  std::array<char*,4> argv{{const_cast<char*>("verify_shell"),const_cast<char*>(prefix),
                            const_cast<char*>(resolution),const_cast<char*>("--require-quality")}};
  return dc_shell_reference_verifier_main(require_quality?4:3,argv.data());
}

} // namespace

TEST_CASE("retained external shell witnesses are geometrically valid but fail the S4 quality screen") {
  constexpr auto root="artifacts/dc-viability-2026-09-09/";
  const std::string n8=std::string{root}+"shell-n8";
  CHECK(verify(n8.c_str(),"8",false)==0);
  CHECK(verify(n8.c_str(),"8",true)==1);
}
