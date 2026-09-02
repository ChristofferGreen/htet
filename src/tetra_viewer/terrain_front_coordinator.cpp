#include "tetra_viewer/terrain_front_coordinator.hpp"

#include <algorithm>
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

bool same_view_payload(
    const TerrainViewIdentity& view,const tetra::Camera& camera,
    std::uint64_t field_revision,std::uint64_t field_signature) noexcept {
  return view.valid()&&view.field_revision==field_revision&&
      view.field_signature==field_signature&&
      view.camera_signature==terrain_camera_signature(camera);
}

bool same_coverage_space(
    const PreviewSpatialKey& first,const PreviewSpatialKey& second) noexcept {
  return first.field_revision==second.field_revision&&
      first.field_signature==second.field_signature&&
      first.chart==second.chart&&
      first.configuration_signature==second.configuration_signature;
}

bool valid_cell(const PreviewCoverageCell& cell) noexcept {
  return cell.minimum_x<cell.maximum_x&&cell.minimum_z<cell.maximum_z;
}

bool contains_cell(
    const PreviewCoverageCell& outer,
    const PreviewCoverageCell& inner) noexcept {
  return valid_cell(outer)&&valid_cell(inner)&&
      outer.minimum_x<=inner.minimum_x&&outer.minimum_z<=inner.minimum_z&&
      outer.maximum_x>=inner.maximum_x&&outer.maximum_z>=inner.maximum_z;
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

TerrainViewIdentity make_terrain_view_identity(
    std::uint64_t view_epoch,std::uint64_t field_revision,
    std::uint64_t field_signature,const tetra::Camera& camera) {
  if(view_epoch==0U)
    throw std::invalid_argument("terrain view epoch must be nonzero");
  return {view_epoch,field_revision,field_signature,
          terrain_camera_signature(camera)};
}

bool preview_coverage_supports(
    const PreviewCoverage& coverage,const PreviewSpatialKey& desired) noexcept {
  if(!same_coverage_space(coverage.spatial_key,desired)||
     desired.level_origins.empty()||
     coverage.guarded_level_origins.size()!=desired.level_origins.size())
    return false;
  for(std::size_t index=0;index<desired.level_origins.size();++index){
    const auto& origin=desired.level_origins[index];
    const auto& guard=coverage.guarded_level_origins[index];
    if(origin.level!=guard.level||origin.sample_x<guard.minimum_x||
       origin.sample_x>guard.maximum_x||origin.sample_z<guard.minimum_z||
       origin.sample_z>guard.maximum_z)return false;
  }
  return true;
}

bool preview_coverage_contains(
    const PreviewCoverage& outer,const PreviewCoverage& inner) noexcept {
  if(!same_coverage_space(outer.spatial_key,inner.spatial_key)||
     outer.covered_cells.empty()||inner.covered_cells.empty())return false;
  if(outer.covered_cells==inner.covered_cells)return true;
  return std::ranges::all_of(inner.covered_cells,[&](const auto& inner_cell){
    return std::ranges::any_of(outer.covered_cells,[&](const auto& outer_cell){
      return contains_cell(outer_cell,inner_cell);
    });
  });
}

TerrainFrontCoordinator::TerrainFrontCoordinator(
    const TerrainViewIdentity& published_exact) {
  if(!published_exact.valid())
    throw std::invalid_argument("initial exact front has no view epoch");
  state_.current_view=published_exact;
  state_.exact_published=published_exact;
}

TerrainViewIdentity TerrainFrontCoordinator::observe_view(
    const tetra::Camera& camera,std::uint64_t field_revision,
    std::uint64_t field_signature,
    std::optional<PreviewSpatialKey> desired_preview_key) {
  if(desired_preview_key&&
     (desired_preview_key->field_revision!=field_revision||
      desired_preview_key->field_signature!=field_signature))
    throw std::invalid_argument(
        "desired preview key does not match the observed field");
  if(!same_view_payload(
         state_.current_view,camera,field_revision,field_signature)){
    if(state_.current_view.view_epoch==
       std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("terrain view epoch overflow");
    state_.current_view=make_terrain_view_identity(
        state_.current_view.view_epoch+1U,field_revision,field_signature,camera);
  }
  state_.desired_preview_key=std::move(desired_preview_key);
  discard_ineligible_preview();
  if(state_.preview_requested&&
     (!state_.desired_preview_key||
      state_.preview_requested->spatial_key!=*state_.desired_preview_key))
    state_.preview_requested.reset();
  if(state_.preview_awaiting_upload&&
     (!state_.desired_preview_key||
      state_.preview_awaiting_upload->request.spatial_key!=
          *state_.desired_preview_key))
    state_.preview_awaiting_upload.reset();
  else if(state_.preview_awaiting_upload)
    state_.preview_awaiting_upload->required_through_view_epoch=
        state_.current_view.view_epoch;
  return state_.current_view;
}

std::optional<PreviewRequestIdentity>
TerrainFrontCoordinator::request_preview() {
  if(!state_.desired_preview_key)return std::nullopt;
  if(state_.preview_visible&&
     state_.preview_visible->request.spatial_key==*state_.desired_preview_key&&
     preview_coverage_supports(
         state_.preview_visible->coverage,*state_.desired_preview_key))
    return std::nullopt;
  if(state_.preview_requested&&
     state_.preview_requested->spatial_key==*state_.desired_preview_key)
    return std::nullopt;
  PreviewRequestIdentity request{state_.current_view,*state_.desired_preview_key};
  state_.preview_requested=request;
  state_.preview_awaiting_upload.reset();
  state_.last_preview_outcome.reset();
  state_.preview_retirement_reason=PreviewRetirementReason::none;
  return request;
}

bool TerrainFrontCoordinator::complete_preview(
    const PreviewRequestIdentity& request,PreviewFrontOutcome outcome,
    std::optional<PreviewCoverage> coverage) {
  if(!request_is_current(request))return false;
  state_.last_preview_outcome=outcome;
  if(outcome==PreviewFrontOutcome::ready){
    if(!coverage||coverage->spatial_key!=request.spatial_key||
       !preview_coverage_supports(*coverage,*state_.desired_preview_key)){
      state_.last_preview_outcome=PreviewFrontOutcome::failed;
      state_.preview_requested.reset();
      state_.preview_awaiting_upload.reset();
      discard_ineligible_preview();
      return false;
    }
    if(published_exact_covers(state_.current_view.view_epoch,*coverage)){
      state_.preview_requested.reset();
      state_.preview_awaiting_upload.reset();
      state_.preview_retirement_reason=PreviewRetirementReason::exact_handoff;
      return false;
    }
    state_.preview_awaiting_upload=StagedPreviewFront{
        request,std::move(*coverage),state_.current_view.view_epoch};
    return true;
  }
  state_.preview_requested.reset();
  state_.preview_awaiting_upload.reset();
  discard_ineligible_preview();
  return false;
}

bool TerrainFrontCoordinator::complete_preview_upload(
    const PreviewRequestIdentity& request,bool succeeded) {
  if(!state_.preview_awaiting_upload||
     state_.preview_awaiting_upload->request!=request||
     !request_is_current(request))return false;
  auto staged=std::move(*state_.preview_awaiting_upload);
  state_.preview_awaiting_upload.reset();
  state_.preview_requested.reset();
  if(!succeeded){
    state_.last_preview_outcome=PreviewFrontOutcome::upload_failed;
    discard_ineligible_preview();
    return false;
  }
  state_.preview_visible=VisiblePreviewFront{
      std::move(staged.request),std::move(staged.coverage),
      staged.required_through_view_epoch};
  state_.last_preview_outcome=PreviewFrontOutcome::ready;
  state_.preview_retirement_reason=PreviewRetirementReason::none;
  return true;
}

void TerrainFrontCoordinator::publish_exact(
    const TerrainViewIdentity& view,const PreviewCoverage& exact_coverage) {
  if(!view.valid())
    throw std::invalid_argument("published exact front has no view epoch");
  if(exact_coverage.spatial_key.field_revision!=view.field_revision||
     exact_coverage.spatial_key.field_signature!=view.field_signature)
    throw std::invalid_argument(
        "published exact coverage does not match its view field");
  if(state_.exact_published&&
     view.view_epoch<state_.exact_published->view_epoch)return;
  if(state_.exact_published&&
     view.view_epoch==state_.exact_published->view_epoch&&
     view!=*state_.exact_published)
    throw std::invalid_argument(
        "one exact view epoch cannot identify two publications");
  state_.exact_published=view;
  state_.exact_published_coverage=exact_coverage;
  bool retired=false;
  if(state_.preview_awaiting_upload&&
     published_exact_covers(
         state_.preview_awaiting_upload->required_through_view_epoch,
         state_.preview_awaiting_upload->coverage)){
    state_.preview_awaiting_upload.reset();
    state_.preview_requested.reset();
    retired=true;
  }
  if(state_.preview_visible&&
     published_exact_covers(
         state_.preview_visible->required_through_view_epoch,
         state_.preview_visible->coverage)){
    state_.preview_visible.reset();
    retired=true;
  }
  if(retired)
    state_.preview_retirement_reason=PreviewRetirementReason::exact_handoff;
}

bool TerrainFrontCoordinator::request_is_current(
    const PreviewRequestIdentity& request) const noexcept {
  return state_.preview_requested&&*state_.preview_requested==request&&
      state_.desired_preview_key&&request.spatial_key==*state_.desired_preview_key;
}

bool TerrainFrontCoordinator::published_exact_covers(
    std::uint64_t required_through_view_epoch,
    const PreviewCoverage& preview_coverage) const noexcept {
  return state_.exact_published&&state_.exact_published_coverage&&
      state_.exact_published->view_epoch>=required_through_view_epoch&&
      state_.exact_published->field_revision==
          preview_coverage.spatial_key.field_revision&&
      state_.exact_published->field_signature==
          preview_coverage.spatial_key.field_signature&&
      preview_coverage_contains(
          *state_.exact_published_coverage,preview_coverage);
}

void TerrainFrontCoordinator::discard_ineligible_preview() {
  if(!state_.preview_visible)return;
  const bool field_changed=
      state_.preview_visible->request.spatial_key.field_revision!=
          state_.current_view.field_revision||
      state_.preview_visible->request.spatial_key.field_signature!=
          state_.current_view.field_signature;
  const bool supported=state_.desired_preview_key&&
      preview_coverage_supports(
          state_.preview_visible->coverage,*state_.desired_preview_key);
  if(field_changed||!supported){
    state_.preview_visible.reset();
    state_.preview_retirement_reason=field_changed
        ?PreviewRetirementReason::field_changed
        :PreviewRetirementReason::coverage_left;
    return;
  }
  state_.preview_visible->required_through_view_epoch=
      state_.current_view.view_epoch;
}

}  // namespace tetra_viewer
