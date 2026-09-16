#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define COMPLETE_N6_DOMAIN_HARNESS_TEST
#include "../../artifacts/dc-viability-2026-09-09/complete_n6_domain_harness.cpp"
#undef COMPLETE_N6_DOMAIN_HARNESS_TEST

TEST_CASE("retained external N6 reference passes geometry but fails quality") {
  CHECK(complete_n6_domain_harness_main() == 0);
}
