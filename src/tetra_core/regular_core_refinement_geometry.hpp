#pragma once

#include "tetra_core/regular_core_refinement.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace tetra {
struct RegularCorePoint { double x{},y{},z{}; friend constexpr bool operator==(RegularCorePoint,RegularCorePoint)=default; };
struct RegularCoreGeometryVertex { std::uint64_t id{}; RegularCorePoint point{}; friend constexpr bool operator==(RegularCoreGeometryVertex,RegularCoreGeometryVertex)=default; };
struct RegularCoreGeometricParent { RegularCoreParentId id{}; std::array<std::uint64_t,4> vertices{}; };
struct RegularCoreGeometryDescriptor { std::vector<RegularCoreGeometryVertex> vertices; std::vector<RegularCoreGeometricParent> parents; };
enum class RegularCoreMaterializationRefusal : std::uint8_t { none, rejected_cut, malformed_geometry, stable_id_mismatch, nonpositive, overlap, partition_failure, quality_failure };
struct RegularCoreChildTetrahedron { RegularCoreLeafAddress address{}; std::array<std::uint64_t,4> vertices{}; friend constexpr bool operator==(RegularCoreChildTetrahedron,RegularCoreChildTetrahedron)=default; };
struct RegularCoreMaterialization {
  RegularCoreMaterializationRefusal refusal{RegularCoreMaterializationRefusal::none};
  std::vector<RegularCoreGeometryVertex> vertices;
  std::vector<RegularCoreChildTetrahedron> children;
  double minimum_dihedral_degrees{};
  double maximum_dihedral_degrees{};
  [[nodiscard]] bool accepted() const noexcept { return refusal==RegularCoreMaterializationRefusal::none; }
};
// Transactional materialization of the fixed red 1->8 grammar. Generated
// midpoint IDs are canonical sorted-edge IDs in a reserved range.
[[nodiscard]] RegularCoreMaterialization materialize_regular_core_red(
    const std::vector<RegularCoreParent>& topology, const RegularCoreRefinementResult& cut,
    RegularCoreGeometryDescriptor geometry);
} // namespace tetra
