#pragma once
#include "tetra_core/regular_core_refinement_geometry.hpp"
#include "tetra_probes/surface_core_contract.hpp"

namespace tetra::probes {
// Provenance only: it validates and canonically describes PLC boundaries. It
// deliberately creates neither a bridge nor any tetrahedra.
enum class NonmatchingPlcManifestFailure : std::uint8_t {
  none, resource_limit, invalid_outer_plc, malformed_core_adjacency,
  invalid_core_classification,
};
struct NonmatchingPlcManifestInput {
  SurfaceCoreTransitionInput outer;
  std::vector<RegularCoreParent> core_topology;
  RegularCoreGeometryDescriptor core_geometry;
  // The former eight-parent default was sufficient only for the two-parent
  // control. Keep an explicit transactional bound, but admit a bounded
  // selected regular-core patch from the real DC fixture by default.
  std::size_t maximum_parents{4096U};
};
struct NonmatchingPlcManifest {
  // Canonically sorted stable vertex id -> position records.  These are the
  // geometry references an offline PLC/CDT consumer needs to resolve facets.
  std::vector<FrozenFacetVertex> vertex_geometry;
  std::vector<FrozenFacetSplit> outer_parent_coverage;
  std::vector<std::array<std::uint64_t,3>> outer_parent_windings;
  // Six parent faces / 24 red subfaces in the two-parent control.
  std::vector<FrozenFacetSplit> external_core_coverage;
  std::vector<std::array<std::uint64_t,3>> external_core_windings;
  std::vector<FrozenFacetIdentity> internal_core_faces;
  // The exact red-refined core to be retained after shell extraction. Its
  // stable vertices share the interface IDs above.
  std::vector<std::array<std::uint64_t,4>> materialized_core_tetrahedra;
};
struct NonmatchingPlcManifestResult {
  NonmatchingPlcManifestFailure failure{NonmatchingPlcManifestFailure::invalid_outer_plc};
  NonmatchingPlcManifest manifest;
  [[nodiscard]] bool accepted() const noexcept { return failure==NonmatchingPlcManifestFailure::none; }
};
[[nodiscard]] NonmatchingPlcManifestResult build_nonmatching_plc_manifest(
    const NonmatchingPlcManifestInput& input);

inline constexpr std::uint32_t nonmatching_plc_manifest_schema_version=1U;
inline constexpr std::uint64_t nonmatching_plc_derived_vertex_id_base=std::uint64_t{1}<<63U;
// Versioned, deterministic adapter payload.  A refused result serializes to
// no bytes.  The payload contains stable geometry, oriented parent records,
// and exact rational subface corners; it remains a manifest, not a mesh.
[[nodiscard]] std::vector<std::uint8_t> serialize_nonmatching_plc_manifest(
    const NonmatchingPlcManifestResult& result);
}  // namespace tetra::probes
