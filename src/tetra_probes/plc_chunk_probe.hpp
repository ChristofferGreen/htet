#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tetra::probes {

struct PlcValidation {
  bool closed{};
  bool consistently_oriented{};
  bool non_self_intersecting{};
  bool terrain_triangles_unchanged{};
  std::size_t vertices{};
  std::size_t triangles{};
  std::size_t boundary_edges{};
  std::string reason;
};

struct PlcScaleResult {
  unsigned int outer_radius{};
  std::size_t available_terrain_triangles{};
  std::size_t selected_terrain_triangles{};
  std::size_t artificial_triangles{};
  std::uint64_t terrain_hash{};
  std::uint64_t plc_hash{};
  PlcValidation validation;
};

struct PlcChunkReport {
  std::vector<PlcScaleResult> scales;
  bool bounded{};
  bool adjacent_interface_identical{};
  std::uint64_t shared_interface_hash{};
  std::string conclusion;
};

// Uses a frozen triangulated plane as a canonical terrain proxy.  The fixed
// left/right requests are adjacent rectangles; each is closed below the
// terrain by deterministic vertical side faces and a bottom cap.  No terrain
// triangle is clipped or recreated.
[[nodiscard]] PlcChunkReport run_plc_chunk_probe(unsigned int max_outer_radius);
[[nodiscard]] std::string make_plc_chunk_report_json(const PlcChunkReport& report);
[[nodiscard]] std::string make_plc_chunk_probe_obj(unsigned int outer_radius, bool left_chunk);

} // namespace tetra::probes
