#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tetra_viewer {

struct Rgb8Image {
  std::uint32_t width{};
  std::uint32_t height{};
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] bool valid() const noexcept;
};

struct Rgb8ImageAnalysis {
  std::array<std::uint8_t,3> minimum{};
  std::array<std::uint8_t,3> maximum{};
  std::array<double,3> mean{};
  double luminance_mean{};
  double luminance_standard_deviation{};
  double black_fraction{};
  double clipped_fraction{};
  std::size_t sampled_pixels{};
};

struct Rgb8ImageComparison {
  std::array<double,3> mean_absolute_error{};
  std::array<double,3> root_mean_square_error{};
  std::array<std::uint8_t,3> maximum_absolute_error{};
  double luminance_mean_absolute_error{};
  double changed_fraction{};
  std::size_t sampled_pixels{};
};

struct ScalarSampleSummary {
  double minimum{};
  double median{};
  double percentile_95{};
  double maximum{};
  std::size_t count{};
};

// Convert a tightly packed four-channel Vulkan readback to a canonical,
// top-to-bottom RGB image.  The explicit layout and row-direction arguments
// keep platform swapchain format and image convention out of golden data.
[[nodiscard]] bool make_rgb8_image(
    std::span<const std::uint8_t> packed_four_channel,
    std::uint32_t width,std::uint32_t height,bool bgra,bool bottom_up,
    Rgb8Image& destination,std::string& error);

[[nodiscard]] std::uint64_t rgb8_hash(const Rgb8Image& image) noexcept;
[[nodiscard]] bool write_ppm(std::string_view path,const Rgb8Image& image,
                             std::string& error);
[[nodiscard]] bool read_ppm(std::string_view path,Rgb8Image& image,
                            std::string& error);
[[nodiscard]] bool make_reversed_depth_mask(
    std::span<const float> reversed_depth,std::uint32_t width,
    std::uint32_t height,std::vector<std::uint8_t>& mask,
    std::string& error);
[[nodiscard]] bool write_pgm(std::string_view path,std::uint32_t width,
                             std::uint32_t height,
                             std::span<const std::uint8_t> values,
                             std::string& error);
[[nodiscard]] std::optional<std::array<std::uint32_t,2>> parse_pixel_extent(
    std::string_view text) noexcept;
[[nodiscard]] std::optional<ScalarSampleSummary> summarize_samples(
    std::span<const double> samples);

// A missing mask includes every pixel.  Otherwise one byte per pixel is
// required and zero excludes that pixel.  This supports stable sky, horizon,
// terrain, silhouette, and shadow regions without baking their policy into the
// generic comparison implementation.
[[nodiscard]] Rgb8ImageAnalysis analyse_rgb8_image(
    const Rgb8Image& image,std::span<const std::uint8_t> mask={});
[[nodiscard]] bool compare_rgb8_images(
    const Rgb8Image& reference,const Rgb8Image& candidate,
    Rgb8ImageComparison& comparison,std::string& error,
    std::span<const std::uint8_t> mask={});

}  // namespace tetra_viewer
