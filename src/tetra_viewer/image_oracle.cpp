#include "tetra_viewer/image_oracle.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace tetra_viewer {
namespace {

constexpr double luminance(std::uint8_t red,std::uint8_t green,
                           std::uint8_t blue) noexcept {
  return (0.2126*red+0.7152*green+0.0722*blue)/255.0;
}

bool mask_is_valid(std::span<const std::uint8_t> mask,
                   std::size_t pixel_count) noexcept {
  return mask.empty()||mask.size()==pixel_count;
}

bool read_token(std::istream& input,std::string& token) {
  token.clear();
  while(input){
    input>>std::ws;
    if(input.peek()!='#')break;
    input.ignore(std::numeric_limits<std::streamsize>::max(),'\n');
  }
  return static_cast<bool>(input>>token);
}

}  // namespace

bool Rgb8Image::valid() const noexcept {
  if(width==0U||height==0U)return false;
  const auto pixel_count=static_cast<std::size_t>(width)*height;
  return pixel_count<=std::numeric_limits<std::size_t>::max()/3U&&
         pixels.size()==pixel_count*3U;
}

bool make_rgb8_image(std::span<const std::uint8_t> source,
                     std::uint32_t width,std::uint32_t height,bool bgra,
                     bool bottom_up,Rgb8Image& destination,
                     std::string& error) {
  destination={};error.clear();
  if(width==0U||height==0U){error="image extent is empty";return false;}
  const auto pixel_count=static_cast<std::size_t>(width)*height;
  if(pixel_count>std::numeric_limits<std::size_t>::max()/4U||
     source.size()!=pixel_count*4U){
    error="four-channel source size does not match image extent";return false;
  }
  destination.width=width;destination.height=height;
  destination.pixels.resize(pixel_count*3U);
  for(std::uint32_t y=0;y<height;++y){
    const auto source_y=bottom_up?height-1U-y:y;
    for(std::uint32_t x=0;x<width;++x){
      const auto source_offset=(static_cast<std::size_t>(source_y)*width+x)*4U;
      const auto destination_offset=(static_cast<std::size_t>(y)*width+x)*3U;
      destination.pixels[destination_offset]=source[source_offset+(bgra?2U:0U)];
      destination.pixels[destination_offset+1U]=source[source_offset+1U];
      destination.pixels[destination_offset+2U]=source[source_offset+(bgra?0U:2U)];
    }
  }
  return true;
}

std::uint64_t rgb8_hash(const Rgb8Image& image) noexcept {
  std::uint64_t hash=1469598103934665603ULL;
  for(const auto value:image.pixels){hash^=value;hash*=1099511628211ULL;}
  return hash;
}

bool write_ppm(std::string_view path,const Rgb8Image& image,
               std::string& error) {
  error.clear();
  if(!image.valid()){error="RGB image is invalid";return false;}
  std::ofstream output(std::string(path),std::ios::binary);
  if(!output){error="could not open image path";return false;}
  output<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
  output.write(reinterpret_cast<const char*>(image.pixels.data()),
               static_cast<std::streamsize>(image.pixels.size()));
  if(!output){error="could not write image";return false;}
  return true;
}

bool read_ppm(std::string_view path,Rgb8Image& image,std::string& error) {
  image={};error.clear();
  std::ifstream input(std::string(path),std::ios::binary);
  if(!input){error="could not open image path";return false;}
  std::string magic,width,height,maximum;
  if(!read_token(input,magic)||!read_token(input,width)||
     !read_token(input,height)||!read_token(input,maximum)||magic!="P6"||
     maximum!="255"){
    error="image is not an 8-bit binary RGB PPM";return false;
  }
  try{
    const auto parsed_width=std::stoul(width);
    const auto parsed_height=std::stoul(height);
    if(parsed_width==0U||parsed_height==0U||
       parsed_width>std::numeric_limits<std::uint32_t>::max()||
       parsed_height>std::numeric_limits<std::uint32_t>::max())
      throw std::out_of_range("PPM extent");
    image.width=static_cast<std::uint32_t>(parsed_width);
    image.height=static_cast<std::uint32_t>(parsed_height);
  }catch(const std::exception&){error="PPM extent is invalid";return false;}
  input.get();
  const auto pixel_count=static_cast<std::size_t>(image.width)*image.height;
  if(pixel_count>std::numeric_limits<std::size_t>::max()/3U){
    error="PPM extent is too large";image={};return false;
  }
  image.pixels.resize(pixel_count*3U);
  input.read(reinterpret_cast<char*>(image.pixels.data()),
             static_cast<std::streamsize>(image.pixels.size()));
  if(input.gcount()!=static_cast<std::streamsize>(image.pixels.size())||
     input.peek()!=std::char_traits<char>::eof()){
    error="PPM payload size does not match its extent";image={};return false;
  }
  return true;
}

bool make_reversed_depth_mask(std::span<const float> reversed_depth,
                              std::uint32_t width,std::uint32_t height,
                              std::vector<std::uint8_t>& mask,
                              std::string& error) {
  mask.clear();error.clear();
  if(width==0U||height==0U||
     reversed_depth.size()!=static_cast<std::size_t>(width)*height){
    error="depth source size does not match image extent";return false;
  }
  mask.resize(reversed_depth.size());
  for(std::size_t index=0;index<reversed_depth.size();++index){
    const float value=reversed_depth[index];
    if(!std::isfinite(value)||value<0.0F||value>1.0F){
      error="reversed depth contains a non-finite or out-of-range value";
      mask.clear();return false;
    }
    mask[index]=value>1.0e-8F?255U:0U;
  }
  return true;
}

bool make_complement_mask(std::span<const std::uint8_t> source,
                          std::vector<std::uint8_t>& mask,
                          std::string& error) {
  mask.clear();error.clear();
  if(source.empty()){
    error="mask source is empty";return false;
  }
  mask.reserve(source.size());
  for(const auto value:source)mask.push_back(value==0U?255U:0U);
  return true;
}

bool make_silhouette_band_mask(std::span<const std::uint8_t> geometry,
                               std::uint32_t width,std::uint32_t height,
                               std::uint32_t radius,
                               std::vector<std::uint8_t>& mask,
                               std::string& error) {
  mask.clear();error.clear();
  if(width==0U||height==0U||radius==0U||
     geometry.size()!=static_cast<std::size_t>(width)*height){
    error="silhouette mask dimensions or radius are invalid";return false;
  }
  mask.assign(geometry.size(),0U);
  for(std::uint32_t y=0;y<height;++y)for(std::uint32_t x=0;x<width;++x){
    const bool inside=geometry[static_cast<std::size_t>(y)*width+x]!=0U;
    const auto minimum_x=x>radius?x-radius:0U;
    const auto minimum_y=y>radius?y-radius:0U;
    const auto maximum_x=static_cast<std::uint32_t>(std::min<std::uint64_t>(
        width-1U,static_cast<std::uint64_t>(x)+radius));
    const auto maximum_y=static_cast<std::uint32_t>(std::min<std::uint64_t>(
        height-1U,static_cast<std::uint64_t>(y)+radius));
    bool boundary=false;
    for(std::uint32_t sample_y=minimum_y;
        sample_y<=maximum_y&&!boundary;++sample_y)
      for(std::uint32_t sample_x=minimum_x;sample_x<=maximum_x;++sample_x)
        if((geometry[static_cast<std::size_t>(sample_y)*width+sample_x]!=0U)!=
           inside){boundary=true;break;}
    if(boundary)mask[static_cast<std::size_t>(y)*width+x]=255U;
  }
  return true;
}

bool make_outer_silhouette_band_mask(
    std::span<const std::uint8_t> geometry,std::uint32_t width,
    std::uint32_t height,std::uint32_t radius,
    std::vector<std::uint8_t>& mask,std::string& error) {
  if(!make_silhouette_band_mask(
         geometry,width,height,radius,mask,error))return false;
  for(std::size_t index=0;index<mask.size();++index)
    if(geometry[index]!=0U)mask[index]=0U;
  return true;
}

bool make_horizontal_band_mask(std::uint32_t width,std::uint32_t height,
                               double centre,double height_fraction,
                               std::vector<std::uint8_t>& mask,
                               std::string& error) {
  mask.clear();error.clear();
  if(width==0U||height==0U||!std::isfinite(centre)||
     !std::isfinite(height_fraction)||centre<0.0||centre>1.0||
     height_fraction<=0.0||height_fraction>1.0){
    error="horizontal mask dimensions or fractions are invalid";return false;
  }
  const double half=height_fraction*0.5;
  const auto first=static_cast<std::uint32_t>(std::floor(
      std::clamp(centre-half,0.0,1.0)*height));
  const auto last=static_cast<std::uint32_t>(std::ceil(
      std::clamp(centre+half,0.0,1.0)*height));
  mask.assign(static_cast<std::size_t>(width)*height,0U);
  for(std::uint32_t y=first;y<std::min(last,height);++y){
    const auto offset=static_cast<std::vector<std::uint8_t>::difference_type>(
        static_cast<std::size_t>(y)*width);
    std::fill_n(mask.begin()+offset,width,255U);
  }
  return true;
}

bool write_pgm(std::string_view path,std::uint32_t width,std::uint32_t height,
               std::span<const std::uint8_t> values,std::string& error) {
  error.clear();
  if(width==0U||height==0U||
     values.size()!=static_cast<std::size_t>(width)*height){
    error="mask size does not match image extent";return false;
  }
  std::ofstream output(std::string(path),std::ios::binary);
  if(!output){error="could not open mask path";return false;}
  output<<"P5\n"<<width<<' '<<height<<"\n255\n";
  output.write(reinterpret_cast<const char*>(values.data()),
               static_cast<std::streamsize>(values.size()));
  if(!output){error="could not write mask";return false;}
  return true;
}

std::optional<std::array<std::uint32_t,2>> parse_pixel_extent(
    std::string_view text) noexcept {
  const auto separator=text.find('x');
  if(separator==std::string_view::npos||separator==0U||
     separator+1U>=text.size()||text.find('x',separator+1U)!=
         std::string_view::npos)
    return std::nullopt;
  const auto parse=[](std::string_view component,
                      std::uint32_t& destination) noexcept {
    std::uint64_t value{};
    for(const char character:component){
      if(character<'0'||character>'9')return false;
      value=value*10U+static_cast<unsigned>(character-'0');
      if(value>8192U)return false;
    }
    if(value<64U)return false;
    destination=static_cast<std::uint32_t>(value);return true;
  };
  std::array<std::uint32_t,2> extent{};
  if(!parse(text.substr(0,separator),extent[0])||
     !parse(text.substr(separator+1U),extent[1]))return std::nullopt;
  return extent;
}

std::optional<ScalarSampleSummary> summarize_samples(
    std::span<const double> samples) {
  if(samples.empty()||std::ranges::any_of(samples,
      [](double value){return !std::isfinite(value)||value<0.0;}))
    return std::nullopt;
  std::vector<double> sorted(samples.begin(),samples.end());
  std::ranges::sort(sorted);
  const auto percentile=[&](double quantile){
    const double position=quantile*static_cast<double>(sorted.size()-1U);
    const auto lower=static_cast<std::size_t>(std::floor(position));
    const auto upper=static_cast<std::size_t>(std::ceil(position));
    const double fraction=position-static_cast<double>(lower);
    return sorted[lower]*(1.0-fraction)+sorted[upper]*fraction;
  };
  return ScalarSampleSummary{sorted.front(),percentile(0.5),percentile(0.95),
                             sorted.back(),sorted.size()};
}

Rgb8ImageAnalysis analyse_rgb8_image(const Rgb8Image& image,
                                     std::span<const std::uint8_t> mask) {
  Rgb8ImageAnalysis result;
  result.minimum.fill(255U);
  if(!image.valid()||!mask_is_valid(mask,image.pixels.size()/3U))return result;
  std::array<double,3> sum{};
  double luminance_sum{},luminance_squared_sum{};
  std::size_t black{},clipped{};
  for(std::size_t pixel=0;pixel<image.pixels.size()/3U;++pixel){
    if(!mask.empty()&&mask[pixel]==0U)continue;
    const auto offset=pixel*3U;
    bool all_black=true,any_clipped=false;
    for(std::size_t channel=0;channel<3U;++channel){
      const auto value=image.pixels[offset+channel];
      result.minimum[channel]=std::min(result.minimum[channel],value);
      result.maximum[channel]=std::max(result.maximum[channel],value);
      sum[channel]+=value;
      all_black=all_black&&value==0U;
      any_clipped=any_clipped||value==255U;
    }
    const double value=luminance(image.pixels[offset],image.pixels[offset+1U],
                                 image.pixels[offset+2U]);
    luminance_sum+=value;luminance_squared_sum+=value*value;
    black+=all_black?1U:0U;clipped+=any_clipped?1U:0U;
    ++result.sampled_pixels;
  }
  if(result.sampled_pixels==0U){result.minimum={};return result;}
  const double count=static_cast<double>(result.sampled_pixels);
  for(std::size_t channel=0;channel<3U;++channel)
    result.mean[channel]=sum[channel]/count;
  result.luminance_mean=luminance_sum/count;
  result.luminance_standard_deviation=std::sqrt(std::max(
      0.0,luminance_squared_sum/count-result.luminance_mean*result.luminance_mean));
  result.black_fraction=static_cast<double>(black)/count;
  result.clipped_fraction=static_cast<double>(clipped)/count;
  return result;
}

bool compare_rgb8_images(const Rgb8Image& reference,
                         const Rgb8Image& candidate,
                         Rgb8ImageComparison& result,std::string& error,
                         std::span<const std::uint8_t> mask) {
  result={};error.clear();
  if(!reference.valid()||!candidate.valid()){
    error="image comparison requires valid RGB images";return false;
  }
  if(reference.width!=candidate.width||reference.height!=candidate.height){
    error="image extents differ";return false;
  }
  const auto pixel_count=reference.pixels.size()/3U;
  if(!mask_is_valid(mask,pixel_count)){
    error="image mask size does not match image extent";return false;
  }
  std::array<double,3> absolute_sum{},squared_sum{};
  double luminance_absolute_sum{};
  std::size_t changed{};
  for(std::size_t pixel=0;pixel<pixel_count;++pixel){
    if(!mask.empty()&&mask[pixel]==0U)continue;
    const auto offset=pixel*3U;
    bool pixel_changed=false;
    for(std::size_t channel=0;channel<3U;++channel){
      const auto difference=std::abs(
          static_cast<int>(candidate.pixels[offset+channel])-
          static_cast<int>(reference.pixels[offset+channel]));
      absolute_sum[channel]+=difference;
      squared_sum[channel]+=static_cast<double>(difference*difference);
      result.maximum_absolute_error[channel]=std::max(
          result.maximum_absolute_error[channel],
          static_cast<std::uint8_t>(difference));
      pixel_changed=pixel_changed||difference!=0;
    }
    const double reference_luminance=luminance(
        reference.pixels[offset],reference.pixels[offset+1U],
        reference.pixels[offset+2U]);
    const double candidate_luminance=luminance(
        candidate.pixels[offset],candidate.pixels[offset+1U],
        candidate.pixels[offset+2U]);
    luminance_absolute_sum+=std::abs(candidate_luminance-reference_luminance);
    changed+=pixel_changed?1U:0U;++result.sampled_pixels;
  }
  if(result.sampled_pixels==0U){error="image mask selects no pixels";return false;}
  const double count=static_cast<double>(result.sampled_pixels);
  for(std::size_t channel=0;channel<3U;++channel){
    result.mean_absolute_error[channel]=absolute_sum[channel]/(count*255.0);
    result.root_mean_square_error[channel]=std::sqrt(squared_sum[channel]/count)/255.0;
  }
  result.luminance_mean_absolute_error=luminance_absolute_sum/count;
  result.changed_fraction=static_cast<double>(changed)/count;
  return true;
}

}  // namespace tetra_viewer
