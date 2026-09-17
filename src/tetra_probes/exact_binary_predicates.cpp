#include "tetra_probes/exact_binary_predicates.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>


namespace tetra::probes {
namespace {

// Exact predicates overwhelmingly operate on a handful of 32-bit limbs.
// Keep those limbs in the Natural itself and spill only unusually wide
// binary64 constructions to the heap. This is a bounded scratch buffer, not
// a precision limit: every operation retains the vector fallback.
class SmallLimbs {
 public:
  [[nodiscard]] bool empty() const noexcept {return size()==0U;}
  [[nodiscard]] std::size_t size() const noexcept {
    return spilled_?heap_.size():inline_size_;
  }
  [[nodiscard]] std::uint32_t& operator[](std::size_t index) noexcept {
    return spilled_?heap_[index]:inline_[index];
  }
  [[nodiscard]] const std::uint32_t& operator[](
      std::size_t index) const noexcept {
    return spilled_?heap_[index]:inline_[index];
  }
  [[nodiscard]] std::uint32_t& back() noexcept {return (*this)[size()-1U];}
  [[nodiscard]] const std::uint32_t& back() const noexcept {
    return (*this)[size()-1U];
  }
  [[nodiscard]] std::uint32_t* begin() noexcept {return data();}
  [[nodiscard]] std::uint32_t* end() noexcept {return data()+size();}
  [[nodiscard]] const std::uint32_t* begin() const noexcept {return data();}
  [[nodiscard]] const std::uint32_t* end() const noexcept {
    return data()+size();
  }
  void reserve(std::size_t capacity) {
    if(capacity>inline_capacity)spill(capacity);
  }
  void push_back(std::uint32_t value) {
    if(spilled_) {heap_.push_back(value);return;}
    if(inline_size_<inline_capacity) {inline_[inline_size_++]=value;return;}
    spill(inline_capacity*2U);heap_.push_back(value);
  }
  void assign(std::size_t count,std::uint32_t value) {
    if(count<=inline_capacity) {
      if(spilled_)heap_.clear();
      spilled_=false;inline_size_=count;
      std::fill_n(inline_.begin(),count,value);return;
    }
    spill(count);heap_.assign(count,value);
  }
  void resize(std::size_t count) {
    const auto old_size=size();
    if(count<=inline_capacity) {
      if(spilled_) {
        std::copy_n(heap_.begin(),std::min(old_size,count),inline_.begin());
        heap_.clear();spilled_=false;
      }
      if(count>old_size)std::fill(inline_.begin()+old_size,
                                  inline_.begin()+count,0U);
      inline_size_=count;return;
    }
    spill(count);heap_.resize(count);
  }
  void pop_back() noexcept {
    if(spilled_)heap_.pop_back();
    else --inline_size_;
  }

 private:
  static constexpr std::size_t inline_capacity=16U;
  [[nodiscard]] std::uint32_t* data() noexcept {
    return spilled_?heap_.data():inline_.data();
  }
  [[nodiscard]] const std::uint32_t* data() const noexcept {
    return spilled_?heap_.data():inline_.data();
  }
  void spill(std::size_t capacity) {
    if(spilled_) {heap_.reserve(capacity);return;}
    heap_.reserve(capacity);
    heap_.assign(inline_.begin(),inline_.begin()+inline_size_);
    spilled_=true;
  }
  std::array<std::uint32_t,inline_capacity> inline_{};
  std::vector<std::uint32_t> heap_;
  std::size_t inline_size_{};
  bool spilled_{};
};

class Natural {
 public:
  Natural()=default;
  explicit Natural(std::uint64_t value) {
    if(value!=0U) { limbs_.push_back(static_cast<std::uint32_t>(value));
      if((value>>32U)!=0U) limbs_.push_back(static_cast<std::uint32_t>(value>>32U)); }
  }
  [[nodiscard]] bool zero() const { return limbs_.empty(); }
  [[nodiscard]] int compare(const Natural& other) const {
    if(limbs_.size()!=other.limbs_.size()) return limbs_.size()<other.limbs_.size()?-1:1;
    for(std::size_t index=limbs_.size();index>0U;--index)
      if(limbs_[index-1U]!=other.limbs_[index-1U]) return limbs_[index-1U]<other.limbs_[index-1U]?-1:1;
    return 0;
  }
  [[nodiscard]] Natural shifted(unsigned bits) const {
    if(zero()) return {};
    Natural result;const auto whole=bits/32U,partial=bits%32U;result.limbs_.assign(whole,0U);std::uint64_t carry{};
    for(const auto limb:limbs_) { const auto value=(static_cast<std::uint64_t>(limb)<<partial)|carry;result.limbs_.push_back(static_cast<std::uint32_t>(value));carry=value>>32U; }
    if(carry!=0U) result.limbs_.push_back(static_cast<std::uint32_t>(carry));return result;
  }
  [[nodiscard]] static Natural add(const Natural& left,const Natural& right) {
    Natural result;const auto count=std::max(left.limbs_.size(),right.limbs_.size());result.limbs_.reserve(count+1U);std::uint64_t carry{};
    for(std::size_t i=0;i<count;++i) {const auto value=carry+(i<left.limbs_.size()?left.limbs_[i]:0U)+(i<right.limbs_.size()?right.limbs_[i]:0U);result.limbs_.push_back(static_cast<std::uint32_t>(value));carry=value>>32U;}
    if(carry!=0U)result.limbs_.push_back(static_cast<std::uint32_t>(carry));return result;
  }
  [[nodiscard]] static Natural subtract(const Natural& left,const Natural& right) {
    Natural result;result.limbs_.resize(left.limbs_.size());std::uint64_t borrow{};
    for(std::size_t i=0;i<left.limbs_.size();++i) {const auto sub=(i<right.limbs_.size()?right.limbs_[i]:0U)+borrow;const auto value=static_cast<std::uint64_t>(left.limbs_[i])-sub;result.limbs_[i]=static_cast<std::uint32_t>(value);borrow=static_cast<std::uint64_t>(left.limbs_[i])<sub?1U:0U;}
    result.trim();return result;
  }
  [[nodiscard]] static Natural multiply(const Natural& left,const Natural& right) {
    if(left.zero()||right.zero())return {};
    Natural result;result.limbs_.assign(left.limbs_.size()+right.limbs_.size(),0U);
    for(std::size_t i=0;i<left.limbs_.size();++i) {std::uint64_t carry{};for(std::size_t j=0;j<right.limbs_.size();++j) {const auto value=static_cast<std::uint64_t>(result.limbs_[i+j])+static_cast<std::uint64_t>(left.limbs_[i])*right.limbs_[j]+carry;result.limbs_[i+j]=static_cast<std::uint32_t>(value);carry=value>>32U;}std::size_t index=i+right.limbs_.size();while(carry!=0U) {const auto value=static_cast<std::uint64_t>(result.limbs_[index])+carry;result.limbs_[index]=static_cast<std::uint32_t>(value);carry=value>>32U;++index;}}
    result.trim();return result;
  }
  [[nodiscard]] double scaled_to_double(int exponent) const {
    if(zero())return 0.0;
    const auto bit_length=[](const Natural& value) {
      const auto top=value.limbs_.back();
      return static_cast<unsigned>((value.limbs_.size()-1U)*32U+
          (32U-std::countl_zero(top)));
    };
    const unsigned bits=bit_length(*this);
    if(bits<=53U) {
      std::uint64_t value{};
      for(std::size_t i=limbs_.size();i>0U;--i)
        value=(value<<32U)|limbs_[i-1U];
      return std::scalbn(static_cast<double>(value),exponent);
    }
    const unsigned discarded=bits-53U;
    const auto bit_at=[&](unsigned bit) {
      return (limbs_[bit/32U]>>(bit%32U))&1U;
    };
    std::uint64_t leading{};
    for(unsigned bit=bits;bit>discarded;--bit)
      leading=(leading<<1U)|bit_at(bit-1U);
    bool sticky=false;
    for(unsigned bit=0U;bit+1U<discarded;++bit)sticky|=bit_at(bit)!=0U;
    if(bit_at(discarded-1U)!=0U&&(sticky||(leading&1U)!=0U))++leading;
    return std::scalbn(static_cast<double>(leading),
                       exponent+static_cast<int>(discarded));
  }
 private:
  void trim(){while(!limbs_.empty()&&limbs_.back()==0U)limbs_.pop_back();}
  SmallLimbs limbs_;
};

class Integer {
 public:
  Integer()=default;
  Integer(bool negative,Natural magnitude):negative_(negative&&!magnitude.zero()),magnitude_(std::move(magnitude)){}
  [[nodiscard]] ExactPredicateSign sign() const {return magnitude_.zero()?ExactPredicateSign::zero:(negative_?ExactPredicateSign::negative:ExactPredicateSign::positive);}
  [[nodiscard]] static Integer add(const Integer& left,const Integer& right) {
    if(left.negative_==right.negative_)return {left.negative_,Natural::add(left.magnitude_,right.magnitude_)};
    const auto comparison=left.magnitude_.compare(right.magnitude_);if(comparison==0)return {};if(comparison>0)return {left.negative_,Natural::subtract(left.magnitude_,right.magnitude_)};return {right.negative_,Natural::subtract(right.magnitude_,left.magnitude_)};
  }
  [[nodiscard]] static Integer subtract(const Integer& left,const Integer& right) {return add(left,{!right.negative_,right.magnitude_});}
  [[nodiscard]] static Integer multiply(const Integer& left,const Integer& right) {return {left.negative_!=right.negative_,Natural::multiply(left.magnitude_,right.magnitude_)};}
  [[nodiscard]] double scaled_to_double(int exponent) const {
    const auto value=magnitude_.scaled_to_double(exponent);
    return negative_?-value:value;
  }
 private:
  bool negative_{};Natural magnitude_{};
};

struct Binary { bool negative{};std::uint64_t mantissa{};int exponent{}; };
Binary binary(double value) {
  const auto bits=std::bit_cast<std::uint64_t>(value);const auto exponent=static_cast<unsigned>((bits>>52U)&0x7ffU);const auto fraction=bits&((std::uint64_t{1}<<52U)-1U);
  if(exponent==0U)return {(bits>>63U)!=0U,fraction,-1074};
  return {(bits>>63U)!=0U,(std::uint64_t{1}<<52U)|fraction,static_cast<int>(exponent)-1023-52};
}
int minimum_exponent(const std::initializer_list<double> values) {int result=0;bool first=true;for(const auto value:values){const auto part=binary(value);if(part.mantissa!=0U&&(first||part.exponent<result)){result=part.exponent;first=false;}}return result;}
Integer scaled(double value,int exponent) {const auto part=binary(value);return {part.negative,Natural{part.mantissa}.shifted(static_cast<unsigned>(part.exponent-exponent))};}
template<std::size_t Size>
Integer determinant(std::array<std::array<Integer,Size>,Size> matrix) {
  std::array<unsigned,Size> permutation{};
  for(unsigned i=0;i<Size;++i)permutation[i]=i;
  Integer sum;
  do {
    unsigned inversions{};
    Integer term{false,Natural{1U}};
    for(std::size_t row=0;row<Size;++row) {
      term=Integer::multiply(term,matrix[row][permutation[row]]);
      for(std::size_t later=row+1U;later<Size;++later)
        if(permutation[row]>permutation[later])++inversions;
    }
    if(inversions%2U!=0U)term=Integer::subtract(Integer{},term);
    sum=Integer::add(sum,term);
  } while(std::next_permutation(permutation.begin(),permutation.end()));
  return sum;
}
Integer orientation_determinant(Vec3 a,Vec3 b,Vec3 c,Vec3 d,int& exponent) {
  exponent=minimum_exponent({a.x,a.y,a.z,b.x,b.y,b.z,c.x,c.y,c.z,d.x,d.y,d.z});
  return determinant(std::array<std::array<Integer,3>,3>{{
      {{Integer::subtract(scaled(b.x,exponent),scaled(a.x,exponent)),Integer::subtract(scaled(b.y,exponent),scaled(a.y,exponent)),Integer::subtract(scaled(b.z,exponent),scaled(a.z,exponent))}},
      {{Integer::subtract(scaled(c.x,exponent),scaled(a.x,exponent)),Integer::subtract(scaled(c.y,exponent),scaled(a.y,exponent)),Integer::subtract(scaled(c.z,exponent),scaled(a.z,exponent))}},
      {{Integer::subtract(scaled(d.x,exponent),scaled(a.x,exponent)),Integer::subtract(scaled(d.y,exponent),scaled(a.y,exponent)),Integer::subtract(scaled(d.z,exponent),scaled(a.z,exponent))}}}});
}
}

ExactPredicateSign exact_orientation_3d(Vec3 a,Vec3 b,Vec3 c,Vec3 d) {int exponent{};return orientation_determinant(a,b,c,d,exponent).sign();}

double exact_orientation_3d_value(Vec3 a,Vec3 b,Vec3 c,Vec3 d) {
  int exponent{};
  return orientation_determinant(a,b,c,d,exponent).scaled_to_double(3*exponent);
}

ExactPredicateSign exact_in_sphere(Vec3 a,Vec3 b,Vec3 c,Vec3 d,Vec3 query) {
  const auto exponent=minimum_exponent({a.x,a.y,a.z,b.x,b.y,b.z,c.x,c.y,c.z,d.x,d.y,d.z,query.x,query.y,query.z});
  const auto row=[exponent,query](Vec3 point) {const auto x=Integer::subtract(scaled(point.x,exponent),scaled(query.x,exponent)),y=Integer::subtract(scaled(point.y,exponent),scaled(query.y,exponent)),z=Integer::subtract(scaled(point.z,exponent),scaled(query.z,exponent));const auto lift=Integer::add(Integer::add(Integer::multiply(x,x),Integer::multiply(y,y)),Integer::multiply(z,z));return std::array<Integer,4>{x,y,z,lift};};
  return determinant(std::array<std::array<Integer,4>,4>{{
      row(a),row(b),row(c),row(d)}}).sign();
}

} // namespace tetra::probes
