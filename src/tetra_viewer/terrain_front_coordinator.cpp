#include "tetra_viewer/terrain_front_coordinator.hpp"

#include <bit>
#include <limits>
#include <stdexcept>

namespace tetra_viewer {
namespace {

constexpr std::uint64_t fnv_offset=1469598103934665603ULL;
constexpr std::uint64_t fnv_prime=1099511628211ULL;

void hash_u64(std::uint64_t& hash,std::uint64_t value) noexcept {
  for(unsigned int byte=0;byte<8U;++byte){
    hash^=(value>>(byte*8U))&0xffU;
    hash*=fnv_prime;
  }
}

void hash_double(std::uint64_t& hash,double value) noexcept {
  hash_u64(hash,std::bit_cast<std::uint64_t>(value));
}

bool same_source_payload(
    const TerrainSourceIdentity& source,const tetra::Camera& camera,
    std::uint64_t field_revision,std::uint64_t field_signature) noexcept {
  return source.valid()&&source.field_revision==field_revision&&
      source.field_signature==field_signature&&
      source.camera_signature==terrain_camera_signature(camera);
}

}  // namespace

std::uint64_t terrain_camera_signature(const tetra::Camera& camera) noexcept {
  std::uint64_t hash=fnv_offset;
  const double values[]{camera.position.x,camera.position.y,camera.position.z,
      camera.forward.x,camera.forward.y,camera.forward.z,
      camera.up.x,camera.up.y,camera.up.z,camera.vertical_fov_radians,
      camera.viewport_height_pixels,camera.aspect_ratio};
  for(const double value:values)hash_double(hash,value);
  return hash;
}

std::uint64_t terrain_field_signature(const tetra::Sphere& field) noexcept {
  std::uint64_t hash=fnv_offset;
  hash_u64(hash,static_cast<std::uint64_t>(field.kind));
  hash_double(hash,field.centre.x);hash_double(hash,field.centre.y);
  hash_double(hash,field.centre.z);hash_double(hash,field.radius);
  hash_double(hash,field.secondary);hash_double(hash,field.frequency);
  hash_double(hash,field.sampling_footprint);
  const auto& p=field.terrain;
  const double values[]{p.height_offset,p.landform_amplitude,p.landform_frequency,
      p.mountain_amplitude,p.mountain_ridge_frequency,p.mountain_range_frequency,
      p.planetary_mountain_amplitude_scale,p.planetary_mountain_frequency_scale,
      p.planetary_mountain_fade_start,p.planetary_mountain_fade_end,
      p.gameplay_hill_amplitude,p.gameplay_hill_frequency,
      p.gameplay_feature_amplitude,p.gameplay_feature_frequency,
      p.gameplay_region_frequency,p.gameplay_corridor_depth,
      p.gameplay_warp_amplitude,p.gameplay_warp_frequency,
      p.ground_roughness_amplitude,p.ground_roughness_frequency,
      p.spawn_flat_radius,p.spawn_blend_radius,p.planet_radius,
      p.analytic_ridge_height,p.analytic_ridge_centre_z,
      p.analytic_ridge_half_width};
  for(const double value:values)hash_double(hash,value);
  hash_u64(hash,p.analytic_ridge?1U:0U);
  return hash;
}

TerrainSourceIdentity make_terrain_source_identity(
    std::uint64_t source_epoch,std::uint64_t field_revision,
    std::uint64_t field_signature,const tetra::Camera& camera) {
  if(source_epoch==0U)
    throw std::invalid_argument("terrain source epoch must be nonzero");
  return {source_epoch,field_revision,field_signature,
          terrain_camera_signature(camera)};
}

TerrainFrontCoordinator::TerrainFrontCoordinator(
    const TerrainSourceIdentity& published_exact) {
  if(!published_exact.valid())
    throw std::invalid_argument("initial exact front has no source epoch");
  state_.current_source=published_exact;
  state_.exact_published=published_exact;
}

TerrainSourceIdentity TerrainFrontCoordinator::observe_source(
    const tetra::Camera& camera,std::uint64_t field_revision,
    std::uint64_t field_signature) {
  if(same_source_payload(
         state_.current_source,camera,field_revision,field_signature))
    return state_.current_source;
  if(state_.current_source.source_epoch==
     std::numeric_limits<std::uint64_t>::max())
    throw std::overflow_error("terrain source epoch overflow");
  const auto next_epoch=state_.current_source.source_epoch+1U;
  const bool retired=state_.preview_requested.has_value()||
      state_.preview_awaiting_upload.has_value()||
      state_.preview_visible.has_value();
  state_.current_source=make_terrain_source_identity(
      next_epoch,field_revision,field_signature,camera);
  state_.preview_requested.reset();
  state_.preview_awaiting_upload.reset();
  state_.preview_visible.reset();
  state_.preview_retirement_reason=retired
      ?PreviewRetirementReason::source_changed
      :PreviewRetirementReason::none;
  return state_.current_source;
}

bool TerrainFrontCoordinator::request_preview(
    const TerrainSourceIdentity& source) {
  if(!is_current(source))return false;
  state_.preview_requested=source;
  state_.preview_awaiting_upload.reset();
  state_.last_preview_outcome.reset();
  state_.preview_retirement_reason=PreviewRetirementReason::none;
  return true;
}

bool TerrainFrontCoordinator::complete_preview(
    const TerrainSourceIdentity& source,PreviewFrontOutcome outcome) {
  if(!state_.preview_requested||*state_.preview_requested!=source||
     !is_current(source))return false;
  state_.last_preview_outcome=outcome;
  if(outcome==PreviewFrontOutcome::ready){
    state_.preview_awaiting_upload=source;
    return true;
  }
  state_.preview_requested.reset();
  state_.preview_awaiting_upload.reset();
  state_.preview_visible.reset();
  return false;
}

bool TerrainFrontCoordinator::complete_preview_upload(
    const TerrainSourceIdentity& source,std::uint64_t coverage_signature,
    bool succeeded) {
  if(!state_.preview_awaiting_upload||
     *state_.preview_awaiting_upload!=source||!is_current(source))return false;
  state_.preview_awaiting_upload.reset();
  state_.preview_requested.reset();
  if(!succeeded){
    state_.last_preview_outcome=PreviewFrontOutcome::upload_failed;
    state_.preview_visible.reset();
    return false;
  }
  state_.preview_visible=VisiblePreviewFront{source,coverage_signature};
  state_.last_preview_outcome=PreviewFrontOutcome::ready;
  state_.preview_retirement_reason=PreviewRetirementReason::none;
  return true;
}

void TerrainFrontCoordinator::publish_exact(
    const TerrainSourceIdentity& source,ExactPreviewCoverage coverage) {
  if(!source.valid())
    throw std::invalid_argument("published exact front has no source epoch");
  state_.exact_published=source;
  bool retired=false;
  if(state_.preview_requested&&exact_can_replace_preview(
         source,*state_.preview_requested,coverage)){
    state_.preview_requested.reset();
    retired=true;
  }
  if(state_.preview_awaiting_upload&&exact_can_replace_preview(
         source,*state_.preview_awaiting_upload,coverage)){
    state_.preview_awaiting_upload.reset();
    retired=true;
  }
  if(state_.preview_visible&&exact_can_replace_preview(
         source,state_.preview_visible->source,coverage)){
    state_.preview_visible.reset();
    retired=true;
  }
  if(retired)
    state_.preview_retirement_reason=PreviewRetirementReason::exact_handoff;
}

bool TerrainFrontCoordinator::is_current(
    const TerrainSourceIdentity& source) const noexcept {
  return source.valid()&&source==state_.current_source;
}

bool TerrainFrontCoordinator::exact_can_replace_preview(
    const TerrainSourceIdentity& exact,
    const TerrainSourceIdentity& preview,
    ExactPreviewCoverage coverage) noexcept {
  return coverage==ExactPreviewCoverage::compatible&&
      exact.source_epoch>=preview.source_epoch&&
      exact.field_revision==preview.field_revision&&
      exact.field_signature==preview.field_signature;
}

}  // namespace tetra_viewer
