#include "tetra_probes/canonical_delaunay_seed.hpp"

#include <chrono>
#include "tetra_probes/wang_constrained_tetrahedralizer.hpp"
#include "tetra_probes/wang_ordered_tet_mesh.hpp"
#include "tetra_probes/wang_local_segment_recovery.hpp"
#include "tetra_probes/wang_segment_scheduler.hpp"
#include "tetra_probes/exact_binary_predicates.hpp"
#include "tetra_probes/surface_core_contract.hpp"
#include "tetra_core/regular_core_arbitrary_refinement.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory_resource>
#include <limits>
#include <numeric>
#include <cfloat>
#include <set>
#include <source_location>
#include <tuple>
#include <unordered_map>

namespace tetra::probes {
namespace {
struct Point { long double x{},y{},z{}; };
Point operator+(Point a,Point b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Point operator-(Point a,Point b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Point operator*(Point a,long double s){return {a.x*s,a.y*s,a.z*s};}
long double dot(Point a,Point b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Point cross(Point a,Point b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
long double orient(Point a,Point b,Point c,Point d){return dot(b-a,cross(c-a,d-a));}
using Tet=std::array<std::uint32_t,4>; using Face=std::array<std::uint32_t,3>;
Face face_key(Face f){std::sort(f.begin(),f.end());return f;}
struct FaceHash {
  [[nodiscard]] std::size_t operator()(const Face& face) const noexcept {
    std::size_t result=0xcbf29ce484222325ULL;
    for(const auto vertex:face) {
      result^=std::hash<std::uint32_t>{}(vertex);
      result*=0x100000001b3ULL;
    }
    return result;
  }
};
Face wang_bw_boundary_face(const Tet& cell,unsigned opposite) {
  static constexpr std::array<std::array<unsigned,3>,4> positions{{
      {{1U,2U,3U}},{{3U,2U,0U}},{{0U,1U,3U}},{{2U,1U,0U}}}};
  return {{cell[positions[opposite][0]],cell[positions[opposite][1]],
           cell[positions[opposite][2]]}};
}
Vec3 as_vec3(Point p) { return {static_cast<double>(p.x),static_cast<double>(p.y),static_cast<double>(p.z)}; }
int sign(ExactPredicateSign value) { return static_cast<int>(value); }

// The pinned implementation obtains Steiner coordinates from its double
// line/triangle routine, independently of the exact predicates used to
// classify the intersection.  Preserve that arithmetic boundary here.  The
// compensated affine combination is the behavioral equivalent of its
// fixedSplitPoint(): it retains the rounding residual of (1-t), rather than
// contracting the expression to p0+(p1-p0)*t.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline,optimize("O0")))
#endif
double wang_orient3d_value(Vec3 a,Vec3 b,Vec3 c,Vec3 d,
                           bool seed_source_convention=false) {
  const double adx=a.x-d.x,bdx=b.x-d.x,cdx=c.x-d.x;
  const double ady=a.y-d.y,bdy=b.y-d.y,cdy=c.y-d.y;
  const double adz=a.z-d.z,bdz=b.z-d.z,cdz=c.z-d.z;
  // Match the reference predicate's public orient3d return arithmetic.  This
  // value subsequently feeds fixedSplitPoint, so contracting it through FMA
  // changes the published Steiner coordinate by several ulps on real FHCs.
  const double bdxcdy=bdx*cdy,cdxbdy=cdx*bdy;
  const double cdxady=cdx*ady,adxcdy=adx*cdy;
  const double adxbdy=adx*bdy,bdxady=bdx*ady;
  // Keep the source's three separately rounded determinant terms.  The
  // point-placement routine consumes these binary64 values directly.
  const double first=adz*(bdxcdy-cdxbdy);
  const double second=bdz*(cdxady-adxcdy);
  const double third=cdz*(adxbdy-bdxady);
  const double partial=first+second;
  const double determinant=partial+third;
  const double permanent=(std::abs(bdxcdy)+std::abs(cdxbdy))*std::abs(adz)+
      (std::abs(cdxady)+std::abs(adxcdy))*std::abs(bdz)+
      (std::abs(adxbdy)+std::abs(bdxady))*std::abs(cdz);
  // The reference's adaptive orient3d returns an expansion-derived magnitude
  // when the ordinary determinant is inside its first error bound. Evaluate
  // the same binary input determinant exactly and round it once to binary64.
  constexpr double epsilon=std::numeric_limits<double>::epsilon()/2.0;
  constexpr double error_bound=(7.0+56.0*epsilon)*epsilon;
  if(std::abs(determinant)<=error_bound*permanent)
    // exact_orientation_3d_value is det(b-a,c-a,d-a), whereas Shewchuk's
    // orient3d (and the direct calculation above) is det(a-d,b-d,c-d).
    // They are sign opposites.  The seed traversal requests the source
    // convention; legacy non-seed callers retain their established contract.
    return (seed_source_convention?-1.0:1.0)*
        exact_orientation_3d_value(a,b,c,d);
  return determinant;
}

std::pair<double,double> two_sum(double a,double b) {
  const double sum=a+b;
  // Retain the reference expansion primitive's individual roundings.  In
  // particular, `avirt` must be materialised before `around`; folding it
  // into `a - (sum-bvirt)` produces a distinct binary64 result in later
  // fixedSplitPoint coordinates.
  const double bvirt=sum-a;
  const double avirt=sum-bvirt;
  const double bround=b-bvirt;
  const double around=a-avirt;
  return {sum,around+bround};
}

std::pair<double,double> two_product(double a,double b) {
  const double product=a*b;
  // Reconstruct the expansion product with Dekker splitting.  The Wang
  // reference is built around the classic expansion-arithmetic primitive;
  // an FMA computes the same exact residual mathematically, but changes the
  // rounded intermediates subsequently consumed by fixedSplitPoint().
  constexpr double splitter=134217729.0; // 2^27 + 1 for IEEE binary64.
  const double a_split=splitter*a;
  const double a_big=a_split-a;
  const double a_high=a_split-a_big;
  const double a_low=a-a_high;
  const double b_split=splitter*b;
  const double b_big=b_split-b;
  const double b_high=b_split-b_big;
  const double b_low=b-b_high;
  const double error1=product-a_high*b_high;
  const double error2=error1-a_low*b_high;
  const double error3=error2-a_high*b_low;
  const double error=a_low*b_low-error3;
  return {product,error};
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline,optimize("O0")))
#endif
double wang_fixed_split_point(double s0,double s1,double p0,double p1) {
  const auto two_one_sum=[](double a1,double a0,double b) {
    const auto [middle,x0]=two_sum(a0,b);
    const auto [x2,x1]=two_sum(a1,middle);
    return std::array<double,3>{{x2,x1,x0}};
  };
  const auto two_two_sum=[&](double a1,double a0,double b1,double b0) {
    const auto first=two_one_sum(a1,a0,b0);
    const auto second=two_one_sum(first[0],first[1],b1);
    return std::array<double,4>{{second[0],second[1],second[2],first[2]}};
  };
  const auto four_one_sum=[&](std::array<double,4> a,double b) {
    const auto low=two_one_sum(a[2],a[3],b);
    const auto high=two_one_sum(a[0],a[1],low[0]);
    return std::array<double,5>{{high[0],high[1],high[2],low[1],low[2]}};
  };
  const auto four_two_sum=[&](std::array<double,4> a,double b1,double b0) {
    const auto low=four_one_sum(a,b0);
    const auto high=four_one_sum(
        {{low[0],low[1],low[2],low[3]}},b1);
    return std::array<double,6>{{high[0],high[1],high[2],high[3],
                                  high[4],low[4]}};
  };
  const auto refined_divide=[&](double numerator,double denominator) {
    const double leading=numerator/denominator;
    const auto product=two_product(leading,denominator);
    const auto residual=two_one_sum(numerator,-product.first,-product.second);
    const auto quotient=two_two_sum(
        leading,residual[0]/denominator,residual[1]/denominator,
        residual[2]/denominator);
    return quotient[0];
  };

  const auto sum=two_sum(s0,s1);
  const double t=refined_divide(s0,sum.first);
  const auto one_minus=two_sum(1.0,-t);
  const auto right=two_product(t,p1);
  const auto left=two_product(one_minus.first,p0);
  const auto left_tail=two_product(one_minus.second,p0);
  const auto value=four_two_sum(
      {{right.first,right.second,left.first,left.second}},
      left_tail.first,left_tail.second);
  return value[0]+value[1];
}

Vec3 wang_segment_plane_hit(Vec3 a,Vec3 b,Vec3 f0,Vec3 f1,Vec3 f2) {
  double da=wang_orient3d_value(f0,f1,f2,a);
  double db=wang_orient3d_value(f0,f1,f2,b);
  // This is intentionally not abs() on both operands: it is the exact
  // `lin_tri_intersect3d` sign-normalization branch before fixedSplitPoint.
  if(da<0.0)da=-da;
  else db=-db;
  return {wang_fixed_split_point(da,db,a.x,b.x),
          wang_fixed_split_point(da,db,a.y,b.y),
          wang_fixed_split_point(da,db,a.z,b.z)};
}
bool point_inside_or_on_tet(
    const std::vector<Point>& points,const Tet& tet,Point query);
CanonicalLiteralEdgeFlipResult bowyer_watson_insert_constraint_vertex(
    const CanonicalPlcConstraintSet& constraints,std::size_t previous_vertex_count,
    const std::vector<Tet>& mesh,const std::vector<Tet>& seed_tetrahedra,
    bool protect_recovered_edge_shell=true,
    bool finite_hull_projection_adapter=true);
bool preserves_recovered_constraints_across_edge_split(
    const CanonicalPlcConstraintSet& before_constraints,
    const std::vector<Tet>& before_mesh,
    const CanonicalPlcConstraintSet& after_constraints,
    const std::vector<Tet>& after_mesh);

// The fast calculation is only a filter.  The exact fallback operates on the
// IEEE values in the seed's documented affine-normalized predicate domain.
// That domain is used only to choose combinatorics: original PLC coordinates
// are retained unchanged in all published cells and constraints.
int robust_orient(Point a, Point b, Point c, Point d) {
  const auto value=orient(a,b,c,d);
  long double magnitude=1.0L;
  for(const auto p:{a,b,c,d}) {
    magnitude=std::max({magnitude,std::abs(p.x),std::abs(p.y),std::abs(p.z)});
  }
  if(std::abs(value)>512.0L*LDBL_EPSILON*magnitude*magnitude*magnitude)
    return value<0.0L?-1:1;
  return sign(exact_orientation_3d(as_vec3(a),as_vec3(b),as_vec3(c),as_vec3(d)));
}
int robust_orient(const std::vector<Point>& points, const Tet& tet) {
  return robust_orient(points[tet[0]],points[tet[1]],points[tet[2]],points[tet[3]]);
}

bool segment_intersects_tetrahedron_interior(
    const std::vector<Point>& points,const Tet& tet,Point start,Point end) {
  long double low=0.0L,high=1.0L;
  for(unsigned omit=0U;omit<4U;++omit) {
    Face face{};unsigned n{};
    for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=tet[i];
    const auto reference=robust_orient(
        points[face[0]],points[face[1]],points[face[2]],points[tet[omit]]);
    if(reference==0)return false;
    const auto start_sign=robust_orient(
        points[face[0]],points[face[1]],points[face[2]],start)*reference;
    const auto end_sign=robust_orient(
        points[face[0]],points[face[1]],points[face[2]],end)*reference;
    if(start_sign<0&&end_sign<0)return false;
    if(start_sign>=0&&end_sign>=0)continue;
    auto first=orient(points[face[0]],points[face[1]],points[face[2]],start)*reference;
    auto second=orient(points[face[0]],points[face[1]],points[face[2]],end)*reference;
    if(first==second)return false;
    const auto crossing=-first/(second-first);
    if(start_sign<0)low=std::max(low,crossing);
    else high=std::min(high,crossing);
    if(!(high>low))return false;
  }
  return high-low>1.0e-14L;
}

long double in_sphere_filter(Point a, Point b, Point c, Point d, Point query) {
  const auto row=[query](Point p) { const auto delta=p-query; return std::array<long double,4>{
    delta.x,delta.y,delta.z,dot(delta,delta)}; };
  std::array<std::array<long double,4>,4> matrix{{row(a),row(b),row(c),row(d)}};
  long double determinant=1.0L;
  for(unsigned column=0;column<4U;++column) {
    unsigned pivot=column;
    for(unsigned candidate=column+1U;candidate<4U;++candidate)
      if(std::abs(matrix[candidate][column])>std::abs(matrix[pivot][column])) pivot=candidate;
    if(matrix[pivot][column]==0.0L) return 0.0L;
    if(pivot!=column) { std::swap(matrix[pivot],matrix[column]); determinant=-determinant; }
    const auto divisor=matrix[column][column]; determinant*=divisor;
    for(unsigned row_index=column+1U;row_index<4U;++row_index) {
      const auto factor=matrix[row_index][column]/divisor;
      for(unsigned index=column+1U;index<4U;++index) matrix[row_index][index]-=factor*matrix[column][index];
    }
  }
  return determinant;
}

int perturbed_in_sphere_sign(const std::vector<Point>& points, const Tet& tet,
                             std::uint32_t query, const std::vector<std::uint32_t>& rank) {
  // Diazzi et al. (2023), Algorithm 1.  This is the simulation-of-simplicity
  // rule used when the exact in-sphere determinant is zero.  The ranks are
  // stable-ID insertion ranks, rather than transient input indices, so a
  // reordered request reaches the same combinatorial decision.
  const std::array<std::uint32_t,5> original{{tet[0],tet[1],tet[2],tet[3],query}};
  auto sorted=original;
  unsigned swaps{};
  for(unsigned pass=0U;pass<sorted.size();++pass) for(unsigned index=1U;index<sorted.size()-pass;++index) {
    if(rank[sorted[index]]<rank[sorted[index-1U]]) {
      std::swap(sorted[index],sorted[index-1U]);++swaps;
    }
  }
  auto result=robust_orient(points[sorted[1]],points[sorted[2]],points[sorted[3]],points[sorted[4]]);
  if(swaps%2U!=0U) result=-result;
  if(result!=0) return result;
  result=robust_orient(points[sorted[0]],points[sorted[2]],points[sorted[3]],points[sorted[4]]);
  if(swaps%2U==0U) result=-result;
  return result;
}

int wang_source_in_sphere_sign(
    const std::vector<Point>& points,const Tet& tet,std::uint32_t query) {
  // The source-compatible seed used to enter arbitrary-precision arithmetic
  // for every Bowyer--Watson conflict predicate.  That makes the depth-six
  // prototype spend minutes rebuilding a well-conditioned seed.  Use the
  // same deliberately conservative long-double filter as sphere_contains:
  // it decides only determinants that are many rounding-error bounds from
  // zero.  Near a cospherical case we retain the exact determinant and,
  // below, the literal DT node-index tie break, so this cannot change the
  // source-defined ambiguous-case behavior.
  const auto& a=points[tet[0]];
  const auto& b=points[tet[1]];
  const auto& c=points[tet[2]];
  const auto& d=points[tet[3]];
  const auto& q=points[query];
  const auto filtered=in_sphere_filter(a,b,c,d,q);
  long double magnitude=1.0L;
  for(const auto vertex:tet) {
    const auto& point=points[vertex];
    magnitude=std::max({magnitude,std::abs(point.x),std::abs(point.y),
                        std::abs(point.z)});
  }
  magnitude=std::max({magnitude,std::abs(q.x),std::abs(q.y),std::abs(q.z)});
  const auto exact=std::abs(filtered)>
          4096.0L*LDBL_EPSILON*magnitude*magnitude*magnitude*magnitude
      ? (filtered<0.0L?-1:1)
      : sign(exact_in_sphere(as_vec3(a),as_vec3(b),as_vec3(c),as_vec3(d),
                             as_vec3(q)));
  if(exact!=0)return exact;

  // Literal source tie break from DT::insphere_s: sort by node index (the
  // author constructor's actual node identity), count the permutation, then
  // use its two prescribed orient3d minors. This is deliberately distinct
  // from the prototype's stable-ID SoS helper above.
  std::array<std::uint32_t,5> nodes{{tet[0],tet[1],tet[2],tet[3],query}};
  unsigned swaps{};
  for(unsigned pass=0U;pass<nodes.size();++pass)
    for(unsigned index=1U;index<nodes.size()-pass;++index)
      if(nodes[index]<nodes[index-1U]) {
        std::swap(nodes[index],nodes[index-1U]);++swaps;
      }
  // GEOM_FUNC::orient3d has the inverse sign of `robust_orient`.
  auto result=-robust_orient(points[nodes[1]],points[nodes[2]],
                             points[nodes[3]],points[nodes[4]]);
  if(swaps%2U!=0U)result=-result;
  if(result!=0)return result;
  result=robust_orient(points[nodes[0]],points[nodes[2]],
                       points[nodes[3]],points[nodes[4]]);
  if(swaps%2U!=0U)result=-result;
  return result;
}

bool sphere_contains(const std::vector<Point>& p,const Tet& t,std::uint32_t query,
                     const std::vector<std::uint32_t>& rank){
  const auto q=p[query];
  const auto filtered=in_sphere_filter(p[t[0]],p[t[1]],p[t[2]],p[t[3]],q);
  long double magnitude=1.0L;
  for(const auto vertex:t) { const auto& point=p[vertex]; magnitude=std::max({magnitude,std::abs(point.x),std::abs(point.y),std::abs(point.z)}); }
  magnitude=std::max({magnitude,std::abs(q.x),std::abs(q.y),std::abs(q.z)});
  int in_sphere_sign{};
  if(std::abs(filtered)>4096.0L*LDBL_EPSILON*magnitude*magnitude*magnitude*magnitude)
    in_sphere_sign=filtered<0.0L?-1:1;
  else
    in_sphere_sign=sign(exact_in_sphere(as_vec3(p[t[0]]),as_vec3(p[t[1]]),as_vec3(p[t[2]]),as_vec3(p[t[3]]),as_vec3(q)));
  const auto orientation=robust_orient(p,t);
  if(orientation==0) return false;
  if(in_sphere_sign==0) {
    in_sphere_sign=perturbed_in_sphere_sign(p,t,query,rank);
    if(in_sphere_sign==0) return false;
  }
  return in_sphere_sign==-orientation;
}
bool wang_sphere_contains(const std::vector<Point>& p,const Tet& t,
                          std::uint32_t query,
                          const std::vector<std::uint32_t>& rank) {
  int in_sphere_sign=wang_source_in_sphere_sign(p,t,query);
  const auto orientation=robust_orient(p,t);
  if(orientation==0)return false;
  // The source's two orient3d minors resolve its ordinary cospherical cases,
  // but a grid-rich input can make both minors zero.  Leaving that final tie
  // unclassified cuts a non-ball Bowyer--Watson cavity.  Complete only this
  // otherwise unresolved case with the stable-rank SoS used by the canonical
  // seed, retaining the source decision whenever it is defined.
  if(in_sphere_sign==0)
    in_sphere_sign=perturbed_in_sphere_sign(p,t,query,rank);
  if(in_sphere_sign==0)return false;
  return in_sphere_sign==-orientation;
}
bool has_dimension_three(const std::vector<Point>& p){
  std::size_t b=1;while(b<p.size()&&p[b].x==p[0].x&&p[b].y==p[0].y&&p[b].z==p[0].z)++b;if(b==p.size())return false;
  std::size_t c=b+1;while(c<p.size()&&cross(p[b]-p[0],p[c]-p[0]).x==0.0L&&cross(p[b]-p[0],p[c]-p[0]).y==0.0L&&cross(p[b]-p[0],p[c]-p[0]).z==0.0L)++c;if(c==p.size())return false;
  for(std::size_t d=c+1;d<p.size();++d)if(sign(exact_orientation_3d(as_vec3(p[0]),as_vec3(p[b]),as_vec3(p[c]),as_vec3(p[d])))!=0)return true;
  return false;
}

std::vector<std::uint32_t> wang_hilbert_order(
    const std::vector<Vec3>& points,std::size_t count) {
  struct Item {std::array<double,3> point{};std::uint32_t index{};};
  std::vector<Item> items;
  items.reserve(count);
  for(std::uint32_t i=0U;i<count;++i)
    items.push_back({{points[i].x,points[i].y,points[i].z},i});
  if(count<2U) {
    std::vector<std::uint32_t> order;
    for(const auto& item:items)order.push_back(item.index);
    return order;
  }
  Vec3 lo=points.front(),hi=points.front();
  for(std::size_t i=1U;i<count;++i) {
    lo.x=std::min(lo.x,points[i].x);lo.y=std::min(lo.y,points[i].y);
    lo.z=std::min(lo.z,points[i].z);
    hi.x=std::max(hi.x,points[i].x);hi.y=std::max(hi.y,points[i].y);
    hi.z=std::max(hi.z,points[i].z);
  }
  // Direct representation of the pinned DT::multiscale_sort/hilbert_sort
  // traversal.  Its in-place partition order is observable by subsequent
  // symbolic in-sphere decisions, so a mathematically equivalent Hilbert key
  // is not a sufficient implementation of the reference constructor.
  static constexpr int trans[8][3][8]={
      {{0,2,6,4,5,7,3,1},{0,4,5,1,3,7,6,2},{0,1,3,2,6,7,5,4}},
      {{1,3,7,5,4,6,2,0},{1,5,4,0,2,6,7,3},{1,0,2,3,7,6,4,5}},
      {{2,0,4,6,7,5,1,3},{2,6,7,3,1,5,4,0},{2,3,1,0,4,5,7,6}},
      {{3,1,5,7,6,4,0,2},{3,7,6,2,0,4,5,1},{3,2,0,1,5,4,6,7}},
      {{4,6,2,0,1,3,7,5},{4,0,1,5,7,3,2,6},{4,5,7,6,2,3,1,0}},
      {{5,7,3,1,0,2,6,4},{5,1,0,4,6,2,3,7},{5,4,6,7,3,2,0,1}},
      {{6,4,0,2,3,1,5,7},{6,2,3,7,5,1,0,4},{6,7,5,4,0,1,3,2}},
      {{7,5,1,3,2,0,4,6},{7,3,2,6,4,0,1,5},{7,6,4,5,1,0,2,3}}};
  static constexpr int hb[8]={0,1,0,2,0,1,0,0};
  const auto split=[&](std::size_t begin,std::size_t size,int gc0,int gc1,
                       const std::array<double,3>& lower,
                       const std::array<double,3>& upper) {
    const auto axis=static_cast<unsigned>((gc0^gc1)>>1);
    const auto plane=.5*(lower[axis]+upper[axis]);
    const auto ascending=(gc0&(1<<axis))==0;
    std::size_t left=0U,right=size;
    while(true) {
      while(left<size&&(ascending?items[begin+left].point[axis]<plane:
                                    items[begin+left].point[axis]>plane))++left;
      while(right>0U&&(ascending?items[begin+right-1U].point[axis]>=plane:
                                  items[begin+right-1U].point[axis]<=plane))--right;
      if(left==right)break;
      std::swap(items[begin+left],items[begin+right-1U]);
      ++left;--right;
    }
    return left;
  };
  std::function<void(std::size_t,std::size_t,int,int,
                     std::array<double,3>,std::array<double,3>,unsigned)>
      hilbert;
  hilbert=[&](std::size_t begin,std::size_t size,int e,int d,
              std::array<double,3> lower,std::array<double,3> upper,
              unsigned depth) {
    std::array<std::size_t,9> p{};p[8]=size;
    p[4]=split(begin,p[8],trans[e][d][3],trans[e][d][4],lower,upper);
    p[2]=split(begin,p[4],trans[e][d][1],trans[e][d][2],lower,upper);
    p[1]=split(begin,p[2],trans[e][d][0],trans[e][d][1],lower,upper);
    p[3]=p[2]+split(begin+p[2],p[4]-p[2],trans[e][d][2],
                    trans[e][d][3],lower,upper);
    p[6]=p[4]+split(begin+p[4],p[8]-p[4],trans[e][d][5],
                    trans[e][d][6],lower,upper);
    p[5]=p[4]+split(begin+p[4],p[6]-p[4],trans[e][d][4],
                    trans[e][d][5],lower,upper);
    p[7]=p[6]+split(begin+p[6],p[8]-p[6],trans[e][d][6],
                    trans[e][d][7],lower,upper);
    if(depth+1U==52U)return;
    for(int w=0;w<8;++w)if(p[w+1]-p[w]>8U) {
      const auto ew0=w==0?0:(2*((w-1)/2))^(2*((w-1)/2)>>1);
      const auto ew=((ew0<<(d+1))&7)|(ew0>>(3-d-1));
      const auto next_e=e^ew;
      const auto dw=w==0?0:(w%2==0?hb[w-1]:hb[w]);
      const auto next_d=(d+dw+1)%3;
      auto next_lower=lower,next_upper=upper;
      for(unsigned axis=0U;axis<3U;++axis) {
        const auto middle=.5*(lower[axis]+upper[axis]);
        if((trans[e][d][w]&(1<<axis))!=0)next_lower[axis]=middle;
        else next_upper[axis]=middle;
      }
      hilbert(begin+p[w],p[w+1]-p[w],next_e,next_d,
              next_lower,next_upper,depth+1U);
    }
  };
  std::function<void(std::size_t,unsigned&)> multiscale;
  multiscale=[&](std::size_t size,unsigned& depth) {
    std::size_t middle{};
    if(size>=64U) {++depth;middle=static_cast<std::size_t>(size*.125);
      multiscale(middle,depth);}
    // Keep the pinned author's guard-box construction exactly.  Although
    // multiplying absolute bounds makes this ordering translation-sensitive,
    // it is observable control state for the source insertion schedule.
    const auto lower=std::array<double,3>{{
        lo.x*1.01,lo.y*1.01,lo.z*1.01}};
    const auto upper=std::array<double,3>{{
        hi.x*1.01,hi.y*1.01,hi.z*1.01}};
    hilbert(middle,size-middle,0,0,lower,upper,0U);
  };
  unsigned multiscale_depth{};multiscale(count,multiscale_depth);
  std::vector<std::uint32_t> order;order.reserve(count);
  for(const auto& item:items)order.push_back(item.index);
  return order;
}
}

std::vector<std::uint32_t> wang_reference_hilbert_order(
    const std::vector<Vec3>& vertices) {
  return wang_hilbert_order(vertices,vertices.size());
}

CanonicalDelaunaySeedResult build_canonical_delaunay_seed(
    const CanonicalDelaunaySeedInput& in){
  const auto& input=in.vertices; const auto refuse=[](auto f){return CanonicalDelaunaySeedResult{f,{}};};
  const auto invalid=[](CanonicalDelaunaySeedInvalidReason reason,Face face={},std::uint32_t witness=0U){
    CanonicalDelaunaySeedResult result;result.failure=CanonicalDelaunaySeedFailure::invalid_output;
    result.invalid_reason=reason;result.invalid_hull_face=face;result.invalid_hull_witness=witness;return result;
  };
  if(input.size()<4U)return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
  if(input.size()>in.maximum_vertices||in.maximum_tetrahedra==0U)return refuse(CanonicalDelaunaySeedFailure::resource_limit);
  if(!in.stable_vertex_ids.empty()&&in.stable_vertex_ids.size()!=input.size())return refuse(CanonicalDelaunaySeedFailure::duplicate_stable_id);
  auto ids=in.stable_vertex_ids;if(ids.empty()){ids.resize(input.size());for(std::size_t i=0;i<ids.size();++i)ids[i]=i;}
  if(std::set<std::uint64_t>(ids.begin(),ids.end()).size()!=ids.size())return refuse(CanonicalDelaunaySeedFailure::duplicate_stable_id);
  for(const auto p:input)if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))return refuse(CanonicalDelaunaySeedFailure::non_finite_input);
  for(std::size_t i=0;i<input.size();++i)for(std::size_t j=0;j<i;++j)if(input[i].x==input[j].x&&input[i].y==input[j].y&&input[i].z==input[j].z)return refuse(CanonicalDelaunaySeedFailure::duplicate_position);
  Vec3 lo=input.front(),hi=input.front();for(const auto p:input){lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);lo.z=std::min(lo.z,p.z);hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);hi.z=std::max(hi.z,p.z);}
  const Vec3 mid=(lo+hi)/2.0;const long double extent=std::max({hi.x-lo.x,hi.y-lo.y,hi.z-lo.z});if(!(extent>0.0L)||!std::isfinite(static_cast<double>(extent)))return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
  std::vector<Point> points;points.reserve(input.size()+4U);
  for(const auto p:input) {
    // The filtered and exact paths must describe one point set.  Keeping an
    // 80-bit quotient here while the exact fallback receives its rounded
    // IEEE-754 double silently creates two geometries near a zero predicate.
    // Canonicalize the normalized predicate domain to double once.
    const auto x=static_cast<double>((static_cast<long double>(p.x)-mid.x)/extent);
    const auto y=static_cast<double>((static_cast<long double>(p.y)-mid.y)/extent);
    const auto z=static_cast<double>((static_cast<long double>(p.z)-mid.z)/extent);
    points.push_back({x,y,z});
  }
  if(!has_dimension_three(points))return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
  const auto first=static_cast<std::uint32_t>(points.size());points.insert(points.end(),{{-16,-16,-16},{16,-16,-16},{0,16,-16},{0,0,16}});
  std::vector<Tet> cells{{first,first+1U,first+2U,first+3U}};
  std::vector<std::uint32_t> order(input.size());
  std::iota(order.begin(),order.end(),0U);
  std::sort(order.begin(),order.end(),[&](auto a,auto b){return std::tie(ids[a],points[a].x,points[a].y,points[a].z)<std::tie(ids[b],points[b].x,points[b].y,points[b].z);});
  std::vector<std::uint32_t> rank(points.size());
  for(std::uint32_t index=0U;index<order.size();++index)rank[order[index]]=index;
  for(std::uint32_t index=first;index<points.size();++index)rank[index]=static_cast<std::uint32_t>(order.size())+index-first;
  const auto semantically_coplanar=[&](const Tet& cell) {
    if(in.exact_affine_planes.empty()||
       std::ranges::any_of(cell,[&](std::uint32_t vertex) {
         return vertex>=input.size();
       })) return false;
    std::array<Vec3,4> positions{};
    std::array<std::uint64_t,4> stable{};
    for(unsigned corner=0U;corner<4U;++corner) {
      positions[corner]=input[cell[corner]];
      stable[corner]=ids[cell[corner]];
    }
    return evaluate_plane_aware_orientation(
        positions,stable,in.exact_affine_planes).semantically_coplanar;
  };
  for(const auto point:order){
    std::vector<bool> conflict(cells.size());
    for(std::size_t cell=0U;cell<cells.size();++cell)
      conflict[cell]=sphere_contains(points,cells[cell],point,rank);
    // If a cavity boundary face would cone to an exact source-plane cell,
    // absorb its adjacent cell. Repeat to the first boundary whose cones are
    // all semantically three-dimensional. This preserves a closed cavity;
    // simply dropping the flat cone would leave a hole.
    for(bool expanded=true;expanded;) {
      expanded=false;
      std::map<Face,std::vector<std::size_t>> uses;
      for(std::size_t cell=0U;cell<cells.size();++cell)
        for(unsigned omit=0U;omit<4U;++omit) {
          Face face{};unsigned cursor{};
          for(unsigned corner=0U;corner<4U;++corner)
            if(corner!=omit)face[cursor++]=cells[cell][corner];
          uses[face_key(face)].push_back(cell);
        }
      for(std::size_t cell=0U;cell<cells.size()&&!expanded;++cell)if(conflict[cell])
        for(unsigned omit=0U;omit<4U&&!expanded;++omit) {
          Face face{};unsigned cursor{};
          for(unsigned corner=0U;corner<4U;++corner)
            if(corner!=omit)face[cursor++]=cells[cell][corner];
          const auto& adjacent=uses.at(face_key(face));
          const auto other=adjacent.size()==2U?
              (adjacent[0]==cell?adjacent[1]:adjacent[0]):cells.size();
          if(other<cells.size()&&!conflict[other]&&semantically_coplanar(
                 {{face[0],face[1],face[2],point}})) {
            conflict[other]=true;expanded=true;
          }
        }
    }
    std::map<Face,std::pair<Face,unsigned>> faces;std::vector<Tet> retained;
    for(std::size_t cell=0U;cell<cells.size();++cell) {
      if(!conflict[cell]) {retained.push_back(cells[cell]);continue;}
      for(unsigned omit=0;omit<4U;++omit){Face f{};unsigned n=0;for(unsigned j=0;j<4U;++j)if(j!=omit)f[n++]=cells[cell][j];auto& entry=faces[face_key(f)];entry.first=f;++entry.second;}
    }
    // Coning requires a simplicial cavity boundary.  A face used by more than
    // two removed cells is not an internal face of a valid 3-ball and must
    // never be silently discarded as though it were one.
    if(std::any_of(faces.begin(),faces.end(),[](const auto& entry){return entry.second.second>2U;}))
      return invalid(CanonicalDelaunaySeedInvalidReason::nonmanifold_cavity);
    std::vector<Face> boundary;
    for(const auto& [key,value]:faces) { (void)key;if(value.second==1U) boundary.push_back(value.first); }
    if(!boundary.empty()) {
      std::vector<bool> visited(boundary.size());
      std::vector<std::size_t> pending{0U};visited[0]=true;
      while(!pending.empty()) {
        const auto current=pending.back();pending.pop_back();
        for(std::size_t candidate=0U;candidate<boundary.size();++candidate) {
          if(visited[candidate]) continue;
          unsigned shared{};
          for(const auto left:boundary[current]) for(const auto right:boundary[candidate])
            if(left==right) ++shared;
          if(shared>=2U) { visited[candidate]=true;pending.push_back(candidate); }
        }
      }
      if(std::any_of(visited.begin(),visited.end(),[](bool value){return !value;}))
        return invalid(CanonicalDelaunaySeedInvalidReason::nonmanifold_cavity);
      std::map<std::array<std::uint32_t,2>,unsigned> boundary_edges;
      for(const auto& face:boundary) for(const auto edge:std::array<std::array<unsigned,2>,3>{{{{0U,1U}},{{1U,2U}},{{2U,0U}}}}) {
        std::array<std::uint32_t,2> key{{face[edge[0]],face[edge[1]]}};
        std::sort(key.begin(),key.end());++boundary_edges[key];
      }
      if(std::any_of(boundary_edges.begin(),boundary_edges.end(),[](const auto& entry){return entry.second!=2U;}))
        return invalid(CanonicalDelaunaySeedInvalidReason::nonmanifold_cavity);
    }
    for(const auto& [key,value]:faces)if(value.second==1U) {
      (void)key;
      Tet child{{value.first[0],value.first[1],value.first[2],point}};
      // An unexpanded semantic-zero cone can only lie on the current mesh
      // boundary: every interior face has an adjacent cell and was absorbed
      // above.  Omitting this zero-volume cone is the standard insertion-on-
      // face operation; cones over the other cavity faces expose the
      // subdivided boundary triangles without changing the occupied volume.
      if(!semantically_coplanar(child))retained.push_back(child);
    }
    if(retained.size()>in.maximum_tetrahedra)
      return refuse(CanonicalDelaunaySeedFailure::resource_limit);
    cells=std::move(retained);
  }
  std::vector<Tet> result;
  for(auto cell:cells)
    if(std::all_of(cell.begin(),cell.end(),[&](auto i){return i<input.size();})) {
      const auto v=robust_orient(points,cell);
      if(v==0)return invalid(
          CanonicalDelaunaySeedInvalidReason::degenerate_final_cell);
      // Source-coplanar cells are symbolic boundary bookkeeping, not volume
      // cells. Drop them and let the incidence, convex-hull and volume audits
      // below prove that the remaining complex is a complete 3D seed.
      if(semantically_coplanar(cell))continue;
      if(v<0)std::swap(cell[0],cell[1]);
      result.push_back(cell);
    }
  if(result.empty())return invalid(
      CanonicalDelaunaySeedInvalidReason::no_final_cells);
  std::set<std::uint32_t> used;std::map<Face,std::vector<std::pair<std::size_t,std::uint32_t>>> ledger;long double cell_volume=0;
  for(std::size_t ti=0;ti<result.size();++ti){const auto& t=result[ti];for(auto v:t)used.insert(v);cell_volume+=orient(points[t[0]],points[t[1]],points[t[2]],points[t[3]])/6;for(unsigned omit=0;omit<4U;++omit){Face f{};unsigned n=0;for(unsigned j=0;j<4U;++j)if(j!=omit)f[n++]=t[j];ledger[face_key(f)].push_back({ti,t[omit]});}}
  if(used.size()!=input.size()||std::any_of(ledger.begin(),ledger.end(),[](const auto& e){return e.second.empty()||e.second.size()>2U;}))return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
  long double boundary_volume=0;for(const auto& [f,uses]:ledger){if(uses.size()==2U){const auto a=points[f[0]],b=points[f[1]],c=points[f[2]];if(robust_orient(a,b,c,points[uses[0].second])*robust_orient(a,b,c,points[uses[1].second])>=0)return invalid(CanonicalDelaunaySeedInvalidReason::same_sided_interior_face);continue;}Face outward=f;if(robust_orient(points[outward[0]],points[outward[1]],points[outward[2]],points[uses[0].second])>0)std::swap(outward[0],outward[1]);for(std::size_t i=0;i<input.size();++i)if(robust_orient(points[outward[0]],points[outward[1]],points[outward[2]],points[i])>0)return invalid(CanonicalDelaunaySeedInvalidReason::nonconvex_hull,outward,static_cast<std::uint32_t>(i));boundary_volume+=dot(points[outward[0]],cross(points[outward[1]],points[outward[2]]))/6;}
  if(std::abs(cell_volume-boundary_volume)>std::max(1.0L,std::abs(boundary_volume))*1e-14L)return invalid(CanonicalDelaunaySeedInvalidReason::volume_disagreement);
  std::sort(result.begin(),result.end(),[&](const Tet& a,const Tet& b){std::array<std::uint64_t,4> ia{},ib{};for(unsigned i=0;i<4;++i){ia[i]=ids[a[i]];ib[i]=ids[b[i]];}std::sort(ia.begin(),ia.end());std::sort(ib.begin(),ib.end());return ia<ib;});return {CanonicalDelaunaySeedFailure::none,std::move(result)};
}
CanonicalDelaunaySeedResult build_canonical_delaunay_seed(const std::vector<Vec3>& vertices,std::size_t maximum_vertices,std::size_t maximum_tetrahedra){return build_canonical_delaunay_seed({vertices,{},maximum_vertices,maximum_tetrahedra});}

namespace {
CanonicalDelaunaySeedResult build_wang_reference_seed(
    const CanonicalDelaunaySeedInput& in,std::size_t original_count,
    std::vector<std::vector<Tet>>* stage_trace=nullptr,
    WangReferenceSeedTrace* seed_trace=nullptr,
    bool capture_diagnostics=true) {
  // Recovery needs only the final physical allocator state.  The extensive
  // per-insertion trace below is retained for source-conformance tests, but
  // copying every cavity, carrier, and slot snapshot into a production run
  // is diagnostic work and cannot affect the reference algorithm.
  auto* final_state_trace=seed_trace;
  if(!capture_diagnostics)seed_trace=nullptr;
  const bool predicate_trace=std::getenv("WANG_PLANE_PREDICATE_TRACE")!=nullptr;
  // Predicate tracing is failure evidence, not a second execution mode. Keep
  // it opt-in and bounded even for a pathological input with many cavities.
  std::size_t remaining_cavity_trace_events=4U;
  const auto take_cavity_trace_event=[&]() {
    return predicate_trace&&remaining_cavity_trace_events--!=0U;
  };
  const auto refuse=[](CanonicalDelaunaySeedFailure failure) {
    CanonicalDelaunaySeedResult result;result.failure=failure;return result;
  };
  const auto invalid=[](CanonicalDelaunaySeedInvalidReason reason,
                        std::source_location where=std::source_location::current()) {
    if(std::getenv("WANG_PLANE_PREDICATE_TRACE")!=nullptr)
      std::cerr<<"plane_seed_invalid reason="<<static_cast<unsigned>(reason)
               <<" line="<<where.line()<<'\n';
    CanonicalDelaunaySeedResult result;
    result.failure=CanonicalDelaunaySeedFailure::invalid_output;
    result.invalid_reason=reason;return result;
  };
  if(original_count<4U||original_count>in.vertices.size())
    return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
  if(in.vertices.size()>in.maximum_vertices||in.maximum_tetrahedra==0U)
    return refuse(CanonicalDelaunaySeedFailure::resource_limit);
  if(!in.stable_vertex_ids.empty()&&
     in.stable_vertex_ids.size()!=in.vertices.size())
    return refuse(CanonicalDelaunaySeedFailure::duplicate_stable_id);
  if(!in.stable_vertex_ids.empty()&&
     std::set<std::uint64_t>(in.stable_vertex_ids.begin(),
                             in.stable_vertex_ids.end()).size()!=
         in.stable_vertex_ids.size())
    return refuse(CanonicalDelaunaySeedFailure::duplicate_stable_id);
  std::vector<Point> points;
  points.reserve(in.vertices.size());
  for(const auto p:in.vertices) {
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))
      return refuse(CanonicalDelaunaySeedFailure::non_finite_input);
    points.push_back({p.x,p.y,p.z});
  }
  for(std::size_t i=0U;i<points.size();++i)
    for(std::size_t j=0U;j<i;++j)
      if(points[i].x==points[j].x&&points[i].y==points[j].y&&
         points[i].z==points[j].z)
        return refuse(CanonicalDelaunaySeedFailure::duplicate_position);

  auto order=wang_hilbert_order(in.vertices,original_count);
  const auto distinct=[&](std::uint32_t a,std::uint32_t b) {
    return points[a].x!=points[b].x||points[a].y!=points[b].y||
           points[a].z!=points[b].z;
  };
  std::size_t second=1U;
  while(second<order.size()&&!distinct(order[0],order[second]))++second;
  if(second==order.size())
    return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
  std::swap(order[1],order[second]);
  std::size_t third=2U;
  while(third<order.size()) {
    const auto normal=cross(points[order[1]]-points[order[0]],
                            points[order[third]]-points[order[0]]);
    if(dot(normal,normal)>0.0L)break;
    ++third;
  }
  if(third==order.size())
    return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
  std::swap(order[2],order[third]);
  std::size_t fourth=3U;
  while(fourth<order.size()&&
        robust_orient(points[order[0]],points[order[1]],points[order[2]],
                      points[order[fourth]])==0)++fourth;
  if(fourth==order.size())
    return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
  std::swap(order[3],order[fourth]);
  if(robust_orient(points[order[0]],points[order[1]],points[order[2]],
                   points[order[3]])<0)
    std::swap(order[0],order[1]);

  const auto ghost=static_cast<std::uint32_t>(points.size());
  const auto plane_aware=[&](const Tet& cell) {
    PlaneAwareOrientation orientation;
    if(in.exact_affine_planes.empty()||
       std::ranges::any_of(cell,[&](std::uint32_t vertex) {
         return vertex>=in.vertices.size();
       })) return orientation;
    std::array<Vec3,4> positions{};
    std::array<std::uint64_t,4> stable{};
    for(unsigned corner=0U;corner<4U;++corner) {
      positions[corner]=in.vertices[cell[corner]];
      stable[corner]=in.stable_vertex_ids.empty()?cell[corner]:
          in.stable_vertex_ids[cell[corner]];
    }
    return evaluate_plane_aware_orientation(
        positions,stable,in.exact_affine_planes);
  };
  if(final_state_trace)final_state_trace->ghost_vertex=ghost;
  std::vector<Tet> cells{{order[0],order[1],order[2],order[3]},
      {order[1],order[2],order[3],ghost},
      {order[2],order[0],order[3],ghost},
      {order[0],order[1],order[3],ghost},
      {order[0],order[2],order[1],ghost}};
  // Preserve the reference allocator's identity independently of the compact
  // topology work list below. `DT::addElem` consumes Evacancy FIFO before it
  // appends, and `finishBW` returns the committed cavity slots afterwards.
  // This table is the first step of the owned slot-lifetime transcription.
  std::vector<Tet> element_slots=cells;
  std::vector<bool> live_slots(cells.size(),true);
  // `cells` is the current topology work list.  Unlike the reference's
  // `Elems`, it is compacted at every commit, so retain the source element
  // identity of each work-list entry explicitly.  Never reconstruct this
  // correspondence from a tetrahedron's vertices: repeated combinatorial
  // forms are possible over the lifetime of the allocator.
  std::vector<std::size_t> cell_slots(cells.size());
  std::iota(cell_slots.begin(),cell_slots.end(),0U);
  std::vector<std::size_t> vacant_slots;
  std::size_t next_vacant_slot=0U;
  std::vector<std::uint32_t> rank(points.size());
  std::iota(rank.begin(),rank.end(),0U);
  std::set<std::uint32_t> inserted{
      order[0],order[1],order[2],order[3]};
  std::vector<std::uint32_t> insertion_order;
  for(const auto vertex:order)if(!inserted.contains(vertex))
    insertion_order.push_back(vertex);
  for(std::uint32_t vertex=static_cast<std::uint32_t>(original_count);
      vertex<points.size();++vertex)insertion_order.push_back(vertex);
  std::vector<std::size_t> point_carrier_slot(points.size());
  std::vector<bool> has_point_carrier(points.size());
  for(const auto vertex:std::array<std::uint32_t,4>{{
          order[0],order[1],order[2],order[3]}}) {
    point_carrier_slot[vertex]=0U;
    has_point_carrier[vertex]=true;
  }
  // `DT::AddBox` carries the last allocated Bowyer--Watson cell into the
  // next insertion.  This is not a performance hint: for an exterior box
  // point, locateRequest selects the particular visible hull component from
  // which findBWCavity grows.  Keep the equivalent live-cell identity across
  // our compact storage rebuilds.
  std::size_t box_search_carrier=0U;

  const auto canonical_face=[](Face face) {
    std::sort(face.begin(),face.end());return face;
  };
  const auto wang_boundary_face=[&](const Tet& tet,unsigned omitted) {
    static constexpr unsigned other[4][3]={
        {1U,2U,3U},{3U,2U,0U},{0U,1U,3U},{2U,1U,0U}};
    Face face{{tet[other[omitted][0]],tet[other[omitted][1]],
               tet[other[omitted][2]]}};
    // Pinned prepareBWFill moves ghost to the last position while retaining
    // its two prescribed swaps verbatim.
    if(face[0]==ghost) {std::swap(face[0],face[2]);std::swap(face[0],face[1]);}
    else if(face[1]==ghost) {std::swap(face[1],face[2]);std::swap(face[1],face[0]);}
    return face;
  };
  const auto finite_snapshot=[&]() {
    std::vector<Tet> finite;
    for(auto cell:cells)if(cell[3]!=ghost) {
      if(robust_orient(points,cell)<0)std::swap(cell[0],cell[1]);
      finite.push_back(cell);
    }
    return finite;
  };
  const auto live_slot_snapshot=[&]() {
    std::vector<WangReferenceSeedTrace::LiveElementSlot> snapshot;
    for(std::size_t slot=0U;slot<element_slots.size();++slot)
      if(live_slots[slot])snapshot.push_back({slot,element_slots[slot]});
    return snapshot;
  };
  const auto point_carrier_snapshot=[&]() {
    constexpr auto absent=std::numeric_limits<std::uint32_t>::max();
    std::vector<std::array<std::uint32_t,4>> snapshot(
        points.size(),{{absent,absent,absent,absent}});
    for(std::size_t vertex=0U;vertex<points.size();++vertex) {
      if(!has_point_carrier[vertex])continue;
      const auto slot=point_carrier_slot[vertex];
      if(slot>=element_slots.size()||!live_slots[slot])continue;
      snapshot[vertex]=element_slots[slot];
    }
    return snapshot;
  };
  std::size_t inserted_originals=4U;
  struct FaceUses {
    using Use=std::pair<std::size_t,unsigned>;
    std::array<Use,2> values{};
    std::size_t count{};

    void push_back(Use use) noexcept {
      if(count<values.size())values[count]=use;
      ++count;
    }
    [[nodiscard]] std::size_t size() const noexcept {return count;}
    [[nodiscard]] const Use& operator[](std::size_t index) const noexcept {
      return values[index];
    }
  };
  using FaceUse=FaceUses::Use;
  constexpr auto invalid_cell=std::numeric_limits<std::size_t>::max();
  const FaceUse invalid_use{invalid_cell,4U};
  std::vector<std::array<FaceUse,4>> neighbours(cells.size());
  for(auto& adjacent:neighbours)adjacent.fill(invalid_use);
  {
    std::unordered_map<Face,FaceUses,FaceHash> initial_ledger;
    initial_ledger.reserve(cells.size()*4U);
    for(std::size_t cell=0U;cell<cells.size();++cell)
      for(unsigned omitted=0U;omitted<4U;++omitted)
        initial_ledger[canonical_face(wang_boundary_face(cells[cell],omitted))]
            .push_back({cell,omitted});
    for(const auto& [face,uses]:initial_ledger) {
      static_cast<void>(face);
      if(uses.size()!=2U)
        return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
      neighbours[uses[0].first][uses[0].second]=uses[1];
      neighbours[uses[1].first][uses[1].second]=uses[0];
    }
  }
  // `insertDelaunayPoints` updates its anchor to the node just inserted;
  // liveHint(anchor) then reads that node's last commitBW p2t assignment.
  // This is distinct from AddBox's physical-element carrier.
  std::uint32_t insertion_anchor=order[0];
  if(stage_trace&&inserted_originals==original_count)
    stage_trace->push_back(finite_snapshot());
  if(seed_trace&&inserted_originals==original_count)
    seed_trace->live_slot_stages.push_back(live_slot_snapshot());
  for(const auto query:insertion_order) {
    bool trace_this_cavity=false;
    std::vector<std::size_t> allocated_slots;
    std::vector<std::size_t> cavity_order;
    const bool eighth_box_insertion=
        query>=original_count&&query-original_count==7U;
    const bool third_box_insertion=
        query>=original_count&&query-original_count==3U;
    const bool seventh_box_insertion=
        query>=original_count&&query-original_count==6U;
    if(seed_trace&&eighth_box_insertion) {
      seed_trace->eighth_query=query;
      seed_trace->eighth_traversal.clear();
      seed_trace->eighth_location_path.clear();
      seed_trace->eighth_location_result=0;
    }
    if(neighbours.size()!=cells.size())
      return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
    if(seed_trace&&query<original_count) {
      std::vector<WangReferenceSeedTrace::HullPredicate> predicates;
      for(std::size_t cell=0U;cell<cells.size();++cell)if(cells[cell][3]==ghost) {
        const auto value=wang_orient3d_value(as_vec3(points[cells[cell][0]]),
            as_vec3(points[cells[cell][1]]),as_vec3(points[cells[cell][2]]),
            as_vec3(points[query]),true);
        const auto inner=neighbours[cell][3U].first;
        if(inner>=cells.size())
          return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
        predicates.push_back({cells[cell],value,cells[inner][3]==ghost?0:
            wang_source_in_sphere_sign(points,cells[inner],query)});
      }
      seed_trace->original_hull_predicates.push_back(std::move(predicates));
    }
    // The detailed reference trace intentionally records the full predicate
    // sweep for its two pinned AddBox insertions.  Recovery does not consume
    // that sweep: its source-shaped cavity walk classifies only reached
    // neighbours below.
    if(seed_trace&&(eighth_box_insertion||third_box_insertion))
      for(const auto& tet:cells) {
        if(tet[3]==ghost)continue;
        const auto selected=wang_sphere_contains(points,tet,query,rank);
        const WangReferenceSeedTrace::Predicate predicate{
            tet,robust_orient(points,tet),
            sign(exact_in_sphere(as_vec3(points[tet[0]]),
                                 as_vec3(points[tet[1]]),
                                 as_vec3(points[tet[2]]),
                                 as_vec3(points[tet[3]]),
                                 as_vec3(points[query]))),
            wang_source_in_sphere_sign(points,tet,query),selected};
        if(eighth_box_insertion)seed_trace->eighth_predicates.push_back(predicate);
        if(third_box_insertion)seed_trace->third_box_predicates.push_back(predicate);
      }
    std::vector<bool> conflict;
    {
      // AddBox initializes `searchtet` by scanning to the first live finite
      // tetrahedron after the ordinary Delaunay insertions.  Our compact
      // vector does not retain the source slot number, so establish that
      // equivalent carrier at the first box point rather than assuming slot
      // zero survived the preceding insertions.
      if(query<original_count) {
        // insertDelaunayPoints carries an anchor *node*, then calls
        // liveHint(anchor), i.e. the last source-assigned incident cell.
        // commitBW assigns that incidence while emitting its boundary faces.
        if(!has_point_carrier[insertion_anchor])
          return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
        box_search_carrier=point_carrier_slot[insertion_anchor];
      } else if(query==original_count) {
        const auto first_finite=std::find_if(element_slots.begin(),
            element_slots.end(),[&](const Tet& cell) {
              const auto slot=static_cast<std::size_t>(&cell-element_slots.data());
              return live_slots[slot]&&cell[3]!=ghost;
            });
        if(first_finite==element_slots.end())
          return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
        const auto source_slot=static_cast<std::size_t>(
            std::distance(element_slots.begin(),first_finite));
        box_search_carrier=source_slot;
      } else {
        if(box_search_carrier>=element_slots.size()||!live_slots[box_search_carrier])
          return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
      }
      const auto carrier=std::find(cell_slots.begin(),cell_slots.end(),
                                   box_search_carrier);
      if(carrier==cell_slots.end())
        return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
      const auto carrier_index=static_cast<std::size_t>(
          std::distance(cell_slots.begin(),carrier));
      if(seed_trace&&query<original_count) {
        seed_trace->original_carrier_slots.push_back(box_search_carrier);
        seed_trace->original_carrier_cells.push_back(cells[carrier_index]);
      }
      // Literal owned transcription of the location half of locateRequest.
      // The source's orient3d sign is opposite robust_orient here, hence a
      // positive local value crosses the face (source: ori < 0).
      const auto neighbour=[&](std::size_t cell,unsigned face)
          ->std::optional<std::size_t> {
        if(cell>=neighbours.size()||face>=4U)return std::nullopt;
        const auto adjacent=neighbours[cell][face];
        if(adjacent.first>=cells.size()||adjacent.second>=4U||
           neighbours[adjacent.first][adjacent.second]!=FaceUse{cell,face})
          return std::nullopt;
        return adjacent.first;
      };
      auto located=carrier_index;
      // locateRequest remembers the supplied element before stepping a hull
      // seed across face 3; a return to that original hull element terminates
      // the walk.  Keeping the post-step cell here changes that cycle test.
      const auto initial=located;
      if(cells[located][3]==ghost) {
        const auto inner=neighbour(located,3U);
        if(!inner)return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
        located=*inner;
      }
      std::optional<std::size_t> previous;
      bool located_cell=false;
      std::array<unsigned,2> zero_faces{};
      unsigned zero_count{};
      for(unsigned attempt=0U;attempt<10000U;++attempt) {
        if(seed_trace&&eighth_box_insertion)
          seed_trace->eighth_location_path.push_back(cell_slots[located]);
        if(cells[located][3]==ghost || (previous&&located==initial)) {
          // locateRequest returns immediately for a hull/cycle endpoint;
          // it must not interpret the preceding cell's zero-face state as
          // a face location after the loop.
          zero_count=0U;
          located_cell=true;
          break;
        }
        double smallest=std::numeric_limits<double>::max();
        std::optional<unsigned> next_face;
        zero_count=0U;
        static constexpr unsigned dnc[4][3]={{1U,2U,3U},{3U,2U,0U},
                                               {0U,1U,3U},{2U,1U,0U}};
        for(unsigned face=0U;face<4U;++face) {
          const auto next=neighbour(located,face);
          if(!next)return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
          if(previous&&*next==*previous)continue;
          const auto& tet=cells[located];
          const auto& a=points[tet[dnc[face][0]]];
          const auto& b=points[tet[dnc[face][1]]];
          const auto& c=points[tet[dnc[face][2]]];
          const auto source_orientation=wang_orient3d_value(
              as_vec3(a),as_vec3(b),as_vec3(c),as_vec3(points[query]),true);
          // The source predicate is evaluated on binary64 positions, while
          // the transition supplies explicit exact-plane provenance for DC
          // facets. A point known to lie on this face must take the source's
          // face/edge location branch even when its independently evaluated
          // coordinates leave a tiny nonzero determinant; treating it as an
          // interior point lets adjustBWCavity delete the entire cavity.
          const auto semantic_zero=plane_aware(
              {{query,tet[dnc[face][0]],tet[dnc[face][1]],tet[dnc[face][2]]}})
              .semantically_coplanar;
          if(source_orientation<0.0&&!semantic_zero) {
            // Literal locateRequest ranking: select the most-negative
            // GEOM_FUNC::orient3d(face, query) / calArea(face).  Do not use
            // the exact topological predicate here: zero detection chooses
            // the initial two-cell cavity seed and is observable later.
            const Vec3 av=as_vec3(a),bv=as_vec3(b),cv=as_vec3(c);
            const auto ab=bv-av,ac=cv-av;
            const double cross_x=ab.y*ac.z-ab.z*ac.y;
            const double cross_y=ab.z*ac.x-ab.x*ac.z;
            const double cross_z=ab.x*ac.y-ab.y*ac.x;
            const double source_area=std::sqrt(
                cross_x*cross_x+cross_y*cross_y+cross_z*cross_z)/2.0;
            const double normalized=source_orientation/
                std::max(source_area,1.0e-30);
            if(!next_face||normalized<smallest) {
              smallest=normalized;next_face=face;
            }
          } else if(source_orientation==0.0||semantic_zero) {
            if(zero_count==zero_faces.size())
              return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
            zero_faces[zero_count++]=face;
          }
        }
        if(!next_face) {located_cell=true;break;}
        previous=located;
        const auto next=neighbour(located,*next_face);
        if(!next)return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
        located=*next;
      }
      if(!located_cell)
        return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
      if(seed_trace&&query<original_count) {
        seed_trace->original_location_results.push_back(
            zero_count==0U?1:zero_count==1U?
                static_cast<int>((zero_faces[0]+1U)<<4U):
                static_cast<int>((zero_faces[0]<<2U)|zero_faces[1]));
        seed_trace->original_location_cells.push_back(cells[located]);
      }
      if(seed_trace&&eighth_box_insertion) {
        if(zero_count==1U)seed_trace->eighth_location_result=
            static_cast<int>((zero_faces[0]+1U)<<4U);
        else if(zero_count==0U)seed_trace->eighth_location_result=1;
      }
      if(seed_trace&&query>=original_count)
        seed_trace->box_carriers.push_back(cells[carrier_index]);

      // findBWCavity begins with the located cell and expands its connected
      // predicate-qualified component. It must not union every globally
      // conflicting hull cell.
      std::vector<bool> connected(cells.size());
      // locateRequest distinguishes an interior cell from a point on one of
      // its faces.  In the latter case findBWCavity appends this cell and
      // that face's neighbour before its breadth-first expansion.  The
      // insertion may have the same cavity either way, but this queue order
      // determines prepareBWFill's face/slot order and therefore AddBox's
      // future carrier.
      std::vector<std::size_t> pending{located};
      connected[located]=true;
      // findBWCavity does not preclassify the entire mesh.  It visits faces
      // FIFO and classifies each neighbour at its first encounter, retaining
      // that result in `testedFlag`.  In particular, hull cells use the
      // oriented hull-face test (and, on zero, the second-neighbour sphere
      // test), not the finite-cell in-sphere predicate.  The visit order is
      // observable through prepareBWFill's face order and slot allocation.
      std::vector<bool> tested(cells.size());
      std::vector<bool> eligible(cells.size());
      const auto include_neighbour=[&](std::size_t candidate) {
        if(tested[candidate])return eligible[candidate];
        tested[candidate]=true;
        const auto& candidate_tet=cells[candidate];
        if(candidate_tet[3]!=ghost)
          return eligible[candidate]=wang_sphere_contains(
              points,candidate_tet,query,rank);
        // `GEOM_FUNC::orient3d` is used directly by the source hull branch.
        // Its rounded return (rather than our exact combinatorial predicate)
        // controls this branch and consequently the FIFO cavity order.
        const auto source_side=wang_orient3d_value(
            as_vec3(points[candidate_tet[0]]),as_vec3(points[candidate_tet[1]]),
            as_vec3(points[candidate_tet[2]]),as_vec3(points[query]),true);
        if(source_side<0.0)return eligible[candidate]=true;
        if(source_side>0.0)return eligible[candidate]=false;
        const auto inner=neighbours[candidate][3U].first;
        if(inner>=cells.size())return eligible[candidate]=false;
        return eligible[candidate]=cells[inner][3]!=ghost&&
            wang_sphere_contains(points,cells[inner],query,rank);
      };
      if(zero_count==1U) {
        const auto adjacent=neighbour(located,zero_faces[0]);
        if(!adjacent)return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
        if(!connected[*adjacent]) {
          connected[*adjacent]=true;
          pending.push_back(*adjacent);
        }
      } else if(zero_count==2U) {
        // Literal `readShell`: for an edge location, findBWCavity seeds the
        // complete cyclic shell before its ordinary FIFO expansion.  The
        // direction around that shell is defined by DDNC, not a set walk.
        unsigned edge_first=4U,edge_second=4U;
        for(int corner=3;corner>=0;--corner)
          if(static_cast<unsigned>(corner)!=zero_faces[0]&&
             static_cast<unsigned>(corner)!=zero_faces[1]) {
            if(edge_first==4U)edge_first=static_cast<unsigned>(corner);
            else edge_second=static_cast<unsigned>(corner);
          }
        if(edge_second==4U)return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
        static constexpr unsigned ddnc[4][4]={
            {2U,3U,0U,1U},{3U,2U,1U,0U},{1U,3U,2U,0U},{2U,1U,3U,0U}};
        // DDNC is asymmetric: select the row explicitly so its source order
        // remains visible instead of deriving an arbitrary complement pair.
        unsigned p0{},p1{};
        switch(edge_first) {
          case 0U: p0=edge_second==1U?2U:edge_second==2U?3U:1U;
                   p1=edge_second==1U?3U:edge_second==2U?1U:2U; break;
          case 1U: p0=edge_second==0U?3U:edge_second==2U?0U:2U;
                   p1=edge_second==0U?2U:edge_second==2U?3U:0U; break;
          case 2U: p0=edge_second==0U?1U:edge_second==1U?3U:0U;
                   p1=edge_second==0U?3U:edge_second==1U?0U:1U; break;
          default: p0=edge_second==0U?2U:edge_second==1U?0U:1U;
                   p1=edge_second==0U?1U:edge_second==1U?2U:0U; break;
        }
        static_cast<void>(ddnc); // Documents the source table above.
        const auto first_vertex=cells[located][p0];
        auto end_vertex=cells[located][p1];
        const auto edge_a=cells[located][edge_first];
        const auto edge_b=cells[located][edge_second];
        auto shell_cell=located;
        while(true) {
          if(!connected[shell_cell]) {
            connected[shell_cell]=true;
            pending.push_back(shell_cell);
          }
          if(end_vertex==first_vertex)break;
          const auto next=neighbour(shell_cell,p0);
          if(!next||pending.size()>10000U)
            return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
          const auto shared=wang_boundary_face(cells[shell_cell],p0);
          std::optional<unsigned> incoming;
          for(unsigned face=0U;face<4U;++face)
            if(canonical_face(wang_boundary_face(cells[*next],face))==
               canonical_face(shared)) {incoming=face;break;}
          if(!incoming)return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
          shell_cell=*next;
          end_vertex=cells[shell_cell][*incoming];
          std::optional<unsigned> next_p0;
          for(unsigned corner=0U;corner<4U;++corner)
            if(corner!=*incoming&&cells[shell_cell][corner]!=edge_a&&
               cells[shell_cell][corner]!=edge_b) {next_p0=corner;break;}
          if(!next_p0)return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
          p0=*next_p0;
        }
      }
      for(std::size_t head=0U;head<pending.size();++head) {
        const auto cell=pending[head];
        for(unsigned omitted=0U;omitted<4U;++omitted) {
          const auto other=neighbours[cell][omitted].first;
          if(other>=cells.size())
            return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
          const auto included=!connected[other]&&include_neighbour(other);
          if(seed_trace&&eighth_box_insertion&&!connected[other])
            seed_trace->eighth_traversal.push_back(
                {cell_slots[cell],cell_slots[other],omitted,included});
          if(included) {
            connected[other]=true;pending.push_back(other);
          }
        }
      }
      conflict=std::move(connected);
      cavity_order=std::move(pending);
    }
    if(std::none_of(conflict.begin(),conflict.end(),[](bool value){return value;}))
      return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
    // A source-plane query on a source-plane cavity face cannot be coned into
    // a volume cell. Grow across such a face only while the added cell leaves
    // at least one live incident cell outside the cavity for every previously
    // inserted vertex. This builds the transverse two-sided patch where it is
    // topologically safe without absorbing a complete old vertex star. The
    // adjustment below shrinks the incident working cell at every remaining
    // semantic-plane face.
    const auto preserves_inserted_stars=[&](std::size_t added) {
      for(const auto vertex:cells[added]) {
        if(vertex>=points.size()||!inserted.contains(vertex))continue;
        bool outside=false;
        for(std::size_t cell=0U;cell<cells.size();++cell)
          if(cell!=added&&!conflict[cell]&&
             std::find(cells[cell].begin(),cells[cell].end(),vertex)!=
                 cells[cell].end()) {
            outside=true;break;
          }
        if(!outside)return false;
      }
      return true;
    };
    // A semantic-plane expansion is not part of the ordinary connected
    // Bowyer--Watson cavity.  Its candidate must therefore prove that the
    // resulting boundary is still one genus-zero two-manifold before it can
    // participate in the fill.  Preserving an incident cell for each old
    // vertex alone does not rule out a pinched/handled cavity; such a cavity
    // can emit the same face more than twice and poison a later insertion.
    const auto preserves_ball_boundary=[&](std::size_t added) {
      std::unordered_map<Face,unsigned,FaceHash> face_uses;
      face_uses.reserve(cells.size()*4U);
      for(std::size_t cell=0U;cell<cells.size();++cell) {
        if(!conflict[cell]&&cell!=added)continue;
        for(unsigned omitted=0U;omitted<4U;++omitted)
          ++face_uses[canonical_face(wang_boundary_face(cells[cell],omitted))];
      }
      std::vector<Face> boundary;
      for(const auto& [face,uses]:face_uses) {
        if(uses>2U)return false;
        if(uses==1U)boundary.push_back(face);
      }
      if(boundary.size()<4U)return false;
      std::map<std::array<std::uint32_t,2>,unsigned> boundary_edges;
      std::set<std::uint32_t> boundary_vertices;
      for(const auto& face:boundary) {
        boundary_vertices.insert(face.begin(),face.end());
        for(const auto endpoints:std::array<std::array<unsigned,2>,3>{{
                {{0U,1U}},{{1U,2U}},{{2U,0U}}}}) {
          std::array<std::uint32_t,2> edge{{face[endpoints[0]],face[endpoints[1]]}};
          std::sort(edge.begin(),edge.end());
          ++boundary_edges[edge];
        }
      }
      if(std::any_of(boundary_edges.begin(),boundary_edges.end(),
          [](const auto& entry) { return entry.second!=2U; }))return false;
      // A connected closed triangulated surface with Euler characteristic 2
      // is a sphere.  Since the added tetrahedron touches the current cavity,
      // this admits exactly the safe local 3-ball expansion required here.
      std::vector<bool> visited(boundary.size());
      std::vector<std::size_t> pending{0U};visited[0]=true;
      while(!pending.empty()) {
        const auto current=pending.back();pending.pop_back();
        for(std::size_t candidate=0U;candidate<boundary.size();++candidate) {
          if(visited[candidate])continue;
          unsigned shared{};
          for(const auto left:boundary[current])for(const auto right:boundary[candidate])
            if(left==right)++shared;
          if(shared>=2U) {visited[candidate]=true;pending.push_back(candidate);}
        }
      }
      return std::ranges::all_of(visited,[](bool value) { return value; })&&
          boundary_vertices.size()+boundary.size()==boundary_edges.size()+2U;
    };
    std::vector<bool> plane_expanded(cells.size());
    for(bool expanded=true;expanded;) {
      expanded=false;
      for(std::size_t cell=0U;cell<cells.size()&&!expanded;++cell)if(conflict[cell])
        for(unsigned omitted=0U;omitted<4U&&!expanded;++omitted) {
          const auto face=wang_boundary_face(cells[cell],omitted);
          const auto other=neighbours[cell][omitted].first;
          if(other>=cells.size())
            return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
          if(conflict[other])continue;
          const Tet child{{query,face[0],face[1],face[2]}};
          const auto semantic_plane=plane_aware(child).semantically_coplanar;
          const auto finite_face=std::find(face.begin(),face.end(),ghost)==face.end();
          const auto closes_expanded_visibility=
              plane_expanded[cell]&&finite_face&&robust_orient(points,child)<=0;
          if((!semantic_plane&&!closes_expanded_visibility)||
             !preserves_inserted_stars(other)||!preserves_ball_boundary(other))continue;
          conflict[other]=true;
          plane_expanded[other]=true;
          cavity_order.push_back(other);
          expanded=true;
        }
    }
    // Record the source-equivalent working cavity before adjustment.  The
    // adjustment can delete entries, so recording it afterwards would hide
    // the first findBWCavity divergence behind a later phase.
    if(seed_trace&&query<original_count) {
      std::vector<Tet> raw_working;
      std::vector<std::size_t> raw_working_slots;
      for(const auto cell:cavity_order)if(conflict[cell]) {
        raw_working.push_back(cells[cell]);
        raw_working_slots.push_back(cell_slots[cell]);
      }
      seed_trace->original_working_cavities.push_back(std::move(raw_working));
      seed_trace->original_working_slots.push_back(std::move(raw_working_slots));
    }
    // `adjustBWCavity` is part of every source BW insertion, including
    // info==0 seed insertion.  Keep its working-cavity slots, per-cell face
    // masks, and LIFO work queue rather than replacing it with a global
    // reclassification pass.  Its removal order is observable: it chooses
    // which marginal cells remain available to the subsequent fill.
    std::vector<std::size_t> working=cavity_order;
    std::vector<bool> working_live(working.size(),true);
    std::vector<std::uint8_t> boundary_masks(working.size());
    std::vector<std::size_t> working_index(cells.size(),cells.size());
    for(std::size_t i=0U;i<working.size();++i)working_index[working[i]]=i;
    if(predicate_trace) {
      std::map<Face,unsigned> raw_uses;
      for(const auto cell:working)
        for(unsigned omitted=0U;omitted<4U;++omitted)
          ++raw_uses[canonical_face(wang_boundary_face(cells[cell],omitted))];
      std::vector<Face> raw_boundary;
      for(const auto& [face,uses]:raw_uses)if(uses==1U)raw_boundary.push_back(face);
      std::map<std::array<std::uint32_t,2>,unsigned> raw_edges;
      for(const auto& face:raw_boundary)for(const auto ends:
          std::array<std::array<unsigned,2>,3>{{{{0U,1U}},{{1U,2U}},{{2U,0U}}}}) {
        std::array<std::uint32_t,2> edge{{face[ends[0]],face[ends[1]]}};
        std::sort(edge.begin(),edge.end());++raw_edges[edge];
      }
      const auto bad_edge=std::find_if(raw_edges.begin(),raw_edges.end(),
          [](const auto& entry) { return entry.second!=2U; });
      trace_this_cavity=bad_edge!=raw_edges.end()&&take_cavity_trace_event();
      if(trace_this_cavity) std::cerr<<"plane_seed_raw_cavity query="<<query
               <<" cells="<<working.size()<<" boundary="<<raw_boundary.size()
               <<" bad_edge="<<bad_edge->second<<'\n';
      if(trace_this_cavity) for(const auto& face:raw_boundary) {
        if(std::find(face.begin(),face.end(),bad_edge->first[0])==face.end()||
           std::find(face.begin(),face.end(),bad_edge->first[1])==face.end())
          continue;
        auto inside=invalid_cell;
        unsigned inside_face=4U;
        for(std::size_t cell=0U;cell<cells.size()&&inside==invalid_cell;++cell)
          if(conflict[cell])for(unsigned omitted=0U;omitted<4U;++omitted)
            if(canonical_face(wang_boundary_face(cells[cell],omitted))==
               canonical_face(face)) {inside=cell;inside_face=omitted;break;}
        if(inside==invalid_cell)continue;
        const auto outside=neighbours[inside][inside_face].first;
        if(outside>=cells.size())continue;
        std::cerr<<"plane_seed_raw_boundary_face query="<<query<<" face="
                 <<face[0]<<','<<face[1]<<','<<face[2]<<" inside="<<inside
                 <<" outside="<<outside<<" outside_tet="
                 <<cells[outside][0]<<','<<cells[outside][1]<<','
                 <<cells[outside][2]<<','<<cells[outside][3]<<" outside_conflict="
                 <<conflict[outside]<<'\n';
      }
    }
    const auto opposite_face=[&](std::size_t cell,unsigned omitted)
        ->std::optional<unsigned> {
      if(cell>=neighbours.size()||omitted>=4U||
         neighbours[cell][omitted].first>=cells.size())return std::nullopt;
      // The stored opposite index is exactly DT::getNeigOrd's result. Do not
      // reconstruct it by comparing the prepareBWFill orientation: ghost
      // normalization intentionally changes that orientation.
      return neighbours[cell][omitted].second;
    };
    for(std::size_t i=0U;i<working.size();++i)
      for(unsigned omitted=0U;omitted<4U;++omitted) {
        const auto other=neighbours[working[i]][omitted].first;
        if(other>=cells.size())
          return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
        if(!conflict[other])boundary_masks[i]|=static_cast<std::uint8_t>(1U<<omitted);
      }
    std::vector<bool> adjust_queued(working.size());
    std::vector<std::size_t> adjust_queue;
    adjust_queue.reserve(working.size());
    for(std::size_t i=0U;i<working.size();++i) {
      adjust_queue.push_back(i);
      adjust_queued[i]=true;
    }
    while(!adjust_queue.empty()) {
      const auto index=adjust_queue.back();adjust_queue.pop_back();
      adjust_queued[index]=false;
      if(!working_live[index])continue;
      const auto cell=working[index];
      bool removed=false;
      for(unsigned omitted=0U;omitted<4U;++omitted) {
        if((boundary_masks[index]&(1U<<omitted))==0U)continue;
        const auto face=wang_boundary_face(cells[cell],omitted);
        const auto other=neighbours[cell][omitted].first;
        if(other>=cells.size())
          return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
        const auto other_index=working_index[other];
        // This is the source's `tetInfo[outtet]` cavity branch. `info==0`
        // has no protected boundary triangle, so both internal mask bits are
        // cleared before the orientation test.
        if(other_index<working.size()&&working_live[other_index]) {
          const auto other_face=opposite_face(cell,omitted);
          if(!other_face)return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
          boundary_masks[index]&=static_cast<std::uint8_t>(~(1U<<omitted));
          boundary_masks[other_index]&=static_cast<std::uint8_t>(~(1U<<*other_face));
          continue;
        }
        if(std::find(face.begin(),face.end(),ghost)!=face.end())continue;
        // DNC(face) gives (b,c,d); the source evaluates orient3d(q,b,d,c).
        // `wang_boundary_face` preserves exactly this DNC ordering, and this
        // oriented child is therefore its sign-equivalent in our predicate
        // convention.
        const Tet child{{query,face[0],face[1],face[2]}};
        // Semantic zero is stronger than the tiny signed volume left by the
        // evaluated coordinates. Apply Wang's existing cavity-adjustment
        // branch so the cell is retained and this invalid cone is not built.
        const auto semantic_plane=plane_aware(child).semantically_coplanar;
        if(!semantic_plane&&robust_orient(points,child)>0)continue;
        for(unsigned neighbour_face=0U;neighbour_face<4U;++neighbour_face) {
          const auto neighbour=opposite_face(cell,neighbour_face);
          if(!neighbour)return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
          const auto outside=neighbours[cell][neighbour_face].first;
          if(outside>=cells.size())
            return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
          const auto outside_index=working_index[outside];
          if(outside_index<working.size()&&working_live[outside_index]) {
            boundary_masks[outside_index]|=static_cast<std::uint8_t>(1U<<*neighbour);
            if(!adjust_queued[outside_index]) {
              adjust_queue.push_back(outside_index);
              adjust_queued[outside_index]=true;
            }
          }
        }
        working_live[index]=false;
        removed=true;
        break;
      }
      static_cast<void>(removed);
    }
    std::vector<Face> boundary;
    for(std::size_t index=0U;index<working.size();++index)if(working_live[index])
      for(unsigned omitted=0U;omitted<4U;++omitted)
        if(boundary_masks[index]&(1U<<omitted))
          boundary.push_back(wang_boundary_face(cells[working[index]],omitted));
    if(predicate_trace) {
      std::map<Face,unsigned> true_uses;
      for(std::size_t index=0U;index<working.size();++index)if(working_live[index])
        for(unsigned omitted=0U;omitted<4U;++omitted)
          ++true_uses[canonical_face(wang_boundary_face(cells[working[index]],omitted))];
      std::vector<Face> true_boundary;
      for(const auto& [face,uses]:true_uses)if(uses==1U)true_boundary.push_back(face);
      std::map<std::array<std::uint32_t,2>,unsigned> true_edges;
      for(const auto& face:true_boundary)for(const auto ends:
          std::array<std::array<unsigned,2>,3>{{{{0U,1U}},{{1U,2U}},{{2U,0U}}}}) {
        std::array<std::uint32_t,2> edge{{face[ends[0]],face[ends[1]]}};
        std::sort(edge.begin(),edge.end());++true_edges[edge];
      }
      const auto bad_edge=std::find_if(true_edges.begin(),true_edges.end(),
          [](const auto& entry) { return entry.second!=2U; });
      if(bad_edge!=true_edges.end()&&
         (trace_this_cavity||take_cavity_trace_event()))
        std::cerr<<"plane_seed_adjusted_boundary query="<<query
                 <<" masks="<<boundary.size()<<" true="<<true_boundary.size()
                 <<" true_bad_edge="<<(bad_edge==true_edges.end()?0U:bad_edge->second)
                 <<" true_semantic="<<std::count_if(true_boundary.begin(),true_boundary.end(),
                    [&](const Face& face) { return plane_aware({{query,face[0],face[1],face[2]}}).semantically_coplanar; })
                 <<'\n';
    }
    if(seed_trace&&query<original_count) {
      std::vector<Tet> adjusted;
      for(std::size_t index=0U;index<working.size();++index)
        if(working_live[index])adjusted.push_back(cells[working[index]]);
      seed_trace->original_adjusted_cavities.push_back(std::move(adjusted));
      seed_trace->original_adjusted_boundary_faces.push_back(boundary);
    }
    if(seed_trace&&eighth_box_insertion) {
      seed_trace->eighth_working_cavity.clear();
      seed_trace->eighth_working_slots.clear();
      seed_trace->eighth_boundary_faces=boundary;
      for(const auto cell:cavity_order)if(conflict[cell]) {
        seed_trace->eighth_working_cavity.push_back(cells[cell]);
        seed_trace->eighth_working_slots.push_back(cell_slots[cell]);
      }
    }
    if(seed_trace&&query==original_count) {
      seed_trace->first_box_working_cavity.clear();
      seed_trace->first_box_working_slots.clear();
      for(const auto cell:cavity_order)if(conflict[cell]) {
        seed_trace->first_box_working_cavity.push_back(cells[cell]);
        seed_trace->first_box_working_slots.push_back(cell_slots[cell]);
      }
      seed_trace->first_box_boundary_faces=boundary;
    }
    if(seed_trace&&query==original_count+4U) {
      seed_trace->fourth_box_working_cavity.clear();
      seed_trace->fourth_box_working_slots.clear();
      for(const auto cell:cavity_order)if(conflict[cell]) {
        seed_trace->fourth_box_working_cavity.push_back(cells[cell]);
        seed_trace->fourth_box_working_slots.push_back(cell_slots[cell]);
      }
      seed_trace->fourth_box_boundary_faces=boundary;
    }
    if(seed_trace&&query==original_count+3U) {
      seed_trace->third_box_working_cavity.clear();
      seed_trace->third_box_working_slots.clear();
      for(const auto cell:cavity_order)if(conflict[cell]) {
        seed_trace->third_box_working_cavity.push_back(cells[cell]);
        seed_trace->third_box_working_slots.push_back(cell_slots[cell]);
      }
      seed_trace->third_box_boundary_faces=boundary;
    }
    if(seed_trace&&seventh_box_insertion) {
      seed_trace->seventh_box_working_cavity.clear();
      seed_trace->seventh_box_working_slots.clear();
      for(const auto cell:cavity_order)if(conflict[cell]) {
        seed_trace->seventh_box_working_cavity.push_back(cells[cell]);
        seed_trace->seventh_box_working_slots.push_back(cell_slots[cell]);
      }
      seed_trace->seventh_box_boundary_faces=boundary;
    }
    if(seed_trace&&query<original_count)
      seed_trace->original_boundary_faces.push_back(boundary);
    std::vector<Tet> next;
    std::vector<std::size_t> next_slots;
    std::vector<std::size_t> old_to_next(cells.size(),invalid_cell);
    for(std::size_t cell=0U;cell<cells.size();++cell)
      if(!conflict[cell]||
         (working_index[cell]<working.size()&&!working_live[working_index[cell]])) {
        old_to_next[cell]=next.size();
        next.push_back(cells[cell]);
        next_slots.push_back(cell_slots[cell]);
      }
    const auto retained_count=next.size();
    for(const auto face:boundary) {
      Tet child{{query,face[0],face[1],face[2]}};
      if(child[3]!=ghost&&(plane_aware(child).semantically_coplanar||
                            robust_orient(points,child)<=0)) {
        return invalid(CanonicalDelaunaySeedInvalidReason::degenerate_final_cell);
      }
      next.push_back(child);
      const auto slot=next_vacant_slot<vacant_slots.size()?
          vacant_slots[next_vacant_slot++]:element_slots.size();
      if(slot==element_slots.size()) {
        element_slots.push_back(child);live_slots.push_back(true);
      } else {
        element_slots[slot]=child;live_slots[slot]=true;
      }
      allocated_slots.push_back(slot);
      next_slots.push_back(slot);
      for(const auto vertex:child)
        if(vertex<points.size()) {
          point_carrier_slot[vertex]=slot;
          has_point_carrier[vertex]=true;
        }
      if(seed_trace&&eighth_box_insertion)
        seed_trace->eighth_fill_cells.push_back(next.back());
    }
    // Untouched retained-retained bonds remain valid by induction. Remap
    // those indices and rebuild only the open cavity shell plus the new cone,
    // rather than hashing every face in the complete mesh a second time.
    std::vector<std::array<FaceUse,4>> next_neighbours(next.size());
    for(auto& adjacent:next_neighbours)adjacent.fill(invalid_use);
    for(std::size_t old=0U;old<cells.size();++old) {
      const auto mapped=old_to_next[old];
      if(mapped==invalid_cell)continue;
      for(unsigned omitted=0U;omitted<4U;++omitted) {
        const auto old_other=neighbours[old][omitted];
        if(old_other.first>=old_to_next.size()||old_other.second>=4U)
          return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
        const auto mapped_other=old_to_next[old_other.first];
        if(mapped_other!=invalid_cell)
          next_neighbours[mapped][omitted]={mapped_other,old_other.second};
      }
    }
    std::unordered_map<Face,FaceUses,FaceHash> changed_faces;
    changed_faces.reserve((next.size()-retained_count)*4U+boundary.size());
    for(std::size_t cell=0U;cell<next.size();++cell)
      for(unsigned omitted=0U;omitted<4U;++omitted)
        if(cell>=retained_count||
           next_neighbours[cell][omitted].first==invalid_cell)
          changed_faces[canonical_face(wang_boundary_face(next[cell],omitted))]
              .push_back({cell,omitted});
    const auto invalid_next=std::find_if(changed_faces.begin(),changed_faces.end(),
        [](const auto& entry) { return entry.second.size()!=2U; });
    if(invalid_next!=changed_faces.end()) {
      if(std::getenv("WANG_PLANE_PREDICATE_TRACE")!=nullptr) {
        std::cerr<<"plane_seed_commit_incidence query="<<query
                 <<" cells="<<next.size()<<" face="
                 <<invalid_next->first[0]<<','<<invalid_next->first[1]<<','
                 <<invalid_next->first[2]<<" uses="<<invalid_next->second.size()
                 <<" boundary="<<boundary.size()<<'\n';
        const auto same_face=invalid_next->first;
        for(std::size_t index=0U;index<next.size();++index)
          for(unsigned omitted=0U;omitted<4U;++omitted)
            if(canonical_face(wang_boundary_face(next[index],omitted))==same_face)
              std::cerr<<"plane_seed_commit_use source="
                       <<(index<next_slots.size()&&
                           std::find(allocated_slots.begin(),allocated_slots.end(),
                                     next_slots[index])!=allocated_slots.end()?"child":"retained")
                       <<" tet="<<next[index][0]<<','<<next[index][1]<<','
                       <<next[index][2]<<','<<next[index][3]<<" omitted="<<omitted<<'\n';
      }
      return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
    }
    for(const auto& [face,uses]:changed_faces) {
      static_cast<void>(face);
      next_neighbours[uses[0].first][uses[0].second]=uses[1];
      next_neighbours[uses[1].first][uses[1].second]=uses[0];
    }
    // `finishBW` deletes cavity elements after every replacement slot has
    // been allocated. Keep the same temporal ordering for later carrier
    // lookup and allocator reuse.
    for(std::size_t work=0U;work<working.size();++work)if(working_live[work]) {
      const auto cell=working[work];
      const auto index=cell_slots[cell];
      if(index>=live_slots.size()||!live_slots[index])
        return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
      live_slots[index]=false;vacant_slots.push_back(index);
    }
    cells=std::move(next);cell_slots=std::move(next_slots);
    neighbours=std::move(next_neighbours);inserted.insert(query);
    if(query<original_count) insertion_anchor=query;
    if(seed_trace&&query<original_count)
      seed_trace->original_slot_stages.push_back(live_slot_snapshot());
    if(query>=original_count) {
      // BW_insert_vertex(..., info=0) returns slots.back() through its
      // caller's carrier.  prepareBWFill and allocation preserve face order,
      // so this is exactly the final appended replacement cell above.
      if(allocated_slots.empty())
        return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
      box_search_carrier=allocated_slots.back();
    }
    if(stage_trace) {
      if(query<original_count) {
        ++inserted_originals;
        if(inserted_originals==original_count)
          stage_trace->push_back(finite_snapshot());
      } else stage_trace->push_back(finite_snapshot());
    }
    if(seed_trace) {
      seed_trace->point_carrier_stages.push_back(point_carrier_snapshot());
      if(query<original_count) {
        if(inserted_originals==original_count)
          seed_trace->live_slot_stages.push_back(live_slot_snapshot());
      } else seed_trace->live_slot_stages.push_back(live_slot_snapshot());
    }
  }
  std::vector<Tet> finite;
  for(auto cell:cells)if(cell[3]!=ghost) {
    const auto orientation=robust_orient(points,cell);
    if(orientation==0)
      return invalid(CanonicalDelaunaySeedInvalidReason::degenerate_final_cell);
    if(orientation<0)std::swap(cell[0],cell[1]);
    finite.push_back(cell);
  }
  std::set<std::uint32_t> used;
  for(const auto& cell:finite)used.insert(cell.begin(),cell.end());
  if(finite.size()>in.maximum_tetrahedra)
    return refuse(CanonicalDelaunaySeedFailure::resource_limit);
  if((finite.empty()||used.size()!=points.size())&&
     std::getenv("WANG_PLANE_PREDICATE_TRACE")!=nullptr) {
    std::cerr<<"plane_seed_coverage used="<<used.size()<<" points="<<points.size()
             <<" missing=";
    for(std::uint32_t vertex=0U;vertex<points.size();++vertex)
      if(!used.contains(vertex))std::cerr<<vertex<<',';
    std::cerr<<'\n';
  }
  if(finite.empty()||used.size()!=points.size())
    return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
  const auto stable_id=[&](std::uint32_t vertex) {
    return in.stable_vertex_ids.empty()?static_cast<std::uint64_t>(vertex):
        in.stable_vertex_ids[vertex];
  };
  std::sort(finite.begin(),finite.end(),[&](const Tet& left,const Tet& right) {
    std::array<std::uint64_t,4> a{},b{};
    for(unsigned i=0U;i<4U;++i){a[i]=stable_id(left[i]);b[i]=stable_id(right[i]);}
    std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());return a<b;
  });
  CanonicalDelaunaySeedResult result;
  result.failure=CanonicalDelaunaySeedFailure::none;
  if(final_state_trace) {
    // DT::buildBndInfo calls setAllP2T after BndPntInst.  This is a physical
    // element-slot scan, not a geometry choice: each finite live slot wins
    // for every one of its vertices as the scan advances.  Recovery's
    // finddirection begins from that resulting carrier, so retain it before
    // exporting the seed into the ordered recovery mesh.
    for(std::size_t slot=0U;slot<element_slots.size();++slot) {
      if(!live_slots[slot]||element_slots[slot][3]==ghost)continue;
      for(const auto vertex:element_slots[slot]) {
        if(vertex>=points.size())continue;
        point_carrier_slot[vertex]=slot;
        has_point_carrier[vertex]=true;
      }
    }
    final_state_trace->point_to_tetrahedron.resize(points.size());
    for(std::size_t vertex=0;vertex<points.size();++vertex)
      if(has_point_carrier[vertex] &&
         point_carrier_slot[vertex]<element_slots.size() &&
         live_slots[point_carrier_slot[vertex]])
        final_state_trace->point_to_tetrahedron[vertex]=
            element_slots[point_carrier_slot[vertex]];
    final_state_trace->final_live_slots=live_slot_snapshot();
    final_state_trace->final_slot_count=element_slots.size();
    final_state_trace->final_vacancy_slots.assign(
        vacant_slots.begin()+static_cast<std::ptrdiff_t>(next_vacant_slot),
        vacant_slots.end());
  }
  result.tetrahedra=std::move(finite);return result;
}
} // namespace

PlaneAwareOrientation evaluate_plane_aware_orientation(
    const std::array<Vec3,4>& positions,
    const std::array<std::uint64_t,4>& stable_vertex_ids,
    std::span<const ExactAffinePlaneProvenance> planes) {
  PlaneAwareOrientation result;
  result.geometric_sign=static_cast<int>(exact_orientation_3d(
      positions[0],positions[1],positions[2],positions[3]));
  result.semantically_coplanar=
      is_semantically_coplanar(stable_vertex_ids,planes);
  std::array<std::pair<std::uint64_t,unsigned>,4> order{};
  for(unsigned i=0U;i<4U;++i)order[i]={stable_vertex_ids[i],i};
  if(std::set<std::uint64_t>(stable_vertex_ids.begin(),stable_vertex_ids.end()).size()!=4U)
    return result;
  unsigned inversions{};
  for(unsigned i=0U;i<4U;++i)for(unsigned j=i+1U;j<4U;++j)
    if(order[j].first<order[i].first)++inversions;
  result.combinatorial_sign=(result.semantically_coplanar||result.geometric_sign==0)?
      ((inversions&1U)!=0U?-1:1):result.geometric_sign;
  return result;
}

WangReferenceSeedTrace trace_wang_reference_seed(
    const CanonicalDelaunaySeedInput& input,std::size_t original_vertex_count) {
  WangReferenceSeedTrace trace;
  trace.insertion_order=wang_hilbert_order(input.vertices,original_vertex_count);
  trace.result=build_wang_reference_seed(
      input,original_vertex_count,&trace.stages,&trace);
  return trace;
}

WangReferenceSeedTrace build_wang_reference_seed_for_recovery(
    const CanonicalDelaunaySeedInput& input,std::size_t original_vertex_count) {
  WangReferenceSeedTrace trace;
  trace.insertion_order=wang_hilbert_order(input.vertices,original_vertex_count);
  trace.result=build_wang_reference_seed(
      input,original_vertex_count,&trace.stages,&trace,false);
  return trace;
}

CanonicalDelaunaySeedResult build_canonical_stellar_seed(const CanonicalDelaunaySeedInput& in) {
  const auto refuse=[](CanonicalDelaunaySeedFailure failure){CanonicalDelaunaySeedResult r;r.failure=failure;return r;};
  const auto invalid=[](CanonicalDelaunaySeedInvalidReason reason,Face face={},std::uint32_t witness=0U){CanonicalDelaunaySeedResult r;r.failure=CanonicalDelaunaySeedFailure::invalid_output;r.invalid_reason=reason;r.invalid_hull_face=face;r.invalid_hull_witness=witness;r.stellar_fallback=true;return r;};
  const auto& input=in.vertices;
  if(input.size()<4U)return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
  if(input.size()>in.maximum_vertices||in.maximum_tetrahedra==0U)return refuse(CanonicalDelaunaySeedFailure::resource_limit);
  auto ids=in.stable_vertex_ids;if(ids.empty()){ids.resize(input.size());std::iota(ids.begin(),ids.end(),0U);}
  if(ids.size()!=input.size()||std::set<std::uint64_t>(ids.begin(),ids.end()).size()!=ids.size())return refuse(CanonicalDelaunaySeedFailure::duplicate_stable_id);
  for(const auto p:input)if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))return refuse(CanonicalDelaunaySeedFailure::non_finite_input);
  for(std::size_t i=0U;i<input.size();++i)for(std::size_t j=0U;j<i;++j)if(input[i].x==input[j].x&&input[i].y==input[j].y&&input[i].z==input[j].z)return refuse(CanonicalDelaunaySeedFailure::duplicate_position);
  Vec3 lo=input.front(),hi=input.front();for(const auto p:input){lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);lo.z=std::min(lo.z,p.z);hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);hi.z=std::max(hi.z,p.z);}
  const Vec3 mid=(lo+hi)/2.0;const long double extent=std::max({hi.x-lo.x,hi.y-lo.y,hi.z-lo.z});
  if(!(extent>0.0L))return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
  std::vector<Point> points;points.reserve(input.size());for(const auto p:input)points.push_back({
      static_cast<double>((static_cast<long double>(p.x)-mid.x)/extent),
      static_cast<double>((static_cast<long double>(p.y)-mid.y)/extent),
      static_cast<double>((static_cast<long double>(p.z)-mid.z)/extent)});
  if(!has_dimension_three(points))return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
  std::vector<std::uint32_t> order(input.size());std::iota(order.begin(),order.end(),0U);
  std::sort(order.begin(),order.end(),[&](auto a,auto b){return std::tie(ids[a],points[a].x,points[a].y,points[a].z)<std::tie(ids[b],points[b].x,points[b].y,points[b].z);});
  // Stellar insertion asks the same coplanar grid predicates many times.
  // Cache their exact signs by the unordered four-point identity; permutation
  // parity restores the sign for each caller's orientation.
  std::map<std::array<std::uint32_t,4>,int> orientation_cache;
  const auto indexed_orient=[&](std::uint32_t a,std::uint32_t b,std::uint32_t c,std::uint32_t d) {
    const auto value=orient(points[a],points[b],points[c],points[d]);
    long double magnitude=1.0L;
    for(const auto index:{a,b,c,d}) {
      const auto& point=points[index];
      magnitude=std::max({magnitude,std::abs(point.x),std::abs(point.y),std::abs(point.z)});
    }
    if(std::abs(value)>512.0L*LDBL_EPSILON*magnitude*magnitude*magnitude)
      return value<0.0L?-1:1;
    std::array<std::uint32_t,4> key{{a,b,c,d}};unsigned swaps{};
    for(unsigned pass=0U;pass<key.size();++pass)for(unsigned i=1U;i<key.size()-pass;++i)
      if(key[i]<key[i-1U]){std::swap(key[i],key[i-1U]);++swaps;}
    if(std::adjacent_find(key.begin(),key.end())!=key.end())return 0;
    const auto [it,inserted]=orientation_cache.emplace(key,0);
    if(inserted)it->second=sign(exact_orientation_3d(
        as_vec3(points[key[0]]),as_vec3(points[key[1]]),
        as_vec3(points[key[2]]),as_vec3(points[key[3]])));
    return swaps%2U==0U?it->second:-it->second;
  };
  const auto indexed_orient_tet=[&](const Tet& cell){return indexed_orient(cell[0],cell[1],cell[2],cell[3]);};

  std::array<std::uint32_t,4> initial{{order[0],order[1],order[2],order[3]}};
  if(input.size()<1024U) {
    bool found{};
    for(std::size_t a=0U;a<order.size()&&!found;++a)for(std::size_t b=a+1U;b<order.size()&&!found;++b)
      for(std::size_t c=b+1U;c<order.size()&&!found;++c)for(std::size_t d=c+1U;d<order.size();++d)
        if(indexed_orient(order[a],order[b],order[c],order[d])!=0){initial={{order[a],order[b],order[c],order[d]}};found=true;break;}
    if(!found)return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
  } else {
    // A first-non-coplanar simplex in stable-ID order can be arbitrarily flat
    // for large grid-heavy PLCs. Select a deterministic, well-spread simplex:
    // lexicographic extreme, farthest point, farthest from its line, then
    // farthest from its plane. Small recovery cavities retain their historical
    // canonical seed above because their exact topology is part of the API.
    const auto coordinate_less=[&](std::uint32_t a,std::uint32_t b) {
      return std::tie(points[a].x,points[a].y,points[a].z,ids[a])<
             std::tie(points[b].x,points[b].y,points[b].z,ids[b]);
    };
    const auto first=*std::min_element(order.begin(),order.end(),coordinate_less);
    const auto select_max=[&](const auto& measure,const std::set<std::uint32_t>& excluded) {
      auto best=first;long double best_value=-1.0L;
      for(const auto vertex:order) {
        if(excluded.contains(vertex))continue;
        const auto value=measure(vertex);
        if(value>best_value||(value==best_value&&ids[vertex]<ids[best])){best=vertex;best_value=value;}
      }
      return std::pair{best,best_value};
    };
    const auto [second,second_measure]=select_max(
        [&](std::uint32_t vertex){const auto delta=points[vertex]-points[first];return dot(delta,delta);},{first});
    if(!(second_measure>0.0L))return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
    const auto axis=points[second]-points[first];
    const auto [third,third_measure]=select_max(
        [&](std::uint32_t vertex){const auto area=cross(axis,points[vertex]-points[first]);return dot(area,area);},{first,second});
    if(!(third_measure>0.0L))return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
    const auto [fourth,fourth_measure]=select_max(
        [&](std::uint32_t vertex){return std::abs(orient(points[first],points[second],points[third],points[vertex]));},{first,second,third});
    if(!(fourth_measure>0.0L)||indexed_orient(first,second,third,fourth)==0)
      return refuse(CanonicalDelaunaySeedFailure::insufficient_dimension);
    initial={{first,second,third,fourth}};
  }
  const Point interior=(points[initial[0]]+points[initial[1]]+points[initial[2]]+points[initial[3]])*.25L;
  std::vector<Face> hull;
  for(unsigned omitted=0U;omitted<4U;++omitted){Face face{};unsigned cursor{};for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=initial[i];if(robust_orient(points[face[0]],points[face[1]],points[face[2]],interior)>0)std::swap(face[0],face[1]);hull.push_back(face);}
  std::set<std::uint32_t> initial_set(initial.begin(),initial.end());
  using Edge=std::array<std::uint32_t,2>;
  for(const auto vertex:order) {
    if(initial_set.contains(vertex))continue;
    std::vector<bool> visible(hull.size());std::size_t visible_count{};
    for(std::size_t i=0U;i<hull.size();++i)if(indexed_orient(hull[i][0],hull[i][1],hull[i][2],vertex)>0){visible[i]=true;++visible_count;}
    if(visible_count==0U)continue;
    std::map<Edge,unsigned> edge_count;
    for(std::size_t i=0U;i<hull.size();++i)if(visible[i])for(unsigned e=0U;e<3U;++e){Edge edge{{hull[i][e],hull[i][(e+1U)%3U]}};std::sort(edge.begin(),edge.end());++edge_count[edge];}
    std::vector<Face> next;next.reserve(hull.size()+edge_count.size());for(std::size_t i=0U;i<hull.size();++i)if(!visible[i])next.push_back(hull[i]);
    for(const auto& [edge,count]:edge_count)if(count==1U){Face face{{edge[0],edge[1],vertex}};if(robust_orient(points[face[0]],points[face[1]],points[face[2]],interior)>0)std::swap(face[0],face[1]);next.push_back(face);}else if(count>2U)return invalid(CanonicalDelaunaySeedInvalidReason::nonmanifold_cavity);
    if(next.empty()) {
      auto result=invalid(CanonicalDelaunaySeedInvalidReason::nonconvex_hull,{},vertex);
      result.diagnostic_hull_faces=hull.size();
      result.diagnostic_cone_cells=visible_count;
      return result;
    }
    hull=std::move(next);
  }
  const auto anchor=initial[0];std::vector<Tet> cells;
  for(const auto face:hull){if(std::find(face.begin(),face.end(),anchor)!=face.end())continue;Tet cell{{anchor,face[0],face[1],face[2]}};const auto orientation=indexed_orient_tet(cell);if(orientation==0)continue;if(orientation<0)std::swap(cell[0],cell[1]);cells.push_back(cell);}
  if(cells.empty()) {
    auto result=invalid(CanonicalDelaunaySeedInvalidReason::no_final_cells);
    result.diagnostic_hull_faces=hull.size();
    result.diagnostic_cone_cells=cells.size();
    return result;
  }

  const auto positive=[&](Tet cell)->std::optional<Tet>{const auto orientation=indexed_orient_tet(cell);if(orientation==0)return std::nullopt;if(orientation<0)std::swap(cell[0],cell[1]);return cell;};
  for(const auto vertex:order) {
    if(std::any_of(cells.begin(),cells.end(),[&](const Tet& cell){return std::find(cell.begin(),cell.end(),vertex)!=cell.end();}))continue;
    struct Containment {std::size_t cell{};std::vector<unsigned> zero_faces;};std::vector<Containment> containing;
    for(std::size_t ci=0U;ci<cells.size();++ci){const auto& cell=cells[ci];Containment hit{ci,{}};bool inside=true;for(unsigned omitted=0U;omitted<4U;++omitted){Face face{};unsigned cursor{};for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];const auto opposite=indexed_orient(face[0],face[1],face[2],cell[omitted]);const auto query=indexed_orient(face[0],face[1],face[2],vertex);if(query==0)hit.zero_faces.push_back(omitted);else if(query!=opposite){inside=false;break;}}if(inside)containing.push_back(std::move(hit));}
    if(containing.empty())return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
    const auto exemplar=*std::max_element(containing.begin(),containing.end(),[](const auto& a,const auto& b){return a.zero_faces.size()<b.zero_faces.size();});
    std::vector<std::size_t> selected;std::vector<Tet> children;
    if(exemplar.zero_faces.empty()) {
      selected.push_back(exemplar.cell);const auto old=cells[exemplar.cell];for(unsigned omitted=0U;omitted<4U;++omitted){Tet child{};unsigned cursor{};for(unsigned i=0U;i<4U;++i)if(i!=omitted)child[cursor++]=old[i];child[3]=vertex;const auto oriented=positive(child);if(!oriented)return invalid(CanonicalDelaunaySeedInvalidReason::degenerate_final_cell);children.push_back(*oriented);}
    } else if(exemplar.zero_faces.size()==1U) {
      const auto old=cells[exemplar.cell];const auto omitted=exemplar.zero_faces[0];Face face{};unsigned cursor{};for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=old[i];
      for(std::size_t ci=0U;ci<cells.size();++ci)if(std::all_of(face.begin(),face.end(),[&](auto value){return std::find(cells[ci].begin(),cells[ci].end(),value)!=cells[ci].end();})){selected.push_back(ci);const auto opposite=*std::find_if(cells[ci].begin(),cells[ci].end(),[&](auto value){return std::find(face.begin(),face.end(),value)==face.end();});for(unsigned edge=0U;edge<3U;++edge){Tet child{{opposite,face[edge],face[(edge+1U)%3U],vertex}};const auto oriented=positive(child);if(!oriented)return invalid(CanonicalDelaunaySeedInvalidReason::degenerate_final_cell);children.push_back(*oriented);}}
    } else if(exemplar.zero_faces.size()==2U) {
      const auto old=cells[exemplar.cell];std::array<std::uint32_t,2> edge{};unsigned cursor{};for(unsigned i=0U;i<4U;++i)if(std::find(exemplar.zero_faces.begin(),exemplar.zero_faces.end(),i)==exemplar.zero_faces.end())edge[cursor++]=old[i];
      for(std::size_t ci=0U;ci<cells.size();++ci)if(std::find(cells[ci].begin(),cells[ci].end(),edge[0])!=cells[ci].end()&&std::find(cells[ci].begin(),cells[ci].end(),edge[1])!=cells[ci].end()){selected.push_back(ci);std::array<std::uint32_t,2> other{};unsigned n{};for(const auto value:cells[ci])if(value!=edge[0]&&value!=edge[1])other[n++]=value;for(const auto endpoint:edge){Tet child{{endpoint,vertex,other[0],other[1]}};const auto oriented=positive(child);if(!oriented)return invalid(CanonicalDelaunaySeedInvalidReason::degenerate_final_cell);children.push_back(*oriented);}}
    } else return refuse(CanonicalDelaunaySeedFailure::duplicate_position);
    std::sort(selected.begin(),selected.end());std::vector<Tet> next;next.reserve(cells.size()-selected.size()+children.size());for(std::size_t ci=0U;ci<cells.size();++ci)if(!std::binary_search(selected.begin(),selected.end(),ci))next.push_back(cells[ci]);next.insert(next.end(),children.begin(),children.end());if(next.size()>in.maximum_tetrahedra)return refuse(CanonicalDelaunaySeedFailure::resource_limit);cells=std::move(next);
  }

  std::set<std::uint32_t> used;std::set<Tet> unique;std::map<Face,std::vector<std::uint32_t>> ledger;long double cell_volume{};
  for(auto& cell:cells){if(indexed_orient_tet(cell)<=0)return invalid(CanonicalDelaunaySeedInvalidReason::degenerate_final_cell);auto key=cell;std::sort(key.begin(),key.end());if(!unique.insert(key).second)return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);for(const auto value:cell)used.insert(value);cell_volume+=orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]])/6.0L;for(unsigned omitted=0U;omitted<4U;++omitted){Face face{};unsigned cursor{};for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];ledger[face_key(face)].push_back(cell[omitted]);}}
  if(used.size()!=input.size())return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);
  long double boundary_volume{};
  for(const auto& [canonical,uses]:ledger){if(uses.size()>2U)return invalid(CanonicalDelaunaySeedInvalidReason::incidence_or_coverage);if(uses.size()==2U){if(indexed_orient(canonical[0],canonical[1],canonical[2],uses[0])*indexed_orient(canonical[0],canonical[1],canonical[2],uses[1])>=0)return invalid(CanonicalDelaunaySeedInvalidReason::same_sided_interior_face);continue;}Face outward=canonical;if(indexed_orient(outward[0],outward[1],outward[2],uses[0])>0)std::swap(outward[0],outward[1]);for(std::uint32_t i=0U;i<input.size();++i)if(indexed_orient(outward[0],outward[1],outward[2],i)>0)return invalid(CanonicalDelaunaySeedInvalidReason::nonconvex_hull,outward,i);boundary_volume+=dot(points[outward[0]],cross(points[outward[1]],points[outward[2]]))/6.0L;}
  if(std::abs(cell_volume-boundary_volume)>std::max(1.0L,std::abs(boundary_volume))*1.0e-13L)return invalid(CanonicalDelaunaySeedInvalidReason::volume_disagreement);
  std::sort(cells.begin(),cells.end(),[&](const Tet& a,const Tet& b){std::array<std::uint64_t,4> left{},right{};for(unsigned i=0U;i<4U;++i){left[i]=ids[a[i]];right[i]=ids[b[i]];}std::sort(left.begin(),left.end());std::sort(right.begin(),right.end());return left<right;});CanonicalDelaunaySeedResult result;result.failure=CanonicalDelaunaySeedFailure::none;result.tetrahedra=std::move(cells);result.stellar_fallback=true;return result;
}

CanonicalDelaunaySeedResult build_canonical_background_seed(const CanonicalDelaunaySeedInput& input) {
  auto result=build_canonical_delaunay_seed(input);
  if(result.accepted())return result;
  auto fallback=build_canonical_stellar_seed(input);
  return fallback;
}

CanonicalPlcSeedInspection inspect_canonical_plc_seed(
    const NonmatchingPlcManifestResult& source,std::size_t maximum_tetrahedra){
  CanonicalPlcSeedInspection report;
  if(!source.accepted())return report;
  CanonicalDelaunaySeedInput input;input.maximum_vertices=source.manifest.vertex_geometry.size();input.maximum_tetrahedra=maximum_tetrahedra;
  std::map<std::uint64_t,FrozenFacetVertex> by_id;
  for(const auto& vertex:source.manifest.vertex_geometry){input.vertices.push_back(vertex.position);input.stable_vertex_ids.push_back(vertex.id);by_id.emplace(vertex.id,vertex);}
  const auto seed=build_canonical_background_seed(input);report.seed_failure=seed.failure;report.seed_invalid_reason=seed.invalid_reason;report.invalid_hull_face=seed.invalid_hull_face;report.invalid_hull_witness=seed.invalid_hull_witness;report.stellar_fallback_used=seed.stellar_fallback;report.diagnostic_hull_faces=seed.diagnostic_hull_faces;report.diagnostic_cone_cells=seed.diagnostic_cone_cells;report.candidate_tetrahedra=seed.tetrahedra.size();
  if(!seed.accepted()){report.failure=CanonicalPlcSeedFailure::delaunay_seed_failed;return report;}
  std::set<std::array<std::uint64_t,3>> seed_faces;
  for(const auto& tet:seed.tetrahedra)for(unsigned omit=0;omit<4U;++omit){std::array<std::uint64_t,3> face{};unsigned n=0;for(unsigned i=0;i<4U;++i)if(i!=omit)face[n++]=input.stable_vertex_ids[tet[i]];std::sort(face.begin(),face.end());seed_faces.insert(face);}
  double extent=1.0;if(!input.vertices.empty()){auto lo=input.vertices.front(),hi=lo;for(const auto p:input.vertices){lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);lo.z=std::min(lo.z,p.z);hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);hi.z=std::max(hi.z,p.z);}extent=std::max({hi.x-lo.x,hi.y-lo.y,hi.z-lo.z,1.0});}
  std::set<std::array<std::uint64_t,3>> resolved_required_faces;
  const auto inspect=[&](const std::vector<FrozenFacetSplit>& splits){for(const auto& split:splits){std::array<FrozenFacetVertex,3> parent{};for(unsigned i=0;i<3U;++i){const auto it=by_id.find(split.parent.vertex_ids[i]);if(it==by_id.end()){report.missing_facets.push_back(split.parent.vertex_ids);++report.required_facets;continue;}parent[i]=it->second;}
      for(const auto& subface:split.subfaces){++report.required_facets;std::array<std::uint64_t,3> face{};bool resolved=true;for(unsigned corner=0;corner<3U;++corner){const auto p=evaluate_facet_barycentric(parent,subface.corners[corner]);auto match=by_id.end();for(auto it=by_id.begin();it!=by_id.end();++it){const auto d=it->second.position-p;if(std::abs(d.x)<=extent*1e-13&&std::abs(d.y)<=extent*1e-13&&std::abs(d.z)<=extent*1e-13){if(match!=by_id.end()){resolved=false;break;}match=it;}}if(match==by_id.end())resolved=false;else face[corner]=match->first;if(!resolved)++report.unresolved_facet_vertices;}std::sort(face.begin(),face.end());if(resolved)resolved_required_faces.insert(face);if(resolved&&seed_faces.contains(face))++report.recovered_facets;else report.missing_facets.push_back(face);}}};
  inspect(source.manifest.outer_parent_coverage);inspect(source.manifest.external_core_coverage);
  std::set<std::array<std::uint64_t,2>> seed_edges,required_edges;
  for(const auto& face:seed_faces)for(const auto edge:std::array<std::array<unsigned,2>,3>{{{{0,1}},{{1,2}},{{0,2}}}})seed_edges.insert({face[edge[0]],face[edge[1]]});
  for(const auto& face:resolved_required_faces)for(const auto edge:std::array<std::array<unsigned,2>,3>{{{{0,1}},{{1,2}},{{0,2}}}})required_edges.insert({face[edge[0]],face[edge[1]]});
  report.required_edges=required_edges.size();report.recovered_edges=static_cast<std::size_t>(std::count_if(required_edges.begin(),required_edges.end(),[&](const auto& edge){return seed_edges.contains(edge);}));
  for(const auto& edge:required_edges)if(!seed_edges.contains(edge))report.missing_edges.push_back(edge);
  std::sort(report.missing_facets.begin(),report.missing_facets.end());report.missing_facets.erase(std::unique(report.missing_facets.begin(),report.missing_facets.end()),report.missing_facets.end());
  report.failure=report.recovered_facets==report.required_facets?CanonicalPlcSeedFailure::none:CanonicalPlcSeedFailure::unrecovered_constraint;
  return report;
}

namespace {
std::uint32_t greatest_common_divisor(std::uint32_t a,std::uint32_t b){while(b!=0U){const auto r=a%b;a=b;b=r;}return a;}
std::uint64_t greatest_common_divisor_64(std::uint64_t a,std::uint64_t b){while(b!=0U){const auto r=a%b;a=b;b=r;}return a;}
bool interpolated_barycentric(const FacetBarycentricPoint& a,const FacetBarycentricPoint& b,
                              std::uint32_t numerator,std::uint32_t denominator,
                              FacetBarycentricPoint& out){
  if(numerator==0U||numerator>=denominator)return false;
  const auto gcd=greatest_common_divisor_64(a.denominator,b.denominator);
  const auto lcm=static_cast<unsigned __int128>(a.denominator/gcd)*b.denominator;
  const auto wide_max=~static_cast<unsigned __int128>(0U);
  if(lcm>wide_max/denominator)return false;
  const auto output_denominator=lcm*denominator;
  std::array<unsigned __int128,3> wide_values{};
  for(unsigned i=0;i<3U;++i)wide_values[i]=
      static_cast<unsigned __int128>(a.numerator[i])*(lcm/a.denominator)*(denominator-numerator)+
      static_cast<unsigned __int128>(b.numerator[i])*(lcm/b.denominator)*numerator;
  const auto gcd_wide=[](unsigned __int128 left,unsigned __int128 right){while(right!=0U){const auto remainder=left%right;left=right;right=remainder;}return left;};
  auto common=output_denominator;
  for(const auto value:wide_values)common=gcd_wide(common,value);
  const auto reduced_denominator=output_denominator/common;
  if(reduced_denominator>std::numeric_limits<std::uint64_t>::max())return false;
  out.denominator=static_cast<std::uint64_t>(reduced_denominator);
  for(unsigned i=0;i<3U;++i){const auto reduced=wide_values[i]/common;if(reduced>std::numeric_limits<std::uint64_t>::max())return false;out.numerator[i]=static_cast<std::uint64_t>(reduced);}
  return true;
}
bool composed_facet_barycentric(
    const std::array<FacetBarycentricPoint,3>& corners,
    std::array<std::uint32_t,3> weights,std::uint32_t denominator,
    FacetBarycentricPoint& out) {
  if(denominator==0U||weights[0]==0U||weights[1]==0U||weights[2]==0U||
     static_cast<std::uint64_t>(weights[0])+weights[1]+weights[2]!=denominator)return false;
  using Wide=unsigned __int128;
  const auto wide_max=~static_cast<Wide>(0U);
  const auto gcd_wide=[](Wide left,Wide right){while(right!=0U){const auto remainder=left%right;left=right;right=remainder;}return left;};
  Wide lcm=1U;
  for(const auto& corner:corners) {
    const auto divisor=gcd_wide(lcm,corner.denominator);
    const auto factor=corner.denominator/divisor;
    if(lcm>wide_max/factor)return false;
    lcm*=factor;
  }
  if(lcm>wide_max/denominator)return false;
  const auto output_denominator=lcm*denominator;
  std::array<Wide,3> values{};
  for(unsigned coordinate=0U;coordinate<3U;++coordinate)
    for(unsigned corner=0U;corner<3U;++corner)
      values[coordinate]+=static_cast<Wide>(corners[corner].numerator[coordinate])*
                          (lcm/corners[corner].denominator)*weights[corner];
  auto common=output_denominator;
  for(const auto value:values)common=gcd_wide(common,value);
  const auto reduced_denominator=output_denominator/common;
  if(reduced_denominator>std::numeric_limits<std::uint64_t>::max())return false;
  out.denominator=static_cast<std::uint64_t>(reduced_denominator);
  for(unsigned i=0U;i<3U;++i){const auto value=values[i]/common;if(value>std::numeric_limits<std::uint64_t>::max())return false;out.numerator[i]=static_cast<std::uint64_t>(value);}
  return true;
}
std::uint64_t canonical_edge_split_id(std::array<std::uint64_t,2> edge,std::uint32_t numerator,std::uint32_t denominator) {
  if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
  std::uint64_t edge_id=1469598103934665603ULL;
  for(const auto word:edge) { edge_id^=word;edge_id*=1099511628211ULL; }
  std::uint64_t value=1469598103934665603ULL;
  // Keep this identical to regular_core_arbitrary_refinement's key for
  // (RegularCoreEdgeId(edge endpoints), 1/2).  The provenance tuple, rather
  // than this compact key, remains authoritative if a collision is detected.
  for(const auto word:{edge_id,static_cast<std::uint64_t>(numerator),static_cast<std::uint64_t>(denominator)}) { value^=word;value*=1099511628211ULL; }
  return value|(std::uint64_t{1}<<63U);
}
std::uint64_t canonical_facet_split_id(FrozenFacetIdentity parent,
                                       const FacetBarycentricPoint& point) {
  std::sort(parent.vertex_ids.begin(),parent.vertex_ids.end());
  std::uint64_t value=1099511628211ULL;
  const auto add=[&](std::uint64_t word){value^=word;value*=1469598103934665603ULL;};
  add(0x464143455453504cULL);
  for(const auto word:parent.vertex_ids)add(word);
  for(const auto word:point.numerator)add(word);
  add(point.denominator);
  return value|(std::uint64_t{1}<<63U);
}
bool has_only_nondegenerate_constraint_facets(const CanonicalPlcConstraintSet& constraints) {
  std::map<std::uint64_t,Point> points;
  for(const auto& vertex:constraints.vertices)
    if(!points.emplace(vertex.id,Point{vertex.position.x,vertex.position.y,vertex.position.z}).second)return false;
  for(const auto& facet:constraints.facets) {
    const auto a=points.find(facet.vertices[0]),b=points.find(facet.vertices[1]),c=points.find(facet.vertices[2]);
    if(a==points.end()||b==points.end()||c==points.end())return false;
    const auto normal=cross(b->second-a->second,c->second-a->second);
    // Boundary recovery may temporarily create very small but valid child
    // facets.  Their size is not a degeneracy criterion; only an exactly
    // collinear triple is.  Quality is evaluated after boundary restoration.
    if(dot(normal,normal)==0.0L)return false;
  }
  return true;
}
}

CanonicalPlcConstraintResult materialize_canonical_plc_constraints(const NonmatchingPlcManifestResult& source){
  CanonicalPlcConstraintResult result;if(!source.accepted())return result;result.constraints.vertices=source.manifest.vertex_geometry;
  std::sort(result.constraints.vertices.begin(),result.constraints.vertices.end(),[](const auto& a,const auto& b){return a.id<b.id;});
  std::map<std::uint64_t,FrozenFacetVertex> vertices;for(const auto& vertex:result.constraints.vertices)if(!vertices.emplace(vertex.id,vertex).second)return result;
  const auto resolve=[&vertices](const FrozenFacetSplit& split,const FacetBarycentricPoint& corner)->std::optional<std::uint64_t>{std::array<FrozenFacetVertex,3> parent{};for(unsigned i=0;i<3U;++i){const auto it=vertices.find(split.parent.vertex_ids[i]);if(it==vertices.end())return std::nullopt;parent[i]=it->second;}const auto point=evaluate_facet_barycentric(parent,corner);std::optional<std::uint64_t> found;for(const auto& [id,vertex]:vertices){if(vertex.position.x==point.x&&vertex.position.y==point.y&&vertex.position.z==point.z){if(found)return std::nullopt;found=id;}}return found;};
  const auto append=[&](const std::vector<FrozenFacetSplit>& splits,bool core_interface){for(const auto& split:splits)for(const auto& subface:split.subfaces){CanonicalPlcConstraintFacet facet;facet.parent=split.parent;facet.corners=subface.corners;facet.core_interface=core_interface;for(unsigned i=0;i<3U;++i){const auto id=resolve(split,facet.corners[i]);if(!id)return false;facet.vertices[i]=*id;}facet.source_vertices=facet.vertices;result.constraints.facets.push_back(facet);}return true;};
  if(!append(source.manifest.outer_parent_coverage,false)||!append(source.manifest.external_core_coverage,true)){result.constraints={};result.failure=CanonicalPlcConstraintFailure::unresolved_corner;return result;}
  if(!has_only_nondegenerate_constraint_facets(result.constraints)){result.constraints={};result.failure=CanonicalPlcConstraintFailure::degenerate_facet;return result;}
  result.failure=CanonicalPlcConstraintFailure::none;return result;
}

CanonicalPlcConstraintResult materialize_canonical_plc_constraints(
    const SurfaceCoreTransitionInput& input) {
  CanonicalPlcConstraintResult result;
  const auto validation=validate_surface_core_transition_input(input);
  if(!validation.accepted) {
    result.surface_core_failure=validation.failure;
    result.failing_element=validation.failing_element;
    result.related_element=validation.related_element;
    return result;
  }
  const auto id=[&](std::uint32_t index) {
    return input.stable_vertex_ids.empty()?static_cast<std::uint64_t>(index):
        input.stable_vertex_ids[index];
  };
  for(std::size_t index=0U;index<input.vertices.size();++index)
    result.constraints.vertices.push_back({id(static_cast<std::uint32_t>(index)),
                                           input.vertices[index]});
  result.constraints.exact_affine_planes=input.exact_affine_planes;
  std::sort(result.constraints.vertices.begin(),result.constraints.vertices.end(),
            [](const auto& left,const auto& right){return left.id<right.id;});
  const auto append_literal=[&](std::array<std::uint32_t,3> face,
                                bool core_interface,std::size_t source_face_index,
                                FacetPreservationMode preservation_mode,
                                FrozenFacetIdentity parent) {
    CanonicalPlcConstraintFacet facet;
    for(std::size_t corner=0U;corner<3U;++corner)
      facet.vertices[corner]=id(face[corner]);
    facet.source_vertices=facet.vertices;
    facet.parent=parent;
    std::sort(facet.parent.vertex_ids.begin(),facet.parent.vertex_ids.end());
    for(std::size_t corner=0U;corner<3U;++corner) {
      const auto position=std::find(facet.parent.vertex_ids.begin(),
                                    facet.parent.vertex_ids.end(),
                                    facet.vertices[corner]);
      facet.corners[corner].denominator=1U;
      facet.corners[corner].numerator[
          static_cast<std::size_t>(position-facet.parent.vertex_ids.begin())]=1U;
    }
    facet.preservation_mode=preservation_mode;
    facet.source_face_index=source_face_index;
    facet.core_interface=core_interface;
    result.constraints.facets.push_back(facet);
  };
  for(std::size_t face_index=0U;face_index<input.outer_faces.size();++face_index) {
    const auto face=input.outer_faces[face_index];
    FrozenFacetIdentity parent{{{id(face[0]),id(face[1]),id(face[2])}}};
    auto mode=FacetPreservationMode::literal;
    if(!input.outer_parent_facets.empty()) {
      parent=input.outer_parent_facets[face_index].identity;
      mode=input.outer_parent_facets[face_index].mode;
    }
    append_literal(face,false,face_index,mode,parent);
  }
  using IndexedFace=std::array<std::uint32_t,3>;
  std::map<IndexedFace,std::pair<IndexedFace,unsigned int>> core_faces;
  for(const auto& tet:input.retained_core_tetrahedra)
    for(std::size_t omitted=0U;omitted<4U;++omitted) {
      IndexedFace oriented{};
      std::size_t cursor{};
      for(std::size_t corner=0U;corner<4U;++corner)
        if(corner!=omitted)oriented[cursor++]=tet[corner];
      auto key=oriented;
      std::sort(key.begin(),key.end());
      auto& use=core_faces[key];
      if(use.second==0U)use.first=oriented;
      ++use.second;
    }
  std::map<FrozenFacetIdentity,SurfaceCoreTransitionInput::ParentFacet> core_parents;
  for(const auto& source:input.core_parent_facets) {
    auto key=source.identity;std::sort(key.vertex_ids.begin(),key.vertex_ids.end());
    core_parents.emplace(key,source);
  }
  std::size_t core_face_index{};
  for(const auto& [canonical,use]:core_faces) {
    if(use.second!=1U)continue;
    auto oriented=canonical;
    // The core face presented to Wang is oriented out of the retained core.
    // Find its unique incident tetrahedron and flip the canonical winding
    // when its normal points toward the opposite vertex.
    for(const auto& tet:input.retained_core_tetrahedra) {
      const auto found=std::find_if(tet.begin(),tet.end(),[&](std::uint32_t v) {
        return std::find(canonical.begin(),canonical.end(),v)==canonical.end();
      });
      if(found==tet.end())continue;
      std::size_t members{};
      for(const auto v:tet)
        members+=std::find(canonical.begin(),canonical.end(),v)!=canonical.end()?1U:0U;
      if(members!=3U)continue;
      const auto first=input.vertices[oriented[1]]-input.vertices[oriented[0]];
      const auto second=input.vertices[oriented[2]]-input.vertices[oriented[0]];
      const Vec3 normal{first.y*second.z-first.z*second.y,
                        first.z*second.x-first.x*second.z,
                        first.x*second.y-first.y*second.x};
      const auto offset=input.vertices[*found]-input.vertices[oriented[0]];
      if(normal.x*offset.x+normal.y*offset.y+normal.z*offset.z>0.0)
        std::swap(oriented[1],oriented[2]);
      break;
    }
    FrozenFacetIdentity parent{{{id(canonical[0]),id(canonical[1]),id(canonical[2])}}};
    auto mode=FacetPreservationMode::literal;
    auto key=parent;std::sort(key.vertex_ids.begin(),key.vertex_ids.end());
    if(const auto at=core_parents.find(key);at!=core_parents.end()) {
      parent=at->second.identity;mode=at->second.mode;
    }
    append_literal(oriented,true,core_face_index++,mode,parent);
  }
  if(!has_only_nondegenerate_constraint_facets(result.constraints)) {
    result.constraints={};
    result.failure=CanonicalPlcConstraintFailure::degenerate_facet;
    return result;
  }
  result.failure=CanonicalPlcConstraintFailure::none;
  return result;
}

CanonicalPlcConstraintResult materialize_canonical_plc_constraints(
    std::span<const FrozenFacetVertex> vertices,
    std::span<const std::array<std::uint64_t,3>> literal_faces,
    bool core_interface) {
  CanonicalPlcConstraintResult result;
  std::set<std::uint64_t> ids;
  for(const auto& vertex:vertices) {
    if(!std::isfinite(vertex.position.x)||!std::isfinite(vertex.position.y)||
       !std::isfinite(vertex.position.z)||!ids.insert(vertex.id).second)return result;
    result.constraints.vertices.push_back(vertex);
  }
  std::sort(result.constraints.vertices.begin(),result.constraints.vertices.end(),
            [](const auto& left,const auto& right){return left.id<right.id;});
  for(const auto& face:literal_faces) {
    if(face[0]==face[1]||face[1]==face[2]||face[0]==face[2]||
       !ids.contains(face[0])||!ids.contains(face[1])||!ids.contains(face[2]))
      return {};
    CanonicalPlcConstraintFacet facet;
    facet.vertices=face;
    facet.source_vertices=face;
    facet.parent.vertex_ids=face;
    std::sort(facet.parent.vertex_ids.begin(),facet.parent.vertex_ids.end());
    for(std::size_t corner=0U;corner<3U;++corner) {
      const auto position=std::find(facet.parent.vertex_ids.begin(),
                                    facet.parent.vertex_ids.end(),face[corner]);
      facet.corners[corner].denominator=1U;
      facet.corners[corner].numerator[
          static_cast<std::size_t>(position-facet.parent.vertex_ids.begin())]=1U;
    }
    facet.core_interface=core_interface;
    result.constraints.facets.push_back(facet);
  }
  if(result.constraints.facets.empty()||
     !has_only_nondegenerate_constraint_facets(result.constraints))return {};
  result.failure=CanonicalPlcConstraintFailure::none;
  return result;
}

CanonicalPlcConstraintResult split_canonical_plc_constraint_edge_at_ratio(const CanonicalPlcConstraintSet& input,std::array<std::uint64_t,2> edge,std::uint32_t numerator,std::uint32_t denominator,std::size_t maximum_vertices,std::size_t maximum_facets){
  CanonicalPlcConstraintResult result;result.constraints=input;if(edge[1]<edge[0])std::swap(edge[0],edge[1]);const auto ratio_gcd=greatest_common_divisor(numerator,denominator);if(ratio_gcd==0U)return result;numerator/=ratio_gcd;denominator/=ratio_gcd;if(numerator==0U||numerator>=denominator||input.vertices.size()>=maximum_vertices)return result;
  std::map<std::uint64_t,FrozenFacetVertex> vertices;for(const auto& vertex:input.vertices)if(!vertices.emplace(vertex.id,vertex).second)return result;const auto a=vertices.find(edge[0]),b=vertices.find(edge[1]);if(a==vertices.end()||b==vertices.end())return result;
  const std::uint64_t midpoint_id=canonical_edge_split_id(edge,numerator,denominator);if(vertices.contains(midpoint_id)){result.constraints={};result.failure=CanonicalPlcConstraintFailure::generated_id_collision;return result;}
  std::optional<Vec3> exact_parent_position;
  std::vector<CanonicalPlcConstraintFacet> replaced_facets;
  std::vector<CanonicalPlcConstraintFacet> refined;bool changed=false;
  for(const auto& face:input.facets){std::array<unsigned,2> where{};unsigned count{};for(unsigned i=0;i<3U;++i)if(face.vertices[i]==edge[0]||face.vertices[i]==edge[1])where[count++]=i;if(count!=2U){refined.push_back(face);continue;}changed=true;replaced_facets.push_back(face);unsigned other=0U;while(other==where[0]||other==where[1])++other;const auto first_is_canonical=face.vertices[where[0]]==edge[0];const auto local_numerator=first_is_canonical?numerator:denominator-numerator;FacetBarycentricPoint middle;if(!interpolated_barycentric(face.corners[where[0]],face.corners[where[1]],local_numerator,denominator,middle)){result.constraints={};result.failure=CanonicalPlcConstraintFailure::rational_overflow;return result;}
    if(!exact_parent_position) {
      std::array<FrozenFacetVertex,3> parent_geometry{};bool resolved=true;
      for(unsigned i=0U;i<3U;++i) {
        const auto parent=vertices.find(face.parent.vertex_ids[i]);
        if(parent==vertices.end()){resolved=false;break;}
        parent_geometry[i]=parent->second;
      }
      if(!resolved){result.constraints={};result.failure=CanonicalPlcConstraintFailure::unresolved_corner;return result;}
      exact_parent_position=evaluate_facet_barycentric(parent_geometry,middle);
    }
    CanonicalPlcConstraintFacet left=face,right=face;left.vertices[where[1]]=midpoint_id;left.corners[where[1]]=middle;right.vertices[where[0]]=midpoint_id;right.corners[where[0]]=middle;refined.push_back(left);refined.push_back(right);}
  if(!changed||!exact_parent_position||refined.size()>maximum_facets)return result;
  for(const auto& split:input.split_vertices)if(split.id==midpoint_id||split.edge==edge){result.constraints={};result.failure=CanonicalPlcConstraintFailure::generated_id_collision;return result;}
  const auto placement=static_cast<double>(numerator)/denominator;
  const Vec3 segment_position{
      a->second.position.x+(b->second.position.x-a->second.position.x)*placement,
      a->second.position.y+(b->second.position.y-a->second.position.y)*placement,
      a->second.position.z+(b->second.position.z-a->second.position.z)*placement};
  // `middle` remains the exact immutable-parent provenance. The explicit
  // floating coordinate must lie on the current floating segment used by
  // containment and intersection predicates; evaluating through a different
  // parent triangle can round to the other side of that segment.
  result.constraints.vertices.push_back({midpoint_id,segment_position});result.constraints.facets=std::move(refined);result.constraints.split_vertices.push_back({midpoint_id,edge,numerator,denominator});
  // An affine plane contains every exact segment combination of two of its
  // members. Carry that source fact by identity; never infer it from the
  // rounded placement used by the mutable geometric predicates.
  for(auto& plane:result.constraints.exact_affine_planes)
    if(std::binary_search(plane.vertex_ids.begin(),plane.vertex_ids.end(),edge[0])&&
       std::binary_search(plane.vertex_ids.begin(),plane.vertex_ids.end(),edge[1])) {
      plane.vertex_ids.push_back(midpoint_id);
      std::sort(plane.vertex_ids.begin(),plane.vertex_ids.end());
    }
  result.constraints.recovery_journal.push_back(
      {CanonicalPlcRecoveryInsertionKind::edge_split,midpoint_id,
       std::move(replaced_facets),input.facets});
  if(!has_only_nondegenerate_constraint_facets(result.constraints)) {
    if(!has_only_nondegenerate_constraint_facets(result.constraints)) {
      result.constraints={};result.failure=CanonicalPlcConstraintFailure::degenerate_facet;
      return result;
    }
  }
  result.failure=CanonicalPlcConstraintFailure::none;return result;
}

CanonicalPlcConstraintResult split_canonical_plc_constraint_edge(const CanonicalPlcConstraintSet& input,std::array<std::uint64_t,2> edge,std::size_t maximum_vertices,std::size_t maximum_facets){
  return split_canonical_plc_constraint_edge_at_ratio(input,edge,1U,2U,maximum_vertices,maximum_facets);
}

CanonicalPlcConstraintResult attach_canonical_plc_boundary_vertex_to_segment(
    const CanonicalPlcConstraintSet& input,
    std::array<std::uint64_t,2> edge,std::uint64_t vertex_id,
    std::size_t maximum_facets) {
  CanonicalPlcConstraintResult refused;
  if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
  if(edge[0]==edge[1]||vertex_id==edge[0]||vertex_id==edge[1])return refused;
  const auto first=std::find_if(input.vertices.begin(),input.vertices.end(),
      [&](const auto& vertex){return vertex.id==edge[0];});
  const auto second=std::find_if(input.vertices.begin(),input.vertices.end(),
      [&](const auto& vertex){return vertex.id==edge[1];});
  const auto existing=std::find_if(input.vertices.begin(),input.vertices.end(),
      [&](const auto& vertex){return vertex.id==vertex_id;});
  if(first==input.vertices.end()||second==input.vertices.end()||
     existing==input.vertices.end())return refused;
  if(std::none_of(input.facets.begin(),input.facets.end(),[&](const auto& facet) {
       return std::find(facet.vertices.begin(),facet.vertices.end(),vertex_id)!=
           facet.vertices.end();
     }))return refused;
  const Point a{first->position.x,first->position.y,first->position.z};
  const Point b{second->position.x,second->position.y,second->position.z};
  const Point p{existing->position.x,existing->position.y,existing->position.z};
  const auto direction=b-a,relative=p-a;
  const auto length2=dot(direction,direction);
  if(!(length2>0.0L))return refused;
  const auto parameter=dot(relative,direction)/length2;
  const auto residual=relative-direction*parameter;
  if(!(parameter>0.0L&&parameter<1.0L)||
     dot(residual,residual)>64.0L*LDBL_EPSILON*length2)return refused;

  // Exact-parent provenance needs a compact rational for the already fixed
  // point. Generate only representations that reproduce its three stored
  // coordinates bit-for-bit; this is a representation search, not a new
  // placement rule. Dyadic candidates cover binary construction, while
  // continued-fraction convergents cover simple non-dyadic input ratios.
  std::set<std::pair<std::uint32_t,std::uint32_t>> ratios;
  for(std::uint64_t denominator=2U;denominator<=(1ULL<<30U);
      denominator*=2U) {
    const auto numerator=static_cast<std::uint64_t>(
        std::llround(parameter*static_cast<long double>(denominator)));
    if(numerator==0U||numerator>=denominator)continue;
    const auto divisor=greatest_common_divisor_64(numerator,denominator);
    ratios.emplace(static_cast<std::uint32_t>(numerator/divisor),
                   static_cast<std::uint32_t>(denominator/divisor));
  }
  long double value=parameter;
  std::uint64_t previous_numerator=0U,numerator=1U;
  std::uint64_t previous_denominator=1U,denominator=0U;
  for(unsigned iteration=0U;iteration<64U;++iteration) {
    const auto whole=static_cast<std::uint64_t>(std::floor(value));
    if(whole>std::numeric_limits<std::uint32_t>::max())break;
    const auto next_numerator=whole*numerator+previous_numerator;
    const auto next_denominator=whole*denominator+previous_denominator;
    if(next_denominator==0U||
       next_numerator>std::numeric_limits<std::uint32_t>::max()||
       next_denominator>std::numeric_limits<std::uint32_t>::max())break;
    if(next_numerator>0U&&next_numerator<next_denominator)
      ratios.emplace(static_cast<std::uint32_t>(next_numerator),
                     static_cast<std::uint32_t>(next_denominator));
    previous_numerator=numerator;numerator=next_numerator;
    previous_denominator=denominator;denominator=next_denominator;
    const auto fraction=value-static_cast<long double>(whole);
    if(fraction==0.0L)break;
    value=1.0L/fraction;
  }
  std::vector<std::pair<std::uint32_t,std::uint32_t>> ordered(
      ratios.begin(),ratios.end());
  std::sort(ordered.begin(),ordered.end(),[](const auto& left,const auto& right) {
    return std::tie(left.second,left.first)<std::tie(right.second,right.first);
  });
  for(const auto [split_numerator,split_denominator]:ordered) {
    auto split=split_canonical_plc_constraint_edge_at_ratio(
        input,edge,split_numerator,split_denominator,input.vertices.size()+1U,
        maximum_facets);
    if(!split.accepted())continue;
    const auto generated=split.constraints.vertices.back();
    if(generated.position.x!=existing->position.x||
       generated.position.y!=existing->position.y||
       generated.position.z!=existing->position.z)continue;
    for(auto& facet:split.constraints.facets)
      for(auto& id:facet.vertices)if(id==generated.id)id=vertex_id;
    split.constraints.vertices.pop_back();
    split.constraints.split_vertices.pop_back();
    split.constraints.recovery_journal.pop_back();
    std::set<std::array<std::uint64_t,3>> unique;
    bool duplicate=false;
    for(const auto& facet:split.constraints.facets) {
      auto key=facet.vertices;std::sort(key.begin(),key.end());
      if(!unique.insert(key).second){duplicate=true;break;}
    }
    if(duplicate||!has_only_nondegenerate_constraint_facets(split.constraints))
      continue;
    split.failure=CanonicalPlcConstraintFailure::none;
    return split;
  }
  refused.failure=CanonicalPlcConstraintFailure::rational_overflow;
  return refused;
}

CanonicalPlcConstraintResult
promote_canonical_interior_steiner_point_to_segment(
    const CanonicalPlcConstraintSet& input,std::array<std::uint64_t,2> edge,
    std::uint64_t vertex_id,std::uint32_t numerator,
    std::uint32_t denominator,std::size_t maximum_facets) {
  CanonicalPlcConstraintResult refused;
  if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
  if(edge[0]==edge[1]||vertex_id==edge[0]||vertex_id==edge[1]||
     denominator==0U||numerator==0U||numerator>=denominator)return refused;
  const auto existing=std::find_if(input.vertices.begin(),input.vertices.end(),
      [&](const auto& vertex){return vertex.id==vertex_id;});
  const auto disposable=std::find_if(input.interior_steiner_vertices.begin(),
      input.interior_steiner_vertices.end(),[&](const auto& vertex) {
        return vertex.id==vertex_id;
      });
  if(existing==input.vertices.end()||
     disposable==input.interior_steiner_vertices.end())return refused;
  const auto split=split_canonical_plc_constraint_edge_at_ratio(
      input,edge,numerator,denominator,input.vertices.size()+1U,maximum_facets);
  if(!split.accepted()||split.constraints.vertices.empty()||
     split.constraints.split_vertices.empty()||
     split.constraints.recovery_journal.empty())return refused;
  const auto generated=split.constraints.vertices.back();
  if(generated.position.x!=existing->position.x||
     generated.position.y!=existing->position.y||
     generated.position.z!=existing->position.z)return refused;
  auto result=split;
  for(auto& facet:result.constraints.facets)
    for(auto& id:facet.vertices)if(id==generated.id)id=vertex_id;
  result.constraints.vertices.pop_back();
  result.constraints.split_vertices.back().id=vertex_id;
  result.constraints.recovery_journal.back().vertex_id=vertex_id;
  result.constraints.interior_steiner_vertices.erase(
      std::remove_if(result.constraints.interior_steiner_vertices.begin(),
                     result.constraints.interior_steiner_vertices.end(),
                     [&](const auto& vertex){return vertex.id==vertex_id;}),
      result.constraints.interior_steiner_vertices.end());
  std::set<std::array<std::uint64_t,3>> unique;
  for(const auto& facet:result.constraints.facets) {
    auto key=facet.vertices;std::sort(key.begin(),key.end());
    if(!unique.insert(key).second||
       !has_only_nondegenerate_constraint_facets(result.constraints))return refused;
  }
  result.failure=CanonicalPlcConstraintFailure::none;
  return result;
}

CanonicalPlcConstraintResult
promote_canonical_interior_steiner_point_to_segment(
    const CanonicalPlcConstraintSet& input,std::array<std::uint64_t,2> edge,
    std::uint64_t vertex_id,std::size_t maximum_facets) {
  CanonicalPlcConstraintResult refused;
  if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
  const auto first=std::find_if(input.vertices.begin(),input.vertices.end(),
      [&](const auto& vertex){return vertex.id==edge[0];});
  const auto second=std::find_if(input.vertices.begin(),input.vertices.end(),
      [&](const auto& vertex){return vertex.id==edge[1];});
  const auto point=std::find_if(input.vertices.begin(),input.vertices.end(),
      [&](const auto& vertex){return vertex.id==vertex_id;});
  if(first==input.vertices.end()||second==input.vertices.end()||
     point==input.vertices.end())return refused;
  const Point a{first->position.x,first->position.y,first->position.z};
  const Point b{second->position.x,second->position.y,second->position.z};
  const Point p{point->position.x,point->position.y,point->position.z};
  const auto direction=b-a,relative=p-a;
  const auto length2=dot(direction,direction);
  if(!(length2>0.0L))return refused;
  const auto parameter=dot(relative,direction)/length2;
  const auto residual=relative-direction*parameter;
  if(!(parameter>0.0L&&parameter<1.0L)||
     dot(residual,residual)>64.0L*LDBL_EPSILON*length2)return refused;
  std::set<std::pair<std::uint32_t,std::uint32_t>> ratios;
  for(std::uint64_t denominator=2U;denominator<=(1ULL<<30U);
      denominator*=2U) {
    const auto numerator=static_cast<std::uint64_t>(
        std::llround(parameter*static_cast<long double>(denominator)));
    if(numerator==0U||numerator>=denominator)continue;
    const auto divisor=greatest_common_divisor_64(numerator,denominator);
    ratios.emplace(static_cast<std::uint32_t>(numerator/divisor),
                   static_cast<std::uint32_t>(denominator/divisor));
  }
  std::vector<std::pair<std::uint32_t,std::uint32_t>> ordered(
      ratios.begin(),ratios.end());
  std::sort(ordered.begin(),ordered.end(),[](const auto& left,const auto& right) {
    return std::tie(left.second,left.first)<std::tie(right.second,right.first);
  });
  for(const auto& [numerator,denominator]:ordered) {
    auto promoted=promote_canonical_interior_steiner_point_to_segment(
        input,edge,vertex_id,numerator,denominator,maximum_facets);
    if(promoted.accepted())return promoted;
  }
  refused.failure=CanonicalPlcConstraintFailure::rational_overflow;
  return refused;
}

CanonicalPlcConstraintResult split_canonical_plc_constraint_facet(
    const CanonicalPlcConstraintSet& input,std::array<std::uint64_t,3> facet,
    std::array<std::uint32_t,3> barycentric,std::uint32_t denominator,
    std::size_t maximum_vertices,std::size_t maximum_facets) {
  CanonicalPlcConstraintResult result;result.constraints=input;
  if(input.vertices.size()>=maximum_vertices||input.facets.size()+2U>maximum_facets)return result;
  auto canonical=facet;std::sort(canonical.begin(),canonical.end());
  const auto found=std::find_if(input.facets.begin(),input.facets.end(),[&](const auto& candidate){auto key=candidate.vertices;std::sort(key.begin(),key.end());return key==canonical;});
  if(found==input.facets.end())return result;
  std::array<std::uint32_t,3> local_weights{};
  for(unsigned i=0U;i<3U;++i){const auto position=std::find(facet.begin(),facet.end(),found->vertices[i]);if(position==facet.end())return result;local_weights[i]=barycentric[static_cast<std::size_t>(position-facet.begin())];}
  FacetBarycentricPoint parent_point;
  if(!composed_facet_barycentric(found->corners,local_weights,denominator,parent_point)){result.constraints={};result.failure=CanonicalPlcConstraintFailure::rational_overflow;return result;}
  const auto id=canonical_facet_split_id(found->parent,parent_point);
  if(std::any_of(input.vertices.begin(),input.vertices.end(),[&](const auto& vertex){return vertex.id==id;})){result.constraints={};result.failure=CanonicalPlcConstraintFailure::generated_id_collision;return result;}
  std::map<std::uint64_t,FrozenFacetVertex> vertices;
  for(const auto& vertex:input.vertices)vertices.emplace(vertex.id,vertex);
  std::array<FrozenFacetVertex,3> parent_geometry{};
  for(unsigned i=0U;i<3U;++i){const auto vertex=vertices.find(found->parent.vertex_ids[i]);if(vertex==vertices.end()){result.constraints={};result.failure=CanonicalPlcConstraintFailure::unresolved_corner;return result;}parent_geometry[i]=vertex->second;}
  const auto position=evaluate_facet_barycentric(parent_geometry,parent_point);
  std::vector<CanonicalPlcConstraintFacet> refined;refined.reserve(input.facets.size()+2U);
  for(auto it=input.facets.begin();it!=input.facets.end();++it)if(it!=found)refined.push_back(*it);
  // splitBndTri appends [p0,p1,new], [p1,p2,new], [p2,p0,new].  Preserve
  // that source order while carrying the immutable parent barycentrics.
  for(unsigned first=0U;first<3U;++first) {
    auto child=*found;
    const auto second=(first+1U)%3U;
    child.vertices={{found->vertices[first],found->vertices[second],id}};
    child.corners={{found->corners[first],found->corners[second],parent_point}};
    refined.push_back(std::move(child));
  }
  result.constraints.vertices.push_back({id,position});
  result.constraints.facets=std::move(refined);
  result.constraints.facet_split_vertices.push_back({id,found->parent,parent_point});
  // A facet split is an exact barycentric combination. It remains in every
  // source plane that contains the complete parent facet.
  for(auto& plane:result.constraints.exact_affine_planes)
    if(std::ranges::all_of(found->vertices,[&](std::uint64_t vertex) {
         return std::binary_search(plane.vertex_ids.begin(),plane.vertex_ids.end(),
                                   vertex);
       })) {
      plane.vertex_ids.push_back(id);
      std::sort(plane.vertex_ids.begin(),plane.vertex_ids.end());
    }
  result.constraints.recovery_journal.push_back(
      {CanonicalPlcRecoveryInsertionKind::facet_split,id,{*found},input.facets});
  if(!has_only_nondegenerate_constraint_facets(result.constraints)){result.constraints={};result.failure=CanonicalPlcConstraintFailure::degenerate_facet;return result;}
  result.failure=CanonicalPlcConstraintFailure::none;
  return result;
}

namespace {
std::set<std::array<std::uint64_t,2>> constrained_parent_boundary_edges(
    const CanonicalPlcConstraintSet& constraints) {
  using StableEdge=std::array<std::uint64_t,2>;
  std::set<StableEdge> boundary;
  for(const auto& facet:constraints.facets) {
    for(const auto pair:std::array<std::array<unsigned,2>,3>{{{{0U,1U}},{{1U,2U}},{{0U,2U}}}}) {
      StableEdge edge{{facet.vertices[pair[0]],facet.vertices[pair[1]]}};
      std::sort(edge.begin(),edge.end());boundary.insert(edge);
    }
  }
  // The pinned SurEdgs/BndEdg tables contain every edge of every current
  // child surface triangle. Radial subdivision edges are temporary boundary
  // constraints too; excluding edges used twice within one parent patch
  // silently bypasses recoverFace's prerequisite-edge calls.
  return boundary;
}

// DT::buildBndInfo constructs SurEdgs by visiting every current surface
// triangle in order and, for m=0..2, inserts (form[(m+1)%3],
// form[(m+2)%3]).  BndEdg is undirected, so the first insertion fixes the
// direction subsequently consumed by splitBndEdge.  A public recovery
// request is an edge identity, not a direction: reconstruct that stored
// direction here rather than allowing caller order to affect the predicate
// sequence.
std::optional<std::array<std::uint64_t,2>> wang_build_bnd_info_direction(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> target) {
  std::sort(target.begin(),target.end());
  std::set<std::array<std::uint64_t,2>> seen;
  for(const auto& facet:constraints.facets) {
    for(unsigned m=0U;m<3U;++m) {
      std::array<std::uint64_t,2> directed{{
          facet.vertices[(m+1U)%3U],facet.vertices[(m+2U)%3U]}};
      auto identity=directed;
      std::sort(identity.begin(),identity.end());
      if(!seen.insert(identity).second)continue;
      if(identity==target)return directed;
    }
  }
  return std::nullopt;
}

// Definition 3.4 measures PLC segments, not temporary spokes introduced only
// to triangulate a parent facet after a boundary split. The recovery scheduler
// still treats every current child-triangle edge as a prerequisite above; for
// the measure, an edge is a segment exactly when it lies on a parent patch's
// boundary (one use within that parent).
std::set<std::array<std::uint64_t,2>> measured_parent_patch_boundary_edges(
    const CanonicalPlcConstraintSet& constraints) {
  using StableEdge=std::array<std::uint64_t,2>;
  std::map<FrozenFacetIdentity,std::map<StableEdge,unsigned>> uses;
  for(const auto& facet:constraints.facets)
    for(const auto pair:std::array<std::array<unsigned,2>,3>{
            {{{0U,1U}},{{1U,2U}},{{0U,2U}}}}) {
      StableEdge edge{{facet.vertices[pair[0]],facet.vertices[pair[1]]}};
      std::sort(edge.begin(),edge.end());
      ++uses[facet.parent][edge];
    }
  std::set<StableEdge> boundary;
  for(const auto& [parent,parent_uses]:uses) {
    (void)parent;
    for(const auto& [edge,count]:parent_uses)if(count==1U)boundary.insert(edge);
  }
  return boundary;
}

// A PLC parent is a geometric triangular patch.  Its historical child
// triangulation is provenance, not a set of non-flat constraints: a valid
// tetrahedralization may choose different coplanar diagonals.  Recognize an
// exact disk made from mesh faces whose vertices have exact barycentrics on
// the same parent.  Boundary chains remain literal; interior spokes do not.
std::map<FrozenFacetIdentity,bool> recovered_parent_patches(
    const CanonicalPlcConstraintSet& constraints,
    const std::set<std::array<std::uint64_t,3>>& mesh_faces,
    std::map<FrozenFacetIdentity,std::vector<std::array<std::uint64_t,3>>>* recovered_faces=nullptr,
    std::map<FrozenFacetIdentity,std::array<std::uint64_t,3>>* missing_targets=nullptr) {
  using StableEdge=std::array<std::uint64_t,2>;
  using StableFace=std::array<std::uint64_t,3>;
  // Every node below dies together when this audit returns. One sized
  // monotonic arena preserves std::map/std::set ordering while avoiding a
  // malloc/free pair for each tiny tree node. The upstream resource remains
  // an exact, unbounded fallback if an unusual PLC exceeds the estimate.
  const auto arena_bytes=std::max<std::size_t>(64U*1024U,
      constraints.facets.size()*512U+mesh_faces.size()*96U);
  std::unique_ptr<std::byte[]> arena_storage(new std::byte[arena_bytes]);
  std::pmr::monotonic_buffer_resource arena(
      arena_storage.get(),arena_bytes,std::pmr::new_delete_resource());
  struct ParentData {
    std::pmr::map<std::uint64_t,FacetBarycentricPoint> points;
    std::pmr::map<StableEdge,unsigned> child_edge_uses;
    std::pmr::vector<StableFace> literal_faces;
    bool consistent{true};
    explicit ParentData(std::pmr::memory_resource* resource)
        : points(resource),child_edge_uses(resource),literal_faces(resource) {}
  };
  std::map<FrozenFacetIdentity,ParentData> parents;
  for(const auto& facet:constraints.facets) {
    auto& parent=parents.try_emplace(facet.parent,&arena).first->second;
    auto literal=facet.vertices;std::sort(literal.begin(),literal.end());parent.literal_faces.push_back(literal);
    for(unsigned i=0U;i<3U;++i) {
      const auto [found,inserted]=parent.points.emplace(facet.vertices[i],facet.corners[i]);
      if(!inserted&&found->second!=facet.corners[i])parent.consistent=false;
    }
    for(const auto pair:std::array<std::array<unsigned,2>,3>{{{{0U,1U}},{{1U,2U}},{{0U,2U}}}}) {
      StableEdge edge{{facet.vertices[pair[0]],facet.vertices[pair[1]]}};
      std::sort(edge.begin(),edge.end());++parent.child_edge_uses[edge];
    }
  }
  std::map<std::uint64_t,std::pmr::vector<FrozenFacetIdentity>> parents_by_point;
  for(const auto& [identity,parent]:parents)
    for(const auto& [point,barycentric]:parent.points) {
      (void)barycentric;
      parents_by_point.try_emplace(point,&arena).first->second.push_back(identity);
    }
  std::map<FrozenFacetIdentity,std::pmr::vector<StableFace>> triangles_by_parent;
  for(const auto& face:mesh_faces) {
    const auto memberships=parents_by_point.find(face[0]);
    if(memberships==parents_by_point.end())continue;
    for(const auto& identity:memberships->second) {
      const auto& points=parents.at(identity).points;
      if(points.contains(face[1])&&points.contains(face[2]))
        triangles_by_parent.try_emplace(identity,&arena).first->second.push_back(face);
    }
  }
  std::map<FrozenFacetIdentity,bool> recovered;
  for(const auto& [identity,parent]:parents) {
    const auto record_missing=[&] {
      if(!missing_targets)return;
      const auto missing=std::find_if(parent.literal_faces.begin(),parent.literal_faces.end(),
          [&](const auto& face){return !mesh_faces.contains(face);});
      (*missing_targets)[identity]=missing==parent.literal_faces.end()?
          identity.vertex_ids:*missing;
    };
    if(!parent.consistent){recovered.emplace(identity,false);record_missing();continue;}
    std::pmr::set<StableEdge> expected_boundary{&arena};
    for(const auto& [edge,count]:parent.child_edge_uses)if(count==1U)expected_boundary.insert(edge);
    std::pmr::vector<StableFace> triangles{&arena};
    const auto triangle_record=triangles_by_parent.find(identity);
    if(triangle_record!=triangles_by_parent.end())triangles=triangle_record->second;
    if(triangles.empty()){recovered.emplace(identity,false);record_missing();continue;}
    std::pmr::map<StableEdge,unsigned> uses{&arena};
    std::pmr::set<std::uint64_t> used_vertices{&arena};
    long double doubled_area{};bool positive=true;
    for(const auto& face:triangles) {
      std::array<std::array<long double,2>,3> p{};
      for(unsigned i=0U;i<3U;++i) {
        const auto& bary=parent.points.at(face[i]);
        if(bary.denominator==0U){positive=false;break;}
        p[i]={{static_cast<long double>(bary.numerator[1])/bary.denominator,
               static_cast<long double>(bary.numerator[2])/bary.denominator}};
        used_vertices.insert(face[i]);
      }
      if(!positive)break;
      const auto area=(p[1][0]-p[0][0])*(p[2][1]-p[0][1])-
                      (p[1][1]-p[0][1])*(p[2][0]-p[0][0]);
      if(area==0.0L){positive=false;break;}doubled_area+=std::abs(area);
      for(const auto pair:std::array<std::array<unsigned,2>,3>{{{{0U,1U}},{{1U,2U}},{{0U,2U}}}}) {
        StableEdge edge{{face[pair[0]],face[pair[1]]}};std::sort(edge.begin(),edge.end());++uses[edge];
      }
    }
    if(!positive){recovered.emplace(identity,false);record_missing();continue;}
    std::pmr::set<StableEdge> actual_boundary{&arena};bool manifold=true;
    for(const auto& [edge,count]:uses) {
      if(count==1U)actual_boundary.insert(edge);
      else if(count!=2U){manifold=false;break;}
    }
    // For a connected planar simplicial disk V-E+F=1.  Together with the
    // exact boundary chain and unit parent area this rejects holes, doubled
    // sheets, disconnected islands and incomplete coverage.
    std::map<std::uint64_t,std::pmr::set<std::uint64_t>> adjacency;
    for(const auto& [edge,count]:uses) {
      (void)count;
      adjacency.try_emplace(edge[0],&arena).first->second.insert(edge[1]);
      adjacency.try_emplace(edge[1],&arena).first->second.insert(edge[0]);
    }
    std::pmr::set<std::uint64_t> visited{&arena};
    if(!used_vertices.empty()) {
      std::pmr::vector<std::uint64_t> stack{&arena};
      stack.push_back(*used_vertices.begin());
      while(!stack.empty()){const auto vertex=stack.back();stack.pop_back();if(!visited.insert(vertex).second)continue;for(const auto next:adjacency[vertex])stack.push_back(next);}
    }
    const auto euler=static_cast<std::int64_t>(used_vertices.size())-
                     static_cast<std::int64_t>(uses.size())+
                     static_cast<std::int64_t>(triangles.size());
    const auto area_error=std::abs(doubled_area-1.0L);
    const bool accepted=manifold&&actual_boundary==expected_boundary&&
        visited.size()==used_vertices.size()&&euler==1&&area_error<=256.0L*LDBL_EPSILON;
    recovered.emplace(identity,accepted);
    if(accepted&&recovered_faces)
      (*recovered_faces)[identity].assign(triangles.begin(),triangles.end());
    if(!accepted)record_missing();
  }
  return recovered;
}
} // namespace

CanonicalPlcSeedInspection inspect_canonical_plc_constraints(const CanonicalPlcConstraintSet& constraints,std::size_t maximum_tetrahedra){
  CanonicalPlcSeedInspection report;CanonicalDelaunaySeedInput input;input.maximum_vertices=constraints.vertices.size();input.maximum_tetrahedra=maximum_tetrahedra;input.exact_affine_planes=constraints.exact_affine_planes;
  for(const auto& vertex:constraints.vertices){input.vertices.push_back(vertex.position);input.stable_vertex_ids.push_back(vertex.id);}
  const auto seed=build_canonical_background_seed(input);report.seed_failure=seed.failure;report.seed_invalid_reason=seed.invalid_reason;report.invalid_hull_face=seed.invalid_hull_face;report.invalid_hull_witness=seed.invalid_hull_witness;report.stellar_fallback_used=seed.stellar_fallback;report.diagnostic_hull_faces=seed.diagnostic_hull_faces;report.diagnostic_cone_cells=seed.diagnostic_cone_cells;report.candidate_tetrahedra=seed.tetrahedra.size();if(!seed.accepted()){report.failure=CanonicalPlcSeedFailure::delaunay_seed_failed;return report;}
  std::set<std::array<std::uint64_t,3>> seed_faces;std::set<std::array<std::uint64_t,2>> seed_edges,required_edges;
  for(const auto& tet:seed.tetrahedra)for(unsigned omit=0;omit<4U;++omit){std::array<std::uint64_t,3> face{};unsigned n=0;for(unsigned i=0;i<4U;++i)if(i!=omit)face[n++]=input.stable_vertex_ids[tet[i]];std::sort(face.begin(),face.end());seed_faces.insert(face);}
  for(const auto& face:seed_faces)for(const auto edge:std::array<std::array<unsigned,2>,3>{{{{0,1}},{{1,2}},{{0,2}}}})seed_edges.insert({face[edge[0]],face[edge[1]]});
  required_edges=constrained_parent_boundary_edges(constraints);
  std::map<FrozenFacetIdentity,std::array<std::uint64_t,3>> missing_targets;
  const auto parent_patches=recovered_parent_patches(constraints,seed_faces,nullptr,&missing_targets);
  report.required_facets=parent_patches.size();report.recovered_facets=static_cast<std::size_t>(std::count_if(parent_patches.begin(),parent_patches.end(),[](const auto& item){return item.second;}));
  report.required_edges=required_edges.size();report.recovered_edges=static_cast<std::size_t>(std::count_if(required_edges.begin(),required_edges.end(),[&](const auto& edge){return seed_edges.contains(edge);}));
  for(const auto& [parent,recovered]:parent_patches)if(!recovered)report.missing_facets.push_back(missing_targets.at(parent));for(const auto& edge:required_edges)if(!seed_edges.contains(edge))report.missing_edges.push_back(edge);
  report.failure=report.recovered_facets==report.required_facets?CanonicalPlcSeedFailure::none:CanonicalPlcSeedFailure::unrecovered_constraint;return report;
}

CanonicalPlcSeedInspection inspect_canonical_plc_tetrahedra(const CanonicalPlcConstraintSet& constraints,const std::vector<std::array<std::uint32_t,4>>& tetrahedra){
  CanonicalPlcSeedInspection report;report.candidate_tetrahedra=tetrahedra.size();std::set<std::array<std::uint64_t,3>> seed_faces;std::set<std::array<std::uint64_t,2>> seed_edges,required_edges;
  for(const auto& tet:tetrahedra){for(const auto vertex:tet)if(vertex>=constraints.vertices.size()){report.seed_failure=CanonicalDelaunaySeedFailure::invalid_output;report.failure=CanonicalPlcSeedFailure::delaunay_seed_failed;return report;}for(unsigned omit=0;omit<4U;++omit){std::array<std::uint64_t,3> face{};unsigned n=0;for(unsigned i=0;i<4U;++i)if(i!=omit)face[n++]=constraints.vertices[tet[i]].id;std::sort(face.begin(),face.end());seed_faces.insert(face);}}
  for(const auto& face:seed_faces)for(const auto edge:std::array<std::array<unsigned,2>,3>{{{{0,1}},{{1,2}},{{0,2}}}})seed_edges.insert({face[edge[0]],face[edge[1]]});required_edges=constrained_parent_boundary_edges(constraints);
  std::map<FrozenFacetIdentity,std::array<std::uint64_t,3>> missing_targets;
  const auto parent_patches=recovered_parent_patches(constraints,seed_faces,nullptr,&missing_targets);
  report.required_facets=parent_patches.size();report.recovered_facets=static_cast<std::size_t>(std::count_if(parent_patches.begin(),parent_patches.end(),[](const auto& item){return item.second;}));report.required_edges=required_edges.size();report.recovered_edges=static_cast<std::size_t>(std::count_if(required_edges.begin(),required_edges.end(),[&](const auto& edge){return seed_edges.contains(edge);}));for(const auto& [parent,recovered]:parent_patches)if(!recovered)report.missing_facets.push_back(missing_targets.at(parent));for(const auto& edge:required_edges)if(!seed_edges.contains(edge))report.missing_edges.push_back(edge);report.failure=report.recovered_facets==report.required_facets?CanonicalPlcSeedFailure::none:CanonicalPlcSeedFailure::unrecovered_constraint;return report;
}

CanonicalLiteralEdgeFlipResult try_recover_literal_edge_by_flip(const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,2> edge,std::size_t maximum_tetrahedra){
  CanonicalLiteralEdgeFlipResult result;if(edge[1]<edge[0])std::swap(edge[0],edge[1]);CanonicalDelaunaySeedInput input;input.maximum_vertices=constraints.vertices.size();input.maximum_tetrahedra=maximum_tetrahedra;input.exact_affine_planes=constraints.exact_affine_planes;
  std::map<std::uint64_t,std::uint32_t> index;std::vector<Point> points;for(std::size_t i=0;i<constraints.vertices.size();++i){input.vertices.push_back(constraints.vertices[i].position);input.stable_vertex_ids.push_back(constraints.vertices[i].id);index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));const auto p=constraints.vertices[i].position;points.push_back({p.x,p.y,p.z});}
  const auto a=index.find(edge[0]),b=index.find(edge[1]);if(a==index.end()||b==index.end())return result;const auto seed=build_canonical_background_seed(input);if(!seed.accepted())return result;
  std::map<Face,std::vector<std::size_t>> ledger;for(std::size_t ti=0;ti<seed.tetrahedra.size();++ti)for(unsigned omit=0;omit<4U;++omit){Face f{};unsigned n=0;for(unsigned i=0;i<4U;++i)if(i!=omit)f[n++]=seed.tetrahedra[ti][i];ledger[face_key(f)].push_back(ti);}
  std::set<std::array<std::uint64_t,3>> constrained;for(const auto& face:constraints.facets){auto f=face.vertices;std::sort(f.begin(),f.end());constrained.insert(f);}
  for(const auto& [face,uses]:ledger){if(uses.size()!=2U)continue;std::array<std::uint64_t,3> ids{{input.stable_vertex_ids[face[0]],input.stable_vertex_ids[face[1]],input.stable_vertex_ids[face[2]]}};std::sort(ids.begin(),ids.end());if(constrained.contains(ids))continue;
    const auto opposite=[&](const auto& cell){for(const auto v:cell)if(v!=face[0]&&v!=face[1]&&v!=face[2])return v;return face[0];};const auto left=opposite(seed.tetrahedra[uses[0]]),right=opposite(seed.tetrahedra[uses[1]]);if(!((left==a->second&&right==b->second)||(left==b->second&&right==a->second)))continue;
    std::array<Tet,3> replacement{{{{left,right,face[0],face[1]}},{{left,right,face[1],face[2]}},{{left,right,face[2],face[0]}}}};bool positive=true;for(auto& tet:replacement){const auto volume=orient(points[tet[0]],points[tet[1]],points[tet[2]],points[tet[3]]);if(std::abs(volume)<1e-20L){positive=false;break;}if(volume<0)std::swap(tet[0],tet[1]);}if(!positive)continue;
    for(std::size_t i=0;i<seed.tetrahedra.size();++i)if(i!=uses[0]&&i!=uses[1])result.tetrahedra.push_back(seed.tetrahedra[i]);result.tetrahedra.insert(result.tetrahedra.end(),replacement.begin(),replacement.end());result.accepted=true;return result;
  }
  return result;
}

CanonicalLiteralSegmentCavity locate_literal_segment_cavity(const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,2> edge,std::size_t maximum_tetrahedra){
  CanonicalLiteralSegmentCavity result;CanonicalDelaunaySeedInput input;input.maximum_vertices=constraints.vertices.size();input.maximum_tetrahedra=maximum_tetrahedra;input.exact_affine_planes=constraints.exact_affine_planes;std::map<std::uint64_t,std::uint32_t> index;std::vector<Point> points;
  for(std::size_t i=0;i<constraints.vertices.size();++i){input.vertices.push_back(constraints.vertices[i].position);input.stable_vertex_ids.push_back(constraints.vertices[i].id);index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));const auto p=constraints.vertices[i].position;points.push_back({p.x,p.y,p.z});}
  const auto a=index.find(edge[0]),b=index.find(edge[1]);if(a==index.end()||b==index.end())return result;const auto seed=build_canonical_background_seed(input);if(!seed.accepted())return result;const auto start=points[a->second],end=points[b->second];
  std::vector<std::size_t> selected;for(std::size_t ti=0;ti<seed.tetrahedra.size();++ti)if(segment_intersects_tetrahedron_interior(points,seed.tetrahedra[ti],start,end))selected.push_back(ti);
  if(selected.empty())return result;std::map<Face,std::pair<Face,unsigned>> boundary;
  for(const auto ti:selected)for(unsigned omit=0;omit<4U;++omit){Face f{};unsigned n=0;for(unsigned i=0;i<4U;++i)if(i!=omit)f[n++]=seed.tetrahedra[ti][i];auto& entry=boundary[face_key(f)];entry.first=f;++entry.second;}
  std::set<std::array<std::uint64_t,3>> frozen;for(const auto& facet:constraints.facets){auto f=facet.vertices;std::sort(f.begin(),f.end());frozen.insert(f);}
  for(const auto ti:selected)result.cells.push_back(seed.tetrahedra[ti]);for(const auto& [key,value]:boundary)if(value.second==1U){result.boundary_faces.push_back(value.first);std::array<std::uint64_t,3> ids{{input.stable_vertex_ids[key[0]],input.stable_vertex_ids[key[1]],input.stable_vertex_ids[key[2]]}};if(frozen.contains(ids))result.touches_frozen_facet=true;}
  result.found=true;return result;
}

namespace {
CanonicalLiteralEdgeFlipResult four_to_four_in_mesh(const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,2> edge,const std::vector<Tet>& mesh){
  CanonicalLiteralEdgeFlipResult result;std::map<std::uint64_t,std::uint32_t> index;std::vector<Point> points;
  for(std::size_t i=0;i<constraints.vertices.size();++i){index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));const auto p=constraints.vertices[i].position;points.push_back({p.x,p.y,p.z});}
  const auto left=index.find(edge[0]),right=index.find(edge[1]);if(left==index.end()||right==index.end())return result;
  const auto start=points[left->second],end=points[right->second];std::vector<std::size_t> selected;
  for(std::size_t ti=0;ti<mesh.size();++ti)if(segment_intersects_tetrahedron_interior(points,mesh[ti],start,end))selected.push_back(ti);
  result.intersected_tetrahedra=selected.size();for(const auto ti:selected)result.cavity_tetrahedra.push_back(mesh[ti]);if(selected.size()!=4U){result.failure=CanonicalLiteralEdgeFlipFailure::non_four_cell_cavity;return result;}std::map<Face,std::pair<Face,unsigned>> boundary;for(const auto ti:selected)for(unsigned omit=0;omit<4U;++omit){Face f{};unsigned n=0;for(unsigned i=0;i<4U;++i)if(i!=omit)f[n++]=mesh[ti][i];auto& item=boundary[face_key(f)];item.first=f;++item.second;}
  std::set<std::array<std::uint64_t,3>> frozen;for(const auto& facet:constraints.facets){auto face=facet.vertices;std::sort(face.begin(),face.end());frozen.insert(face);}for(const auto& [key,item]:boundary)if(item.second==1U){std::array<std::uint64_t,3> ids{{constraints.vertices[key[0]].id,constraints.vertices[key[1]].id,constraints.vertices[key[2]].id}};if(frozen.contains(ids))result.touches_frozen_facet=true;}
  std::set<std::uint32_t> common(mesh[selected.front()].begin(),mesh[selected.front()].end()),all;for(const auto ti:selected){std::set<std::uint32_t> next;for(const auto v:mesh[ti])if(common.contains(v))next.insert(v);common=std::move(next);all.insert(mesh[ti].begin(),mesh[ti].end());}if(common.size()!=2U||!all.contains(left->second)||!all.contains(right->second)){result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;return result;}
  std::vector<std::uint32_t> axis(common.begin(),common.end()),other;for(const auto v:all)if(v!=left->second&&v!=right->second&&!common.contains(v))other.push_back(v);if(other.size()!=2U){result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;return result;}
  std::array<Tet,4> replacement{{{{left->second,right->second,axis[0],other[0]}},{{left->second,right->second,other[0],axis[1]}},{{left->second,right->second,axis[1],other[1]}},{{left->second,right->second,other[1],axis[0]}}}};for(auto& cell:replacement){const auto volume=orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]);if(std::abs(volume)<1e-20L){result.failure=CanonicalLiteralEdgeFlipFailure::nonpositive_replacement;return result;}if(volume<0)std::swap(cell[0],cell[1]);}
  const auto boundary_of=[](const auto& cells){std::map<Face,unsigned> uses;for(const auto& cell:cells)for(unsigned omit=0;omit<4U;++omit){Face f{};unsigned n=0;for(unsigned i=0;i<4U;++i)if(i!=omit)f[n++]=cell[i];++uses[face_key(f)];}std::set<Face> out;for(const auto& [face,count]:uses)if(count==1U)out.insert(face);return out;};std::vector<Tet> cavity;for(const auto ti:selected)cavity.push_back(mesh[ti]);if(boundary_of(cavity)!=boundary_of(replacement)){result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;return result;}
  std::set<std::size_t> removed(selected.begin(),selected.end());for(std::size_t i=0;i<mesh.size();++i)if(!removed.contains(i))result.tetrahedra.push_back(mesh[i]);result.tetrahedra.insert(result.tetrahedra.end(),replacement.begin(),replacement.end());result.accepted=true;result.failure=CanonicalLiteralEdgeFlipFailure::none;return result;
}

bool constraint_mesh_is_valid(const CanonicalPlcConstraintSet& constraints,
                              const std::vector<Tet>& mesh) {
  std::vector<Point> points;points.reserve(constraints.vertices.size());
  for(const auto& vertex:constraints.vertices)
    points.push_back({vertex.position.x,vertex.position.y,vertex.position.z});
  std::set<Tet> unique;
  std::map<Face,std::vector<std::uint32_t>> opposites;
  for(const auto& cell:mesh) {
    for(const auto vertex:cell)if(vertex>=points.size())return false;
    auto key=cell;std::sort(key.begin(),key.end());
    std::array<Vec3,4> positions{};
    std::array<std::uint64_t,4> stable{};
    for(unsigned corner=0U;corner<4U;++corner) {
      positions[corner]=constraints.vertices[cell[corner]].position;
      stable[corner]=constraints.vertices[cell[corner]].id;
    }
    const auto plane_orientation=evaluate_plane_aware_orientation(
        positions,stable,constraints.exact_affine_planes);
    if(!unique.insert(key).second||plane_orientation.geometric_sign==0||
       plane_orientation.semantically_coplanar)return false;
    for(unsigned omitted=0U;omitted<4U;++omitted){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[n++]=cell[i];opposites[face_key(face)].push_back(cell[omitted]);}
  }
  for(const auto& [face,uses]:opposites) {
    if(uses.size()>2U||uses.empty())return false;
    if(uses.size()==2U) {
      const auto first=robust_orient(points[face[0]],points[face[1]],points[face[2]],points[uses[0]]);
      const auto second=robust_orient(points[face[0]],points[face[1]],points[face[2]],points[uses[1]]);
      if(first==0||second==0||first==second)return false;
    }
  }
  return true;
}

// Publication repair is specifically entered with zero-volume or declared-
// plane cells, so its intake gate must distinguish those repair targets from
// unrelated topology corruption.  Shared faces between two ordinary cells
// still require opposite orientations; a zero orientation is tolerated only
// long enough for the bounded repair to remove its incident bad cell.
bool constraint_mesh_is_repairable(const CanonicalPlcConstraintSet& constraints,
                                   const std::vector<Tet>& mesh) {
  std::vector<Point> points;points.reserve(constraints.vertices.size());
  for(const auto& vertex:constraints.vertices)
    points.push_back({vertex.position.x,vertex.position.y,vertex.position.z});
  std::set<Tet> unique;
  std::map<Face,std::vector<std::uint32_t>> opposites;
  for(const auto& cell:mesh) {
    for(const auto vertex:cell)if(vertex>=points.size())return false;
    auto key=cell;std::sort(key.begin(),key.end());
    if(!unique.insert(key).second)return false;
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned corner=0U;corner<4U;++corner)
        if(corner!=omitted)face[cursor++]=cell[corner];
      opposites[face_key(face)].push_back(cell[omitted]);
    }
  }
  for(const auto& [face,uses]:opposites) {
    if(uses.empty()||uses.size()>2U)return false;
    if(uses.size()!=2U)continue;
    const auto first=robust_orient(points[face[0]],points[face[1]],
                                   points[face[2]],points[uses[0]]);
    const auto second=robust_orient(points[face[0]],points[face[1]],
                                    points[face[2]],points[uses[1]]);
    if(first!=0&&second!=0&&first==second)return false;
  }
  return true;
}

// Validate only the geometry touched by a local transaction.  A complete
// pairwise overlap audit is quadratic in the several-thousand-cell private
// seed and is needlessly repeated for every recovery attempt.  Any overlap
// newly introduced by a transaction must involve at least one added cell, so
// comparing those cells with the retained mesh is both sufficient and cheap.
bool constraint_mesh_mutation_is_valid(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<Tet>& original,const std::vector<Tet>& candidate) {
  const auto canonical=[](Tet cell){std::sort(cell.begin(),cell.end());return cell;};
  std::map<Tet,Tet> original_cells;
  for(const auto& cell:original)original_cells.emplace(canonical(cell),cell);
  std::set<Tet> candidate_cells;
  std::vector<std::size_t> added;
  for(std::size_t i=0U;i<candidate.size();++i) {
    for(const auto vertex:candidate[i])
      if(vertex>=constraints.vertices.size())return false;
    const auto key=canonical(candidate[i]);
    if(!candidate_cells.insert(key).second)return false;
    if(!original_cells.contains(key))added.push_back(i);
  }
  // The caller supplies a valid original mesh.  Rechecking every unchanged
  // cell with exact predicates for every local trial dominated the N5 Wang
  // transaction.  Only added cells can introduce a new zero-volume or
  // semantically coplanar tetrahedron.
  for(const auto index:added) {
    std::array<Vec3,4> positions{};
    std::array<std::uint64_t,4> stable{};
    for(unsigned corner=0U;corner<4U;++corner) {
      positions[corner]=constraints.vertices[candidate[index][corner]].position;
      stable[corner]=constraints.vertices[candidate[index][corner]].id;
    }
    const auto orientation=evaluate_plane_aware_orientation(
        positions,stable,constraints.exact_affine_planes);
    if(orientation.geometric_sign==0||orientation.semantically_coplanar)
      return false;
  }
  std::set<Face> touched_faces;
  const auto touch=[&](const Tet& cell) {
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned corner=0U;corner<4U;++corner)
        if(corner!=omitted)face[cursor++]=cell[corner];
      touched_faces.insert(face_key(face));
    }
  };
  for(const auto index:added)touch(candidate[index]);
  std::vector<Point> robust_points;robust_points.reserve(constraints.vertices.size());
  for(const auto& vertex:constraints.vertices)
    robust_points.push_back({vertex.position.x,vertex.position.y,vertex.position.z});
  std::map<Face,std::vector<std::uint32_t>> opposites;
  for(const auto& cell:candidate)
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned corner=0U;corner<4U;++corner)
        if(corner!=omitted)face[cursor++]=cell[corner];
      const auto key=face_key(face);
      if(touched_faces.contains(key))opposites[key].push_back(cell[omitted]);
    }
  for(const auto& face:touched_faces) {
    const auto found=opposites.find(face);
    const std::vector<std::uint32_t> empty;
    const auto& uses=found==opposites.end()?empty:found->second;
    if(uses.empty()||uses.size()>2U)return false;
    if(uses.size()==2U) {
      const auto first=robust_orient(robust_points[face[0]],robust_points[face[1]],
                                     robust_points[face[2]],robust_points[uses[0]]);
      const auto second=robust_orient(robust_points[face[0]],robust_points[face[1]],
                                      robust_points[face[2]],robust_points[uses[1]]);
      if(first==0||second==0||first==second)return false;
    }
  }
  const auto points=[&](const Tet& cell) {
    return std::array<Vec3,4>{{constraints.vertices[cell[0]].position,
                              constraints.vertices[cell[1]].position,
                              constraints.vertices[cell[2]].position,
                              constraints.vertices[cell[3]].position}};
  };
  struct Bounds {Vec3 minimum;Vec3 maximum;};
  std::vector<Bounds> bounds;bounds.reserve(candidate.size());
  for(const auto& cell:candidate) {
    const auto p=points(cell);Bounds box{p[0],p[0]};
    for(std::size_t i=1U;i<p.size();++i) {
      box.minimum.x=std::min(box.minimum.x,p[i].x);
      box.minimum.y=std::min(box.minimum.y,p[i].y);
      box.minimum.z=std::min(box.minimum.z,p[i].z);
      box.maximum.x=std::max(box.maximum.x,p[i].x);
      box.maximum.y=std::max(box.maximum.y,p[i].y);
      box.maximum.z=std::max(box.maximum.z,p[i].z);
    }
    bounds.push_back(box);
  }
  const auto may_overlap=[](const Bounds& left,const Bounds& right) {
    constexpr double tolerance=1.0e-12;
    return left.maximum.x>right.minimum.x+tolerance&&
        right.maximum.x>left.minimum.x+tolerance&&
        left.maximum.y>right.minimum.y+tolerance&&
        right.maximum.y>left.minimum.y+tolerance&&
        left.maximum.z>right.minimum.z+tolerance&&
        right.maximum.z>left.minimum.z+tolerance;
  };
  std::set<std::size_t> added_set(added.begin(),added.end());
  for(std::size_t ai=0U;ai<added.size();++ai) {
    const auto left_index=added[ai];const auto left=points(candidate[left_index]);
    for(std::size_t right_index=0U;right_index<candidate.size();++right_index) {
      if(right_index==left_index)continue;
      if(added_set.contains(right_index)&&right_index<left_index)continue;
      if(!may_overlap(bounds[left_index],bounds[right_index]))continue;
      if(strict_tetrahedra_overlap(left,points(candidate[right_index])))return false;
    }
  }
  return true;
}

CanonicalLiteralEdgeFlipResult remove_mesh_edge_in_mesh(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint32_t,2> edge,const std::vector<Tet>& mesh,
    std::vector<CanonicalLiteralEdgeFlipResult>* all_valid=nullptr,
    bool exhaustive=false,bool validate_entire_mesh=true) {
  CanonicalLiteralEdgeFlipResult result;
  result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;
  std::sort(edge.begin(),edge.end());
  if(edge[0]>=constraints.vertices.size()||edge[1]>=constraints.vertices.size()||
     edge[0]==edge[1])return result;

  const std::array<std::uint64_t,2> stable_edge{{
      std::min(constraints.vertices[edge[0]].id,constraints.vertices[edge[1]].id),
      std::max(constraints.vertices[edge[0]].id,constraints.vertices[edge[1]].id)}};
  for(const auto& facet:constraints.facets)
    for(const auto pair:std::array<std::array<unsigned,2>,3>{{
        {{0U,1U}},{{1U,2U}},{{0U,2U}}}}) {
      std::array<std::uint64_t,2> protected_edge{{
          facet.vertices[pair[0]],facet.vertices[pair[1]]}};
      std::sort(protected_edge.begin(),protected_edge.end());
      if(protected_edge==stable_edge) {
        result.failure=CanonicalLiteralEdgeFlipFailure::frozen_cavity_boundary;
        result.touches_frozen_facet=true;
        return result;
      }
    }

  std::vector<std::size_t> shell;
  std::map<std::uint32_t,std::set<std::uint32_t>> ring_adjacency;
  for(std::size_t i=0U;i<mesh.size();++i) {
    if(std::find(mesh[i].begin(),mesh[i].end(),edge[0])==mesh[i].end()||
       std::find(mesh[i].begin(),mesh[i].end(),edge[1])==mesh[i].end())continue;
    shell.push_back(i);
    std::array<std::uint32_t,2> pair{};unsigned count{};
    for(const auto vertex:mesh[i])if(vertex!=edge[0]&&vertex!=edge[1])
      pair[count++]=vertex;
    if(count!=2U)return result;
    ring_adjacency[pair[0]].insert(pair[1]);
    ring_adjacency[pair[1]].insert(pair[0]);
  }
  result.intersected_tetrahedra=shell.size();
  for(const auto cell:shell)result.cavity_tetrahedra.push_back(mesh[cell]);
  if(shell.size()<3U||shell.size()>10U||ring_adjacency.size()!=shell.size()||
     std::any_of(ring_adjacency.begin(),ring_adjacency.end(),
                 [](const auto& item){return item.second.size()!=2U;}))return result;

  std::vector<std::uint32_t> ring;
  ring.reserve(ring_adjacency.size());
  const auto start=ring_adjacency.begin()->first;
  auto previous=std::numeric_limits<std::uint32_t>::max();
  auto current=start;
  for(std::size_t step=0U;step<ring_adjacency.size();++step) {
    ring.push_back(current);
    const auto& neighbours=ring_adjacency.at(current);
    auto next=*neighbours.begin();
    if(next==previous)next=*std::next(neighbours.begin());
    previous=current;current=next;
  }
  if(current!=start||std::set<std::uint32_t>(ring.begin(),ring.end()).size()!=ring.size())
    return result;

  using Triangle=std::array<std::size_t,3>;
  using Triangulation=std::vector<Triangle>;
  std::function<std::vector<Triangulation>(std::size_t,std::size_t)> triangulate=
      [&](const std::size_t first,const std::size_t last) {
        if(last<=first+1U)return std::vector<Triangulation>{{}};
        std::vector<Triangulation> generated;
        for(std::size_t middle=first+1U;middle<last;++middle) {
          const auto left=triangulate(first,middle);
          const auto right=triangulate(middle,last);
          for(const auto& a:left)for(const auto& b:right) {
            Triangulation candidate=a;
            candidate.insert(candidate.end(),b.begin(),b.end());
            candidate.push_back({first,middle,last});
            generated.push_back(std::move(candidate));
          }
        }
        return generated;
      };
  const auto triangulations=triangulate(0U,ring.size()-1U);
  std::vector<Point> points;
  points.reserve(constraints.vertices.size());
  for(const auto& vertex:constraints.vertices) {
    const auto p=vertex.position;points.push_back({p.x,p.y,p.z});
  }
  const auto boundary_of=[](const auto& cells) {
    std::map<Face,unsigned> counts;
    for(const auto& cell:cells)for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];
      ++counts[face_key(face)];
    }
    std::set<Face> boundary;
    for(const auto& [face,count]:counts) {
      if(count==1U)boundary.insert(face);
      else if(count!=2U)return std::set<Face>{};
    }
    return boundary;
  };
  const auto expected_boundary=boundary_of(result.cavity_tetrahedra);
  long double old_volume{};
  for(const auto& cell:result.cavity_tetrahedra)
    old_volume+=std::abs(orient(points[cell[0]],points[cell[1]],
                               points[cell[2]],points[cell[3]]));
  const std::set<std::size_t> removed(shell.begin(),shell.end());
  std::size_t trial{};
  for(const auto& triangles:triangulations) {
    // Generalized edge removal is a local search, not an unbounded Catalan
    // sweep. The reference implementation likewise limits flip depth before
    // moving to FHC insertion. Keep enough alternatives for degree-10 rings
    // while preventing repeated full-mesh validation from dominating recovery.
    constexpr std::size_t maximum_trials=64U;
    if(!exhaustive&&trial==maximum_trials)break;
    ++trial;
    std::vector<Tet> replacement;
    replacement.reserve(triangles.size()*2U);
    bool positive=true;
    for(const auto triangle:triangles) {
      Tet first{{edge[0],ring[triangle[0]],ring[triangle[1]],ring[triangle[2]]}};
      Tet second{{edge[1],ring[triangle[0]],ring[triangle[2]],ring[triangle[1]]}};
      for(auto* cell:std::array<Tet*,2>{{&first,&second}}) {
        const auto orientation=robust_orient(points,*cell);
        if(orientation==0){positive=false;break;}
        if(orientation<0)std::swap((*cell)[0],(*cell)[1]);
      }
      if(!positive)break;
      replacement.push_back(first);replacement.push_back(second);
    }
    if(!positive||boundary_of(replacement)!=expected_boundary)continue;
    long double new_volume{};
    for(const auto& cell:replacement)
      new_volume+=std::abs(orient(points[cell[0]],points[cell[1]],
                                 points[cell[2]],points[cell[3]]));
    if(std::abs(old_volume-new_volume)>std::max(1.0L,old_volume)*1e-14L)continue;
    std::vector<Tet> candidate;
    for(std::size_t i=0U;i<mesh.size();++i)
      if(!removed.contains(i))candidate.push_back(mesh[i]);
    candidate.insert(candidate.end(),replacement.begin(),replacement.end());
    if(validate_entire_mesh&&
       !constraint_mesh_mutation_is_valid(constraints,mesh,candidate))continue;
    auto accepted=result;
    accepted.tetrahedra=std::move(candidate);
    accepted.retriangulation_trials=trial;
    accepted.accepted=true;
    accepted.failure=CanonicalLiteralEdgeFlipFailure::none;
    if(all_valid)all_valid->push_back(accepted);
    if(!exhaustive)return accepted;
  }
  if(all_valid&&!all_valid->empty())return all_valid->front();
  result.retriangulation_trials=trial;
  result.failure=CanonicalLiteralEdgeFlipFailure::nonpositive_replacement;
  return result;
}

struct ConstraintMeshIntersection {
  std::array<std::uint64_t,2> constraint_edge{};
  Face mesh_face{};
  std::uint32_t numerator{};
  std::uint32_t denominator{};
};
using PoststallIntersectionKey=std::array<std::uint64_t,8>;

std::optional<ConstraintMeshIntersection> find_constraint_edge_mesh_face_intersection(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint64_t,2>>& missing_edges,
    const std::vector<Tet>& mesh,
    const std::set<PoststallIntersectionKey>& excluded);

CanonicalLiteralEdgeFlipResult face_flip_for_specific_face(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<Tet>& mesh,Face face,bool validate_entire_mesh=true) {
  CanonicalLiteralEdgeFlipResult result;
  result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;
  face=face_key(face);
  std::vector<Point> points;
  points.reserve(constraints.vertices.size());
  for(const auto& vertex:constraints.vertices)
    points.push_back({vertex.position.x,vertex.position.y,vertex.position.z});

  std::vector<std::size_t> uses;
  for(std::size_t cell=0U;cell<mesh.size();++cell)
    if(std::all_of(face.begin(),face.end(),[&](const auto vertex) {
      return std::find(mesh[cell].begin(),mesh[cell].end(),vertex)!=mesh[cell].end();
    }))uses.push_back(cell);
  if(uses.size()!=2U)return result;
  result.cavity_tetrahedra={mesh[uses[0]],mesh[uses[1]]};
  result.intersected_tetrahedra=2U;

  std::array<std::uint64_t,3> stable_face{{
      constraints.vertices[face[0]].id,constraints.vertices[face[1]].id,
      constraints.vertices[face[2]].id}};
  std::sort(stable_face.begin(),stable_face.end());
  for(const auto& facet:constraints.facets) {
    auto frozen=facet.vertices;std::sort(frozen.begin(),frozen.end());
    if(frozen==stable_face) {
      result.failure=CanonicalLiteralEdgeFlipFailure::frozen_cavity_boundary;
      result.touches_frozen_facet=true;
      return result;
    }
  }

  const auto opposite=[&](const Tet& cell) {
    return *std::find_if(cell.begin(),cell.end(),[&](const auto vertex) {
      return std::find(face.begin(),face.end(),vertex)==face.end();
    });
  };
  const auto left=opposite(mesh[uses[0]]);
  const auto right=opposite(mesh[uses[1]]);
  if(left==right)return result;
  std::array<Tet,3> replacement{{
      {{left,right,face[0],face[1]}},
      {{left,right,face[1],face[2]}},
      {{left,right,face[2],face[0]}}}};
  std::array<int,3> orientations{};
  for(std::size_t i=0U;i<replacement.size();++i)
    orientations[i]=sign(exact_orientation_3d(
        as_vec3(points[replacement[i][0]]),
        as_vec3(points[replacement[i][1]]),
        as_vec3(points[replacement[i][2]]),
        as_vec3(points[replacement[i][3]])));
  const auto positive=static_cast<unsigned>(std::count(
      orientations.begin(),orientations.end(),1));
  const auto negative=static_cast<unsigned>(std::count(
      orientations.begin(),orientations.end(),-1));
  if(positive!=3U&&negative!=3U) {
    std::size_t blocked{};
    if(positive>negative)
      blocked=static_cast<std::size_t>(std::find_if(
          orientations.begin(),orientations.end(),[](const auto sign) {
            return sign<=0;
          })-orientations.begin());
    else if(negative>positive)
      blocked=static_cast<std::size_t>(std::find_if(
          orientations.begin(),orientations.end(),[](const auto sign) {
            return sign>=0;
          })-orientations.begin());
    else
      blocked=static_cast<std::size_t>(std::find(
          orientations.begin(),orientations.end(),0)-orientations.begin());
    if(blocked<3U)result.blocking_mesh_edge=std::array<std::uint32_t,2>{{
        face[blocked],face[(blocked+1U)%3U]}};
    result.failure=CanonicalLiteralEdgeFlipFailure::nonpositive_replacement;
    return result;
  }
  for(std::size_t i=0U;i<replacement.size();++i)
    if(orientations[i]<0)std::swap(replacement[i][0],replacement[i][1]);
  const auto boundary_of=[](const auto& cells) {
    std::map<Face,unsigned> counts;
    for(const auto& cell:cells)for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face candidate{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)candidate[cursor++]=cell[i];
      ++counts[face_key(candidate)];
    }
    std::set<Face> boundary;
    for(const auto& [candidate,count]:counts) {
      if(count==1U)boundary.insert(candidate);
      else if(count!=2U)return std::set<Face>{};
    }
    return boundary;
  };
  const std::vector<Tet> original{mesh[uses[0]],mesh[uses[1]]};
  const std::vector<Tet> replacement_vector(replacement.begin(),replacement.end());
  if(boundary_of(original)!=boundary_of(replacement_vector)) {
    result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;
    return result;
  }
  long double old_volume{},new_volume{};
  for(const auto& cell:original)
    old_volume+=std::abs(orient(points[cell[0]],points[cell[1]],
                               points[cell[2]],points[cell[3]]));
  for(const auto& cell:replacement_vector)
    new_volume+=std::abs(orient(points[cell[0]],points[cell[1]],
                               points[cell[2]],points[cell[3]]));
  if(std::abs(old_volume-new_volume)>std::max(1.0L,old_volume)*1e-14L) {
    result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;
    return result;
  }
  for(std::size_t cell=0U;cell<mesh.size();++cell)
    if(cell!=uses[0]&&cell!=uses[1])result.tetrahedra.push_back(mesh[cell]);
  result.tetrahedra.insert(result.tetrahedra.end(),replacement.begin(),replacement.end());
  if(validate_entire_mesh&&
     !constraint_mesh_mutation_is_valid(constraints,mesh,result.tetrahedra)) {
    result.tetrahedra.clear();
    result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;
    return result;
  }
  result.accepted=true;
  result.failure=CanonicalLiteralEdgeFlipFailure::none;
  return result;
}

CanonicalLiteralEdgeFlipResult face_flip_for_segment(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> edge,const std::vector<Tet>& mesh,
    bool first_intersection_only=false,bool reverse_direction=false);

struct GeneralizedFaceRecovery {
  CanonicalLiteralEdgeFlipResult flip;
  CanonicalPlcConstraintSet constraints;
  std::size_t edge_removal_attempts{};
  std::size_t edge_removals{};
  std::size_t edge_retriangulation_trials{};
  std::size_t maximum_edge_degree{};
  bool boundary_vertex_attached{};
  std::uint64_t attached_boundary_vertex{};
};

enum class EndpointStarFeatureKind : std::uint8_t {
  none,
  recovered_segment,
  vertex,
  edge,
  face,
};

struct EndpointStarFeature {
  EndpointStarFeatureKind kind{EndpointStarFeatureKind::none};
  std::uint32_t vertex{};
  std::array<std::uint32_t,2> edge{};
  Face face{};
  Tet source_cell{};
  Point s0{};
  long double parameter{};
};

std::optional<EndpointStarFeature>
first_segment_feature_from_endpoint_star(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,const std::vector<Tet>& mesh,
    bool reverse_direction);

CanonicalLiteralEdgeFlipResult remove_intervening_mesh_vertex(
    const CanonicalPlcConstraintSet& constraints,std::uint32_t vertex,
    const std::vector<Tet>& mesh,std::size_t maximum_edge_attempts=100U) {
  CanonicalLiteralEdgeFlipResult refused;
  refused.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;
  if(vertex>=constraints.vertices.size())return refused;
  const auto vertex_id=constraints.vertices[vertex].id;
  if(std::any_of(constraints.facets.begin(),constraints.facets.end(),
       [&](const auto& facet) {
         return std::find(facet.vertices.begin(),facet.vertices.end(),vertex_id)!=
             facet.vertices.end();
       })) {
    refused.failure=CanonicalLiteralEdgeFlipFailure::frozen_cavity_boundary;
    return refused;
  }

  std::vector<Point> points;
  points.reserve(constraints.vertices.size());
  for(const auto& item:constraints.vertices) {
    const auto p=item.position;
    points.push_back({p.x,p.y,p.z});
  }
  std::vector<std::size_t> star;
  std::map<long double,std::uint32_t> candidates;
  const auto p=points[vertex];
  for(std::size_t i=0U;i<mesh.size();++i) {
    if(std::find(mesh[i].begin(),mesh[i].end(),vertex)==mesh[i].end())continue;
    star.push_back(i);
    for(const auto neighbour:mesh[i])if(neighbour!=vertex) {
      const auto delta=points[neighbour]-p;
      candidates.emplace(dot(delta,delta),neighbour);
    }
  }
  if(star.empty())return refused;
  refused.cavity_tetrahedra.reserve(star.size());
  for(const auto cell:star)refused.cavity_tetrahedra.push_back(mesh[cell]);
  refused.intersected_tetrahedra=star.size();

  std::size_t attempts{};
  for(const auto& [length,neighbour]:candidates) {
    (void)length;
    if(attempts++>=maximum_edge_attempts)break;
    std::set<std::size_t> shell,remaining_star;
    for(const auto cell:star) {
      if(std::find(mesh[cell].begin(),mesh[cell].end(),neighbour)!=
         mesh[cell].end())shell.insert(cell);
      else remaining_star.insert(cell);
    }
    if(shell.empty())continue;
    std::vector<Tet> candidate;
    candidate.reserve(mesh.size()-shell.size());
    bool valid=true;
    for(std::size_t i=0U;i<mesh.size();++i) {
      if(shell.contains(i))continue;
      auto cell=mesh[i];
      if(remaining_star.contains(i)) {
        const auto before=robust_orient(points,cell);
        *std::find(cell.begin(),cell.end(),vertex)=neighbour;
        const auto after=robust_orient(points,cell);
        if(after==0||(before!=0&&before!=after)){valid=false;break;}
      }
      candidate.push_back(cell);
    }
    if(!valid||!constraint_mesh_mutation_is_valid(
         constraints,mesh,candidate))continue;
    CanonicalLiteralEdgeFlipResult removed;
    removed.accepted=true;
    removed.failure=CanonicalLiteralEdgeFlipFailure::none;
    removed.cavity_tetrahedra=refused.cavity_tetrahedra;
    removed.intersected_tetrahedra=star.size();
    removed.retriangulation_trials=attempts;
    removed.tetrahedra=std::move(candidate);
    return removed;
  }

  // Pinned removePnt ends with flip41 when the point sphere has four cells.
  if(star.size()==4U) {
    std::set<std::uint32_t> neighbours;
    for(const auto cell:star)for(const auto corner:mesh[cell])
      if(corner!=vertex)neighbours.insert(corner);
    if(neighbours.size()==4U) {
      Tet replacement{};
      std::copy(neighbours.begin(),neighbours.end(),replacement.begin());
      const auto orientation=robust_orient(points,replacement);
      if(orientation!=0) {
        if(orientation<0)std::swap(replacement[0],replacement[1]);
        const std::set<std::size_t> removed_cells(star.begin(),star.end());
        std::vector<Tet> candidate;
        for(std::size_t i=0U;i<mesh.size();++i)
          if(!removed_cells.contains(i))candidate.push_back(mesh[i]);
        candidate.push_back(replacement);
        if(constraint_mesh_mutation_is_valid(constraints,mesh,candidate)) {
          CanonicalLiteralEdgeFlipResult removed;
          removed.accepted=true;
          removed.failure=CanonicalLiteralEdgeFlipFailure::none;
          removed.cavity_tetrahedra=refused.cavity_tetrahedra;
          removed.intersected_tetrahedra=star.size();
          removed.retriangulation_trials=attempts;
          removed.tetrahedra=std::move(candidate);
          return removed;
        }
      }
    }
  }
  refused.retriangulation_trials=attempts;
  return refused;
}

GeneralizedFaceRecovery generalized_face_flip_for_segment(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,const std::vector<Tet>& mesh,
    bool first_intersection_only=false,bool reverse_direction=false,
    std::size_t maximum_flip_depth=16U) {
  GeneralizedFaceRecovery result;
  if(first_intersection_only) {
    auto current=mesh;
    bool changed_any=false;
    CanonicalLiteralEdgeFlipResult first_failure;
    first_failure.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;
    bool have_first_failure=false;
    // recoverEdgebyFlip's `tried` guard permits 1001 walk iterations.  The
    // fliplevel argument limits each removeface/removeEdge search; it is not
    // a bound on the number of successive crossed simplices.
    for(std::size_t walk_step=0U;walk_step<=1000U;++walk_step) {
      (void)walk_step;
      const auto feature=first_segment_feature_from_endpoint_star(
          constraints,segment,current,reverse_direction);
      if(!feature)break;
      if(feature->kind==EndpointStarFeatureKind::recovered_segment) {
        result.flip.accepted=true;
        result.flip.failure=CanonicalLiteralEdgeFlipFailure::none;
        result.flip.tetrahedra=std::move(current);
        return result;
      }
      if(feature->kind==EndpointStarFeatureKind::vertex) {
        const auto stable_vertex=feature->vertex<constraints.vertices.size()?
            constraints.vertices[feature->vertex].id:0U;
        const bool boundary_vertex=feature->vertex<constraints.vertices.size()&&
            std::any_of(constraints.facets.begin(),constraints.facets.end(),
                [&](const auto& facet) {
                  return std::find(facet.vertices.begin(),facet.vertices.end(),
                                   stable_vertex)!=facet.vertices.end();
                });
        if(boundary_vertex) {
          auto attached=attach_canonical_plc_boundary_vertex_to_segment(
              constraints,segment,stable_vertex);
          if(attached.accepted()) {
            result.constraints=std::move(attached.constraints);
            result.boundary_vertex_attached=true;
            result.attached_boundary_vertex=stable_vertex;
            result.flip.accepted=true;
            result.flip.failure=CanonicalLiteralEdgeFlipFailure::none;
            result.flip.tetrahedra=std::move(current);
            return result;
          }
          break;
        }
        auto removed=remove_intervening_mesh_vertex(
            constraints,feature->vertex,current);
        if(!removed.accepted) {
          if(!have_first_failure) {
            first_failure=std::move(removed);
            have_first_failure=true;
          }
          break;
        }
        current=std::move(removed.tetrahedra);
        changed_any=true;
        const auto after=first_segment_feature_from_endpoint_star(
            constraints,segment,current,reverse_direction);
        if(after&&after->kind==EndpointStarFeatureKind::recovered_segment) {
          result.flip.accepted=true;
          result.flip.failure=CanonicalLiteralEdgeFlipFailure::none;
          result.flip.tetrahedra=std::move(current);
          return result;
        }
        continue;
      }

      CanonicalLiteralEdgeFlipResult operation;
      bool removed_mesh_edge=false;
      if(feature->kind==EndpointStarFeatureKind::edge) {
        operation=remove_mesh_edge_in_mesh(constraints,feature->edge,current);
        removed_mesh_edge=true;
      } else if(feature->kind==EndpointStarFeatureKind::face) {
        operation=face_flip_for_specific_face(
            constraints,current,feature->face);
        if(!operation.accepted&&operation.blocking_mesh_edge) {
          if(!have_first_failure) {
            first_failure=operation;
            have_first_failure=true;
          }
          operation=remove_mesh_edge_in_mesh(
              constraints,*operation.blocking_mesh_edge,current);
          removed_mesh_edge=true;
        }
      } else {
        break;
      }
      if(removed_mesh_edge) {
        ++result.edge_removal_attempts;
        result.edge_retriangulation_trials+=operation.retriangulation_trials;
        result.maximum_edge_degree=std::max(
            result.maximum_edge_degree,operation.intersected_tetrahedra);
      }
      if(!operation.accepted) {
        if(!have_first_failure) {
          first_failure=operation;
          have_first_failure=true;
        }
        break;
      }
      if(removed_mesh_edge)++result.edge_removals;
      current=std::move(operation.tetrahedra);
      changed_any=true;
      const auto after=first_segment_feature_from_endpoint_star(
          constraints,segment,current,reverse_direction);
      if(after&&after->kind==EndpointStarFeatureKind::recovered_segment) {
        result.flip.accepted=true;
        result.flip.failure=CanonicalLiteralEdgeFlipFailure::none;
        result.flip.tetrahedra=std::move(current);
        return result;
      }
    }
    // Pinned recoverEdgebyFlip commits each successful local removal before
    // walking again. A later stall returns failure to recoverEdge, but those
    // mutations remain visible to its reverse-direction call.
    if(changed_any) {
      result.flip.accepted=true;
      result.flip.failure=CanonicalLiteralEdgeFlipFailure::none;
      result.flip.tetrahedra=std::move(current);
    } else if(have_first_failure)result.flip=std::move(first_failure);
    else result.flip.failure=
        CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;
    return result;
  }
  const auto initial=face_flip_for_segment(
      constraints,segment,mesh,first_intersection_only,reverse_direction);
  result.flip=initial;
  if(initial.accepted||!initial.blocking_mesh_edge)return result;
  auto current=mesh;
  auto obstruction=initial;
  for(std::size_t depth=0U;depth<maximum_flip_depth;++depth) {
    if(!obstruction.blocking_mesh_edge)break;
    auto removed=remove_mesh_edge_in_mesh(
        constraints,*obstruction.blocking_mesh_edge,current);
    ++result.edge_removal_attempts;
    result.edge_retriangulation_trials+=removed.retriangulation_trials;
    result.maximum_edge_degree=std::max(
        result.maximum_edge_degree,removed.intersected_tetrahedra);
    if(!removed.accepted)break;
    ++result.edge_removals;
    current=std::move(removed.tetrahedra);
    obstruction=face_flip_for_segment(
        constraints,segment,current,first_intersection_only,reverse_direction);
    if(obstruction.accepted) {
      result.flip=std::move(obstruction);
      return result;
    }
  }
  // Generalized face removal is one transaction. If it cannot remove the
  // crossed face, discard its temporary edge flips and classify the original
  // configuration for FHC insertion.
  result.edge_removals=0U;
  result.flip=initial;
  return result;
}

// A segment-crossing cavity is a topological ball whose endpoints lie on its
// boundary.  When either endpoint sees the whole opposite boundary disk, the
// cavity has a direct tetrahedralization: cone that disk to the endpoint.  It
// preserves every cavity boundary triangle and necessarily introduces the
// requested endpoint-to-endpoint edge.  Absolute volume agreement rejects a
// geometrically folded cone, so this is not merely a combinatorial rewrite.
CanonicalLiteralEdgeFlipResult endpoint_cone_retriangulation(
    const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,2> edge,
    const std::vector<Tet>& mesh,const std::vector<Tet>& cavity) {
  CanonicalLiteralEdgeFlipResult result;result.cavity_tetrahedra=cavity;
  result.intersected_tetrahedra=cavity.size();
  if(cavity.empty()) {result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;return result;}
  std::map<std::uint64_t,std::uint32_t> index;std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto p=constraints.vertices[i].position;points.push_back({p.x,p.y,p.z});
  }
  const auto left=index.find(edge[0]),right=index.find(edge[1]);
  if(left==index.end()||right==index.end())return result;
  std::map<Face,unsigned> original_uses;long double target_volume{};
  for(const auto& cell:cavity) {
    target_volume+=std::abs(orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]));
    for(unsigned omit=0U;omit<4U;++omit) {Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=cell[i];++original_uses[face_key(face)];}
  }
  std::set<Face> boundary;
  for(const auto& [face,count]:original_uses) {
    if(count>2U){result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;return result;}
    if(count==1U)boundary.insert(face);
  }
  std::set<std::array<std::uint64_t,3>> frozen;
  for(const auto& facet:constraints.facets){auto face=facet.vertices;std::sort(face.begin(),face.end());frozen.insert(face);}
  for(const auto& face:boundary){std::array<std::uint64_t,3> ids{{constraints.vertices[face[0]].id,constraints.vertices[face[1]].id,constraints.vertices[face[2]].id}};if(frozen.contains(ids))result.touches_frozen_facet=true;}
  for(const auto apex:std::array<std::uint32_t,2>{{left->second,right->second}}) {
    ++result.retriangulation_trials;std::vector<Tet> replacement;long double replacement_volume{};bool has_edge{};bool positive=true;
    for(const auto& face:boundary) {
      if(std::find(face.begin(),face.end(),apex)!=face.end())continue;
      Tet cell{{apex,face[0],face[1],face[2]}};
      auto volume=orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]);
      if(std::abs(volume)<1e-20L){positive=false;break;}
      if(volume<0.)std::swap(cell[0],cell[1]);
      replacement_volume+=std::abs(volume);has_edge=has_edge||(
          std::find(cell.begin(),cell.end(),left->second)!=cell.end()&&
          std::find(cell.begin(),cell.end(),right->second)!=cell.end());
      replacement.push_back(cell);
    }
    if(!positive||!has_edge)continue;
    std::map<Face,unsigned> replacement_uses;std::set<Tet> unique;
    for(const auto& cell:replacement) {auto key=cell;std::sort(key.begin(),key.end());if(!unique.insert(key).second){positive=false;break;}for(unsigned omit=0U;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=cell[i];++replacement_uses[face_key(face)];}}
    if(!positive)continue;
    std::set<Face> replacement_boundary;bool manifold=true;
    for(const auto& [face,count]:replacement_uses){if(count==1U)replacement_boundary.insert(face);else if(count!=2U){manifold=false;break;}}
    const auto tolerance=std::max(1.0L,target_volume)*1e-14L;
    if(!manifold||replacement_boundary!=boundary||std::abs(replacement_volume-target_volume)>tolerance)continue;
    const auto canonical=[](Tet cell){std::sort(cell.begin(),cell.end());return cell;};std::set<Tet> removed;
    for(const auto& cell:cavity)removed.insert(canonical(cell));
    for(const auto& cell:mesh)if(!removed.contains(canonical(cell)))result.tetrahedra.push_back(cell);
    result.tetrahedra.insert(result.tetrahedra.end(),replacement.begin(),replacement.end());
    result.accepted=true;result.failure=CanonicalLiteralEdgeFlipFailure::none;return result;
  }
  result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;return result;
}

// The segment-crossing cavity is often larger than the tiny exhaustive
// kernel below, especially for an immutable structured-core edge. Grow that
// cavity across faces which hide an endpoint until the endpoint sees its
// complete opposite boundary. Coning that boundary to the endpoint then
// recovers the requested edge without splitting or moving either endpoint.
// Every accepted replacement must preserve the exact cavity boundary and
// absolute volume, so an overgrown/folded cone is refused rather than exposed.
CanonicalLiteralEdgeFlipResult expanded_endpoint_cone_retriangulation(
    const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,2> edge,
    const std::vector<Tet>& mesh,const std::vector<Tet>& initial_cavity) {
  CanonicalLiteralEdgeFlipResult result;result.cavity_tetrahedra=initial_cavity;
  result.intersected_tetrahedra=initial_cavity.size();
  if(initial_cavity.empty())return result;
  std::map<std::uint64_t,std::uint32_t> index;std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i){index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));const auto p=constraints.vertices[i].position;points.push_back({p.x,p.y,p.z});}
  const auto left=index.find(edge[0]),right=index.find(edge[1]);
  if(left==index.end()||right==index.end())return result;
  const auto canonical=[](Tet cell){std::sort(cell.begin(),cell.end());return cell;};
  std::map<Tet,std::size_t> mesh_index;for(std::size_t i=0U;i<mesh.size();++i)mesh_index.emplace(canonical(mesh[i]),i);
  std::set<std::size_t> initial;
  for(const auto& cell:initial_cavity){const auto found=mesh_index.find(canonical(cell));if(found==mesh_index.end()){result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;return result;}initial.insert(found->second);}
  std::map<Face,std::vector<std::pair<std::size_t,std::uint32_t>>> global_faces;
  for(std::size_t ti=0U;ti<mesh.size();++ti)for(unsigned omit=0U;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=mesh[ti][i];global_faces[face_key(face)].push_back({ti,mesh[ti][omit]});}
  std::set<Face> protected_faces;
  for(const auto& facet:constraints.facets){Face face{};bool resolved=true;for(unsigned i=0U;i<3U;++i){const auto found=index.find(facet.vertices[i]);if(found==index.end()){resolved=false;break;}face[i]=found->second;}if(resolved&&global_faces.contains(face_key(face)))protected_faces.insert(face_key(face));}
  constexpr std::size_t maximum_expansions=64U;
  for(const auto apex:std::array<std::uint32_t,2>{{left->second,right->second}}) {
    auto selected=initial;
    for(std::size_t expansion=0U;expansion<=maximum_expansions;++expansion) {
      result.retriangulation_trials=std::max(result.retriangulation_trials,expansion+1U);
      std::map<Face,std::vector<std::pair<std::size_t,std::uint32_t>>> local_faces;
      long double old_volume{};
      for(const auto ti:selected){const auto& cell=mesh[ti];old_volume+=std::abs(orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]));for(unsigned omit=0U;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=cell[i];local_faces[face_key(face)].push_back({ti,cell[omit]});}}
      std::set<Face> boundary;bool manifold=true;
      for(const auto& [face,uses]:local_faces){if(uses.size()==1U)boundary.insert(face);else if(uses.size()!=2U){manifold=false;break;}}
      if(!manifold||boundary.empty())break;
      std::optional<std::size_t> neighbour;
      for(const auto& face:boundary) {
        if(std::find(face.begin(),face.end(),apex)!=face.end())continue;
        const auto inside=robust_orient(points[face[0]],points[face[1]],points[face[2]],points[local_faces.at(face).front().second]);
        const auto query=robust_orient(points[face[0]],points[face[1]],points[face[2]],points[apex]);
        if(query!=0&&query==inside)continue;
        const auto adjacent=global_faces.find(face);
        if(adjacent==global_faces.end()||adjacent->second.size()!=2U||protected_faces.contains(face)){neighbour.reset();manifold=false;break;}
        for(const auto& [candidate,opposite]:adjacent->second){(void)opposite;if(!selected.contains(candidate)){neighbour=candidate;break;}}
        if(!neighbour){manifold=false;break;}
        break;
      }
      if(!manifold)break;
      if(neighbour){selected.insert(*neighbour);continue;}
      std::vector<Tet> replacement;long double new_volume{};bool has_edge{};bool valid=true;
      for(const auto& face:boundary){if(std::find(face.begin(),face.end(),apex)!=face.end())continue;Tet cell{{apex,face[0],face[1],face[2]}};auto volume=orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]);if(std::abs(volume)<1e-20L){valid=false;break;}if(volume<0.0L)std::swap(cell[0],cell[1]);new_volume+=std::abs(volume);has_edge=has_edge||(std::find(cell.begin(),cell.end(),left->second)!=cell.end()&&std::find(cell.begin(),cell.end(),right->second)!=cell.end());replacement.push_back(cell);}
      if(!valid||!has_edge||std::abs(new_volume-old_volume)>std::max(1.0L,old_volume)*1e-14L)break;
      std::map<Face,unsigned> replacement_uses;for(const auto& cell:replacement)for(unsigned omit=0U;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=cell[i];++replacement_uses[face_key(face)];}
      std::set<Face> replacement_boundary;for(const auto& [face,count]:replacement_uses){if(count==1U)replacement_boundary.insert(face);else if(count!=2U){valid=false;break;}}
      if(!valid||replacement_boundary!=boundary)break;
      for(std::size_t ti=0U;ti<mesh.size();++ti)if(!selected.contains(ti))result.tetrahedra.push_back(mesh[ti]);
      result.tetrahedra.insert(result.tetrahedra.end(),replacement.begin(),replacement.end());result.cavity_tetrahedra.clear();for(const auto ti:selected)result.cavity_tetrahedra.push_back(mesh[ti]);result.intersected_tetrahedra=selected.size();result.retriangulation_trials=expansion+1U;result.accepted=true;result.failure=CanonicalLiteralEdgeFlipFailure::none;return result;
    }
  }
  result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;return result;
}

CanonicalLiteralEdgeFlipResult segment_kernel_cone_retriangulation(
    const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,2> edge,
    const std::vector<Tet>& mesh,const std::vector<Tet>& cavity) {
  CanonicalLiteralEdgeFlipResult result;result.cavity_tetrahedra=cavity;result.intersected_tetrahedra=cavity.size();
  if(cavity.empty())return result;if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
  std::map<std::uint64_t,std::uint32_t> index;std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i){index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));const auto p=constraints.vertices[i].position;points.push_back({p.x,p.y,p.z});}
  const auto first=index.find(edge[0]),second=index.find(edge[1]);if(first==index.end()||second==index.end())return result;
  std::map<Face,unsigned> original_uses;long double target_volume{};
  for(const auto& cell:cavity){target_volume+=std::abs(orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]));for(unsigned omit=0U;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=cell[i];++original_uses[face_key(face)];}}
  std::set<Face> boundary;for(const auto& [face,count]:original_uses){if(count>2U)return result;if(count==1U)boundary.insert(face);}
  const auto av=constraints.vertices[first->second].position,bv=constraints.vertices[second->second].position;
  for(std::uint32_t denominator=2U;denominator<=64U;denominator*=2U)for(std::uint32_t numerator=1U;numerator<denominator;numerator+=2U) {
    ++result.retriangulation_trials;const auto t=static_cast<double>(numerator)/denominator;
    const Vec3 inserted{av.x+(bv.x-av.x)*t,av.y+(bv.y-av.y)*t,av.z+(bv.z-av.z)*t};
    const Point candidate{inserted.x,inserted.y,inserted.z};const auto candidate_index=static_cast<std::uint32_t>(points.size());
    points.push_back(candidate);std::vector<Tet> replacement;long double replacement_volume{};bool valid=true;
    for(const auto& face:boundary){Tet cell{{candidate_index,face[0],face[1],face[2]}};auto volume=orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]);if(std::abs(volume)<1e-20L){valid=false;break;}if(volume<0.)std::swap(cell[0],cell[1]);replacement_volume+=std::abs(volume);replacement.push_back(cell);}
    points.pop_back();if(!valid)continue;
    std::map<Face,unsigned> replacement_uses;for(const auto& cell:replacement)for(unsigned omit=0U;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=cell[i];++replacement_uses[face_key(face)];}
    std::set<Face> replacement_boundary;for(const auto& [face,count]:replacement_uses){if(count==1U)replacement_boundary.insert(face);else if(count!=2U){valid=false;break;}}
    const auto tolerance=std::max(1.0L,target_volume)*1e-14L;
    if(!valid||replacement_boundary!=boundary||std::abs(replacement_volume-target_volume)>tolerance)continue;
    const auto canonical=[](Tet cell){std::sort(cell.begin(),cell.end());return cell;};std::set<Tet> removed;for(const auto& cell:cavity)removed.insert(canonical(cell));
    for(const auto& cell:mesh)if(!removed.contains(canonical(cell)))result.tetrahedra.push_back(cell);result.tetrahedra.insert(result.tetrahedra.end(),replacement.begin(),replacement.end());
    result.inserted_steiner_vertex=inserted;result.recovers_by_constraint_split=true;result.constraint_split_numerator=numerator;result.constraint_split_denominator=denominator;result.accepted=true;result.failure=CanonicalLiteralEdgeFlipFailure::none;return result;
  }
  result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;return result;
}

CanonicalLiteralEdgeFlipResult advancing_ridge_insert_facet(
    const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,3> target,
    std::array<std::uint64_t,2> ridge,const std::vector<Tet>& mesh) {
  CanonicalLiteralEdgeFlipResult result;std::map<std::uint64_t,std::uint32_t> index;std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i){index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));const auto p=constraints.vertices[i].position;points.push_back({p.x,p.y,p.z});}
  const auto refuse=[&](CanonicalAdvancingRidgeFailure failure) {
    result.advancing_ridge_failure=failure;
    result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;
    return result;
  };
  std::array<std::uint32_t,3> face{};for(unsigned i=0U;i<3U;++i){const auto found=index.find(target[i]);if(found==index.end())return refuse(CanonicalAdvancingRidgeFailure::invalid_target_or_ridge);face[i]=found->second;}std::sort(face.begin(),face.end());
  std::array<std::uint32_t,2> edge{};for(unsigned i=0U;i<2U;++i){const auto found=index.find(ridge[i]);if(found==index.end())return refuse(CanonicalAdvancingRidgeFailure::invalid_target_or_ridge);edge[i]=found->second;}std::sort(edge.begin(),edge.end());
  if(!std::includes(face.begin(),face.end(),edge.begin(),edge.end()))
    return refuse(CanonicalAdvancingRidgeFailure::invalid_target_or_ridge);
  const auto third=std::find_if(face.begin(),face.end(),[&](auto value){return value!=edge[0]&&value!=edge[1];});
  if(third==face.end())return refuse(CanonicalAdvancingRidgeFailure::invalid_target_or_ridge);
  const auto p=*third;
  std::map<Face,std::vector<std::size_t>> ledger;
  for(std::size_t ci=0U;ci<mesh.size();++ci)for(unsigned omit=0U;omit<4U;++omit){Face f{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)f[n++]=mesh[ci][i];ledger[face_key(f)].push_back(ci);}
  if(ledger.contains(face)) {result.accepted=true;result.failure=CanonicalLiteralEdgeFlipFailure::none;result.tetrahedra=mesh;return result;}
  std::set<Face> protected_faces;for(const auto& facet:constraints.facets){Face f{};bool resolved=true;for(unsigned i=0U;i<3U;++i){const auto found=index.find(facet.vertices[i]);if(found==index.end()){resolved=false;break;}f[i]=found->second;}if(resolved){f=face_key(f);if(ledger.contains(f))protected_faces.insert(f);}}
  std::vector<std::size_t> starts;for(std::size_t ci=0U;ci<mesh.size();++ci)if(std::find(mesh[ci].begin(),mesh[ci].end(),edge[0])!=mesh[ci].end()&&std::find(mesh[ci].begin(),mesh[ci].end(),edge[1])!=mesh[ci].end())starts.push_back(ci);
  if(starts.empty())return refuse(CanonicalAdvancingRidgeFailure::no_starting_cell);
  const auto cell_key=[&](std::size_t ci){std::array<std::uint64_t,4> key{};for(unsigned i=0U;i<4U;++i)key[i]=constraints.vertices[mesh[ci][i]].id;std::sort(key.begin(),key.end());return key;};
  std::sort(starts.begin(),starts.end(),[&](auto a,auto b){return cell_key(a)<cell_key(b);});
  const auto boundary_of=[](const auto& cells){std::map<Face,unsigned> uses;for(const auto& cell:cells)for(unsigned omit=0U;omit<4U;++omit){Face f{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)f[n++]=cell[i];++uses[face_key(f)];}std::set<Face> out;for(const auto& [f,count]:uses)if(count==1U)out.insert(f);else if(count!=2U)return std::set<Face>{};return out;};
  CanonicalAdvancingRidgeFailure last_failure=CanonicalAdvancingRidgeFailure::nonmanifold_cavity;
  for(const auto start:starts) {
    std::set<std::size_t> selected{{start}};bool refused=false;
    while(!refused) {
      std::optional<std::pair<Face,std::size_t>> expansion;
      for(const auto ci:selected)for(unsigned omit=0U;omit<4U;++omit){Face f{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)f[n++]=mesh[ci][i];f=face_key(f);const auto& uses=ledger.at(f);if(uses.size()==2U&&selected.contains(uses[0])&&selected.contains(uses[1]))continue;if(std::find(f.begin(),f.end(),p)!=f.end())continue;const auto opposite=*std::find_if(mesh[ci].begin(),mesh[ci].end(),[&](auto value){return std::find(f.begin(),f.end(),value)==f.end();});const auto inside=robust_orient(points[f[0]],points[f[1]],points[f[2]],points[opposite]);const auto query=robust_orient(points[f[0]],points[f[1]],points[f[2]],points[p]);if(query!=0&&query==inside)continue;if(protected_faces.contains(f)){last_failure=CanonicalAdvancingRidgeFailure::protected_facet_blocked;refused=true;break;}if(uses.size()!=2U){last_failure=CanonicalAdvancingRidgeFailure::hull_blocked;refused=true;break;}const auto outside=uses[0]==ci?uses[1]:uses[0];if(!expansion||f<expansion->first)expansion={{f,outside}};}
      if(refused)break;if(!expansion)break;selected.insert(expansion->second);if(selected.size()>mesh.size()){last_failure=CanonicalAdvancingRidgeFailure::expansion_limit;refused=true;break;}
    }
    if(refused)continue;
    std::vector<Tet> cavity;for(const auto ci:selected)cavity.push_back(mesh[ci]);const auto boundary=boundary_of(cavity);if(boundary.empty()){last_failure=CanonicalAdvancingRidgeFailure::nonmanifold_cavity;continue;}
    std::vector<Tet> replacement;long double old_volume{},new_volume{};bool positive=true;
    for(const auto& cell:cavity)old_volume+=std::abs(orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]));
    for(const auto& f:boundary){if(std::find(f.begin(),f.end(),p)!=f.end())continue;Tet cell{{p,f[0],f[1],f[2]}};auto volume=orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]);if(std::abs(volume)<1e-20L){positive=false;break;}if(volume<0.)std::swap(cell[0],cell[1]);new_volume+=std::abs(volume);replacement.push_back(cell);}
    if(!positive||replacement.empty()){last_failure=CanonicalAdvancingRidgeFailure::nonpositive_replacement;continue;}
    if(boundary_of(replacement)!=boundary){last_failure=CanonicalAdvancingRidgeFailure::changed_boundary;continue;}
    if(std::abs(old_volume-new_volume)>std::max(1.0L,old_volume)*1e-14L){last_failure=CanonicalAdvancingRidgeFailure::volume_disagreement;continue;}
    const auto replacement_faces=boundary_of(replacement);(void)replacement_faces;bool target_present=false;for(const auto& cell:replacement)for(unsigned omit=0U;omit<4U;++omit){Face f{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)f[n++]=cell[i];if(face_key(f)==face)target_present=true;}
    if(!target_present){last_failure=CanonicalAdvancingRidgeFailure::target_face_absent;continue;}for(std::size_t ci=0U;ci<mesh.size();++ci)if(!selected.contains(ci))result.tetrahedra.push_back(mesh[ci]);result.tetrahedra.insert(result.tetrahedra.end(),replacement.begin(),replacement.end());result.cavity_tetrahedra=std::move(cavity);result.intersected_tetrahedra=result.cavity_tetrahedra.size();result.accepted=true;result.failure=CanonicalLiteralEdgeFlipFailure::none;return result;
  }
  return refuse(last_failure);
}

CanonicalLiteralEdgeFlipResult stellar_insert_constraint_vertex(
    const CanonicalPlcConstraintSet& constraints,std::size_t previous_vertex_count,
    const std::vector<Tet>& mesh,bool validate_entire_mesh=true,
    bool preserve_finite_boundary=true) {
  CanonicalLiteralEdgeFlipResult result;
  if(constraints.vertices.size()!=previous_vertex_count+1U)return result;
  const auto vertex=static_cast<std::uint32_t>(previous_vertex_count);
  std::vector<Point> points;points.reserve(constraints.vertices.size());
  for(const auto& item:constraints.vertices){const auto p=item.position;points.push_back({p.x,p.y,p.z});}
  struct Containment {std::size_t cell{};std::vector<unsigned> zero_faces;};
  std::vector<Containment> containing;
  for(std::size_t ci=0U;ci<mesh.size();++ci) {
    const auto& cell=mesh[ci];Containment hit{ci,{}};bool inside=true;
    for(unsigned omit=0U;omit<4U;++omit) {
      Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=cell[i];
      const auto opposite=robust_orient(points[face[0]],points[face[1]],points[face[2]],points[cell[omit]]);
      const auto query=robust_orient(points[face[0]],points[face[1]],points[face[2]],points[vertex]);
      if(query==0)hit.zero_faces.push_back(omit);else if(query!=opposite){inside=false;break;}
    }
    if(inside)containing.push_back(std::move(hit));
  }
  if(containing.empty()){result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;return result;}
  const auto exemplar=*std::max_element(containing.begin(),containing.end(),[](const auto& a,const auto& b){return a.zero_faces.size()<b.zero_faces.size();});
  std::vector<std::size_t> selected;std::vector<Tet> children;
  const auto positive=[&](Tet cell)->std::optional<Tet>{const auto orientation=robust_orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]);if(orientation==0)return std::nullopt;if(orientation<0)std::swap(cell[0],cell[1]);return cell;};
  if(exemplar.zero_faces.empty()) {
    selected.push_back(exemplar.cell);const auto old=mesh[exemplar.cell];
    for(unsigned omit=0U;omit<4U;++omit){Tet child{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)child[n++]=old[i];child[3]=vertex;const auto oriented=positive(child);if(!oriented)return result;children.push_back(*oriented);}
  } else if(exemplar.zero_faces.size()==1U) {
    const auto old=mesh[exemplar.cell];Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=exemplar.zero_faces[0])face[n++]=old[i];
    for(std::size_t ci=0U;ci<mesh.size();++ci)if(std::all_of(face.begin(),face.end(),[&](auto value){return std::find(mesh[ci].begin(),mesh[ci].end(),value)!=mesh[ci].end();})) {
      selected.push_back(ci);const auto opposite=*std::find_if(mesh[ci].begin(),mesh[ci].end(),[&](auto value){return std::find(face.begin(),face.end(),value)==face.end();});
      for(unsigned e=0U;e<3U;++e){const auto oriented=positive({{opposite,face[e],face[(e+1U)%3U],vertex}});if(!oriented)return result;children.push_back(*oriented);}
    }
  } else if(exemplar.zero_faces.size()==2U) {
    const auto old=mesh[exemplar.cell];std::array<std::uint32_t,2> common{};unsigned n{};
    for(unsigned i=0U;i<4U;++i)if(std::find(exemplar.zero_faces.begin(),exemplar.zero_faces.end(),i)==exemplar.zero_faces.end()){
      if(n>=common.size())return result;common[n++]=old[i];
    }
    if(n!=common.size())return result;
    for(std::size_t ci=0U;ci<mesh.size();++ci)if(std::find(mesh[ci].begin(),mesh[ci].end(),common[0])!=mesh[ci].end()&&std::find(mesh[ci].begin(),mesh[ci].end(),common[1])!=mesh[ci].end()) {
      selected.push_back(ci);std::array<std::uint32_t,2> other{};unsigned cursor{};for(const auto value:mesh[ci])if(value!=common[0]&&value!=common[1])other[cursor++]=value;
      for(const auto endpoint:common){const auto oriented=positive({{endpoint,vertex,other[0],other[1]}});if(!oriented)return result;children.push_back(*oriented);}
    }
  } else {result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;return result;}
  if(selected.empty()||children.empty())return result;
  const auto boundary_of=[](const auto& cells){std::map<Face,unsigned> uses;for(const auto& cell:cells)for(unsigned omit=0U;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=cell[i];++uses[face_key(face)];}std::set<Face> boundary;for(const auto& [face,count]:uses)if(count==1U)boundary.insert(face);else if(count!=2U)return std::set<Face>{};return boundary;};
  std::vector<Tet> removed_cells;for(const auto ci:selected)removed_cells.push_back(mesh[ci]);
  if(preserve_finite_boundary&&boundary_of(removed_cells)!=boundary_of(children)){
    result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;return result;
  }
  std::set<std::size_t> removed(selected.begin(),selected.end());
  for(std::size_t ci=0U;ci<mesh.size();++ci)if(!removed.contains(ci))result.tetrahedra.push_back(mesh[ci]);
  result.tetrahedra.insert(result.tetrahedra.end(),children.begin(),children.end());
  if(validate_entire_mesh&&
     !constraint_mesh_mutation_is_valid(constraints,mesh,result.tetrahedra)) {
    result.tetrahedra.clear();result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;
    return result;
  }
  result.cavity_tetrahedra=std::move(removed_cells);
  result.intersected_tetrahedra=selected.size();
  result.accepted=true;result.failure=CanonicalLiteralEdgeFlipFailure::none;return result;
}

CanonicalLiteralEdgeFlipResult restellarize_bypassed_constraint_edge(
    const CanonicalPlcConstraintSet& constraints,
    const CanonicalPlcSplitVertex& split,const std::vector<Tet>& mesh) {
  CanonicalLiteralEdgeFlipResult result;
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;points.reserve(constraints.vertices.size());
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto p=constraints.vertices[i].position;
    points.push_back({p.x,p.y,p.z});
  }
  if(!index.contains(split.edge[0])||!index.contains(split.edge[1])||
     !index.contains(split.id))return result;
  const auto left=index.at(split.edge[0]),right=index.at(split.edge[1]);
  const auto middle=index.at(split.id);
  std::set<std::size_t> selected;
  std::vector<Tet> children;
  for(std::size_t ci=0U;ci<mesh.size();++ci) {
    const auto& cell=mesh[ci];
    if(std::find(cell.begin(),cell.end(),left)==cell.end()||
       std::find(cell.begin(),cell.end(),right)==cell.end())continue;
    selected.insert(ci);result.cavity_tetrahedra.push_back(cell);
    std::array<std::uint32_t,2> other{};unsigned count{};
    for(const auto vertex:cell)if(vertex!=left&&vertex!=right)
      other[count++]=vertex;
    if(count!=2U)return result;
    for(const auto endpoint:std::array<std::uint32_t,2>{{left,right}}) {
      Tet child{{endpoint,middle,other[0],other[1]}};
      const auto orientation=robust_orient(
          points[child[0]],points[child[1]],points[child[2]],points[child[3]]);
      if(orientation==0)return result;
      if(orientation<0)std::swap(child[0],child[1]);
      children.push_back(child);
    }
  }
  result.intersected_tetrahedra=selected.size();
  if(selected.empty())return result;
  for(std::size_t ci=0U;ci<mesh.size();++ci)
    if(!selected.contains(ci))result.tetrahedra.push_back(mesh[ci]);
  result.tetrahedra.insert(
      result.tetrahedra.end(),children.begin(),children.end());
  if(!constraint_mesh_mutation_is_valid(
         constraints,mesh,result.tetrahedra)) {
    result.tetrahedra.clear();
    result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;
    return result;
  }
  result.accepted=true;
  result.failure=CanonicalLiteralEdgeFlipFailure::none;
  return result;
}

CanonicalLiteralEdgeFlipResult forced_cavity_insert_constraint_vertex(
    const CanonicalPlcConstraintSet& constraints,std::size_t previous_vertex_count,
    const std::vector<Tet>& mesh,const std::vector<Tet>& forced_cavity) {
  CanonicalLiteralEdgeFlipResult result;result.cavity_tetrahedra=forced_cavity;
  result.intersected_tetrahedra=forced_cavity.size();
  if(constraints.vertices.size()!=previous_vertex_count+1U||forced_cavity.empty()) {
    result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;return result;
  }
  std::vector<Point> points;points.reserve(constraints.vertices.size());
  for(const auto& item:constraints.vertices)
    points.push_back({item.position.x,item.position.y,item.position.z});
  const auto vertex=static_cast<std::uint32_t>(previous_vertex_count);
  const auto canonical=[](Tet cell){std::sort(cell.begin(),cell.end());return cell;};
  std::map<Tet,std::size_t> mesh_indices;
  for(std::size_t i=0U;i<mesh.size();++i)mesh_indices.emplace(canonical(mesh[i]),i);
  std::set<std::size_t> selected;
  for(const auto& cell:forced_cavity) {
    const auto found=mesh_indices.find(canonical(cell));
    if(found==mesh_indices.end()||!selected.insert(found->second).second) {
      result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;return result;
    }
  }
  std::map<Face,unsigned> uses;long double old_volume{};
  for(const auto index:selected) {
    const auto& cell=mesh[index];
    old_volume+=std::abs(orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]));
    for(unsigned omit=0U;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=cell[i];++uses[face_key(face)];}
  }
  std::set<Face> boundary;
  for(const auto& [face,count]:uses) {
    if(count==1U)boundary.insert(face);
    else if(count!=2U){result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;return result;}
  }
  std::map<std::uint64_t,std::uint32_t> by_id;
  for(std::size_t i=0U;i<constraints.vertices.size();++i)
    by_id.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
  for(const auto& facet:constraints.facets) {
    Face face{};bool resolved=true;
    for(unsigned i=0U;i<3U;++i){const auto found=by_id.find(facet.vertices[i]);if(found==by_id.end()){resolved=false;break;}face[i]=found->second;}
    if(resolved) {
      const auto key=face_key(face);
      if(uses.contains(key)&&!boundary.contains(key)) {
        result.touches_frozen_facet=true;
        result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;return result;
      }
    }
  }
  std::vector<Tet> replacement;long double new_volume{};
  for(const auto& face:boundary) {
    Tet cell{{vertex,face[0],face[1],face[2]}};
    const auto orientation=robust_orient(points,cell);
    if(orientation==0){result.failure=CanonicalLiteralEdgeFlipFailure::nonpositive_replacement;return result;}
    if(orientation<0)std::swap(cell[0],cell[1]);
    new_volume+=std::abs(orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]));
    replacement.push_back(cell);
  }
  const auto boundary_of=[](const std::vector<Tet>& cells){std::map<Face,unsigned> counts;for(const auto& cell:cells)for(unsigned omit=0U;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=cell[i];++counts[face_key(face)];}std::set<Face> result;for(const auto& [face,count]:counts){if(count==1U)result.insert(face);else if(count!=2U)return std::set<Face>{};}return result;};
  if(boundary_of(replacement)!=boundary||
     std::abs(old_volume-new_volume)>std::max(1.0L,old_volume)*1e-14L) {
    result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;return result;
  }
  for(std::size_t i=0U;i<mesh.size();++i)if(!selected.contains(i))result.tetrahedra.push_back(mesh[i]);
  result.tetrahedra.insert(result.tetrahedra.end(),replacement.begin(),replacement.end());
  // FHC insertion may change unconstrained topology, but it must not undo a
  // constrained segment recovered earlier in the serial transaction.
  const auto mesh_edges=[&](const std::vector<Tet>& cells){
    std::set<std::array<std::uint64_t,2>> edges;
    for(const auto& cell:cells)for(unsigned a=0U;a<4U;++a)for(unsigned b=a+1U;b<4U;++b){
      std::array<std::uint64_t,2> edge{{constraints.vertices[cell[a]].id,
                                       constraints.vertices[cell[b]].id}};
      std::sort(edge.begin(),edge.end());edges.insert(edge);
    }
    return edges;
  };
  const auto before_edges=mesh_edges(mesh),after_edges=mesh_edges(result.tetrahedra);
  for(const auto& edge:constrained_parent_boundary_edges(constraints))
    if(before_edges.contains(edge)&&!after_edges.contains(edge)) {
      result.tetrahedra.clear();
      result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;return result;
    }
  if(!constraint_mesh_mutation_is_valid(constraints,mesh,result.tetrahedra)) {
    result.tetrahedra.clear();result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;
    return result;
  }
  result.accepted=true;result.failure=CanonicalLiteralEdgeFlipFailure::none;return result;
}

std::optional<std::pair<std::uint32_t,std::uint32_t>> endpoint_star_split_ratio(
    const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,2> edge,
    const std::vector<Tet>& mesh) {
  if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
  std::map<std::uint64_t,std::uint32_t> index;std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i){index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));const auto p=constraints.vertices[i].position;points.push_back({p.x,p.y,p.z});}
  const auto first=index.find(edge[0]),second=index.find(edge[1]);if(first==index.end()||second==index.end())return std::nullopt;
  const auto av=constraints.vertices[first->second].position,bv=constraints.vertices[second->second].position;
  for(std::uint32_t denominator=2U;denominator<=(1U<<30U);denominator*=2U) {
    for(unsigned side=0U;side<2U;++side) {
      const auto start=side==0U?av:bv,end=side==0U?bv:av;
      const auto endpoint=side==0U?first->second:second->second;
      const auto t=1.0/static_cast<double>(denominator);
      const Point candidate{start.x+(end.x-start.x)*t,start.y+(end.y-start.y)*t,start.z+(end.z-start.z)*t};
      if(candidate.x==start.x&&candidate.y==start.y&&candidate.z==start.z)continue;
      for(const auto& cell:mesh) {
        if(std::find(cell.begin(),cell.end(),endpoint)==cell.end())continue;
        bool inside=true;
        for(unsigned omit=0U;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=cell[i];const auto opposite=robust_orient(points[face[0]],points[face[1]],points[face[2]],points[cell[omit]]);const auto query=robust_orient(points[face[0]],points[face[1]],points[face[2]],candidate);if(query!=0&&query!=opposite){inside=false;break;}}
        if(inside)return std::pair<std::uint32_t,std::uint32_t>{side==0U?1U:denominator-1U,denominator};
      }
    }
  }
  return std::nullopt;
}

// Hang Si's segment-recovery construction splits a missing segment relative
// to an encroaching mesh vertex, rather than at the first crossed face (which
// can generate a 4095/4096 refinement chain). Keep the new point in the middle
// half of the segment; this is the bounded no-acute-endpoint form of that
// strategy and guarantees both children make geometric progress.
std::optional<std::pair<std::uint32_t,std::uint32_t>> encroaching_split_ratio(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> edge) {
  if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
  const auto first=std::find_if(constraints.vertices.begin(),constraints.vertices.end(),[&](const auto& vertex){return vertex.id==edge[0];});
  const auto second=std::find_if(constraints.vertices.begin(),constraints.vertices.end(),[&](const auto& vertex){return vertex.id==edge[1];});
  if(first==constraints.vertices.end()||second==constraints.vertices.end())return std::nullopt;
  const Point a{first->position.x,first->position.y,first->position.z};
  const Point b{second->position.x,second->position.y,second->position.z};
  const auto direction=b-a;
  const auto length2=dot(direction,direction);
  if(!(length2>0.0L))return std::nullopt;
  std::optional<Point> reference;long double best_score=-1.0L;
  for(const auto& vertex:constraints.vertices) {
    if(vertex.id==edge[0]||vertex.id==edge[1])continue;
    const Point p{vertex.position.x,vertex.position.y,vertex.position.z};
    const auto ap=p-a,bp=p-b;
    if(dot(ap,bp)>64.0L*LDBL_EPSILON*length2)continue;
    const auto normal=cross(direction,ap);
    const auto area2=dot(normal,normal);
    if(area2<=64.0L*LDBL_EPSILON*length2*length2)continue;
    const auto score=dot(ap,ap)*dot(bp,bp)/area2;
    if(score>best_score){best_score=score;reference=p;}
  }
  // Boundary insertion is temporary in the Wang pipeline.  A modest dyadic
  // grid keeps the journal exact through many reverse-removal operations;
  // 1/4096 compounded its denominator at every descendant and overflowed
  // before the planar fixture finished segment recovery.
  constexpr std::uint32_t denominator=1U<<6U;
  long double parameter=0.5L;
  if(reference)parameter=dot(*reference-a,direction)/length2;
  if(!(parameter>=0.25L&&parameter<=0.75L))parameter=0.5L;
  auto numerator=static_cast<std::uint32_t>(std::llround(parameter*denominator));
  numerator=std::clamp(numerator,denominator/4U,3U*denominator/4U);
  const auto divisor=greatest_common_divisor(numerator,denominator);
  return std::pair<std::uint32_t,std::uint32_t>{numerator/divisor,denominator/divisor};
}

std::optional<ConstraintMeshIntersection> find_constraint_edge_mesh_face_intersection(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint64_t,2>>& missing_edges,
    const std::vector<Tet>& mesh,
    const std::set<PoststallIntersectionKey>& excluded) {
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto point=constraints.vertices[i].position;
    points.push_back({point.x,point.y,point.z});
  }
  std::set<Face> mesh_faces;
  for(const auto& cell:mesh)for(unsigned omit=0U;omit<4U;++omit) {
    Face face{};unsigned n{};
    for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=cell[i];
    mesh_faces.insert(face_key(face));
  }
  // This point is temporary and journaled for reverse removal.  A compact
  // dyadic ratio avoids overflowing exact parent-facet provenance when a
  // segment needs several successive recovery insertions.
  constexpr std::uint32_t denominator=1U<<6U;
  for(auto edge:missing_edges) {
    if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
    const auto first=index.find(edge[0]),second=index.find(edge[1]);
    if(first==index.end()||second==index.end())continue;
    const auto a=points[first->second],b=points[second->second];
    for(const auto& face:mesh_faces) {
      if(std::find(face.begin(),face.end(),first->second)!=face.end()||
         std::find(face.begin(),face.end(),second->second)!=face.end())continue;
      const auto oa=robust_orient(points[face[0]],points[face[1]],points[face[2]],a);
      const auto ob=robust_orient(points[face[0]],points[face[1]],points[face[2]],b);
      if(oa==0||ob==0||oa==ob)continue;
      const auto da=orient(points[face[0]],points[face[1]],points[face[2]],a);
      const auto db=orient(points[face[0]],points[face[1]],points[face[2]],b);
      const auto t=da/(da-db);
      if(!(t>0.0L&&t<1.0L))continue;
      const auto q=a+(b-a)*t;
      const auto v0=points[face[1]]-points[face[0]],v1=points[face[2]]-points[face[0]],v2=q-points[face[0]];
      const auto d00=dot(v0,v0),d01=dot(v0,v1),d11=dot(v1,v1),d20=dot(v2,v0),d21=dot(v2,v1);
      const auto determinant=d00*d11-d01*d01;
      if(!(determinant>0.0L))continue;
      const auto u=(d11*d20-d01*d21)/determinant;
      const auto v=(d00*d21-d01*d20)/determinant;
      const auto scale=std::max({1.0L,d00,d11});
      const auto epsilon=2048.0L*LDBL_EPSILON*scale;
      if(!(u>epsilon&&v>epsilon&&u+v<1.0L-epsilon))continue;
      auto numerator=static_cast<std::uint32_t>(std::llround(t*denominator));
      if(numerator==0U||numerator>=denominator)continue;
      const auto divisor=greatest_common_divisor(numerator,denominator);
      numerator/=divisor;
      const auto reduced_denominator=denominator/divisor;
      if(excluded.contains({0U,edge[0],edge[1],numerator,reduced_denominator,0U,0U,0U}))continue;
      return ConstraintMeshIntersection{edge,face,numerator,reduced_denominator};
    }
  }
  return std::nullopt;
}

struct MeshEdgeConstraintFacetIntersection {
  std::array<std::uint64_t,3> constraint_facet{};
  std::array<std::uint64_t,2> mesh_edge{};
  std::array<std::uint32_t,3> barycentric{};
  std::uint32_t denominator{};
};

std::optional<MeshEdgeConstraintFacetIntersection>
find_mesh_edge_constraint_facet_intersection(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint64_t,3>>& missing_facets,
    const std::vector<Tet>& mesh,const std::set<PoststallIntersectionKey>& excluded) {
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i){index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));const auto point=constraints.vertices[i].position;points.push_back({point.x,point.y,point.z});}
  std::set<std::array<std::uint64_t,2>> mesh_edges;
  for(const auto& cell:mesh)for(unsigned a=0U;a<4U;++a)for(unsigned b=a+1U;b<4U;++b){std::array<std::uint64_t,2> edge{{constraints.vertices[cell[a]].id,constraints.vertices[cell[b]].id}};std::sort(edge.begin(),edge.end());mesh_edges.insert(edge);}
  constexpr std::uint32_t denominator=1U<<12U;
  for(auto facet:missing_facets) {
    std::sort(facet.begin(),facet.end());
    std::array<std::uint32_t,3> face{};bool resolved=true;
    for(unsigned i=0U;i<3U;++i){const auto found=index.find(facet[i]);if(found==index.end()){resolved=false;break;}face[i]=found->second;}
    if(!resolved)continue;
    const auto a=points[face[0]],v0=points[face[1]]-a,v1=points[face[2]]-a;
    const auto d00=dot(v0,v0),d01=dot(v0,v1),d11=dot(v1,v1),determinant=d00*d11-d01*d01;
    if(!(determinant>0.0L))continue;
    for(const auto& stable_edge:mesh_edges) {
      if(std::find(facet.begin(),facet.end(),stable_edge[0])!=facet.end()||std::find(facet.begin(),facet.end(),stable_edge[1])!=facet.end())continue;
      const auto first=index.find(stable_edge[0]),second=index.find(stable_edge[1]);if(first==index.end()||second==index.end())continue;
      const auto p=points[first->second],q=points[second->second];
      const auto op=robust_orient(points[face[0]],points[face[1]],points[face[2]],p),oq=robust_orient(points[face[0]],points[face[1]],points[face[2]],q);
      if(op==0||oq==0||op==oq)continue;
      const auto dp=orient(points[face[0]],points[face[1]],points[face[2]],p),dq=orient(points[face[0]],points[face[1]],points[face[2]],q);
      const auto t=dp/(dp-dq);if(!(t>0.0L&&t<1.0L))continue;
      const auto hit=p+(q-p)*t,relative=hit-a;
      const auto d20=dot(relative,v0),d21=dot(relative,v1);
      const auto w1=(d11*d20-d01*d21)/determinant,w2=(d00*d21-d01*d20)/determinant,w0=1.0L-w1-w2;
      const auto epsilon=2048.0L*LDBL_EPSILON;
      if(!(w0>epsilon&&w1>epsilon&&w2>epsilon))continue;
      std::array<std::uint32_t,3> weights{{
          static_cast<std::uint32_t>(std::llround(w0*denominator)),
          static_cast<std::uint32_t>(std::llround(w1*denominator)),0U}};
      if(weights[0]>=denominator||weights[1]>=denominator||weights[0]+weights[1]>=denominator)continue;
      weights[2]=denominator-weights[0]-weights[1];if(weights[0]==0U||weights[1]==0U||weights[2]==0U)continue;
      const PoststallIntersectionKey key{{1U,facet[0],facet[1],facet[2],weights[0],weights[1],weights[2],denominator}};
      if(excluded.contains(key))continue;
      return MeshEdgeConstraintFacetIntersection{facet,stable_edge,weights,denominator};
    }
  }
  return std::nullopt;
}

struct ConstraintMeshEdgeIntersection {
  std::array<std::uint64_t,2> constraint_edge{};
  std::array<std::uint64_t,2> mesh_edge{};
  std::uint32_t numerator{};
  std::uint32_t denominator{};
};

std::optional<ConstraintMeshEdgeIntersection> find_constraint_mesh_edge_intersection(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint64_t,2>>& missing_edges,
    const std::vector<Tet>& mesh,const std::set<PoststallIntersectionKey>& excluded) {
  std::map<std::uint64_t,std::uint32_t> index;std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i){index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));const auto point=constraints.vertices[i].position;points.push_back({point.x,point.y,point.z});}
  std::set<std::array<std::uint64_t,2>> mesh_edges;
  for(const auto& cell:mesh)for(unsigned a=0U;a<4U;++a)for(unsigned b=a+1U;b<4U;++b){std::array<std::uint64_t,2> edge{{constraints.vertices[cell[a]].id,constraints.vertices[cell[b]].id}};std::sort(edge.begin(),edge.end());mesh_edges.insert(edge);}
  constexpr std::uint32_t denominator=1U<<12U;
  for(auto constraint_edge:missing_edges) {
    std::sort(constraint_edge.begin(),constraint_edge.end());
    const auto ai=index.find(constraint_edge[0]),bi=index.find(constraint_edge[1]);if(ai==index.end()||bi==index.end())continue;
    const auto a=points[ai->second],b=points[bi->second],u=b-a;
    for(const auto& mesh_edge:mesh_edges) {
      if(constraint_edge[0]==mesh_edge[0]||constraint_edge[0]==mesh_edge[1]||constraint_edge[1]==mesh_edge[0]||constraint_edge[1]==mesh_edge[1])continue;
      const auto ci=index.find(mesh_edge[0]),di=index.find(mesh_edge[1]);if(ci==index.end()||di==index.end())continue;
      const auto c=points[ci->second],d=points[di->second],v=d-c,w=a-c;
      // Rounded boundary Steiner points need not remain exactly coplanar with
      // the mesh edge they topologically intersect (Wang et al. Sec. 4.3).
      // The closest-line test below is scale-aware and remains authoritative;
      // exact coplanarity is only the fast, unambiguous case.
      const auto uu=dot(u,u),uv=dot(u,v),vv=dot(v,v),uw=dot(u,w),vw=dot(v,w),determinant=uu*vv-uv*uv;
      if(!(determinant>0.0L))continue;
      const auto s=(uv*vw-vv*uw)/determinant,t=(uu*vw-uv*uw)/determinant;
      if(!(s>0.0L&&s<1.0L&&t>0.0L&&t<1.0L))continue;
      const auto on_constraint=a+u*s,on_mesh=c+v*t,difference=on_constraint-on_mesh;
      const auto tolerance=1.0e-20L*std::max({1.0L,uu,vv});
      if(dot(difference,difference)>tolerance)continue;
      auto numerator=static_cast<std::uint32_t>(std::llround(s*denominator));
      if(numerator==0U||numerator>=denominator)continue;
      const auto divisor=greatest_common_divisor(numerator,denominator);numerator/=divisor;const auto reduced_denominator=denominator/divisor;
      const PoststallIntersectionKey key{{2U,constraint_edge[0],constraint_edge[1],mesh_edge[0],mesh_edge[1],numerator,reduced_denominator,0U}};
      if(excluded.contains(key))continue;
      return ConstraintMeshEdgeIntersection{constraint_edge,mesh_edge,numerator,reduced_denominator};
    }
  }
  return std::nullopt;
}

std::size_t constraint_segment_obstruction_count(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> edge,const std::vector<Tet>& mesh) {
  if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
  std::set<PoststallIntersectionKey> excluded;
  std::size_t count{};
  const auto limit=4U*mesh.size()+1U;
  while(count<limit) {
    const auto hit=find_constraint_edge_mesh_face_intersection(
        constraints,{edge},mesh,excluded);
    if(!hit)break;
    const PoststallIntersectionKey key{{0U,edge[0],edge[1],hit->numerator,
                                        hit->denominator,0U,0U,0U}};
    if(!excluded.insert(key).second)break;
    ++count;
  }
  while(count<limit) {
    const auto hit=find_constraint_mesh_edge_intersection(
        constraints,{edge},mesh,excluded);
    if(!hit)break;
    const PoststallIntersectionKey key{{2U,edge[0],edge[1],hit->mesh_edge[0],
        hit->mesh_edge[1],hit->numerator,hit->denominator,0U}};
    if(!excluded.insert(key).second)break;
    ++count;
  }
  return count;
}

// A deliberately small exhaustive retriangulator. It is not an oracle: it
// enumerates only tetrahedra made from the recovered cavity vertices, keeps
// the exact cavity boundary, and is bounded by both vertices and trials.
CanonicalLiteralEdgeFlipResult bounded_cavity_retriangulation(const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,2> edge,const std::vector<Tet>& mesh,const std::vector<Tet>& cavity,std::optional<Point> inserted={},std::size_t maximum_retriangulation_trials=500U){
  CanonicalLiteralEdgeFlipResult result;result.cavity_tetrahedra=cavity;result.intersected_tetrahedra=cavity.size();if(cavity.empty()||cavity.size()>8U){result.failure=CanonicalLiteralEdgeFlipFailure::non_four_cell_cavity;return result;}
  std::map<std::uint64_t,std::uint32_t> index;std::vector<Point> points;for(std::size_t i=0;i<constraints.vertices.size();++i){index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));const auto p=constraints.vertices[i].position;points.push_back({p.x,p.y,p.z});}const auto left=index.find(edge[0]),right=index.find(edge[1]);if(left==index.end()||right==index.end())return result;
  std::map<Face,unsigned> original_uses;std::set<std::uint32_t> vertices;long double target_volume{};for(const auto& cell:cavity){vertices.insert(cell.begin(),cell.end());target_volume+=std::abs(orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]));for(unsigned omit=0;omit<4U;++omit){Face f{};unsigned n=0;for(unsigned i=0;i<4U;++i)if(i!=omit)f[n++]=cell[i];++original_uses[face_key(f)];}}
  std::set<Face> boundary;for(const auto& [face,count]:original_uses)if(count==1U)boundary.insert(face);std::set<std::array<std::uint64_t,3>> frozen;for(const auto& facet:constraints.facets){auto face=facet.vertices;std::sort(face.begin(),face.end());frozen.insert(face);}for(const auto& face:boundary){std::array<std::uint64_t,3> ids{{constraints.vertices[face[0]].id,constraints.vertices[face[1]].id,constraints.vertices[face[2]].id}};if(frozen.contains(ids))result.touches_frozen_facet=true;}
  struct Candidate { Tet cell;std::array<Face,4> faces;bool has_edge{};long double volume{};};std::vector<std::uint32_t> ids(vertices.begin(),vertices.end());if(inserted){ids.push_back(static_cast<std::uint32_t>(points.size()));points.push_back(*inserted);result.inserted_steiner_vertex=Vec3{static_cast<double>(inserted->x),static_cast<double>(inserted->y),static_cast<double>(inserted->z)};}std::vector<Candidate> candidates;
  for(std::size_t a=0;a<ids.size();++a)for(std::size_t b=a+1;b<ids.size();++b)for(std::size_t c=b+1;c<ids.size();++c)for(std::size_t d=c+1;d<ids.size();++d){Tet cell{{ids[a],ids[b],ids[c],ids[d]}};auto volume=orient(points[cell[0]],points[cell[1]],points[cell[2]],points[cell[3]]);if(std::abs(volume)<1e-20L)continue;if(volume<0)std::swap(cell[0],cell[1]);Candidate candidate;candidate.cell=cell;candidate.volume=std::abs(volume);candidate.has_edge=std::find(cell.begin(),cell.end(),left->second)!=cell.end()&&std::find(cell.begin(),cell.end(),right->second)!=cell.end();for(unsigned omit=0;omit<4U;++omit){Face f{};unsigned n=0;for(unsigned i=0;i<4U;++i)if(i!=omit)f[n++]=cell[i];candidate.faces[omit]=face_key(f);}candidates.push_back(candidate);}
  if(candidates.empty()){result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;return result;}std::map<Face,std::vector<std::size_t>> by_boundary;for(std::size_t i=0;i<candidates.size();++i)for(const auto& face:candidates[i].faces)if(boundary.contains(face))by_boundary[face].push_back(i);
  std::map<Face,unsigned> uses;std::vector<std::size_t> chosen;std::set<std::size_t> chosen_set;std::size_t trials{};bool trial_limit{};std::function<bool(long double,bool)> search=[&](long double volume,bool has_edge){if(++trials>maximum_retriangulation_trials){trial_limit=true;return false;}if(chosen.size()>24U)return false;Face next{};bool need=false;for(const auto& face:boundary)if(uses[face]==0U){next=face;need=true;break;}if(!need){if(!has_edge||std::abs(volume-target_volume)>std::max(1.0L,target_volume)*1e-14L)return false;for(const auto& [face,count]:uses)if(count!=0U&&(boundary.contains(face)?count!=1U:count!=2U))return false;return true;}for(const auto ci:by_boundary[next])if(!chosen_set.contains(ci)){const auto& candidate=candidates[ci];bool valid=true;for(const auto& face:candidate.faces){const auto count=uses[face]+1U;if((boundary.contains(face)&&count>1U)||(!boundary.contains(face)&&count>2U)){valid=false;break;}}if(!valid)continue;for(const auto& face:candidate.faces)++uses[face];chosen.push_back(ci);chosen_set.insert(ci);if(search(volume+candidate.volume,has_edge||candidate.has_edge))return true;chosen_set.erase(ci);chosen.pop_back();for(const auto& face:candidate.faces)--uses[face];}return false;};
  if(!search(0.,false)){result.retriangulation_trials=trials;result.failure=trial_limit?CanonicalLiteralEdgeFlipFailure::retriangulation_trial_limit:CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;return result;}result.retriangulation_trials=trials;const auto canonical=[](Tet cell){std::sort(cell.begin(),cell.end());return cell;};std::set<Tet> removed;for(const auto& cell:cavity)removed.insert(canonical(cell));for(const auto& cell:mesh)if(!removed.contains(canonical(cell)))result.tetrahedra.push_back(cell);for(const auto ci:chosen)result.tetrahedra.push_back(candidates[ci].cell);result.accepted=true;result.failure=CanonicalLiteralEdgeFlipFailure::none;return result;
}

CanonicalLiteralEdgeFlipResult recover_segment_with_fhc_steiner(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> edge,const std::vector<Tet>& mesh,
    const std::vector<Tet>& cavity,std::size_t maximum_attempts) {
  CanonicalLiteralEdgeFlipResult result;
  result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;
  result.cavity_tetrahedra=cavity;
  if(cavity.empty()||cavity.size()>8U||maximum_attempts==0U)return result;

  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto p=constraints.vertices[i].position;
    points.push_back({p.x,p.y,p.z});
  }
  const auto left=index.find(edge[0]),right=index.find(edge[1]);
  if(left==index.end()||right==index.end())return result;
  const auto segment_midpoint=(points[left->second]+points[right->second])*.5L;

  // Wang et al. place the point inside the flip-hard tetrahedral cavity, not
  // on the PLC segment.  Cell centroids are exact convex combinations and
  // therefore safe interior candidates; segment-biased combinations direct
  // the Bowyer-Watson-style cavity change towards the missing segment.
  std::vector<Point> candidates;
  Point weighted{};long double total_weight{};
  for(const auto& cell:cavity) {
    const auto centroid=(points[cell[0]]+points[cell[1]]+points[cell[2]]+
                         points[cell[3]])*.25L;
    const auto volume=std::abs(orient(points[cell[0]],points[cell[1]],
                                      points[cell[2]],points[cell[3]]));
    candidates.push_back(centroid);
    candidates.push_back(centroid*.75L+segment_midpoint*.25L);
    weighted=weighted+centroid*volume;total_weight+=volume;
  }
  if(total_weight>0.0L) {
    const auto centre=weighted*(1.0L/total_weight);
    candidates.push_back(centre);
    candidates.push_back(centre*.75L+segment_midpoint*.25L);
  }
  std::sort(candidates.begin(),candidates.end(),[](Point a,Point b){
    return std::tie(a.x,a.y,a.z)<std::tie(b.x,b.y,b.z);
  });
  candidates.erase(std::unique(candidates.begin(),candidates.end(),
      [](Point a,Point b){return a.x==b.x&&a.y==b.y&&a.z==b.z;}),
      candidates.end());
  std::size_t attempts{};
  for(const auto candidate:candidates) {
    if(attempts++==maximum_attempts)break;
    std::uint64_t next{};
    for(const auto& vertex:constraints.vertices)next=std::max(next,vertex.id);
    if(next==std::numeric_limits<std::uint64_t>::max())break;
    auto augmented=constraints;
    augmented.vertices.push_back({next+1U,
        {static_cast<double>(candidate.x),static_cast<double>(candidate.y),
         static_cast<double>(candidate.z)}});
    auto inserted=forced_cavity_insert_constraint_vertex(
        augmented,constraints.vertices.size(),mesh,cavity);
    ++result.retriangulation_trials;
    if(!inserted.accepted)continue;
    inserted.inserted_steiner_vertex=augmented.vertices.back().position;
    return inserted;
  }
  return result;
}

using IntersectionSimplexKey=std::array<std::uint64_t,5>;

struct ExactSegmentFaceIntersection {
  Face mesh_face{};
  Point s0{};
  long double parameter{};
};

std::vector<ExactSegmentFaceIntersection> exact_segment_face_intersections(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,const std::vector<Tet>& mesh) {
  std::sort(segment.begin(),segment.end());
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto p=constraints.vertices[i].position;
    points.push_back({p.x,p.y,p.z});
  }
  if(!index.contains(segment[0])||!index.contains(segment[1]))return {};
  const auto a=points[index.at(segment[0])],b=points[index.at(segment[1])];
  std::set<Face> unique_faces;
  for(const auto& cell:mesh)for(unsigned omitted=0U;omitted<4U;++omitted) {
    Face face{};unsigned cursor{};
    for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];
    unique_faces.insert(face_key(face));
  }
  std::vector<Face> faces(unique_faces.begin(),unique_faces.end());
  const auto stable_face=[&](const Face& face) {
    std::array<std::uint64_t,3> ids{{constraints.vertices[face[0]].id,
        constraints.vertices[face[1]].id,constraints.vertices[face[2]].id}};
    std::sort(ids.begin(),ids.end());return ids;
  };
  std::sort(faces.begin(),faces.end(),[&](const Face& left,const Face& right) {
    return stable_face(left)<stable_face(right);
  });
  std::vector<ExactSegmentFaceIntersection> result;
  for(const auto& face:faces) {
    const auto ids=stable_face(face);
    if(std::find(ids.begin(),ids.end(),segment[0])!=ids.end()||
       std::find(ids.begin(),ids.end(),segment[1])!=ids.end())continue;
    const auto sign_a=robust_orient(
        points[face[0]],points[face[1]],points[face[2]],a);
    const auto sign_b=robust_orient(
        points[face[0]],points[face[1]],points[face[2]],b);
    if(sign_a==0||sign_b==0||sign_a==sign_b)continue;
    const auto distance_a=orient(
        points[face[0]],points[face[1]],points[face[2]],a);
    const auto distance_b=orient(
        points[face[0]],points[face[1]],points[face[2]],b);
    const auto parameter=distance_a/(distance_a-distance_b);
    if(!(parameter>0.0L&&parameter<1.0L))continue;
    // Classification and ordering remain predicate-driven, while the
    // published intersection coordinate follows lin_tri_intersect3d's
    // double-domain fixed split point from the pinned implementation.
    const auto reference_hit=wang_segment_plane_hit(
        as_vec3(a),as_vec3(b),as_vec3(points[face[0]]),
        as_vec3(points[face[1]]),as_vec3(points[face[2]]));
    const Point hit{reference_hit.x,reference_hit.y,reference_hit.z};
    const auto origin=points[face[0]];
    const auto v0=points[face[1]]-origin,v1=points[face[2]]-origin;
    const auto relative=hit-origin;
    const auto d00=dot(v0,v0),d01=dot(v0,v1),d11=dot(v1,v1);
    const auto denominator=d00*d11-d01*d01;
    if(!(denominator>0.0L))continue;
    const auto d20=dot(relative,v0),d21=dot(relative,v1);
    const auto w1=(d11*d20-d01*d21)/denominator;
    const auto w2=(d00*d21-d01*d20)/denominator;
    const auto w0=1.0L-w1-w2;
    const auto epsilon=256.0L*LDBL_EPSILON;
    if(w0>epsilon&&w1>epsilon&&w2>epsilon)
      result.push_back({face,hit,parameter});
  }
  std::sort(result.begin(),result.end(),[&](const auto& left,const auto& right) {
    if(left.parameter!=right.parameter)return left.parameter<right.parameter;
    return stable_face(left.mesh_face)<stable_face(right.mesh_face);
  });
  return result;
}

std::optional<EndpointStarFeature>
first_segment_feature_from_endpoint_star(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,const std::vector<Tet>& mesh,
    bool reverse_direction) {
  std::sort(segment.begin(),segment.end());
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  points.reserve(constraints.vertices.size());
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto p=constraints.vertices[i].position;
    points.push_back({p.x,p.y,p.z});
  }
  if(!index.contains(segment[0])||!index.contains(segment[1]))
    return std::nullopt;
  const auto start_id=reverse_direction?segment[1]:segment[0];
  const auto end_id=reverse_direction?segment[0]:segment[1];
  const auto start_index=index.at(start_id),end_index=index.at(end_id);
  const auto start=points[start_index],end=points[end_index];
  const auto stable_feature=[&](const EndpointStarFeature& feature) {
    std::array<std::uint64_t,4> key{{
        static_cast<std::uint64_t>(feature.kind),0U,0U,0U}};
    if(feature.kind==EndpointStarFeatureKind::vertex) {
      key[1]=constraints.vertices[feature.vertex].id;
    } else if(feature.kind==EndpointStarFeatureKind::edge) {
      key[1]=constraints.vertices[feature.edge[0]].id;
      key[2]=constraints.vertices[feature.edge[1]].id;
      if(key[2]<key[1])std::swap(key[1],key[2]);
    } else if(feature.kind==EndpointStarFeatureKind::face) {
      key[1]=constraints.vertices[feature.face[0]].id;
      key[2]=constraints.vertices[feature.face[1]].id;
      key[3]=constraints.vertices[feature.face[2]].id;
      std::sort(key.begin()+1,key.end());
    }
    return key;
  };
  std::optional<EndpointStarFeature> best;
  bool start_is_in_mesh=false;
  for(const auto& cell:mesh) {
    const auto start_position=std::find(cell.begin(),cell.end(),start_index);
    if(start_position==cell.end())continue;
    start_is_in_mesh=true;
    if(std::find(cell.begin(),cell.end(),end_index)!=cell.end()) {
      EndpointStarFeature recovered;
      recovered.kind=EndpointStarFeatureKind::recovered_segment;
      recovered.source_cell=cell;
      return recovered;
    }
    std::array<std::uint32_t,3> opposite{};unsigned cursor{};
    for(const auto vertex:cell)if(vertex!=start_index)opposite[cursor++]=vertex;
    const auto b=opposite[0],c=opposite[1],d=opposite[2];
    const auto sign_start=robust_orient(points[b],points[c],points[d],start);
    const auto sign_end=robust_orient(points[b],points[c],points[d],end);
    if(sign_start==0||sign_end==0||sign_start==sign_end)continue;
    const auto distance_start=orient(points[b],points[c],points[d],start);
    const auto distance_end=orient(points[b],points[c],points[d],end);
    const auto parameter=distance_start/(distance_start-distance_end);
    if(!(parameter>0.0L&&parameter<1.0L))continue;

    // These are the three finddirection signs in barycentric order.  Relative
    // to orient(start,b,c,d), they are the exact weights of b, c, and d at
    // the ray's intersection with the opposite face.  Zero weights therefore
    // reproduce the pinned vertex/edge/face classification without a
    // floating-point tolerance.
    const auto volume_sign=robust_orient(start,points[b],points[c],points[d]);
    const std::array<int,3> weights{{
        robust_orient(start,points[c],points[d],end),
        robust_orient(start,points[d],points[b],end),
        robust_orient(start,points[b],points[c],end)}};
    if(volume_sign==0||std::any_of(weights.begin(),weights.end(),
         [&](const auto weight){return weight!=0&&weight!=volume_sign;}))
      continue;
    const auto nonzero=static_cast<unsigned>(std::count_if(
        weights.begin(),weights.end(),[](const auto weight){return weight!=0;}));
    if(nonzero==0U)continue;

    EndpointStarFeature candidate;
    candidate.source_cell=cell;
    candidate.parameter=parameter;
    candidate.s0=start+(end-start)*parameter;
    if(nonzero==1U) {
      candidate.kind=EndpointStarFeatureKind::vertex;
      candidate.vertex=opposite[static_cast<std::size_t>(
          std::find_if(weights.begin(),weights.end(),
                       [](const auto weight){return weight!=0;})-weights.begin())];
    } else if(nonzero==2U) {
      candidate.kind=EndpointStarFeatureKind::edge;
      unsigned edge_cursor{};
      for(unsigned i=0U;i<3U;++i)
        if(weights[i]!=0)candidate.edge[edge_cursor++]=opposite[i];
      std::sort(candidate.edge.begin(),candidate.edge.end());
    } else {
      candidate.kind=EndpointStarFeatureKind::face;
      candidate.face=face_key(Face{{b,c,d}});
      const auto reference_hit=wang_segment_plane_hit(
          as_vec3(start),as_vec3(end),as_vec3(points[b]),
          as_vec3(points[c]),as_vec3(points[d]));
      candidate.s0={reference_hit.x,reference_hit.y,reference_hit.z};
    }
    if(!best||candidate.parameter<best->parameter||
       (candidate.parameter==best->parameter&&
        stable_feature(candidate)<stable_feature(*best)))
      best=std::move(candidate);
  }
  // The public local-flip primitive also has historical synthetic tests whose
  // supplied mesh omits both constraint endpoints.  Pinned finddirection
  // cannot represent that state, while the production Wang seed always
  // contains every input vertex.  Preserve the primitive's exhaustive
  // diagnostic behavior only for that non-production input shape.
  if(!start_is_in_mesh) {
    auto all=exact_segment_face_intersections(constraints,segment,mesh);
    if(all.empty())return std::nullopt;
    const auto& selected=reverse_direction?all.back():all.front();
    EndpointStarFeature fallback;
    fallback.kind=EndpointStarFeatureKind::face;
    fallback.face=selected.mesh_face;
    for(const auto& cell:mesh)
      if(std::all_of(selected.mesh_face.begin(),selected.mesh_face.end(),
           [&](const auto vertex) {
             return std::find(cell.begin(),cell.end(),vertex)!=cell.end();
           })) {fallback.source_cell=cell;break;}
    fallback.s0=selected.s0;
    fallback.parameter=selected.parameter;
    return fallback;
  }
  return best;
}

CanonicalLiteralEdgeFlipResult face_flip_for_segment(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,const std::vector<Tet>& mesh,
    bool first_intersection_only,bool reverse_direction) {
  CanonicalLiteralEdgeFlipResult fallback;
  fallback.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;
  bool has_specific_failure=false;
  std::vector<ExactSegmentFaceIntersection> intersections;
  if(first_intersection_only) {
    if(const auto first=first_segment_feature_from_endpoint_star(
           constraints,segment,mesh,reverse_direction);
       first&&first->kind==EndpointStarFeatureKind::face)
      intersections.push_back({first->face,first->s0,first->parameter});
  } else {
    intersections=exact_segment_face_intersections(constraints,segment,mesh);
    if(reverse_direction)
      std::reverse(intersections.begin(),intersections.end());
  }
  for(const auto& intersection:intersections) {
    auto candidate=face_flip_for_specific_face(
        constraints,mesh,intersection.mesh_face);
    if(candidate.accepted)return candidate;
    // Prefer the Definition 3.2 evidence over boundary/nonmanifold faces, but
    // keep searching because another crossed interior face may be flippable.
    if(candidate.blocking_mesh_edge) {
      fallback=std::move(candidate);
      has_specific_failure=true;
    } else if(!has_specific_failure&&
              candidate.failure!=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star) {
      fallback=std::move(candidate);
    }
    if(first_intersection_only)break;
  }
  return fallback;
}

std::optional<Point> exact_segment_edge_intersection(
    Point a,Point b,Point c,Point d) {
  if(robust_orient(a,b,c,d)!=0)return std::nullopt;
  const auto u=b-a,v=d-c,w=a-c;
  const auto uu=dot(u,u),uv=dot(u,v),vv=dot(v,v);
  const auto determinant=uu*vv-uv*uv;
  if(!(determinant>0.0L))return std::nullopt;
  const auto uw=dot(u,w),vw=dot(v,w);
  const auto s=(uv*vw-vv*uw)/determinant;
  const auto t=(uu*vw-uv*uw)/determinant;
  if(!(s>0.0L&&s<1.0L&&t>0.0L&&t<1.0L))return std::nullopt;
  const auto on_segment=a+u*s,on_edge=c+v*t;
  const auto delta=on_segment-on_edge;
  const auto tolerance=64.0L*LDBL_EPSILON*
      std::max({1.0L,uu,vv});
  if(dot(delta,delta)>tolerance*tolerance)return std::nullopt;
  // This is the unquantized line/line solution. The older post-stall key's
  // 1/4096 rational is deliberately not used for Cascade S0.
  return on_segment;
}

std::set<IntersectionSimplexKey> segment_intersection_simplices(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,const std::vector<Tet>& mesh) {
  std::sort(segment.begin(),segment.end());
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto p=constraints.vertices[i].position;
    points.push_back({p.x,p.y,p.z});
  }
  const auto first=index.find(segment[0]),second=index.find(segment[1]);
  if(first==index.end()||second==index.end())return {};
  const auto a=points[first->second],b=points[second->second];
  std::set<std::array<std::uint32_t,2>> edges;
  std::set<Face> faces;
  for(const auto& cell:mesh) {
    for(unsigned i=0U;i<4U;++i)for(unsigned j=i+1U;j<4U;++j) {
      auto edge=std::array<std::uint32_t,2>{{cell[i],cell[j]}};
      std::sort(edge.begin(),edge.end());edges.insert(edge);
    }
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];
      faces.insert(face_key(face));
    }
  }
  std::set<IntersectionSimplexKey> result;
  for(const auto& edge:edges) {
    const auto id0=constraints.vertices[edge[0]].id;
    const auto id1=constraints.vertices[edge[1]].id;
    if(id0==segment[0]||id0==segment[1]||
       id1==segment[0]||id1==segment[1])continue;
    if(exact_segment_edge_intersection(
           a,b,points[edge[0]],points[edge[1]])) {
      auto stable=std::array<std::uint64_t,2>{{id0,id1}};
      std::sort(stable.begin(),stable.end());
      result.insert({{1U,stable[0],stable[1],0U,0U}});
    }
  }
  for(const auto& face:faces) {
    const std::array<std::uint64_t,3> ids{{
        constraints.vertices[face[0]].id,constraints.vertices[face[1]].id,
        constraints.vertices[face[2]].id}};
    if(std::find(ids.begin(),ids.end(),segment[0])!=ids.end()||
       std::find(ids.begin(),ids.end(),segment[1])!=ids.end())continue;
    const auto sa=robust_orient(
        points[face[0]],points[face[1]],points[face[2]],a);
    const auto sb=robust_orient(
        points[face[0]],points[face[1]],points[face[2]],b);
    if(sa==0||sb==0||sa==sb)continue;
    const auto da=orient(points[face[0]],points[face[1]],points[face[2]],a);
    const auto db=orient(points[face[0]],points[face[1]],points[face[2]],b);
    const auto t=da/(da-db);
    if(!(t>0.0L&&t<1.0L))continue;
    const auto hit=a+(b-a)*t;
    const auto origin=points[face[0]];
    const auto v0=points[face[1]]-origin,v1=points[face[2]]-origin;
    const auto relative=hit-origin;
    const auto d00=dot(v0,v0),d01=dot(v0,v1),d11=dot(v1,v1);
    const auto denominator=d00*d11-d01*d01;
    if(!(denominator>0.0L))continue;
    const auto d20=dot(relative,v0),d21=dot(relative,v1);
    const auto w1=(d11*d20-d01*d21)/denominator;
    const auto w2=(d00*d21-d01*d20)/denominator;
    const auto w0=1.0L-w1-w2;
    const auto epsilon=256.0L*LDBL_EPSILON;
    if(!(w0>epsilon&&w1>epsilon&&w2>epsilon))continue;
    auto stable=ids;std::sort(stable.begin(),stable.end());
    result.insert({{2U,stable[0],stable[1],stable[2],0U}});
  }
  return result;
}

bool mesh_contains_stable_edge(const CanonicalPlcConstraintSet& constraints,
                               std::array<std::uint64_t,2> edge,
                               const std::vector<Tet>& mesh) {
  std::sort(edge.begin(),edge.end());
  for(const auto& cell:mesh)for(unsigned i=0U;i<4U;++i)
    for(unsigned j=i+1U;j<4U;++j) {
      auto stable=std::array<std::uint64_t,2>{{
          constraints.vertices[cell[i]].id,constraints.vertices[cell[j]].id}};
      std::sort(stable.begin(),stable.end());
      if(stable==edge)return true;
    }
  return false;
}

bool preserves_previously_recovered_constraints(
    const CanonicalPlcConstraintSet& constraints,const std::vector<Tet>& before,
    const std::vector<Tet>& after) {
  const auto old=inspect_canonical_plc_tetrahedra(constraints,before);
  const auto now=inspect_canonical_plc_tetrahedra(constraints,after);
  const std::set<std::array<std::uint64_t,2>> old_missing_edges(
      old.missing_edges.begin(),old.missing_edges.end());
  const std::set<std::array<std::uint64_t,3>> old_missing_facets(
      old.missing_facets.begin(),old.missing_facets.end());
  return std::none_of(now.missing_edges.begin(),now.missing_edges.end(),
             [&](const auto& edge){return !old_missing_edges.contains(edge);})&&
      std::none_of(now.missing_facets.begin(),now.missing_facets.end(),
             [&](const auto& face){return !old_missing_facets.contains(face);});
}
}

std::optional<WangEndpointStarFeatureDiagnostic>
inspect_wang_endpoint_star_feature(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    bool reverse_direction) {
  const auto selected=first_segment_feature_from_endpoint_star(
      constraints,segment,tetrahedra,reverse_direction);
  if(!selected)return std::nullopt;
  WangEndpointStarFeatureDiagnostic result;
  result.kind=static_cast<WangEndpointStarFeatureKind>(selected->kind);
  result.parameter=selected->parameter;
  const auto stable=[&](std::uint32_t index) {
    return index<constraints.vertices.size()?constraints.vertices[index].id:0U;
  };
  for(unsigned i=0;i<4U;++i)
    result.source_tetrahedron[i]=stable(selected->source_cell[i]);
  if(selected->kind==EndpointStarFeatureKind::vertex)
    result.feature[0]=stable(selected->vertex);
  else if(selected->kind==EndpointStarFeatureKind::edge) {
    result.feature[0]=stable(selected->edge[0]);
    result.feature[1]=stable(selected->edge[1]);
  } else if(selected->kind==EndpointStarFeatureKind::face)
    for(unsigned i=0;i<3U;++i)result.feature[i]=stable(selected->face[i]);
  return result;
}

CanonicalLiteralEdgeFlipResult try_recover_literal_edge_by_four_to_four(const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,2> edge,std::size_t maximum_tetrahedra){
  CanonicalDelaunaySeedInput input;input.maximum_vertices=constraints.vertices.size();input.maximum_tetrahedra=maximum_tetrahedra;input.exact_affine_planes=constraints.exact_affine_planes;for(const auto& vertex:constraints.vertices){input.vertices.push_back(vertex.position);input.stable_vertex_ids.push_back(vertex.id);}const auto seed=build_canonical_background_seed(input);if(!seed.accepted()){CanonicalLiteralEdgeFlipResult result;result.failure=CanonicalLiteralEdgeFlipFailure::seed_failed;return result;}return try_recover_literal_edge_by_four_to_four(constraints,edge,seed.tetrahedra);
}
CanonicalLiteralEdgeFlipResult try_recover_literal_edge_by_four_to_four(const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,2> edge,const std::vector<std::array<std::uint32_t,4>>& tetrahedra){return four_to_four_in_mesh(constraints,edge,tetrahedra);}
CanonicalLiteralEdgeFlipResult try_recover_literal_edge_by_face_flip(
    const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,2> edge,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra) {
  return face_flip_for_segment(constraints,edge,tetrahedra);
}
CanonicalLiteralEdgeFlipResult try_remove_mesh_edge_by_flip(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint32_t,2> mesh_edge,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra) {
  return remove_mesh_edge_in_mesh(constraints,mesh_edge,tetrahedra);
}
WangSegmentLocalFlipResult try_recover_wang_segment_by_local_flips(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    WangSegmentFlipSearchMode search_mode,bool reverse_direction,
    std::size_t search_depth) {
  WangSegmentLocalFlipResult result;
  if(segment[1]<segment[0])std::swap(segment[0],segment[1]);

  const auto contains_segment=[&](const std::vector<Tet>& mesh) {
    std::map<std::uint64_t,std::uint32_t> index;
    for(std::size_t i=0U;i<constraints.vertices.size();++i)
      index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto first=index.find(segment[0]),second=index.find(segment[1]);
    if(first==index.end()||second==index.end())return false;
    return std::any_of(mesh.begin(),mesh.end(),[&](const auto& cell) {
      return std::find(cell.begin(),cell.end(),first->second)!=cell.end()&&
             std::find(cell.begin(),cell.end(),second->second)!=cell.end();
    });
  };
  if(contains_segment(tetrahedra)) {
    result.flip.accepted=true;
    result.flip.failure=CanonicalLiteralEdgeFlipFailure::none;
    result.flip.tetrahedra=tetrahedra;
    result.segment_recovered=true;
    return result;
  }

  const auto preserves_recovered_constraints=[&](const std::vector<Tet>& mesh) {
    // Most pinned recoverEdgebyFlip calls find no admissible mutation.  The
    // preservation comparison is a commit gate, so derive its before-state
    // only after a local operation has produced a candidate to commit.
    const auto before=inspect_canonical_plc_tetrahedra(
        constraints,tetrahedra);
    const std::set<std::array<std::uint64_t,2>> missing_edges_before(
        before.missing_edges.begin(),before.missing_edges.end());
    const std::set<std::array<std::uint64_t,3>> missing_facets_before(
        before.missing_facets.begin(),before.missing_facets.end());
    const auto after=inspect_canonical_plc_tetrahedra(constraints,mesh);
    const auto lost_edge=std::any_of(
        after.missing_edges.begin(),after.missing_edges.end(),
        [&](const auto& edge){return !missing_edges_before.contains(edge);});
    const auto lost_facet=std::any_of(
        after.missing_facets.begin(),after.missing_facets.end(),
        [&](const auto& facet){return !missing_facets_before.contains(facet);});
    return !lost_edge&&!lost_facet;
  };
  const auto validate=[&](CanonicalLiteralEdgeFlipResult candidate) {
    if(!candidate.accepted)return candidate;
    if(!constraint_mesh_mutation_is_valid(
           constraints,tetrahedra,candidate.tetrahedra)) {
      candidate.accepted=false;
      candidate.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;
      candidate.tetrahedra.clear();
      return candidate;
    }
    if(!preserves_recovered_constraints(candidate.tetrahedra)) {
      result.preserved_recovered_constraints=false;
      candidate.accepted=false;
      candidate.failure=CanonicalLiteralEdgeFlipFailure::frozen_cavity_boundary;
      candidate.tetrahedra.clear();
    }
    return candidate;
  };

  // Algorithm 2 line 4 and Section 3.1: face removal may use generalized
  // edge removal to eliminate a reflex edge before retrying the crossed face.
  // Pinned recoverEdgebyFlip uses a directional first-obstruction walk for
  // ordinary search. Its info&1 branch instead enumerates all intersections.
  const auto generalized=generalized_face_flip_for_segment(
      constraints,segment,tetrahedra,
      search_mode==WangSegmentFlipSearchMode::easy,reverse_direction,
      search_depth);
  result.edge_removal_attempts=generalized.edge_removal_attempts;
  result.edge_removals=generalized.edge_removals;
  result.edge_retriangulation_trials=generalized.edge_retriangulation_trials;
  result.maximum_edge_degree=generalized.maximum_edge_degree;
  result.constraints=generalized.constraints;
  result.boundary_vertex_attached=generalized.boundary_vertex_attached;
  result.attached_boundary_vertex=generalized.attached_boundary_vertex;
  result.flip=validate(generalized.flip);

  // A 4-to-4 bistellar move is another classical local flip. It is attempted
  // only when generalized face/edge removal cannot change the configuration.
  if(!result.flip.accepted&&!result.boundary_vertex_attached) {
    auto four_to_four=validate(
        four_to_four_in_mesh(constraints,segment,tetrahedra));
    if(four_to_four.accepted)result.flip=std::move(four_to_four);
  }
  result.segment_recovered=result.boundary_vertex_attached||
      (result.flip.accepted&&contains_segment(result.flip.tetrahedra));
  return result;
}

WangFacetLocalFlipResult try_recover_wang_facet_by_local_flips(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,3> facet,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::size_t maximum_passes) {
  WangFacetLocalFlipResult result;
  result.tetrahedra=tetrahedra;
  std::sort(facet.begin(),facet.end());
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto p=constraints.vertices[i].position;
    points.push_back({p.x,p.y,p.z});
  }
  Face target{};
  for(unsigned i=0U;i<3U;++i) {
    const auto found=index.find(facet[i]);
    if(found==index.end())return result;
    target[i]=found->second;
  }
  const auto contains_target=[&](const std::vector<Tet>& mesh) {
    return std::any_of(mesh.begin(),mesh.end(),[&](const Tet& cell) {
      return std::all_of(target.begin(),target.end(),[&](std::uint32_t vertex) {
        return std::find(cell.begin(),cell.end(),vertex)!=cell.end();
      });
    });
  };
  if(contains_target(result.tetrahedra)) {
    result.facet_recovered=true;
    return result;
  }
  const auto constrained_edges=constrained_parent_boundary_edges(constraints);
  const auto canonical_topology=[](const std::vector<Tet>& mesh) {
    std::vector<Tet> key=mesh;
    for(auto& cell:key)std::sort(cell.begin(),cell.end());
    std::sort(key.begin(),key.end());
    return key;
  };
  std::set<std::vector<Tet>> visited;
  for(std::size_t pass=0U;pass<maximum_passes;++pass) {
    if(!visited.insert(canonical_topology(result.tetrahedra)).second)return result;
    std::set<std::array<std::uint32_t,2>> mesh_edges;
    for(const auto& cell:result.tetrahedra)
      for(unsigned i=0U;i<4U;++i)for(unsigned j=i+1U;j<4U;++j) {
        std::array<std::uint32_t,2> edge{{cell[i],cell[j]}};
        std::sort(edge.begin(),edge.end());
        mesh_edges.insert(edge);
      }
    struct Crossing {
      std::array<std::uint64_t,2> stable{};
      std::array<std::uint32_t,2> mesh{};
    };
    std::vector<Crossing> crossings;
    const auto a=points[target[0]],b=points[target[1]],c=points[target[2]];
    const auto ab=b-a,ac=c-a;
    const auto d00=dot(ab,ab),d01=dot(ab,ac),d11=dot(ac,ac);
    const auto determinant=d00*d11-d01*d01;
    if(!(determinant>0.0L))return result;
    for(const auto& edge:mesh_edges) {
      std::array<std::uint64_t,2> stable{{
          constraints.vertices[edge[0]].id,constraints.vertices[edge[1]].id}};
      std::sort(stable.begin(),stable.end());
      if(std::find(facet.begin(),facet.end(),stable[0])!=facet.end()||
         std::find(facet.begin(),facet.end(),stable[1])!=facet.end()||
         constrained_edges.contains(stable))continue;
      const auto p=points[edge[0]],q=points[edge[1]];
      const auto sp=robust_orient(a,b,c,p),sq=robust_orient(a,b,c,q);
      if(sp==0||sq==0||sp==sq)continue;
      const auto dp=orient(a,b,c,p),dq=orient(a,b,c,q);
      const auto t=dp/(dp-dq);
      if(!(t>0.0L&&t<1.0L))continue;
      const auto relative=(p+(q-p)*t)-a;
      const auto d20=dot(relative,ab),d21=dot(relative,ac);
      const auto w1=(d11*d20-d01*d21)/determinant;
      const auto w2=(d00*d21-d01*d20)/determinant;
      const auto w0=1.0L-w1-w2;
      const auto epsilon=512.0L*LDBL_EPSILON;
      if(w0<-epsilon||w1<-epsilon||w2<-epsilon)continue;
      const auto edge_contacts=static_cast<unsigned>(std::abs(w0)<=epsilon)+
          static_cast<unsigned>(std::abs(w1)<=epsilon)+
          static_cast<unsigned>(std::abs(w2)<=epsilon);
      if(edge_contacts>1U)continue;
      crossings.push_back({stable,edge});
    }
    std::sort(crossings.begin(),crossings.end(),[](const auto& left,const auto& right) {
      return left.stable<right.stable;
    });
    result.intersecting_edges+=crossings.size();
    bool changed=false;
    for(const auto& crossing:crossings) {
      ++result.edge_removal_attempts;
      auto removed=remove_mesh_edge_in_mesh(
          constraints,crossing.mesh,result.tetrahedra);
      result.edge_retriangulation_trials+=removed.retriangulation_trials;
      if(!removed.accepted)continue;
      if(!preserves_previously_recovered_constraints(
             constraints,result.tetrahedra,removed.tetrahedra)) {
        result.preserved_recovered_constraints=false;
        continue;
      }
      result.tetrahedra=std::move(removed.tetrahedra);
      ++result.edge_removals;
      changed=true;
      break;
    }
    if(contains_target(result.tetrahedra)) {
      result.facet_recovered=true;
      return result;
    }
    if(!changed)return result;
  }
  return result;
}

WangFacetInteriorInsertionResult insert_wang_facet_interior_points(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,3> facet,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::size_t maximum_local_flip_passes,WangOrderedTetMesh* ordered_mesh) {
  WangFacetInteriorInsertionResult result;
  result.constraints=constraints;
  result.tetrahedra=tetrahedra;
  std::sort(facet.begin(),facet.end());
  result.facet=facet;
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  std::uint64_t next_id{};
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    next_id=std::max(next_id,constraints.vertices[i].id);
    const auto p=constraints.vertices[i].position;
    points.push_back({p.x,p.y,p.z});
  }
  Face stable_target{};
  for(unsigned i=0U;i<3U;++i) {
    const auto found=index.find(facet[i]);
    if(found==index.end())return result;
    stable_target[i]=found->second;
  }
  Face oriented_target=stable_target;
  for(const auto& candidate:constraints.facets) {
    auto key=candidate.vertices;
    std::sort(key.begin(),key.end());
    if(key!=facet)continue;
    bool resolved=true;
    for(unsigned i=0U;i<3U;++i) {
      const auto found=index.find(candidate.vertices[i]);
      if(found==index.end()){resolved=false;break;}
      oriented_target[i]=found->second;
    }
    if(resolved)break;
  }
  const auto a=points[oriented_target[0]];
  const auto b=points[oriented_target[1]];
  const auto c=points[oriented_target[2]];
  auto normal=cross(b-a,c-a);
  const auto normal_length=std::sqrt(dot(normal,normal));
  if(!(normal_length>0.0L))return result;
  normal=normal*(1.0L/normal_length);
  const auto centroid=(a+b+c)*(1.0L/3.0L);

  struct Residual {
    long double clearance{};
    std::array<std::uint64_t,2> stable_edge{};
    Point intersection{};
    std::array<long double,2> heights{};
  };
  std::optional<Residual> best;
  // Reference FaceRecoveryPatch starts from the target's three vertex stars
  // and crosses only faces incident to an intersecting edge.  A global edge
  // scan can select a remote crossing which DT never considers here.
  std::map<Face,std::vector<std::uint32_t>> face_cells;
  for(std::size_t cell=0U;cell<tetrahedra.size();++cell)
    for(unsigned omitted=0U;omitted<4U;++omitted)
      face_cells[face_key(wang_bw_boundary_face(tetrahedra[cell],omitted))].push_back(
          static_cast<std::uint32_t>(cell));
  const auto ab=b-a,ac=c-a;
  const auto d00=dot(ab,ab),d01=dot(ab,ac),d11=dot(ac,ac);
  const auto denominator=d00*d11-d01*d01;
  if(!(denominator>0.0L))return result;
  const auto touches_facet=[&](std::array<std::uint32_t,2> edge) {
    // FaceRecoveryPatch::collect expands through all `lin_tri_intersect3d`
    // FAC and EDG contacts.  In particular, an endpoint lying on the target
    // facet is a valid patch connection even though it is not a residual
    // transverse edge eligible for addInteriorFacePoints.
    const auto inside_or_on=[&](Point hit) {
      const auto relative=hit-a;
      const auto w1=(d11*dot(relative,ab)-d01*dot(relative,ac))/denominator;
      const auto w2=(d00*dot(relative,ac)-d01*dot(relative,ab))/denominator;
      constexpr long double epsilon=512.0L*LDBL_EPSILON;
      return 1.0L-w1-w2>=-epsilon&&w1>=-epsilon&&w2>=-epsilon;
    };
    const auto first_sign=robust_orient(a,b,c,points[edge[0]]);
    const auto second_sign=robust_orient(a,b,c,points[edge[1]]);
    if(first_sign==0&&inside_or_on(points[edge[0]]))return true;
    if(second_sign==0&&inside_or_on(points[edge[1]]))return true;
    if(first_sign==0||second_sign==0||first_sign==second_sign)return false;
    const auto hp=dot(points[edge[0]]-a,normal),hq=dot(points[edge[1]]-a,normal);
    if(hp==hq)return false;
    return inside_or_on(points[edge[0]]+
                        (points[edge[1]]-points[edge[0]])*(hp/(hp-hq)));
  };
  std::vector<std::uint32_t> pending;
  std::set<std::uint32_t> visited;
  const auto enqueue=[&](std::uint32_t cell) {
    if(visited.insert(cell).second)pending.push_back(cell);
  };
  for(std::size_t cell=0U;cell<tetrahedra.size();++cell)
    for(const auto vertex:oriented_target)
      if(std::find(tetrahedra[cell].begin(),tetrahedra[cell].end(),vertex)!=
         tetrahedra[cell].end()) {enqueue(static_cast<std::uint32_t>(cell));break;}
  std::set<std::array<std::uint32_t,2>> mesh_edges;
  for(std::size_t next=0U;next<pending.size();++next) {
    const auto& cell=tetrahedra[pending[next]];
    for(unsigned i=0U;i<4U;++i)for(unsigned j=i+1U;j<4U;++j) {
      std::array<std::uint32_t,2> edge{{cell[i],cell[j]}};
      std::sort(edge.begin(),edge.end());
      if(!touches_facet(edge))continue;
      if(std::find(oriented_target.begin(),oriented_target.end(),edge[0])==
             oriented_target.end()&&
         std::find(oriented_target.begin(),oriented_target.end(),edge[1])==
             oriented_target.end())mesh_edges.insert(edge);
      for(unsigned omitted=0U;omitted<4U;++omitted)if(omitted!=i&&omitted!=j) {
        const auto found=face_cells.find(face_key(wang_bw_boundary_face(cell,omitted)));
        if(found==face_cells.end())continue;
        for(const auto neighbour:found->second)enqueue(neighbour);
      }
    }
  }
  const auto constrained_edges=constrained_parent_boundary_edges(constraints);
  for(const auto& edge:mesh_edges) {
    std::array<std::uint64_t,2> stable_edge{{
        constraints.vertices[edge[0]].id,constraints.vertices[edge[1]].id}};
    std::sort(stable_edge.begin(),stable_edge.end());
    if(constrained_edges.contains(stable_edge)||
       std::find(facet.begin(),facet.end(),stable_edge[0])!=facet.end()||
       std::find(facet.begin(),facet.end(),stable_edge[1])!=facet.end())continue;
    const auto p=points[edge[0]],q=points[edge[1]];
    auto hp=dot(p-a,normal),hq=dot(q-a,normal);
    if(!((hp>0.0L&&hq<0.0L)||(hp<0.0L&&hq>0.0L)))continue;
    const auto t=hp/(hp-hq);
    if(!(t>0.0L&&t<1.0L))continue;
    const auto intersection=p+(q-p)*t;
    const auto relative=intersection-a;
    const auto d20=dot(relative,ab),d21=dot(relative,ac);
    const auto w1=(d11*d20-d01*d21)/denominator;
    const auto w2=(d00*d21-d01*d20)/denominator;
    const auto w0=1.0L-w1-w2;
    const auto epsilon=512.0L*LDBL_EPSILON;
    if(!(w0>epsilon&&w1>epsilon&&w2>epsilon))continue;
    if(hp<0.0L)std::swap(hp,hq);
    const auto clearance=std::min(hp,-hq);
    Residual candidate{clearance,stable_edge,intersection,{{hp,hq}}};
    if(!best||candidate.clearance>best->clearance||
       (candidate.clearance==best->clearance&&
        candidate.stable_edge<best->stable_edge))best=candidate;
  }
  if(!best) {
    result.failure=WangFacetInteriorInsertionFailure::no_residual_crossing;
    return result;
  }
  result.residual_mesh_edge=best->stable_edge;
  result.residual_intersection=as_vec3(best->intersection);
  const auto base=centroid+(best->intersection-centroid)*0.5L;
  result.blended_base=as_vec3(base);
  result.signed_heights=best->heights;

  for(unsigned side=0U;side<2U;++side) {
    const auto side_normal=normal*(side==0U?1.0L:-1.0L);
    const auto distance=std::abs(best->heights[side]);
    for(long double ratio=0.5L;ratio>=0.01L;ratio*=0.5L) {
      if(next_id==std::numeric_limits<std::uint64_t>::max())break;
      const auto position=base+side_normal*(distance*ratio);
      auto augmented=result.constraints;
      augmented.vertices.push_back({next_id+1U,as_vec3(position)});
      augmented.interior_steiner_vertices.push_back(
          {next_id+1U,CanonicalInteriorSteinerKind::facet_interior});
      std::vector<Point> augmented_points;
      augmented_points.reserve(augmented.vertices.size());
      for(const auto& vertex:augmented.vertices)
        augmented_points.push_back(
            {vertex.position.x,vertex.position.y,vertex.position.z});
      std::vector<Tet> seeds;
      for(const auto& cell:result.tetrahedra)
        if(point_inside_or_on_tet(augmented_points,cell,augmented_points.back()))
          seeds.push_back(cell);
      if(seeds.empty())continue;
      ++result.bowyer_watson_attempts;
      const auto inserted=bowyer_watson_insert_constraint_vertex(
          augmented,result.constraints.vertices.size(),result.tetrahedra,seeds);
      if(!inserted.accepted)continue;
      if(!preserves_previously_recovered_constraints(
             result.constraints,result.tetrahedra,inserted.tetrahedra)) {
        result.failure=
            WangFacetInteriorInsertionFailure::recovered_constraint_lost;
        return result;
      }
      // DT inserts while its ordered hull, source slots, and P2T carriers
      // remain live. Commit the exact constrained-BW cavity to that state
      // before retrying face recovery; the finite vector only validates it.
      if(ordered_mesh) {
        const auto committed=ordered_mesh->replace_cavity_with_appended_vertex(
            inserted.ordered_cavity_tetrahedra,inserted.replacement_tetrahedra,
            augmented.vertices.back().id,augmented.exact_affine_planes);
        if(!committed.accepted)continue;
      }
      result.constraints=std::move(augmented);
      result.tetrahedra=inserted.tetrahedra;
      ++next_id;
      result.inserted_points.push_back(as_vec3(position));
      // `addInteriorFacePoints` immediately re-enters recoverFace after a
      // successful side insertion, before trying the opposite side.
      if(ordered_mesh) {
        auto retry=recover_wang_facet_by_flip_split(
            result.constraints,result.facet,0U,*ordered_mesh);
        if(!retry.recovered)
          retry=recover_wang_facet_by_local_flips(
              result.constraints,result.facet,maximum_local_flip_passes,
              *ordered_mesh);
        if(retry.recovered) {
          result.facet_recovered=true;
        }
        // Subsequent side placement must seed from the mesh *after* the
        // immediate retry, not from the pre-retry finite BW snapshot.
        result.tetrahedra.clear();
        for(const auto& cell:ordered_mesh->cells())
          if(!cell.deleted&&std::find(cell.vertices.begin(),cell.vertices.end(),
              static_cast<std::uint32_t>(ordered_mesh->ghost_vertex()))==
                 cell.vertices.end())
            result.tetrahedra.push_back(cell.vertices);
        if(result.facet_recovered)break;
      }
      break;
    }
  }
  result.failure=result.changed()?WangFacetInteriorInsertionFailure::none:
      WangFacetInteriorInsertionFailure::bowyer_watson_failed;
  return result;
}

WangFacetBoundaryInsertionResult insert_wang_facet_boundary_steiner_point(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,3> facet,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::size_t maximum_vertices,std::size_t maximum_facets,
    WangOrderedTetMesh* ordered_mesh) {
  WangFacetBoundaryInsertionResult result;
  std::sort(facet.begin(),facet.end());
  result.facet=facet;
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto p=constraints.vertices[i].position;
    points.push_back({p.x,p.y,p.z});
  }
  Face target{};
  for(unsigned i=0U;i<3U;++i) {
    const auto found=index.find(facet[i]);
    if(found==index.end())return result;
    target[i]=found->second;
  }
  const auto a=points[target[0]],ab=points[target[1]]-a,
             ac=points[target[2]]-a;
  const auto d00=dot(ab,ab),d01=dot(ab,ac),d11=dot(ac,ac);
  const auto determinant=d00*d11-d01*d01;
  if(!(determinant>0.0L))return result;
  const auto constrained_edges=constrained_parent_boundary_edges(constraints);
  struct Crossing {
    std::array<std::uint64_t,2> stable_edge{};
    std::array<std::uint32_t,2> mesh_edge{};
    std::array<long double,3> weights{};
  };
  std::vector<Crossing> crossings;
  std::set<std::array<std::uint32_t,2>> mesh_edges;
  for(const auto& cell:tetrahedra)
    for(unsigned i=0U;i<4U;++i)for(unsigned j=i+1U;j<4U;++j) {
      std::array<std::uint32_t,2> edge{{cell[i],cell[j]}};
      std::sort(edge.begin(),edge.end());mesh_edges.insert(edge);
    }
  for(const auto& edge:mesh_edges) {
    std::array<std::uint64_t,2> stable_edge{{
        constraints.vertices[edge[0]].id,constraints.vertices[edge[1]].id}};
    std::sort(stable_edge.begin(),stable_edge.end());
    if(constrained_edges.contains(stable_edge)||
       std::find(facet.begin(),facet.end(),stable_edge[0])!=facet.end()||
       std::find(facet.begin(),facet.end(),stable_edge[1])!=facet.end())continue;
    const auto p=points[edge[0]],q=points[edge[1]];
    const auto sp=robust_orient(points[target[0]],points[target[1]],
                                points[target[2]],p);
    const auto sq=robust_orient(points[target[0]],points[target[1]],
                                points[target[2]],q);
    if(sp==0||sq==0||sp==sq)continue;
    const auto dp=orient(points[target[0]],points[target[1]],
                         points[target[2]],p);
    const auto dq=orient(points[target[0]],points[target[1]],
                         points[target[2]],q);
    const auto t=dp/(dp-dq);
    if(!(t>0.0L&&t<1.0L))continue;
    const auto relative=(p+(q-p)*t)-a;
    const auto d20=dot(relative,ab),d21=dot(relative,ac);
    const auto w1=(d11*d20-d01*d21)/determinant;
    const auto w2=(d00*d21-d01*d20)/determinant;
    const auto w0=1.0L-w1-w2;
    const auto epsilon=512.0L*LDBL_EPSILON;
    if(!(w0>epsilon&&w1>epsilon&&w2>epsilon))continue;
    crossings.push_back({stable_edge,edge,{{w0,w1,w2}}});
  }
  if(crossings.empty()) {
    result.failure=WangFacetBoundaryInsertionFailure::no_intersecting_mesh_edge;
    return result;
  }
  std::sort(crossings.begin(),crossings.end(),[](const auto& left,const auto& right) {
    return left.stable_edge<right.stable_edge;
  });
  const auto& crossing=crossings.front();
  result.intersecting_mesh_edge=crossing.stable_edge;
  constexpr std::uint32_t denominator=1U<<24U;
  auto first=static_cast<std::uint32_t>(
      std::llround(crossing.weights[0]*denominator));
  auto second=static_cast<std::uint32_t>(
      std::llround(crossing.weights[1]*denominator));
  if(first==0U||second==0U||first+second>=denominator) {
    result.failure=WangFacetBoundaryInsertionFailure::invalid_facet;
    return result;
  }
  const auto third=denominator-first-second;
  if(third==0U)return result;
  result.barycentric={{first,second,third}};
  result.denominator=denominator;
  for(const auto& cell:tetrahedra) {
    const auto has_first=std::find(cell.begin(),cell.end(),crossing.mesh_edge[0])!=
        cell.end();
    const auto has_second=std::find(cell.begin(),cell.end(),crossing.mesh_edge[1])!=
        cell.end();
    if(has_first&&has_second)result.bowyer_watson_seeds.push_back(cell);
  }
  if(result.bowyer_watson_seeds.empty()) {
    result.failure=WangFacetBoundaryInsertionFailure::no_intersecting_mesh_edge;
    return result;
  }
  const auto split=split_canonical_plc_constraint_facet(
      constraints,facet,result.barycentric,result.denominator,
      maximum_vertices,maximum_facets);
  result.constraint_failure=split.failure;
  if(!split.accepted()) {
    result.failure=WangFacetBoundaryInsertionFailure::constraint_split_failed;
    return result;
  }
  const auto inserted=bowyer_watson_insert_constraint_vertex(
      split.constraints,constraints.vertices.size(),tetrahedra,
      result.bowyer_watson_seeds);
  result.insertion_failure=inserted.failure;
  result.bowyer_watson_cavity=inserted.cavity_tetrahedra;
  result.ordered_cavity_tetrahedra=inserted.ordered_cavity_tetrahedra;
  result.replacement_tetrahedra=inserted.replacement_tetrahedra;
  if(!inserted.accepted) {
    result.failure=WangFacetBoundaryInsertionFailure::bowyer_watson_failed;
    return result;
  }
  if(!preserves_recovered_constraints_across_edge_split(
         constraints,tetrahedra,split.constraints,inserted.tetrahedra)) {
    result.failure=WangFacetBoundaryInsertionFailure::recovered_constraint_lost;
    return result;
  }
  // `splitBndTri` mutates the active DT directly.  Preserve that ordering in
  // the owned path: the PLC does not become visible until its matching
  // constrained-BW cavity has committed to the live ordered mesh.
  if(ordered_mesh) {
    const auto committed=ordered_mesh->replace_cavity_with_appended_vertex(
        inserted.ordered_cavity_tetrahedra,inserted.replacement_tetrahedra,
        split.constraints.vertices.back().id,
        split.constraints.exact_affine_planes);
    if(!committed.accepted) {
      result.failure=WangFacetBoundaryInsertionFailure::bowyer_watson_failed;
      return result;
    }
  }
  result.constraints=split.constraints;
  result.tetrahedra=inserted.tetrahedra;
  result.failure=WangFacetBoundaryInsertionFailure::none;
  return result;
}

WangCascadeFhcConfiguration classify_wang_cascade_fhc(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra) {
  WangCascadeFhcConfiguration fallback;
  std::sort(segment.begin(),segment.end());
  fallback.segment=segment;
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    if(!index.emplace(constraints.vertices[i].id,
                      static_cast<std::uint32_t>(i)).second)return fallback;
    const auto p=constraints.vertices[i].position;
    points.push_back({p.x,p.y,p.z});
  }
  if(segment[0]==segment[1]||!index.contains(segment[0])||
     !index.contains(segment[1]))return fallback;

  const auto before_intersections=
      segment_intersection_simplices(constraints,segment,tetrahedra);
  struct OrderedIntersectingEdge {
    std::array<std::uint64_t,2> edge{};
    long double parameter{};
  };
  std::vector<OrderedIntersectingEdge> intersecting_edges;
  const auto segment_start=points[index.at(segment[0])];
  const auto segment_end=points[index.at(segment[1])];
  const auto segment_delta=segment_end-segment_start;
  const auto segment_length_squared=dot(segment_delta,segment_delta);
  for(const auto& key:before_intersections)if(key[0]==1U) {
    const std::array<std::uint64_t,2> edge{{key[1],key[2]}};
    const auto s0=exact_segment_edge_intersection(
        segment_start,segment_end,
        points[index.at(edge[0])],points[index.at(edge[1])]);
    if(s0&&segment_length_squared>0.0L)
      intersecting_edges.push_back(
          {edge,dot(*s0-segment_start,segment_delta)/segment_length_squared});
  }
  std::sort(intersecting_edges.begin(),intersecting_edges.end(),
      [](const auto& left,const auto& right) {
        if(left.parameter!=right.parameter)return left.parameter<right.parameter;
        return left.edge<right.edge;
      });
  if(intersecting_edges.empty()) {
    fallback.failure=WangCascadeFhcFailure::no_intersecting_edge;
    return fallback;
  }

  for(const auto& ordered_edge:intersecting_edges) {
    const auto stable_edge=ordered_edge.edge;
    WangCascadeFhcConfiguration configuration;
    configuration.segment=segment;
    configuration.main_intersecting_edge=stable_edge;
    const auto edge_first=index.at(stable_edge[0]);
    const auto edge_second=index.at(stable_edge[1]);
    const auto s0=exact_segment_edge_intersection(
        points[index.at(segment[0])],points[index.at(segment[1])],
        points[edge_first],points[edge_second]);
    if(!s0)continue;
    configuration.exact_s0=as_vec3(*s0);

    std::vector<CanonicalLiteralEdgeFlipResult> valid_candidates;
    (void)remove_mesh_edge_in_mesh(
        constraints,{{edge_first,edge_second}},tetrahedra,&valid_candidates,true);
    std::vector<const CanonicalLiteralEdgeFlipResult*> admissible;
    for(const auto& candidate:valid_candidates)
      if(preserves_previously_recovered_constraints(
             constraints,tetrahedra,candidate.tetrahedra))
        admissible.push_back(&candidate);
    configuration.valid_removal_flips=admissible.size();
    if(admissible.empty()) {
      configuration.failure=WangCascadeFhcFailure::intersecting_edge_unflippable;
      fallback=configuration;
      continue;
    }
    for(const auto* candidate:admissible) {
      const auto after_intersections=segment_intersection_simplices(
          constraints,segment,candidate->tetrahedra);
      const auto introduced=std::any_of(
          after_intersections.begin(),after_intersections.end(),
          [&](const auto& item){return !before_intersections.contains(item);});
      if(!mesh_contains_stable_edge(
             constraints,segment,candidate->tetrahedra)&&introduced)
        ++configuration.cascading_removal_flips;
    }
    if(configuration.cascading_removal_flips!=
       configuration.valid_removal_flips) {
      configuration.failure=WangCascadeFhcFailure::flip_does_not_cascade;
      fallback=configuration;
      continue;
    }

    std::map<std::uint32_t,std::set<std::uint32_t>> adjacency;
    for(const auto& cell:tetrahedra) {
      if(std::find(cell.begin(),cell.end(),edge_first)==cell.end()||
         std::find(cell.begin(),cell.end(),edge_second)==cell.end())continue;
      configuration.edge_shell.push_back(cell);
      std::array<std::uint32_t,2> pair{};unsigned count{};
      for(const auto vertex:cell)
        if(vertex!=edge_first&&vertex!=edge_second)pair[count++]=vertex;
      if(count==2U) {
        adjacency[pair[0]].insert(pair[1]);
        adjacency[pair[1]].insert(pair[0]);
      }
    }
    if(adjacency.empty()||std::any_of(
           adjacency.begin(),adjacency.end(),
           [](const auto& item){return item.second.size()!=2U;})) {
      configuration.failure=WangCascadeFhcFailure::no_paper_smoothing_direction;
      fallback=configuration;
      continue;
    }

    const auto stable_id=[&](std::uint32_t vertex) {
      return constraints.vertices[vertex].id;
    };
    const auto start=std::min_element(
        adjacency.begin(),adjacency.end(),[&](const auto& left,const auto& right) {
          return stable_id(left.first)<stable_id(right.first);
        })->first;
    const auto walk=[&](std::uint32_t next) {
      std::vector<std::uint32_t> order{start};
      auto previous=start,current=next;
      while(current!=start&&order.size()<=adjacency.size()) {
        order.push_back(current);
        const auto& neighbours=adjacency.at(current);
        auto following=*neighbours.begin();
        if(following==previous)following=*std::next(neighbours.begin());
        previous=current;current=following;
      }
      return order;
    };
    const auto& start_neighbours=adjacency.at(start);
    auto forward=walk(*start_neighbours.begin());
    auto reverse=walk(*std::next(start_neighbours.begin()));
    const auto stable_sequence=[&](const auto& order) {
      std::vector<std::uint64_t> ids;ids.reserve(order.size());
      for(const auto vertex:order)ids.push_back(stable_id(vertex));
      return ids;
    };
    if(stable_sequence(reverse)<stable_sequence(forward))forward=std::move(reverse);

    const auto edge0=points[edge_first],edge1=points[edge_second];
    const auto distance0=dot(edge0-*s0,edge0-*s0);
    const auto distance1=dot(edge1-*s0,edge1-*s0);
    const auto b_index=distance0>distance1?edge_first:
        distance1>distance0?edge_second:
        (stable_id(edge_first)<stable_id(edge_second)?edge_second:edge_first);
    configuration.endpoint_b=stable_id(b_index);
    const auto midpoint=(*s0+points[b_index])*.5L;
    configuration.midpoint=as_vec3(midpoint);
    auto normal=cross(segment_delta,midpoint-segment_start);
    const auto normal_length=std::sqrt(dot(normal,normal));
    if(!(normal_length>0.0L)) {
      configuration.failure=WangCascadeFhcFailure::no_paper_smoothing_direction;
      fallback=configuration;
      continue;
    }
    normal=normal*(1.0L/normal_length);

    // The paper and reference implementation associate h with a triangle in
    // the ordered shell of e_bc. The paper leaves ties unspecified; use the
    // canonical cyclic shell order, then the first non-coplanar associated
    // triangle. This is topology-derived and independent of tetrahedron order.
    std::optional<std::uint32_t> h_index;
    for(const auto vertex:forward) {
      const auto id=stable_id(vertex);
      if(id==segment[0]||id==segment[1]||
         id==stable_edge[0]||id==stable_edge[1])continue;
      if(dot(cross(edge1-edge0,points[vertex]-edge0),
             cross(edge1-edge0,points[vertex]-edge0))==0.0L)continue;
      const auto side=dot(normal,points[vertex]-segment_start);
      if(side==0.0L)continue;
      configuration.associated_h=id;
      h_index=vertex;
      break;
    }
    if(!h_index) {
      configuration.failure=WangCascadeFhcFailure::no_paper_smoothing_direction;
      fallback=configuration;
      continue;
    }

    // addinnerSteiner_Edge obtains the edge hit from the same double
    // line/triangle routine as the Locked-FHC branch, using the selected
    // shell vertex as the third triangle point.  Recompute every placement
    // role from that finite-precision coordinate; the long-double line/line
    // solution above is only classification evidence.
    const auto reference_s0=wang_segment_plane_hit(
        as_vec3(segment_start),as_vec3(segment_end),as_vec3(edge0),
        as_vec3(edge1),as_vec3(points[*h_index]));
    configuration.exact_s0=reference_s0;
    const Point placed_s0{reference_s0.x,reference_s0.y,reference_s0.z};
    const auto placed_distance0=dot(edge0-placed_s0,edge0-placed_s0);
    const auto placed_distance1=dot(edge1-placed_s0,edge1-placed_s0);
    const auto placed_b_index=placed_distance0>placed_distance1?edge_first:
        placed_distance1>placed_distance0?edge_second:
        (stable_id(edge_first)<stable_id(edge_second)?edge_second:edge_first);
    configuration.endpoint_b=stable_id(placed_b_index);
    const auto placed_midpoint=(placed_s0+points[placed_b_index])*.5L;
    configuration.midpoint=as_vec3(placed_midpoint);
    normal=cross(segment_delta,placed_midpoint-segment_start);
    const auto placed_normal_length=std::sqrt(dot(normal,normal));
    if(!(placed_normal_length>0.0L)) {
      configuration.failure=WangCascadeFhcFailure::no_paper_smoothing_direction;
      fallback=configuration;
      continue;
    }
    normal=normal*(1.0L/placed_normal_length);
    if(dot(normal,points[*h_index]-segment_start)>0.0L)normal=normal*-1.0L;
    configuration.oriented_normal=as_vec3(normal);
    configuration.failure=WangCascadeFhcFailure::none;
    return configuration;
  }
  return fallback;
}

CanonicalLiteralEdgeFlipResult insert_forced_cavity_vertex(
    const CanonicalPlcConstraintSet& constraints,std::size_t previous_vertex_count,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    const std::vector<std::array<std::uint32_t,4>>& forced_cavity) {
  return forced_cavity_insert_constraint_vertex(
      constraints,previous_vertex_count,tetrahedra,forced_cavity);
}

CanonicalLiteralEdgeFlipResult insert_wang_constrained_bowyer_watson_vertex(
    const CanonicalPlcConstraintSet& constraints,
    std::size_t previous_vertex_count,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    const std::vector<std::array<std::uint32_t,4>>& seed_tetrahedra) {
  return bowyer_watson_insert_constraint_vertex(
      constraints,previous_vertex_count,tetrahedra,seed_tetrahedra);
}

CanonicalLiteralEdgeFlipResult insert_cascade_fhc_vertex(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    std::array<std::uint64_t,2> blocking_edge,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::size_t maximum_relaxation_steps) {
  CanonicalLiteralEdgeFlipResult refused;
  refused.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;
  if(maximum_relaxation_steps==0U)return refused;
  std::sort(segment.begin(),segment.end());
  std::sort(blocking_edge.begin(),blocking_edge.end());
  const auto configuration=classify_wang_cascade_fhc(
      constraints,segment,tetrahedra);
  refused.cavity_tetrahedra=configuration.edge_shell;
  refused.intersected_tetrahedra=configuration.edge_shell.size();
  if(!configuration.classified()||
     configuration.main_intersecting_edge!=blocking_edge)return refused;

  std::uint64_t next_id{};
  for(const auto& vertex:constraints.vertices)next_id=std::max(next_id,vertex.id);
  if(next_id==std::numeric_limits<std::uint64_t>::max())return refused;
  const auto new_vertex=static_cast<std::uint32_t>(constraints.vertices.size());
  const auto canonical=[](Tet cell){std::sort(cell.begin(),cell.end());return cell;};
  std::map<Tet,std::size_t> mesh_indices;
  for(std::size_t i=0U;i<tetrahedra.size();++i)
    mesh_indices.emplace(canonical(tetrahedra[i]),i);
  std::set<std::size_t> selected;
  for(const auto& cell:configuration.edge_shell) {
    const auto found=mesh_indices.find(canonical(cell));
    if(found==mesh_indices.end()||!selected.insert(found->second).second)
      return refused;
  }
  std::map<Face,unsigned> face_uses;
  std::vector<Point> old_points;
  for(const auto& vertex:constraints.vertices) {
    const auto p=vertex.position;old_points.push_back({p.x,p.y,p.z});
  }
  long double old_volume{};
  for(const auto cell:selected) {
    const auto& tet=tetrahedra[cell];
    old_volume+=std::abs(orient(old_points[tet[0]],old_points[tet[1]],
                               old_points[tet[2]],old_points[tet[3]]));
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=tet[i];
      ++face_uses[face_key(face)];
    }
  }
  std::set<Face> boundary;
  for(const auto& [face,count]:face_uses) {
    if(count==1U)boundary.insert(face);
    else if(count!=2U)return refused;
  }
  // Section 4.2 first creates this single star at the exact midpoint. Its
  // connectivity is independent of coordinates, so retain it while s moves.
  std::vector<Tet> midpoint_topology;
  for(std::size_t i=0U;i<tetrahedra.size();++i)
    if(!selected.contains(i))midpoint_topology.push_back(tetrahedra[i]);
  const auto first_new_cell=midpoint_topology.size();
  for(const auto face:boundary)
    midpoint_topology.push_back({{new_vertex,face[0],face[1],face[2]}});

  const auto segment_start_it=std::find_if(
      constraints.vertices.begin(),constraints.vertices.end(),
      [&](const auto& vertex){return vertex.id==segment[0];});
  const auto segment_end_it=std::find_if(
      constraints.vertices.begin(),constraints.vertices.end(),
      [&](const auto& vertex){return vertex.id==segment[1];});
  if(segment_start_it==constraints.vertices.end()||
     segment_end_it==constraints.vertices.end())return refused;
  const Point segment_start{segment_start_it->position.x,
                            segment_start_it->position.y,
                            segment_start_it->position.z};
  const Point segment_end{segment_end_it->position.x,segment_end_it->position.y,
                          segment_end_it->position.z};
  const Point midpoint{configuration.midpoint.x,configuration.midpoint.y,
                       configuration.midpoint.z};
  const auto segment_delta=segment_end-segment_start;
  const auto normal_offset=cross(segment_delta,midpoint-segment_start);
  const auto segment_length=std::sqrt(dot(segment_delta,segment_delta));
  if(!(segment_length>0.0L))return refused;
  const auto height=std::sqrt(dot(normal_offset,normal_offset))/segment_length;
  const Point normal{configuration.oriented_normal.x,
                     configuration.oriented_normal.y,
                     configuration.oriented_normal.z};
  auto relocated=constraints;
  relocated.vertices.push_back({next_id+1U,configuration.midpoint});
  auto points=old_points;points.push_back(midpoint);

  for(std::size_t step=0U;step<maximum_relaxation_steps;++step) {
    const auto distance=height*std::ldexp(0.5L,-static_cast<int>(step));
    const auto moved=midpoint+normal*distance;
    const Vec3 candidate=as_vec3(moved);
    relocated.vertices.back().position=candidate;
    auto candidate_mesh=midpoint_topology;
    points.back()=moved;
    bool positive=true;long double new_volume{};
    for(std::size_t i=first_new_cell;i<candidate_mesh.size();++i) {
      auto& cell=candidate_mesh[i];
      const auto orientation=robust_orient(points,cell);
      if(orientation==0){positive=false;break;}
      if(orientation<0)std::swap(cell[0],cell[1]);
      new_volume+=std::abs(orient(points[cell[0]],points[cell[1]],
                                 points[cell[2]],points[cell[3]]));
    }
    if(!positive||
       std::abs(old_volume-new_volume)>std::max(1.0L,old_volume)*1e-14L)
      continue;
    std::sort(candidate_mesh.begin(),candidate_mesh.end());
    if(!constraint_mesh_mutation_is_valid(
           relocated,tetrahedra,candidate_mesh)||
       !preserves_previously_recovered_constraints(
           relocated,tetrahedra,candidate_mesh))continue;
    CanonicalLiteralEdgeFlipResult inserted;
    inserted.accepted=true;
    inserted.failure=CanonicalLiteralEdgeFlipFailure::none;
    inserted.cavity_tetrahedra=configuration.edge_shell;
    inserted.intersected_tetrahedra=configuration.edge_shell.size();
    inserted.retriangulation_trials=step+1U;
    inserted.inserted_steiner_vertex=candidate;
    inserted.tetrahedra=std::move(candidate_mesh);
    return inserted;
  }
  return refused;
}

WangLockedFhcConfiguration classify_wang_locked_fhc(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra) {
  WangLockedFhcConfiguration fallback;
  std::sort(segment.begin(),segment.end());
  fallback.segment=segment;
  std::map<std::uint64_t,std::uint32_t> index;
  for(std::size_t i=0U;i<constraints.vertices.size();++i)
    if(!index.emplace(constraints.vertices[i].id,
                      static_cast<std::uint32_t>(i)).second)return fallback;
  if(segment[0]==segment[1]||!index.contains(segment[0])||
     !index.contains(segment[1]))return fallback;
  const auto constrained_edges=constrained_parent_boundary_edges(constraints);
  if(!constrained_edges.contains(segment))return fallback;
  const auto intersections=exact_segment_face_intersections(
      constraints,segment,tetrahedra);
  if(intersections.empty()) {
    fallback.failure=WangLockedFhcFailure::no_intersecting_face;
    return fallback;
  }
  const auto inspection=inspect_canonical_plc_tetrahedra(constraints,tetrahedra);
  const std::set<std::array<std::uint64_t,2>> missing_edges(
      inspection.missing_edges.begin(),inspection.missing_edges.end());
  const auto failure_rank=[](WangLockedFhcFailure failure) {
    switch(failure) {
      case WangLockedFhcFailure::intersecting_face_flippable:return 4U;
      case WangLockedFhcFailure::locking_constraint_not_recovered:return 3U;
      case WangLockedFhcFailure::locking_edge_not_constraint:return 2U;
      case WangLockedFhcFailure::no_locking_face_edge:return 1U;
      default:return 0U;
    }
  };
  const auto retain_more_specific_failure=[&](
      const WangLockedFhcConfiguration& configuration) {
    // The intersections have canonical stable-face order. Keep the first
    // result at equal specificity, and do not let a later boundary face erase
    // evidence from an actual two-tetrahedron crossed face.
    if(failure_rank(configuration.failure)>failure_rank(fallback.failure))
      fallback=configuration;
  };

  for(const auto& intersection:intersections) {
    WangLockedFhcConfiguration configuration;
    configuration.segment=segment;
    configuration.exact_s0=as_vec3(intersection.s0);
    for(unsigned i=0U;i<3U;++i)
      configuration.intersecting_face[i]=
          constraints.vertices[intersection.mesh_face[i]].id;
    std::sort(configuration.intersecting_face.begin(),
              configuration.intersecting_face.end());
    const auto face_flip=face_flip_for_specific_face(
        constraints,tetrahedra,intersection.mesh_face);
    configuration.incident_tetrahedra=face_flip.cavity_tetrahedra;
    if(face_flip.accepted) {
      configuration.failure=WangLockedFhcFailure::intersecting_face_flippable;
      retain_more_specific_failure(configuration);
      continue;
    }
    if(face_flip.failure!=CanonicalLiteralEdgeFlipFailure::nonpositive_replacement||
       !face_flip.blocking_mesh_edge) {
      configuration.failure=WangLockedFhcFailure::no_locking_face_edge;
      retain_more_specific_failure(configuration);
      continue;
    }
    configuration.locking_mesh_edge=*face_flip.blocking_mesh_edge;
    std::sort(configuration.locking_mesh_edge.begin(),
              configuration.locking_mesh_edge.end());
    configuration.locking_constraint_edge={{
        constraints.vertices[configuration.locking_mesh_edge[0]].id,
        constraints.vertices[configuration.locking_mesh_edge[1]].id}};
    std::sort(configuration.locking_constraint_edge.begin(),
              configuration.locking_constraint_edge.end());
    if(!constrained_edges.contains(configuration.locking_constraint_edge)) {
      configuration.failure=WangLockedFhcFailure::locking_edge_not_constraint;
      retain_more_specific_failure(configuration);
      continue;
    }
    if(missing_edges.contains(configuration.locking_constraint_edge)||
       !mesh_contains_stable_edge(
           constraints,configuration.locking_constraint_edge,tetrahedra)) {
      configuration.failure=WangLockedFhcFailure::locking_constraint_not_recovered;
      retain_more_specific_failure(configuration);
      continue;
    }
    // Definition 3.2 requires the relevant alternative flip to be forbidden
    // by the recovered constraint, not merely absent for numerical reasons.
    const auto removal=remove_mesh_edge_in_mesh(
        constraints,configuration.locking_mesh_edge,tetrahedra);
    if(removal.accepted||
       removal.failure!=CanonicalLiteralEdgeFlipFailure::frozen_cavity_boundary) {
      configuration.failure=WangLockedFhcFailure::locking_edge_not_constraint;
      retain_more_specific_failure(configuration);
      continue;
    }
    const auto& p_j=constraints.vertices[configuration.locking_mesh_edge[0]].position;
    const auto& p_k=constraints.vertices[configuration.locking_mesh_edge[1]].position;
    // The pinned Locked-FHC placement operates on its double coordinate
    // arrays.  Round S0 once, then perform the three-point barycenter in that
    // same domain; retaining an 80-bit intermediate made the subsequent
    // zero-orientation decision optimization-dependent on x86-style builds.
    configuration.barycenter={
        (configuration.exact_s0.x+p_j.x+p_k.x)/3.0,
        (configuration.exact_s0.y+p_j.y+p_k.y)/3.0,
        (configuration.exact_s0.z+p_j.z+p_k.z)/3.0};
    configuration.failure=WangLockedFhcFailure::none;
    return configuration;
  }
  return fallback;
}

CanonicalLiteralEdgeFlipResult insert_locked_fhc_vertex(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    std::array<std::uint32_t,2> blocking_mesh_edge,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra) {
  CanonicalLiteralEdgeFlipResult refused;
  refused.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;
  if(blocking_mesh_edge[0]>=constraints.vertices.size()||
     blocking_mesh_edge[1]>=constraints.vertices.size()||
     blocking_mesh_edge[0]==blocking_mesh_edge[1])return refused;
  std::sort(segment.begin(),segment.end());
  std::sort(blocking_mesh_edge.begin(),blocking_mesh_edge.end());
  const auto configuration=classify_wang_locked_fhc(
      constraints,segment,tetrahedra);
  refused.cavity_tetrahedra=configuration.incident_tetrahedra;
  refused.intersected_tetrahedra=configuration.incident_tetrahedra.size();
  if(!configuration.classified()||
     configuration.locking_mesh_edge!=blocking_mesh_edge)return refused;

  std::uint64_t next_id{};
  for(const auto& vertex:constraints.vertices)
    next_id=std::max(next_id,vertex.id);
  if(next_id==std::numeric_limits<std::uint64_t>::max())return refused;
  auto augmented=constraints;
  augmented.vertices.push_back({next_id+1U,configuration.barycenter});
  auto inserted=forced_cavity_insert_constraint_vertex(
      augmented,constraints.vertices.size(),tetrahedra,
      configuration.incident_tetrahedra);
  if(!inserted.accepted)return inserted;
  if(!preserves_previously_recovered_constraints(
         augmented,tetrahedra,inserted.tetrahedra))return refused;
  std::sort(inserted.tetrahedra.begin(),inserted.tetrahedra.end());
  inserted.inserted_steiner_vertex=configuration.barycenter;
  inserted.blocking_mesh_edge=blocking_mesh_edge;
  return inserted;
}

WangFhcCandidateSchedule schedule_wang_fhc_candidates(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra) {
  std::sort(segment.begin(),segment.end());
  WangFhcCandidateSchedule schedule;
  schedule.cascade=classify_wang_cascade_fhc(constraints,segment,tetrahedra);
  schedule.locked=classify_wang_locked_fhc(constraints,segment,tetrahedra);
  if(!schedule.cascade.classified()&&!schedule.locked.classified())return schedule;
  if(!schedule.cascade.classified()) {
    schedule.first=WangFhcCandidateKind::locked;
    return schedule;
  }
  if(!schedule.locked.classified()) {
    schedule.first=WangFhcCandidateKind::cascade;
    return schedule;
  }
  const auto first=std::find_if(constraints.vertices.begin(),
      constraints.vertices.end(),
      [&](const auto& vertex){return vertex.id==segment[0];});
  const auto second=std::find_if(constraints.vertices.begin(),
      constraints.vertices.end(),
      [&](const auto& vertex){return vertex.id==segment[1];});
  if(first==constraints.vertices.end()||second==constraints.vertices.end())
    return schedule;
  const Point a{first->position.x,first->position.y,first->position.z};
  const Point b{second->position.x,second->position.y,second->position.z};
  const auto delta=b-a;
  const auto length_squared=dot(delta,delta);
  if(!(length_squared>0.0L))return schedule;
  const auto parameter=[&](Vec3 point) {
    return dot(Point{point.x,point.y,point.z}-a,delta)/length_squared;
  };
  schedule.first=parameter(schedule.cascade.exact_s0)<
                         parameter(schedule.locked.exact_s0)?
      WangFhcCandidateKind::cascade:WangFhcCandidateKind::locked;
  return schedule;
}

namespace {
std::set<std::array<std::uint64_t,3>> stable_mesh_faces(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<Tet>& mesh) {
  std::set<std::array<std::uint64_t,3>> result;
  for(const auto& cell:mesh)for(unsigned omitted=0U;omitted<4U;++omitted) {
    std::array<std::uint64_t,3> face{};unsigned cursor{};
    for(unsigned i=0U;i<4U;++i)if(i!=omitted)
      face[cursor++]=constraints.vertices[cell[i]].id;
    std::sort(face.begin(),face.end());result.insert(face);
  }
  return result;
}

std::optional<long double> unrecovered_constraint_measure(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<Tet>& mesh) {
  const auto inspection=inspect_canonical_plc_tetrahedra(constraints,mesh);
  if(inspection.failure!=CanonicalPlcSeedFailure::none&&
     inspection.failure!=CanonicalPlcSeedFailure::unrecovered_constraint)
    return std::nullopt;
  std::map<std::uint64_t,Point> points;
  for(const auto& vertex:constraints.vertices)
    points.emplace(vertex.id,Point{vertex.position.x,vertex.position.y,
                                   vertex.position.z});
  long double measure{};
  const auto mesh_faces=stable_mesh_faces(constraints,mesh);
  std::set<std::array<std::uint64_t,2>> mesh_edges;
  for(const auto& face:mesh_faces)
    for(const auto pair:std::array<std::array<unsigned,2>,3>{
            {{{0U,1U}},{{1U,2U}},{{0U,2U}}}})
      mesh_edges.insert({face[pair[0]],face[pair[1]]});
  for(const auto& edge:measured_parent_patch_boundary_edges(constraints)) {
    if(mesh_edges.contains(edge))continue;
    if(!points.contains(edge[0])||!points.contains(edge[1]))return std::nullopt;
    const auto delta=points.at(edge[1])-points.at(edge[0]);
    measure+=std::sqrt(dot(delta,delta));
  }
  const auto recovered=recovered_parent_patches(constraints,mesh_faces);
  for(const auto& [parent,is_recovered]:recovered)if(!is_recovered) {
    if(!points.contains(parent.vertex_ids[0])||
       !points.contains(parent.vertex_ids[1])||
       !points.contains(parent.vertex_ids[2]))return std::nullopt;
    const auto normal=cross(points.at(parent.vertex_ids[1])-
                                points.at(parent.vertex_ids[0]),
                            points.at(parent.vertex_ids[2])-
                                points.at(parent.vertex_ids[0]));
    measure+=0.5L*std::sqrt(dot(normal,normal));
  }
  return measure;
}

bool preserves_recovered_constraints_across_edge_split(
    const CanonicalPlcConstraintSet& before_constraints,
    const std::vector<Tet>& before_mesh,
    const CanonicalPlcConstraintSet& after_constraints,
    const std::vector<Tet>& after_mesh) {
  const auto before=inspect_canonical_plc_tetrahedra(
      before_constraints,before_mesh);
  if(before.failure!=CanonicalPlcSeedFailure::none&&
     before.failure!=CanonicalPlcSeedFailure::unrecovered_constraint)
    return false;
  const std::set<std::array<std::uint64_t,2>> missing_before(
      before.missing_edges.begin(),before.missing_edges.end());
  for(const auto& edge:constrained_parent_boundary_edges(before_constraints))
    if(!missing_before.contains(edge)&&!mesh_contains_stable_edge(
           after_constraints,edge,after_mesh)) {
      if(std::getenv("WANG_BOUNDARY_SPLIT_DIAGNOSTICS")!=nullptr)
        std::cerr<<"owned_split_lost_recovered_edge "<<edge[0]<<' '<<edge[1]<<'\n';
      return false;
    }

  std::set<FrozenFacetIdentity> deliberately_refined_parents;
  if(!after_constraints.split_vertices.empty()) {
    auto split_edge=after_constraints.split_vertices.back().edge;
    std::sort(split_edge.begin(),split_edge.end());
    for(const auto& facet:before_constraints.facets) {
      auto count=0U;
      for(const auto id:facet.vertices)
        if(id==split_edge[0]||id==split_edge[1])++count;
      if(count==2U)deliberately_refined_parents.insert(facet.parent);
    }
  }

  const auto parents_before=recovered_parent_patches(
      before_constraints,stable_mesh_faces(before_constraints,before_mesh));
  const auto parents_after=recovered_parent_patches(
      after_constraints,stable_mesh_faces(after_constraints,after_mesh));
  for(const auto& [parent,recovered]:parents_before)
    if(recovered&&!deliberately_refined_parents.contains(parent)) {
      const auto found=parents_after.find(parent);
    if(found==parents_after.end()||!found->second) {
      if(std::getenv("WANG_BOUNDARY_SPLIT_DIAGNOSTICS")!=nullptr)
        std::cerr<<"owned_split_lost_recovered_parent "
                 <<parent.vertex_ids[0]<<' '<<parent.vertex_ids[1]<<' '
                 <<parent.vertex_ids[2]<<" split ";
      if(std::getenv("WANG_BOUNDARY_SPLIT_DIAGNOSTICS")!=nullptr) {
        if(after_constraints.split_vertices.empty())std::cerr<<"none";
        else {
          const auto split_edge=after_constraints.split_vertices.back().edge;
          std::cerr<<split_edge[0]<<' '<<split_edge[1];
        }
        std::cerr<<" before_faces";
        for(const auto& facet:before_constraints.facets)
          if(facet.parent==parent)
            std::cerr<<' '<<facet.vertices[0]<<','<<facet.vertices[1]<<','
                     <<facet.vertices[2];
        std::cerr<<" after_faces";
        for(const auto& facet:after_constraints.facets)
          if(facet.parent==parent)
            std::cerr<<' '<<facet.vertices[0]<<','<<facet.vertices[1]<<','
                     <<facet.vertices[2];
        std::map<std::uint64_t,Vec3> diagnostic_points;
        for(const auto& vertex:after_constraints.vertices)
          diagnostic_points.emplace(vertex.id,vertex.position);
        const auto split_id=after_constraints.vertices.back().id;
        std::cerr<<std::hexfloat<<" point "
                 <<diagnostic_points.at(split_id).x<<' '
                 <<diagnostic_points.at(split_id).y<<' '
                 <<diagnostic_points.at(split_id).z<<" parent_points";
        for(const auto id:parent.vertex_ids) {
          const auto& point=diagnostic_points.at(id);
          std::cerr<<' '<<point.x<<','<<point.y<<','<<point.z;
        }
        std::cerr<<std::defaultfloat;
        std::cerr<<'\n';
      }
      return false;
    }
  }
  return true;
}

struct WangBoundarySeed {
  std::uint32_t numerator{};
  std::uint32_t denominator{};
  // splitBndEdge inserts the coordinate returned by lin_tri_intersect3d.
  // The ratio below is retained solely as immutable parent-facet provenance;
  // rebuilding a point from that quantised ratio moves it by several ULPs.
  Vec3 position{};
  bool midpoint_fallback{};
  std::array<std::uint64_t,2> intersected_edge{};
  std::array<std::uint64_t,3> intersected_face{};
  std::vector<Tet> tetrahedra;
};

std::pair<std::uint32_t,std::uint32_t> dyadic_ratio(
    long double parameter) {
  constexpr std::uint32_t denominator=1U<<30U;
  auto numerator=static_cast<std::uint32_t>(std::llround(
      parameter*static_cast<long double>(denominator)));
  numerator=std::clamp(numerator,1U,denominator-1U);
  const auto divisor=greatest_common_divisor(numerator,denominator);
  return {numerator/divisor,denominator/divisor};
}

bool point_inside_or_on_tet(
    const std::vector<Point>& points,const Tet& tet,Point query) {
  for(unsigned omitted=0U;omitted<4U;++omitted) {
    Face face{};unsigned cursor{};
    for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=tet[i];
    const auto reference=robust_orient(
        points[face[0]],points[face[1]],points[face[2]],points[tet[omitted]]);
    const auto side=robust_orient(
        points[face[0]],points[face[1]],points[face[2]],query);
    if(reference==0||(side!=0&&side!=reference))return false;
  }
  return true;
}

std::optional<WangBoundarySeed> select_wang_boundary_intersection_seed(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,const std::vector<Tet>& mesh,
    WangOrderedTetMesh* ordered_mesh=nullptr) {
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto p=constraints.vertices[i].position;
    points.push_back({p.x,p.y,p.z});
  }
  if(!index.contains(segment[0])||!index.contains(segment[1]))return std::nullopt;
  const auto start=points[index.at(segment[0])];
  const auto end=points[index.at(segment[1])];
  const auto direction=end-start;
  const auto length_squared=dot(direction,direction);
  if(!(length_squared>0.0L))return std::nullopt;

  std::map<std::array<std::uint32_t,2>,std::vector<Tet>> edge_shells;
  std::map<Face,std::vector<Tet>> face_uses;
  for(const auto& cell:mesh) {
    for(unsigned i=0U;i<4U;++i)for(unsigned j=i+1U;j<4U;++j) {
      std::array<std::uint32_t,2> edge{{cell[i],cell[j]}};
      std::sort(edge.begin(),edge.end());edge_shells[edge].push_back(cell);
    }
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];
      face_uses[face_key(face)].push_back(cell);
    }
  }
  const auto stable_tet_key=[&](const Tet& cell) {
    std::array<std::uint64_t,4> key{};
    for(unsigned i=0U;i<4U;++i)key[i]=constraints.vertices[cell[i]].id;
    std::sort(key.begin(),key.end());return key;
  };
  const auto sort_seeds=[&](std::vector<Tet>& seeds) {
    std::sort(seeds.begin(),seeds.end(),[&](const Tet& left,const Tet& right) {
      return stable_tet_key(left)<stable_tet_key(right);
    });
  };
  struct Candidate {
    long double parameter{};
    long double midpoint_distance{};
    std::array<std::uint64_t,3> stable_simplex{};
    std::vector<Tet> seeds;
    Vec3 position{};
  };
  const auto better=[](const Candidate& left,const Candidate& right) {
    if(left.midpoint_distance!=right.midpoint_distance)
      return left.midpoint_distance<right.midpoint_distance;
    if(left.parameter!=right.parameter)return left.parameter<right.parameter;
    return left.stable_simplex<right.stable_simplex;
  };

  std::optional<Candidate> best_edge;
  for(const auto& [edge,shell]:edge_shells) {
    const auto id0=constraints.vertices[edge[0]].id;
    const auto id1=constraints.vertices[edge[1]].id;
    if(id0==segment[0]||id0==segment[1]||
       id1==segment[0]||id1==segment[1])continue;
    const auto hit=exact_segment_edge_intersection(
        start,end,points[edge[0]],points[edge[1]]);
    if(!hit)continue;
    const auto parameter=dot(*hit-start,direction)/length_squared;
    if(!(parameter>0.0L&&parameter<1.0L))continue;
    Candidate candidate;
    candidate.parameter=parameter;
    candidate.midpoint_distance=std::abs(0.5L-parameter);
    candidate.stable_simplex={{std::min(id0,id1),std::max(id0,id1),0U}};
    candidate.seeds=shell;sort_seeds(candidate.seeds);
    // `findIntersectwithEdgs` gives splitBndEdge a mesh edge, then it calls
    // lin_tri_intersect3d on an incident edge triangle. Reconstruct that
    // calculation (including fixedSplitPoint's binary64 rounding), instead
    // of publishing the higher-precision classification intersection.
    // splitBndEdge takes the first findShell ring point which is neither the
    // crossed edge nor either endpoint of the missing boundary segment.  In
    // particular, an arbitrary third vertex of the first shell tet can be a
    // segment endpoint, which makes lin_tri_intersect3d degenerate.
    std::optional<std::uint32_t> ring_point;
    for(const auto& shell_tet:shell) {
      for(const auto vertex:shell_tet)
        if(vertex!=edge[0]&&vertex!=edge[1]&&
           vertex!=index.at(segment[0])&&vertex!=index.at(segment[1])) {
          ring_point=vertex;
          break;
        }
      if(ring_point)break;
    }
    if(!ring_point)continue;
    const auto start_vec=constraints.vertices[index.at(segment[0])].position;
    const auto end_vec=constraints.vertices[index.at(segment[1])].position;
    // splitBndEdge itself fills linep in SurEdg order. Its preceding walk
    // reverses a separate query line, but that does not supply this point.
    candidate.position=wang_segment_plane_hit(start_vec,end_vec,
        constraints.vertices[edge[0]].position,constraints.vertices[edge[1]].position,
        constraints.vertices[*ring_point].position);
    if(std::getenv("WANG_BOUNDARY_SPLIT_DIAGNOSTICS")!=nullptr)
      std::cerr<<std::hexfloat<<"owned_split_edge_candidate "
               <<segment[0]<<' '<<segment[1]<<" edge "<<id0<<' '<<id1
               <<" point "<<candidate.position.x<<' '<<candidate.position.y
               <<' '<<candidate.position.z<<std::defaultfloat<<'\n';
    if(!best_edge||better(candidate,*best_edge))best_edge=std::move(candidate);
  }
  if(best_edge) {
    const auto ratio=dyadic_ratio(best_edge->parameter);
    return WangBoundarySeed{ratio.first,ratio.second,best_edge->position,false,
        {{best_edge->stable_simplex[0],best_edge->stable_simplex[1]}},{},
        std::move(best_edge->seeds)};
  }

  // splitBndEdge does not enumerate a globally sorted face set. Its Vid is
  // the ordered findIntersectwithEdgs walk from the current P2T carrier.
  // When the owned ordered mesh is available, replay that ordered face list
  // before retaining the vector-mesh fallback used by standalone callers.
  if(ordered_mesh!=nullptr) {
    const auto walked=inspect_wang_full_search_features(
        constraints,segment,*ordered_mesh);
    if(walked.failure==WangOwnedFullSearchFailure::none) {
      std::optional<Candidate> source_best_face;
      for(const auto& feature:walked.features) {
        if(feature.kind!=WangOwnedFullSearchFeatureKind::face)continue;
        const Face face=feature.vertices;
        if(std::find(face.begin(),face.end(),index.at(segment[0]))!=face.end()||
           std::find(face.begin(),face.end(),index.at(segment[1]))!=face.end())
          continue;
        const auto uses=face_uses.find(face_key(face));
        if(uses==face_uses.end())continue;
        const auto hit=wang_segment_plane_hit(
            constraints.vertices[index.at(segment[0])].position,
            constraints.vertices[index.at(segment[1])].position,
            constraints.vertices[face[0]].position,
            constraints.vertices[face[1]].position,
            constraints.vertices[face[2]].position);
        const auto& from=constraints.vertices[index.at(segment[0])].position;
        const auto& to=constraints.vertices[index.at(segment[1])].position;
        const auto distance=[](Vec3 a,Vec3 b) {
          const auto d=a-b;return std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z);
        };
        const auto first_distance=distance(hit,from);
        const auto second_distance=distance(hit,to);
        const auto parameter=static_cast<long double>(
            first_distance/(first_distance+second_distance));
        if(!(parameter>0.0L&&parameter<1.0L))continue;
        Candidate candidate;
        candidate.parameter=parameter;
        candidate.midpoint_distance=std::abs(0.5L-parameter);
        candidate.position=hit;
        candidate.seeds=uses->second;sort_seeds(candidate.seeds);
        for(unsigned i=0U;i<3U;++i)
          candidate.stable_simplex[i]=constraints.vertices[face[i]].id;
        std::sort(candidate.stable_simplex.begin(),candidate.stable_simplex.end());
        if(!source_best_face||better(candidate,*source_best_face))
          source_best_face=std::move(candidate);
      }
      if(source_best_face) {
        const auto ratio=dyadic_ratio(source_best_face->parameter);
        return WangBoundarySeed{ratio.first,ratio.second,
            source_best_face->position,false,{},source_best_face->stable_simplex,
            std::move(source_best_face->seeds)};
      }
    }
  }

  std::optional<Candidate> best_face;
  for(const auto& [face,uses]:face_uses) {
    if(std::find(face.begin(),face.end(),index.at(segment[0]))!=face.end()||
       std::find(face.begin(),face.end(),index.at(segment[1]))!=face.end())
      continue;
    const auto start_sign=robust_orient(
        points[face[0]],points[face[1]],points[face[2]],start);
    const auto end_sign=robust_orient(
        points[face[0]],points[face[1]],points[face[2]],end);
    if(start_sign==0||end_sign==0||start_sign==end_sign)continue;
    const auto start_value=orient(
        points[face[0]],points[face[1]],points[face[2]],start);
    const auto end_value=orient(
        points[face[0]],points[face[1]],points[face[2]],end);
    const auto parameter=start_value/(start_value-end_value);
    if(!(parameter>0.0L&&parameter<1.0L))continue;
    const auto hit=start+direction*parameter;
    const auto origin=points[face[0]];
    const auto v0=points[face[1]]-origin,v1=points[face[2]]-origin;
    const auto relative=hit-origin;
    const auto d00=dot(v0,v0),d01=dot(v0,v1),d11=dot(v1,v1);
    const auto determinant=d00*d11-d01*d01;
    if(!(determinant>0.0L))continue;
    const auto d20=dot(relative,v0),d21=dot(relative,v1);
    const auto w1=(d11*d20-d01*d21)/determinant;
    const auto w2=(d00*d21-d01*d20)/determinant;
    const auto w0=1.0L-w1-w2;
    const auto epsilon=256.0L*LDBL_EPSILON;
    if(!(w0>epsilon&&w1>epsilon&&w2>epsilon))continue;
    Candidate candidate;
    candidate.parameter=parameter;
    candidate.midpoint_distance=std::abs(0.5L-parameter);
    for(unsigned i=0U;i<3U;++i)
      candidate.stable_simplex[i]=constraints.vertices[face[i]].id;
    std::sort(candidate.stable_simplex.begin(),candidate.stable_simplex.end());
    candidate.seeds=uses;sort_seeds(candidate.seeds);
    candidate.position=wang_segment_plane_hit(
        constraints.vertices[index.at(segment[0])].position,
        constraints.vertices[index.at(segment[1])].position,
        constraints.vertices[face[0]].position,constraints.vertices[face[1]].position,
        constraints.vertices[face[2]].position);
    if(std::getenv("WANG_BOUNDARY_SPLIT_DIAGNOSTICS")!=nullptr)
      std::cerr<<std::hexfloat<<"owned_split_face_candidate "
               <<segment[0]<<' '<<segment[1]<<" face "
               <<constraints.vertices[face[0]].id<<' '
               <<constraints.vertices[face[1]].id<<' '
               <<constraints.vertices[face[2]].id<<" point "
               <<candidate.position.x<<' '<<candidate.position.y<<' '
               <<candidate.position.z<<std::defaultfloat<<'\n';
    if(!best_face||better(candidate,*best_face))best_face=std::move(candidate);
  }
  if(best_face) {
    const auto ratio=dyadic_ratio(best_face->parameter);
    return WangBoundarySeed{ratio.first,ratio.second,best_face->position,false,{},
        best_face->stable_simplex,std::move(best_face->seeds)};
  }
  return std::nullopt;
}

std::optional<WangBoundarySeed> select_wang_boundary_midpoint_seed(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,const std::vector<Tet>& mesh) {
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto point=constraints.vertices[i].position;
    points.push_back({point.x,point.y,point.z});
  }
  if(!index.contains(segment[0])||!index.contains(segment[1]))return std::nullopt;
  const auto midpoint=(points[index.at(segment[0])]+points[index.at(segment[1])])*
      0.5L;
  std::vector<Tet> seeds;
  for(const auto& cell:mesh)
    if(point_inside_or_on_tet(points,cell,midpoint))seeds.push_back(cell);
  if(seeds.empty())return std::nullopt;
  const auto stable_key=[&](const Tet& cell) {
    std::array<std::uint64_t,4> key{};
    for(unsigned i=0U;i<4U;++i)key[i]=constraints.vertices[cell[i]].id;
    std::sort(key.begin(),key.end());return key;
  };
  std::sort(seeds.begin(),seeds.end(),[&](const Tet& left,const Tet& right) {
    return stable_key(left)<stable_key(right);
  });
  return WangBoundarySeed{1U,2U,{static_cast<double>(midpoint.x),
                                 static_cast<double>(midpoint.y),
                                 static_cast<double>(midpoint.z)},true,{},{},
                          std::move(seeds)};
}

CanonicalLiteralEdgeFlipResult bowyer_watson_insert_constraint_vertex(
    const CanonicalPlcConstraintSet& constraints,std::size_t previous_vertex_count,
    const std::vector<Tet>& mesh,const std::vector<Tet>& seed_tetrahedra,
    bool protect_recovered_edge_shell,bool finite_hull_projection_adapter) {
  CanonicalLiteralEdgeFlipResult result;
  result.failure=CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star;
  if(constraints.vertices.size()!=previous_vertex_count+1U||mesh.empty()||
     seed_tetrahedra.empty())
    return result;
  const auto query=static_cast<std::uint32_t>(previous_vertex_count);
  std::vector<Point> points;points.reserve(constraints.vertices.size());
  std::vector<std::uint64_t> ids;ids.reserve(constraints.vertices.size());
  for(const auto& vertex:constraints.vertices) {
    points.push_back({vertex.position.x,vertex.position.y,vertex.position.z});
    ids.push_back(vertex.id);
  }
  std::vector<std::uint32_t> order(ids.size());
  std::iota(order.begin(),order.end(),0U);
  std::sort(order.begin(),order.end(),[&](auto left,auto right) {
    return std::tie(ids[left],points[left].x,points[left].y,points[left].z)<
           std::tie(ids[right],points[right].x,points[right].y,points[right].z);
  });
  std::vector<std::uint32_t> rank(ids.size());
  for(std::uint32_t i=0U;i<order.size();++i)rank[order[i]]=i;
  const auto semantically_coplanar=[&](const Tet& cell) {
    std::array<std::uint64_t,4> stable{};
    for(unsigned corner=0U;corner<4U;++corner)stable[corner]=ids[cell[corner]];
    return is_semantically_coplanar(stable,constraints.exact_affine_planes);
  };

  const auto canonical=[](Tet cell){std::sort(cell.begin(),cell.end());return cell;};
  std::map<Tet,std::size_t> mesh_index;
  for(std::size_t i=0U;i<mesh.size();++i)mesh_index.emplace(canonical(mesh[i]),i);
  std::set<std::size_t> selected;
  std::vector<std::size_t> working_order;
  std::vector<std::size_t> pending;
  std::vector<Tet> initial_seeds=seed_tetrahedra;
  bool finite_hull_edge_split=false;
  // Pinned findBWCavity runs locateRequest when given one carrier.  A point
  // on a face starts from both incident tetrahedra; a point on an edge starts
  // from the complete cyclic edge shell.  Starting from only the hinted cell
  // changes removebadtet_addPnt's first accepted candidate.
  if(initial_seeds.size()==1U) {
    auto carrier=mesh_index.find(canonical(initial_seeds.front()));
    if(carrier==mesh_index.end())return result;
    if(!point_inside_or_on_tet(points,mesh[carrier->second],points[query])) {
      carrier=std::find_if(mesh_index.begin(),mesh_index.end(),[&](const auto& item) {
        return point_inside_or_on_tet(points,mesh[item.second],points[query]);
      });
      if(carrier==mesh_index.end())return result;
    }
    const auto& cell=mesh[carrier->second];
    std::vector<unsigned> zero_faces;
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];
      if(robust_orient(points[face[0]],points[face[1]],points[face[2]],
                       points[query])==0)
        zero_faces.push_back(omitted);
    }
    if(zero_faces.size()>=3U)return result;
    initial_seeds.clear();
    if(zero_faces.size()==2U) {
      std::array<std::uint32_t,2> edge{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)
        if(i!=zero_faces[0]&&i!=zero_faces[1])edge[cursor++]=cell[i];
      for(const auto& candidate:mesh)
        if(std::find(candidate.begin(),candidate.end(),edge[0])!=candidate.end()&&
           std::find(candidate.begin(),candidate.end(),edge[1])!=candidate.end())
          initial_seeds.push_back(candidate);
      std::map<Face,unsigned> shell_faces;
      for(const auto& candidate:initial_seeds)
        for(unsigned omitted=0U;omitted<4U;++omitted) {
          Face face{};unsigned face_cursor{};
          for(unsigned i=0U;i<4U;++i)if(i!=omitted)
            face[face_cursor++]=candidate[i];
          ++shell_faces[face_key(face)];
        }
      finite_hull_edge_split=std::any_of(
          shell_faces.begin(),shell_faces.end(),[&](const auto& item) {
            return item.second==1U&&
                std::find(item.first.begin(),item.first.end(),edge[0])!=
                    item.first.end()&&
                std::find(item.first.begin(),item.first.end(),edge[1])!=
                    item.first.end();
          });
    } else if(zero_faces.size()==1U) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=zero_faces[0])face[cursor++]=cell[i];
      for(const auto& candidate:mesh)
        if(std::all_of(face.begin(),face.end(),[&](const auto vertex) {
             return std::find(candidate.begin(),candidate.end(),vertex)!=
                    candidate.end();
           }))initial_seeds.push_back(candidate);
    } else initial_seeds.push_back(cell);
  }
  // DT::splitBndEdge still uses the normal BW transaction for an open shell;
  // only unrelated legacy callers retain their existing finite projection.
  if(finite_hull_edge_split&&finite_hull_projection_adapter)
    return stellar_insert_constraint_vertex(
        constraints,previous_vertex_count,mesh,false,false);
  for(const auto& seed:initial_seeds) {
    const auto found=mesh_index.find(canonical(seed));
    if(found==mesh_index.end())return result;
    if(selected.insert(found->second).second) {
      pending.push_back(found->second);
      working_order.push_back(found->second);
    }
  }

  std::map<Face,std::vector<std::size_t>> ledger;
  for(std::size_t cell=0U;cell<mesh.size();++cell)
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=mesh[cell][i];
      ledger[face_key(face)].push_back(cell);
    }
  if(std::any_of(ledger.begin(),ledger.end(),[](const auto& item) {
       return item.second.empty()||item.second.size()>2U;
     }))return result;

  std::map<std::uint64_t,std::uint32_t> by_id;
  for(std::size_t i=0U;i<constraints.vertices.size();++i)
    by_id.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
  std::set<Face> recovered_barriers;
  for(const auto& facet:constraints.facets) {
    Face face{};bool resolved=true;
    for(unsigned i=0U;i<3U;++i) {
      const auto found=by_id.find(facet.vertices[i]);
      if(found==by_id.end()){resolved=false;break;}
      face[i]=found->second;
    }
    if(resolved&&ledger.contains(face_key(face)))
      recovered_barriers.insert(face_key(face));
  }
  while(!pending.empty()) {
    const auto cell=pending.back();pending.pop_back();
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=mesh[cell][i];
      const auto key=face_key(face);
      const auto& uses=ledger.at(key);
      if(uses.size()!=2U)continue;
      if(recovered_barriers.contains(key))continue;
      const auto other=uses[0]==cell?uses[1]:uses[0];
      if(!selected.contains(other)&&sphere_contains(points,mesh[other],query,rank)) {
        selected.insert(other);pending.push_back(other);working_order.push_back(other);
      }
    }
  }

  // Pinned `adjustBWCavity`, info == 3: if the cavity contains the complete
  // shell of an already recovered boundary edge, remove the last working
  // cavity tetrahedron in that shell and repeat. Otherwise coning the cavity
  // boundary would erase the recovered edge. A finite hull edge is already
  // safe when it occurs on a cavity-boundary face; the reference represents
  // that case with ghost tetrahedra instead.
  const auto constrained_edges=constrained_parent_boundary_edges(constraints);
  std::vector<bool> plane_expanded(mesh.size());
  const auto preserves_existing_stars=[&](std::size_t added) {
    for(const auto vertex:mesh[added]) {
      if(vertex>=previous_vertex_count)continue;
      bool outside=false;
      for(std::size_t cell=0U;cell<mesh.size();++cell)
        if(cell!=added&&!selected.contains(cell)&&
           std::find(mesh[cell].begin(),mesh[cell].end(),vertex)!=mesh[cell].end()) {
          outside=true;break;
        }
      if(!outside)return false;
    }
    return true;
  };
  for(bool expanded=true;expanded;) {
    expanded=false;
    std::map<Face,unsigned> uses;
    for(const auto cell:selected)
      for(unsigned omitted=0U;omitted<4U;++omitted) {
        Face face{};unsigned cursor{};
        for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=mesh[cell][i];
        ++uses[face_key(face)];
      }
    for(const auto cell:std::vector<std::size_t>(selected.begin(),selected.end())) {
      if(expanded)break;
      for(unsigned omitted=0U;omitted<4U;++omitted) {
        Face face{};unsigned cursor{};
        for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=mesh[cell][i];
        const auto key=face_key(face);
        if(uses[key]!=1U||recovered_barriers.contains(key))continue;
        const auto adjacent=ledger.find(key);
        if(adjacent==ledger.end()||adjacent->second.size()!=2U)continue;
        const auto other=adjacent->second[0]==cell?
            adjacent->second[1]:adjacent->second[0];
        if(selected.contains(other))continue;
        const Tet child{{query,face[0],face[1],face[2]}};
        const auto query_side=robust_orient(
            points[face[0]],points[face[1]],points[face[2]],points[query]);
        const auto interior_side=robust_orient(
            points[face[0]],points[face[1]],points[face[2]],points[mesh[cell][omitted]]);
        const bool closes_expanded_visibility=plane_expanded[cell]&&
            (query_side==0||query_side!=interior_side);
        if((!semantically_coplanar(child)&&!closes_expanded_visibility)||
           !preserves_existing_stars(other))continue;
        selected.insert(other);working_order.push_back(other);
        plane_expanded[other]=true;expanded=true;break;
      }
    }
  }
  for(;;) {
    std::map<Face,unsigned> selected_face_uses;
    for(const auto cell:selected)
      for(unsigned omitted=0U;omitted<4U;++omitted) {
        Face face{};unsigned cursor{};
        for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=mesh[cell][i];
        ++selected_face_uses[face_key(face)];
      }
    bool adjusted=false;
    // The first half of pinned `adjustBWCavity` removes a working cell when
    // one of its exposed faces cannot be positively coned to the insertion
    // point. Express the test without relying on the reference's local face
    // permutation: query and the cell's omitted vertex must lie strictly on
    // the same side of the exposed face.
    for(auto it=working_order.rbegin();it!=working_order.rend()&&!adjusted;++it) {
      if(!selected.contains(*it))continue;
      const auto& cell=mesh[*it];
      for(unsigned omitted=0U;omitted<4U;++omitted) {
        Face face{};unsigned cursor{};
        for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];
        if(selected_face_uses[face_key(face)]!=1U)continue;
        const auto query_side=robust_orient(
            points[face[0]],points[face[1]],points[face[2]],points[query]);
        const auto interior_side=robust_orient(
            points[face[0]],points[face[1]],points[face[2]],points[cell[omitted]]);
        const Tet child{{query,face[0],face[1],face[2]}};
        if(!semantically_coplanar(child)&&query_side!=0&&
           query_side==interior_side)continue;
        selected.erase(*it);
        adjusted=true;
        break;
      }
    }
    if(adjusted) {
      if(selected.empty())return result;
      continue;
    }
    if(protect_recovered_edge_shell)for(const auto& stable_edge:constrained_edges) {
      const auto first=by_id.find(stable_edge[0]);
      const auto second=by_id.find(stable_edge[1]);
      if(first==by_id.end()||second==by_id.end())continue;
      const auto a=first->second,b=second->second;
      std::vector<std::size_t> shell;
      for(std::size_t cell=0U;cell<mesh.size();++cell)
        if(std::find(mesh[cell].begin(),mesh[cell].end(),a)!=mesh[cell].end()&&
           std::find(mesh[cell].begin(),mesh[cell].end(),b)!=mesh[cell].end())
          shell.push_back(cell);
      if(shell.empty()||std::any_of(shell.begin(),shell.end(),[&](auto cell) {
           return !selected.contains(cell);
         }))continue;
      bool exposed=false;
      for(const auto& [face,count]:selected_face_uses) {
        if(count==1U&&std::find(face.begin(),face.end(),a)!=face.end()&&
           std::find(face.begin(),face.end(),b)!=face.end()) {
          exposed=true;break;
        }
      }
      if(exposed)continue;
      for(auto it=working_order.rbegin();it!=working_order.rend();++it) {
        if(!selected.contains(*it))continue;
        const auto& cell=mesh[*it];
        if(std::find(cell.begin(),cell.end(),a)==cell.end()||
           std::find(cell.begin(),cell.end(),b)==cell.end())continue;
        selected.erase(*it);
        adjusted=true;
        break;
      }
      if(adjusted)break;
    }
    if(!adjusted)break;
    if(selected.empty())return result;
  }

  std::map<Face,unsigned> cavity_faces;
  long double old_volume{};
  for(const auto cell:working_order)
    if(selected.contains(cell))result.ordered_cavity_tetrahedra.push_back(mesh[cell]);
  for(const auto cell:selected) {
    result.cavity_tetrahedra.push_back(mesh[cell]);
    old_volume+=std::abs(orient(points[mesh[cell][0]],points[mesh[cell][1]],
                               points[mesh[cell][2]],points[mesh[cell][3]]));
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=mesh[cell][i];
      const auto key=face_key(face);
      ++cavity_faces[key];
    }
  }
  // prepareBWFill emits exposed faces in working-cavity order and then in
  // local face-number order.  That order is also the element-allocation
  // order, so it must survive even though canonical face keys are convenient
  // for incidence counting.
  std::vector<Face> ordered_boundary_faces;
  for(const auto cell:working_order) {
    if(!selected.contains(cell))continue;
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)
        if(i!=omitted)face[cursor++]=mesh[cell][i];
      const auto key=face_key(face);
      if(cavity_faces.at(key)==1U)
        ordered_boundary_faces.push_back(
            wang_bw_boundary_face(mesh[cell],omitted));
    }
  }
  // Pinned pairBWBoundary requires the exposed triangle complex to be closed:
  // each of its edges must pair with exactly one other exposed face edge.
  // Face multiplicity alone does not establish this for a nonmanifold cavity.
  std::map<std::array<std::uint32_t,2>,unsigned> boundary_edge_uses;
  for(const auto& [face,count]:cavity_faces) {
    if(count==2U)continue;
    if(count!=1U)return result;
    for(const auto pair:std::array<std::array<unsigned,2>,3>{
            {{{0U,1U}},{{1U,2U}},{{0U,2U}}}}) {
      std::array<std::uint32_t,2> edge{{face[pair[0]],face[pair[1]]}};
      std::sort(edge.begin(),edge.end());++boundary_edge_uses[edge];
    }
  }
  if(std::any_of(boundary_edge_uses.begin(),boundary_edge_uses.end(),
       [](const auto& item){return item.second!=2U;}))return result;
  std::vector<Tet> replacement;
  long double new_volume{};
  const auto exposed_count=static_cast<std::size_t>(std::count_if(
      cavity_faces.begin(),cavity_faces.end(),
      [](const auto& entry){return entry.second==1U;}));
  if(ordered_boundary_faces.size()!=exposed_count)return result;
  for(const auto& directed:ordered_boundary_faces) {
    Tet ordered_cell{{query,directed[0],directed[1],directed[2]}};
    Tet cell=ordered_cell;
    if(semantically_coplanar(cell)) {
      result.failure=CanonicalLiteralEdgeFlipFailure::nonpositive_replacement;
      return result;
    }
    const auto orientation=robust_orient(points,cell);
    if(orientation==0) {
      result.failure=CanonicalLiteralEdgeFlipFailure::nonpositive_replacement;
      return result;
    }
    if(orientation<0)std::swap(cell[0],cell[1]);
    new_volume+=std::abs(orient(points[cell[0]],points[cell[1]],
                               points[cell[2]],points[cell[3]]));
    result.replacement_tetrahedra.push_back(ordered_cell);
    replacement.push_back(cell);
  }
  if(replacement.empty()||
     std::abs(old_volume-new_volume)>std::max(1.0L,old_volume)*1e-14L) {
    result.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;
    return result;
  }
  for(std::size_t cell=0U;cell<mesh.size();++cell)
    if(!selected.contains(cell))result.tetrahedra.push_back(mesh[cell]);
  result.tetrahedra.insert(
      result.tetrahedra.end(),replacement.begin(),replacement.end());
  // Pinned BW_insert_vertex validates the planned cavity and its paired
  // boundary, then commits it without rescanning unrelated tetrahedra.  That
  // distinction matters in removebadtet_addPnt: it is deliberately invoked
  // while a different tiny tetrahedron may still be inverted.  The cavity
  // face counts, positive cones, and volume agreement above are the local
  // transaction checks corresponding to prepareBWFill/commitBW.
  const auto stable_key=[&](const Tet& cell) {
    std::array<std::uint64_t,4> key{};
    for(unsigned i=0U;i<4U;++i)key[i]=ids[cell[i]];
    std::sort(key.begin(),key.end());return key;
  };
  std::sort(result.tetrahedra.begin(),result.tetrahedra.end(),
            [&](const Tet& left,const Tet& right) {
              return stable_key(left)<stable_key(right);
            });
  std::sort(result.cavity_tetrahedra.begin(),result.cavity_tetrahedra.end(),
            [&](const Tet& left,const Tet& right) {
              return stable_key(left)<stable_key(right);
            });
  result.intersected_tetrahedra=selected.size();
  result.accepted=true;
  result.failure=CanonicalLiteralEdgeFlipFailure::none;
  return result;
}
} // namespace

Vec3 compute_wang_segment_triangle_hit(
    Vec3 segment_start,Vec3 segment_end,Vec3 triangle_a,Vec3 triangle_b,
    Vec3 triangle_c) {
  return wang_segment_plane_hit(segment_start,segment_end,triangle_a,
                                triangle_b,triangle_c);
}

std::array<double,2> compute_wang_segment_triangle_weights(
    Vec3 segment_start,Vec3 segment_end,Vec3 triangle_a,Vec3 triangle_b,
    Vec3 triangle_c) {
  auto start=wang_orient3d_value(triangle_a,triangle_b,triangle_c,segment_start);
  auto end=wang_orient3d_value(triangle_a,triangle_b,triangle_c,segment_end);
  return {{start,end}};
}

WangSegmentBoundaryInsertionResult insert_wang_segment_boundary_steiner_point(
    const CanonicalPlcConstraintSet& constraints,
    std::array<std::uint64_t,2> segment,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::size_t maximum_vertices,std::size_t maximum_facets,
    WangOrderedTetMesh* ordered_mesh) {
  WangSegmentBoundaryInsertionResult result;
  std::sort(segment.begin(),segment.end());result.segment=segment;
  const auto constrained_edges=constrained_parent_boundary_edges(constraints);
  if(segment[0]==segment[1]||!constrained_edges.contains(segment))return result;
  // splitBndEdge gets this direction from SurEdgs[lostE], whose first
  // buildBndInfo insertion is independent of recoverEdge's caller.  Keep
  // canonical `segment` for all identity/provenance work below.
  const auto stored_direction=wang_build_bnd_info_direction(constraints,segment);
  if(!stored_direction)return result;
  const auto directed_segment=*stored_direction;
  const auto before_inspection=inspect_canonical_plc_tetrahedra(
      constraints,tetrahedra);
  if(before_inspection.failure!=CanonicalPlcSeedFailure::none&&
     before_inspection.failure!=CanonicalPlcSeedFailure::unrecovered_constraint)
    return result;
  if(std::find(before_inspection.missing_edges.begin(),
               before_inspection.missing_edges.end(),segment)==
     before_inspection.missing_edges.end()) {
    result.failure=WangSegmentBoundaryInsertionFailure::segment_already_recovered;
    return result;
  }
  const auto before_measure=unrecovered_constraint_measure(
      constraints,tetrahedra);
  if(!before_measure)return result;
  result.unrecovered_measure_before=*before_measure;

  const auto attempt=[&](const WangBoundarySeed& seed) {
    WangSegmentBoundaryInsertionResult candidate;
    candidate.segment=segment;
    candidate.unrecovered_measure_before=*before_measure;
    candidate.split_numerator=seed.numerator;
    candidate.split_denominator=seed.denominator;
    candidate.used_midpoint_fallback=seed.midpoint_fallback;
    candidate.intersected_mesh_edge=seed.intersected_edge;
    candidate.intersected_mesh_face=seed.intersected_face;
    candidate.bowyer_watson_seed_tetrahedra=seed.tetrahedra;
    candidate.seed_tetrahedron=seed.tetrahedra.front();
    auto split=split_canonical_plc_constraint_edge_at_ratio(
        constraints,segment,seed.numerator,seed.denominator,
        maximum_vertices,maximum_facets);
    candidate.constraint_failure=split.failure;
    if(!split.accepted()) {
      candidate.failure=WangSegmentBoundaryInsertionFailure::constraint_split_failed;
      return candidate;
    }
    // Keep exact boundary-facet lineage in `split_vertices`, but use the
    // source intersection coordinate for BW and every subsequent predicate.
    split.constraints.vertices.back().position=seed.position;
    const auto inserted=bowyer_watson_insert_constraint_vertex(
        split.constraints,constraints.vertices.size(),tetrahedra,
        seed.tetrahedra,true,false);
    candidate.insertion_failure=inserted.failure;
    candidate.bowyer_watson_cavity=inserted.cavity_tetrahedra;
    if(!inserted.accepted) {
      candidate.failure=WangSegmentBoundaryInsertionFailure::bowyer_watson_failed;
      return candidate;
    }
    candidate.ordered_cavity_tetrahedra=inserted.ordered_cavity_tetrahedra;
    candidate.replacement_tetrahedra=inserted.replacement_tetrahedra;
    if(!preserves_recovered_constraints_across_edge_split(
           constraints,tetrahedra,split.constraints,inserted.tetrahedra)) {
      candidate.failure=
          WangSegmentBoundaryInsertionFailure::previously_recovered_constraint_lost;
      return candidate;
    }
    const auto split_id=split.constraints.vertices.back().id;
    const std::array<std::array<std::uint64_t,2>,2> children{{
        {{segment[0],split_id}},{{split_id,segment[1]}}}};
    for(auto child:children) {
      std::sort(child.begin(),child.end());
      if(mesh_contains_stable_edge(split.constraints,child,inserted.tetrahedra)) {
        candidate.recovered_child_segment=child;
        break;
      }
    }
    const auto after_measure=unrecovered_constraint_measure(
        split.constraints,inserted.tetrahedra);
    if(after_measure)candidate.unrecovered_measure_after=*after_measure;
    candidate.constraints=split.constraints;
    candidate.tetrahedra=inserted.tetrahedra;
    candidate.failure=WangSegmentBoundaryInsertionFailure::none;
    return candidate;
  };

  if(const auto intersection=select_wang_boundary_intersection_seed(
         constraints,directed_segment,tetrahedra,ordered_mesh)) {
    auto candidate=attempt(*intersection);
    if(candidate.accepted())return candidate;
    result=std::move(candidate);
    // Pinned splitBndEdge(info=1) recursively selects info=0 whenever the
    // intersection insertion cannot be represented/committed.  Exact parent
    // barycentrics are a prototype-only representation: exhausting their
    // compact uint64_t form is therefore the representation analogue of an
    // unusable intersection, not a paper-stage recovery failure.  Preserve
    // the pinned midpoint retry for that case.  Capacity, topology, and other
    // constraint failures remain hard failures rather than being hidden by a
    // broader fallback rule.
    const auto retry_midpoint=
        result.failure==WangSegmentBoundaryInsertionFailure::bowyer_watson_failed||
        (result.failure==WangSegmentBoundaryInsertionFailure::constraint_split_failed&&
         result.constraint_failure==CanonicalPlcConstraintFailure::rational_overflow);
    if(!retry_midpoint)
      return result;
  }
  const auto midpoint=select_wang_boundary_midpoint_seed(
      constraints,directed_segment,tetrahedra);
  if(!midpoint) {
    if(result.failure==WangSegmentBoundaryInsertionFailure::invalid_segment)
      result.failure=
          WangSegmentBoundaryInsertionFailure::no_intersected_tetrahedron;
    return result;
  }
  return attempt(*midpoint);
}

CanonicalBoundaryRestorationResult relocate_last_boundary_steiner_point(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<Tet>& tetrahedra,
    const CanonicalPlcRecoveryInsertion& entry,
    std::size_t retry_level=0U) {
  // Pinned removeEdgStiner rebuilds the sub-triangle hash and invokes
  // recoverFace(..., 0) for every missing child face before classifying the
  // Steiner point sphere. Tiny-tet repair can remove one of those barriers,
  // so this pass is required again on recursive entry.
  auto working_mesh=tetrahedra;
  for(const auto& facet:constraints.facets) {
    if(std::find(facet.vertices.begin(),facet.vertices.end(),entry.vertex_id)==
       facet.vertices.end())continue;
    const auto recovered=try_recover_wang_facet_by_local_flips(
        constraints,facet.vertices,working_mesh);
    if(recovered.edge_removals>0U)
      working_mesh=recovered.tetrahedra;
  }
  CanonicalBoundaryRestorationResult refused;
  refused.constraints=constraints;refused.tetrahedra=working_mesh;
  refused.failure=CanonicalBoundaryRestorationFailure::incompatible_one_ring;
  std::map<std::uint64_t,std::uint32_t> index;
  std::uint64_t next_id{};
  for(std::size_t i=0U;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    next_id=std::max(next_id,constraints.vertices[i].id);
  }
  if(!index.contains(entry.vertex_id))return refused;
  const auto steiner=index.at(entry.vertex_id);

  if(entry.kind==CanonicalPlcRecoveryInsertionKind::edge_split) {
    const auto split=std::find_if(
        constraints.split_vertices.begin(),constraints.split_vertices.end(),
        [&](const auto& candidate){return candidate.id==entry.vertex_id;});
    if(split!=constraints.split_vertices.end()&&
       index.contains(split->edge[0])&&index.contains(split->edge[1])) {
      const std::array<std::uint32_t,2> coarse{{
          index.at(split->edge[0]),index.at(split->edge[1])}};
      const auto present=std::any_of(
          working_mesh.begin(),working_mesh.end(),[&](const auto& cell) {
            return std::find(cell.begin(),cell.end(),coarse[0])!=cell.end()&&
                   std::find(cell.begin(),cell.end(),coarse[1])!=cell.end();
          });
      if(present) {
        std::vector<CanonicalLiteralEdgeFlipResult> candidates;
        const auto removed=remove_mesh_edge_in_mesh(
            constraints,coarse,working_mesh,&candidates,true,false);
        if(removed.accepted)
          return relocate_last_boundary_steiner_point(
              constraints,removed.tetrahedra,entry,retry_level);
        if(!entry.facets_before.empty()) {
          auto interior_constraints=constraints;
          interior_constraints.facets=entry.facets_before;
          interior_constraints.split_vertices.erase(
              std::remove_if(interior_constraints.split_vertices.begin(),
                             interior_constraints.split_vertices.end(),
                             [&](const auto& candidate) {
                               return candidate.id==entry.vertex_id;
                             }),
              interior_constraints.split_vertices.end());
          interior_constraints.recovery_journal.pop_back();
          interior_constraints.interior_steiner_vertices.push_back(
              {entry.vertex_id,
               CanonicalInteriorSteinerKind::boundary_relocation});
          CanonicalBoundaryRestorationResult restored;
          restored.constraints=std::move(interior_constraints);
          restored.tetrahedra=std::move(working_mesh);
          restored.restored_points=1U;
          restored.interior_points_inserted=1U;
          restored.relocation_regions=1U;
          restored.failure=CanonicalBoundaryRestorationFailure::none;
          return restored;
        }
      }
    }
  }

  std::map<Face,std::array<std::uint64_t,3>> barriers;
  for(const auto& facet:constraints.facets) {
    if(std::find(facet.vertices.begin(),facet.vertices.end(),entry.vertex_id)==
       facet.vertices.end())continue;
    Face face{};bool resolved=true;
    for(unsigned i=0U;i<3U;++i) {
      const auto found=index.find(facet.vertices[i]);
      if(found==index.end()){resolved=false;break;}face[i]=found->second;
    }
    auto immediate_parent=facet.source_vertices;
    const auto predecessor=std::find_if(
        entry.replaced_facets.begin(),entry.replaced_facets.end(),
        [&](const auto& candidate) {
          if(candidate.parent!=facet.parent||
             candidate.source_vertices!=facet.source_vertices)return false;
          for(const auto id:facet.vertices)
            if(id!=entry.vertex_id&&
               std::find(candidate.vertices.begin(),candidate.vertices.end(),id)==
                   candidate.vertices.end())return false;
          return true;
        });
    // Pinned removeEdgStiner reads SurTris[Tid], the immediate triangles
    // incident to the edge before it was split. For a nested split these can
    // contain an earlier Steiner point; immutable root source_vertices are
    // therefore not the relocation boundary or the bridge face.
    if(predecessor!=entry.replaced_facets.end())
      immediate_parent=predecessor->vertices;
    if(resolved)barriers.emplace(face_key(face),immediate_parent);
  }
  if(barriers.empty())return refused;

  std::vector<std::size_t> incident_global;
  for(std::size_t i=0U;i<working_mesh.size();++i)
    if(std::find(working_mesh[i].begin(),working_mesh[i].end(),steiner)!=
       working_mesh[i].end())incident_global.push_back(i);
  if(incident_global.empty())return refused;
  std::map<Face,std::vector<std::size_t>> face_cells;
  for(std::size_t local=0U;local<incident_global.size();++local) {
    const auto& cell=working_mesh[incident_global[local]];
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];
      face=face_key(face);
      if(std::find(face.begin(),face.end(),steiner)!=face.end())
        face_cells[face].push_back(local);
    }
  }
  std::vector<std::vector<std::size_t>> adjacency(incident_global.size());
  for(const auto& [face,cells]:face_cells) {
    if(barriers.contains(face)||cells.size()!=2U)continue;
    adjacency[cells[0]].push_back(cells[1]);
    adjacency[cells[1]].push_back(cells[0]);
  }
  std::vector<std::size_t> component(incident_global.size(),
                                     std::numeric_limits<std::size_t>::max());
  std::size_t component_count{};
  for(std::size_t seed=0U;seed<component.size();++seed) {
    if(component[seed]!=std::numeric_limits<std::size_t>::max())continue;
    std::vector<std::size_t> stack{seed};component[seed]=component_count;
    while(!stack.empty()) {
      const auto cell=stack.back();stack.pop_back();
      for(const auto neighbour:adjacency[cell])
        if(component[neighbour]==std::numeric_limits<std::size_t>::max()) {
          component[neighbour]=component_count;stack.push_back(neighbour);
        }
    }
    ++component_count;
  }
  if(component_count<=1U)return refused;
  refused.relocation_regions=component_count;

  std::vector<CanonicalPlcConstraintFacet> restored_facets;
  if(!entry.facets_before.empty()) {
    restored_facets=entry.facets_before;
  } else if(!entry.replaced_facets.empty()) {
    for(const auto& facet:constraints.facets)
      if(std::find(facet.vertices.begin(),facet.vertices.end(),entry.vertex_id)==
         facet.vertices.end())restored_facets.push_back(facet);
    restored_facets.insert(restored_facets.end(),entry.replaced_facets.begin(),
                           entry.replaced_facets.end());
  } else {
    std::vector<bool> consumed(constraints.facets.size());
    const auto expected_children=
        entry.kind==CanonicalPlcRecoveryInsertionKind::edge_split?2U:3U;
    for(std::size_t i=0U;i<constraints.facets.size();++i) {
    if(consumed[i])continue;
    const auto& first=constraints.facets[i];
    if(std::find(first.vertices.begin(),first.vertices.end(),entry.vertex_id)==
       first.vertices.end()) {
      restored_facets.push_back(first);consumed[i]=true;continue;
    }
    std::vector<std::size_t> group{i};
    for(std::size_t j=i+1U;j<constraints.facets.size();++j) {
      if(consumed[j])continue;
      const auto& second=constraints.facets[j];
      if(second.parent==first.parent&&second.source_vertices==first.source_vertices&&
         std::find(second.vertices.begin(),second.vertices.end(),entry.vertex_id)!=
             second.vertices.end())group.push_back(j);
    }
    if(group.size()!=expected_children) {
      refused.failure=CanonicalBoundaryRestorationFailure::constraint_reconstruction_failed;
      return refused;
    }
    auto merged=first;merged.vertices=first.source_vertices;
    for(unsigned corner=0U;corner<3U;++corner) {
      const auto id=merged.vertices[corner];
      const auto take=[&](const auto& facet)->std::optional<FacetBarycentricPoint> {
        const auto found=std::find(facet.vertices.begin(),facet.vertices.end(),id);
        if(found==facet.vertices.end())return std::nullopt;
        return facet.corners[static_cast<std::size_t>(found-facet.vertices.begin())];
      };
      std::optional<FacetBarycentricPoint> value;
      for(const auto child:group) {
        value=take(constraints.facets[child]);if(value)break;
      }
      if(!value) {
        refused.failure=CanonicalBoundaryRestorationFailure::constraint_reconstruction_failed;
        return refused;
      }
      merged.corners[corner]=*value;
    }
    restored_facets.push_back(std::move(merged));
      for(const auto child:group)consumed[child]=true;
    }
  }

  std::vector<Point> points;
  for(const auto& vertex:constraints.vertices) {
    const auto p=vertex.position;points.push_back({p.x,p.y,p.z});
  }
  const auto origin=points[steiner];
  std::vector<Point> directions(component_count);
  std::vector<std::size_t> direction_contributions(component_count);
  std::vector<long double> steps(component_count,
                                  std::numeric_limits<long double>::max());
  std::vector<std::set<std::array<std::uint64_t,3>>> component_sources(
      component_count);
  for(const auto& [face,source]:barriers) {
    const auto uses=face_cells.find(face);
    if(uses==face_cells.end())continue;
    for(const auto local:uses->second) {
      const auto region=component[local];
      const auto first_source_use=component_sources[region].insert(source).second;
      // removeEdgStiner's PTV map contributes each original incident facet
      // once per half-ball.  Both split children bound the same half-ball in
      // the prototype, so accumulating per child would double the normal.
      if(entry.kind==CanonicalPlcRecoveryInsertionKind::edge_split&&
         !first_source_use)continue;
      const auto& cell=working_mesh[incident_global[local]];
      const auto opposite=*std::find_if(cell.begin(),cell.end(),[&](const auto vertex){
        return std::find(face.begin(),face.end(),vertex)==face.end();
      });
      Face normal_face=face;
      if(entry.kind==CanonicalPlcRecoveryInsertionKind::edge_split) {
        bool resolved=true;
        for(unsigned i=0U;i<3U;++i) {
          const auto found=index.find(source[i]);
          if(found==index.end()){resolved=false;break;}
          normal_face[i]=found->second;
        }
        if(!resolved)return refused;
      }
      auto normal=cross(points[normal_face[1]]-points[normal_face[0]],
                        points[normal_face[2]]-points[normal_face[0]]);
      if(dot(normal,points[opposite]-points[normal_face[0]])<0.0L)
        normal=normal*-1.0L;
      const auto length=std::sqrt(dot(normal,normal));
      if(length>0.0L) {
        directions[region]=directions[region]+normal*(1.0L/length);
        ++direction_contributions[region];
      }
    }
  }
  if(entry.kind==CanonicalPlcRecoveryInsertionKind::facet_split) {
    if(component_count!=2U)return refused;
    const auto record=std::find_if(
        constraints.facet_split_vertices.begin(),
        constraints.facet_split_vertices.end(),
        [&](const auto& candidate){return candidate.id==entry.vertex_id;});
    if(record==constraints.facet_split_vertices.end())return refused;
    long double step=std::numeric_limits<long double>::max();
    for(const auto id:record->parent.vertex_ids) {
      const auto vertex=index.find(id);
      if(vertex==index.end())return refused;
      const auto delta=points[vertex->second]-origin;
      step=std::min(step,std::sqrt(dot(delta,delta)));
    }
    for(std::size_t region=0U;region<component_count;++region) {
      // removeTriStiner sums the three oriented child-subfacet normals and
      // divides by three; it deliberately does not normalize that average.
      if(direction_contributions[region]!=3U)return refused;
      directions[region]=directions[region]*(1.0L/3.0L);
      steps[region]=step;
    }
  } else {
    const auto record=std::find_if(
        constraints.split_vertices.begin(),constraints.split_vertices.end(),
        [&](const auto& candidate){return candidate.id==entry.vertex_id;});
    if(record==constraints.split_vertices.end()||
       !index.contains(record->edge[0])||!index.contains(record->edge[1]))
      return refused;
    const auto delta=points[index.at(record->edge[1])]-
                     points[index.at(record->edge[0])];
    const auto step=std::sqrt(dot(delta,delta))/10.0L;
    for(std::size_t region=0U;region<component_count;++region) {
      // removeEdgStiner requires the two original facets bounding this
      // half-ball region and averages their unit normals without a final
      // normalization.
      if(component_sources[region].size()!=2U||
         direction_contributions[region]!=2U)return refused;
      directions[region]=directions[region]*(1.0L/2.0L);
      steps[region]=step;
    }
  }
  for(std::size_t region=0U;region<component_count;++region) {
    if(!(dot(directions[region],directions[region])>0.0L)||
       !(steps[region]>0.0L)||component_sources[region].empty())
      return refused;
  }
  const auto boundary_of=[](const auto& cells,
                            std::pair<Face,unsigned>* invalid=nullptr) {
    std::map<Face,unsigned> counts;
    for(const auto& cell:cells)for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];
      ++counts[face_key(face)];
    }
    std::set<Face> boundary;
    for(const auto& [face,count]:counts) {
      if(count==1U)boundary.insert(face);
      else if(count!=2U) {
        if(invalid)*invalid={face,count};
        return std::set<Face>{};
      }
    }
    return boundary;
  };
  std::vector<Tet> old_cavity;
  for(const auto global:incident_global)old_cavity.push_back(working_mesh[global]);
  const auto old_boundary=boundary_of(old_cavity);
  long double old_volume{};
  for(const auto& cell:old_cavity)
    old_volume+=std::abs(orient(points[cell[0]],points[cell[1]],
                               points[cell[2]],points[cell[3]]));

  std::vector<Point> source_relocation_points(component_count);
  {
    const auto shortest_distance=
        entry.kind==CanonicalPlcRecoveryInsertionKind::facet_split?1e-10L:1e-16L;
    for(std::size_t region=0U;region<component_count;++region) {
      bool positioned=false;
      for(long double step=steps[region];step>shortest_distance;step*=0.5L) {
        const auto candidate=origin+directions[region]*step;
        bool preserves_region=true;
        for(std::size_t local=0U;local<incident_global.size();++local) {
          if(component[local]!=region)continue;
          auto before=working_mesh[incident_global[local]];
          auto after=before;
          const auto at=std::find(after.begin(),after.end(),steiner);
          if(at==after.end()){preserves_region=false;break;}
          const auto original_sign=robust_orient(points,before);
          std::vector<Point> candidate_points=points;
          candidate_points[steiner]=candidate;
          const auto candidate_sign=robust_orient(candidate_points,after);
          if(original_sign==0||candidate_sign!=original_sign) {
            preserves_region=false;
            break;
          }
        }
        if(!preserves_region)continue;
        source_relocation_points[region]=candidate;
        positioned=true;
        break;
      }
      if(!positioned) {
        // On recursive removeEdgStiner calls the pinned implementation keeps
        // the original Steiner position for a region that still has no
        // positive displacement, then continues with the bridge rebuild and
        // point-removal stages.  Only the level-zero non-hull failure enters
        // the topology-repair scan below.
        if(entry.kind==CanonicalPlcRecoveryInsertionKind::edge_split&&
           retry_level>0U) {
          source_relocation_points[region]=origin;
          continue;
        }
        // removeEdgStiner alone has a recursive recovery branch.  At level
        // zero, a non-hull region with no positive relocation position runs
        // removebadtet on each incident tet below the pinned 1e-14 volume
        // threshold, then retries the entire edge restoration once.
        if(entry.kind!=CanonicalPlcRecoveryInsertionKind::edge_split||
           retry_level!=0U)return refused;
        std::map<Face,unsigned> global_face_uses;
        for(const auto& cell:working_mesh)
          for(unsigned omitted=0U;omitted<4U;++omitted) {
            Face face{};unsigned cursor{};
            for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];
            ++global_face_uses[face_key(face)];
          }
        bool hull_region=false;
        for(const auto global:incident_global) {
          const auto& cell=working_mesh[global];
          for(unsigned omitted=0U;omitted<4U;++omitted) {
            Face face{};unsigned cursor{};
            for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];
            face=face_key(face);
            if(std::find(face.begin(),face.end(),steiner)!=face.end()&&
               global_face_uses[face]==1U)hull_region=true;
          }
        }
        if(hull_region)return refused;

        CanonicalBoundaryRestorationResult::RetryAttempt trace;
        trace.level=retry_level;
        auto improved_constraints=constraints;
        auto improved_mesh=working_mesh;
        const std::array<std::array<unsigned,2>,6> edge_order{{
            {{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
        const auto canonical=[](Tet cell){std::sort(cell.begin(),cell.end());return cell;};
        using RepairPointKind=CanonicalBoundaryRestorationResult::RepairPointCandidateKind;
        const auto repair_point=[&](Point candidate,RepairPointKind kind,
                                    std::optional<Tet> preferred_seed=std::nullopt) {
          ++trace.repair_point_candidate_attempts;
          trace.repair_point_candidates.push_back({kind,as_vec3(candidate)});
          std::uint64_t next_id{};
          for(const auto& item:improved_constraints.vertices)
            next_id=std::max(next_id,item.id);
          if(next_id==std::numeric_limits<std::uint64_t>::max())return false;
          auto augmented=improved_constraints;
          augmented.vertices.push_back({next_id+1U,as_vec3(candidate)});
          augmented.interior_steiner_vertices.push_back(
              {next_id+1U,CanonicalInteriorSteinerKind::topology_repair});
          std::vector<Point> augmented_points;
          for(const auto& item:augmented.vertices) {
            const auto p=item.position;
            augmented_points.push_back({p.x,p.y,p.z});
          }
          std::optional<Tet> seed;
          if(preferred_seed&&point_inside_or_on_tet(
               augmented_points,*preferred_seed,augmented_points.back()))seed=*preferred_seed;
          if(!seed)for(const auto& cell:improved_mesh)
            if(point_inside_or_on_tet(
                 augmented_points,cell,augmented_points.back())){seed=cell;break;}
          if(!seed){++trace.repair_point_location_refusals;return false;}
          const auto inserted=bowyer_watson_insert_constraint_vertex(
              augmented,improved_constraints.vertices.size(),improved_mesh,{*seed},
              false);
          if(!inserted.accepted) {
            ++trace.repair_point_insertion_refusals[static_cast<std::size_t>(inserted.failure)];
            return false;
          }
          ++trace.repair_point_insertions;
          ++trace.repair_point_smoothing_attempts;
          auto smoothed=smooth_canonical_interior_steiner_sus(
              augmented,inserted.tetrahedra,next_id+1U);
          if(smoothed.moved)++trace.repair_point_smoothing_moves;
          improved_constraints=std::move(smoothed.constraints);
          improved_mesh=std::move(smoothed.tetrahedra);
          return true;
        };
        const auto constrained_edges=constrained_parent_boundary_edges(constraints);
        std::set<std::uint32_t> boundary_vertices;
        for(const auto& facet:constraints.facets)
          for(const auto id:facet.vertices) {
            const auto found=index.find(id);
            if(found!=index.end())boundary_vertices.insert(found->second);
          }
        for(const auto global:incident_global) {
          const auto original_cell=working_mesh[global];
          const auto six_volume=std::abs(orient(
              points[original_cell[0]],points[original_cell[1]],
              points[original_cell[2]],points[original_cell[3]]));
          if(six_volume/6.0L>=1e-14L)continue;
          ++trace.tiny_tetrahedra;
          const auto current=std::find_if(
              improved_mesh.begin(),improved_mesh.end(),
              [&](const auto& cell){return canonical(cell)==canonical(original_cell);});
          if(current==improved_mesh.end())continue;
          const auto current_cell=*current;
          bool changed=false;
          for(const auto pair:edge_order) {
            ++trace.edge_attempts;
            auto removed=remove_mesh_edge_in_mesh(
                improved_constraints,
                {current_cell[pair[0]],current_cell[pair[1]]},
                improved_mesh,nullptr,false,false);
            if(!removed.accepted)continue;
            improved_mesh=std::move(removed.tetrahedra);
            ++trace.accepted_topology_mutations;
            changed=true;
            break;
          }
          if(changed)continue;
          for(unsigned omitted=0U;omitted<4U;++omitted) {
            Face face{};unsigned cursor{};
            for(unsigned i=0U;i<4U;++i)
              if(i!=omitted)face[cursor++]=current_cell[i];
            ++trace.face_attempts;
            auto removed=face_flip_for_specific_face(
                improved_constraints,improved_mesh,face,false);
            if(!removed.accepted)continue;
            improved_mesh=std::move(removed.tetrahedra);
            ++trace.accepted_topology_mutations;
            changed=true;
            break;
          }
          if(changed)continue;
          ++trace.repair_point_attempts;
          const auto live=std::find_if(
              improved_mesh.begin(),improved_mesh.end(),
              [&](const auto& cell){return canonical(cell)==canonical(original_cell);});
          if(live==improved_mesh.end())continue;
          const auto bad_cell=*live;
          if(std::count_if(bad_cell.begin(),bad_cell.end(),
                 [&](const auto corner){return boundary_vertices.contains(corner);})<4)
            continue;

          // Fixed-boundary removebadtet_addPnt first searches the union of
          // the four vertex spheres for the longest eligible mesh edge.
          std::set<std::size_t> neighbourhood;
          for(std::size_t i=0U;i<improved_mesh.size();++i)
            if(std::any_of(bad_cell.begin(),bad_cell.end(),[&](const auto corner) {
              return std::find(improved_mesh[i].begin(),improved_mesh[i].end(),corner)!=
                  improved_mesh[i].end();
            }))neighbourhood.insert(i);
          std::optional<std::array<std::uint32_t,2>> longest;
          long double longest_squared{};
          for(const auto cell_index:neighbourhood)
            for(const auto pair:edge_order) {
              std::array<std::uint32_t,2> edge{{
                  improved_mesh[cell_index][pair[0]],
                  improved_mesh[cell_index][pair[1]]}};
              std::array<std::uint64_t,2> stable{{
                  improved_constraints.vertices[edge[0]].id,
                  improved_constraints.vertices[edge[1]].id}};
              std::sort(stable.begin(),stable.end());
              if(constrained_edges.contains(stable))continue;
              const auto a=improved_constraints.vertices[edge[0]].position;
              const auto b=improved_constraints.vertices[edge[1]].position;
              const auto dx=static_cast<long double>(b.x)-a.x;
              const auto dy=static_cast<long double>(b.y)-a.y;
              const auto dz=static_cast<long double>(b.z)-a.z;
              const auto squared=dx*dx+dy*dy+dz*dz;
              if(!longest||squared>longest_squared) {
                longest=edge;longest_squared=squared;
              }
            }
          if(longest) {
            const auto a=improved_constraints.vertices[(*longest)[0]].position;
            const auto b=improved_constraints.vertices[(*longest)[1]].position;
            repair_point(Point{(a.x+b.x)*0.5L,(a.y+b.y)*0.5L,
                               (a.z+b.z)*0.5L},
                         RepairPointKind::neighbourhood_edge_midpoint,bad_cell);
          }

          auto bad_still_live=[&]() {
            return std::find_if(
                improved_mesh.begin(),improved_mesh.end(),
                [&](const auto& cell){return canonical(cell)==canonical(bad_cell);});
          };
          if(bad_still_live()==improved_mesh.end())continue;
          std::array<std::pair<long double,std::array<std::uint32_t,2>>,6> ordered{};
          for(std::size_t i=0U;i<edge_order.size();++i) {
            const auto pair=edge_order[i];
            const std::array<std::uint32_t,2> edge{{
                bad_cell[pair[0]],bad_cell[pair[1]]}};
            const auto a=improved_constraints.vertices[edge[0]].position;
            const auto b=improved_constraints.vertices[edge[1]].position;
            const auto dx=static_cast<long double>(b.x)-a.x;
            const auto dy=static_cast<long double>(b.y)-a.y;
            const auto dz=static_cast<long double>(b.z)-a.z;
            ordered[i]={dx*dx+dy*dy+dz*dz,edge};
          }
          std::stable_sort(ordered.begin(),ordered.end(),
              [](const auto& a,const auto& b){return a.first>b.first;});
          std::size_t boundary_edge_count{};
          for(const auto pair:edge_order) {
            std::array<std::uint64_t,2> stable{{
                improved_constraints.vertices[bad_cell[pair[0]]].id,
                improved_constraints.vertices[bad_cell[pair[1]]].id}};
            std::sort(stable.begin(),stable.end());
            if(constrained_edges.contains(stable))++boundary_edge_count;
          }
          for(const auto& [length,edge]:ordered) {
            (void)length;
            if(bad_still_live()==improved_mesh.end())break;
            std::array<std::uint64_t,2> stable{{
                improved_constraints.vertices[edge[0]].id,
                improved_constraints.vertices[edge[1]].id}};
            std::sort(stable.begin(),stable.end());
            if(constrained_edges.contains(stable))continue;
            Point candidate{};std::size_t contributors{};
            auto kind=RepairPointKind::bad_cell_edge_midpoint;
            if(boundary_edge_count==5U) {
              // Pinned nBndpnt==4 && nBndedg==5 branch: average the free
              // edge endpoints with the cyclic shell vertices, rather than
              // inserting the edge midpoint.
              std::set<std::uint32_t> shell_vertices;
              for(const auto& cell:improved_mesh)
                if(std::find(cell.begin(),cell.end(),edge[0])!=cell.end()&&
                   std::find(cell.begin(),cell.end(),edge[1])!=cell.end())
                  for(const auto corner:cell)
                    if(corner!=edge[0]&&corner!=edge[1])shell_vertices.insert(corner);
              const auto first=improved_constraints.vertices[edge[0]].position;
              const auto second=improved_constraints.vertices[edge[1]].position;
              candidate={first.x+second.x,first.y+second.y,first.z+second.z};
              contributors=2U;
              for(const auto corner:shell_vertices) {
                const auto point=improved_constraints.vertices[corner].position;
                candidate=candidate+Point{point.x,point.y,point.z};++contributors;
              }
              candidate=candidate*(1.0L/static_cast<long double>(contributors));
              kind=RepairPointKind::five_boundary_edge_shell_average;
            } else {
              const auto first=improved_constraints.vertices[edge[0]].position;
              const auto second=improved_constraints.vertices[edge[1]].position;
              candidate=Point{(first.x+second.x)*0.5L,(first.y+second.y)*0.5L,
                              (first.z+second.z)*0.5L};
            }
            repair_point(candidate,kind,bad_cell);
          }
          if(bad_still_live()==improved_mesh.end())continue;
          Point centroid{};
          for(const auto corner:bad_cell) {
            const auto p=improved_constraints.vertices[corner].position;
            centroid=centroid+Point{p.x,p.y,p.z};
          }
          repair_point(centroid*0.25L,RepairPointKind::bad_cell_centroid,bad_cell);
        }
        trace.recursive_retry_invoked=true;
        auto retried=relocate_last_boundary_steiner_point(
            improved_constraints,improved_mesh,entry,retry_level+1U);
        retried.retry_attempts.insert(retried.retry_attempts.begin(),trace);
        return retried;
      }
    }
  }

  constexpr std::size_t maximum_attempts=1U;
  for(std::size_t attempt=0U;attempt<maximum_attempts;++attempt) {
    auto augmented=constraints;
    augmented.facets=restored_facets;
    augmented.split_vertices.erase(
        std::remove_if(augmented.split_vertices.begin(),augmented.split_vertices.end(),
                       [&](const auto& candidate){return candidate.id==entry.vertex_id;}),
        augmented.split_vertices.end());
    augmented.facet_split_vertices.erase(
        std::remove_if(augmented.facet_split_vertices.begin(),
                       augmented.facet_split_vertices.end(),
                       [&](const auto& candidate){return candidate.id==entry.vertex_id;}),
        augmented.facet_split_vertices.end());
    augmented.recovery_journal.pop_back();
    std::vector<std::uint32_t> relocated(component_count);
    std::vector<std::uint64_t> relocated_ids;
    auto trial_points=points;
    std::vector<bool> retained_origin(component_count);
    std::vector<Vec3> relocated_positions;
    for(std::size_t region=0U;region<component_count;++region) {
      const auto candidate=source_relocation_points[region];
      const Vec3 position{static_cast<double>(candidate.x),
                          static_cast<double>(candidate.y),
                          static_cast<double>(candidate.z)};
      retained_origin[region]=
          position.x==constraints.vertices[steiner].position.x&&
          position.y==constraints.vertices[steiner].position.y&&
          position.z==constraints.vertices[steiner].position.z;
      relocated[region]=static_cast<std::uint32_t>(augmented.vertices.size());
      const auto id=++next_id;
      relocated_ids.push_back(id);
      augmented.vertices.push_back({id,position});
      augmented.interior_steiner_vertices.push_back(
          {id,CanonicalInteriorSteinerKind::boundary_relocation});
      trial_points.push_back({position.x,position.y,position.z});
      relocated_positions.push_back(position);
    }
    std::vector<Tet> new_cavity;
    std::size_t bridge_tetrahedra{};
    bool valid=true;
    for(std::size_t local=0U;local<incident_global.size();++local) {
      auto cell=working_mesh[incident_global[local]];
      *std::find(cell.begin(),cell.end(),steiner)=relocated[component[local]];
      const auto orientation=robust_orient(trial_points,cell);
      if(orientation==0){
        refused.last_retriangulation_rejection=
            CanonicalBoundaryRetriangulationRejection::zero_incident_tetrahedron;
        valid=false;break;
      }
      if(orientation<0)std::swap(cell[0],cell[1]);new_cavity.push_back(cell);
    }
    if(!valid)continue;
    for(std::size_t region=0U;region<component_count&&valid;++region)
      for(const auto& source:component_sources[region]) {
        Tet bridge{{relocated[region],0U,0U,0U}};
        for(unsigned i=0U;i<3U;++i) {
          const auto found=index.find(source[i]);
          if(found==index.end()){valid=false;break;}bridge[i+1U]=found->second;
        }
        if(!valid)break;
        const auto orientation=robust_orient(trial_points,bridge);
        if(orientation==0&&!retained_origin[region]){
          refused.last_retriangulation_rejection=
              CanonicalBoundaryRetriangulationRejection::zero_bridge_tetrahedron;
          valid=false;break;
        }
        if(orientation<0)std::swap(bridge[0],bridge[1]);
        new_cavity.push_back(bridge);
        ++bridge_tetrahedra;
      }
    if(!valid)continue;
    std::pair<Face,unsigned> invalid_boundary{};
    const auto new_boundary=boundary_of(new_cavity,&invalid_boundary);
    if(new_boundary!=old_boundary) {
      refused.last_retriangulation_rejection=
          CanonicalBoundaryRetriangulationRejection::cavity_boundary_mismatch;
      std::vector<Face> old_only,new_only;
      std::set_difference(old_boundary.begin(),old_boundary.end(),
                          new_boundary.begin(),new_boundary.end(),
                          std::back_inserter(old_only));
      std::set_difference(new_boundary.begin(),new_boundary.end(),
                          old_boundary.begin(),old_boundary.end(),
                          std::back_inserter(new_only));
      const auto stable_face=[&](const Face& face) {
        std::array<std::uint64_t,3> stable{};
        for(unsigned i=0U;i<3U;++i)
          stable[i]=face[i]<constraints.vertices.size()
              ?constraints.vertices[face[i]].id
              :relocated_ids[face[i]-constraints.vertices.size()];
        std::sort(stable.begin(),stable.end());
        return stable;
      };
      for(const auto& face:old_only)
        refused.retriangulation_old_boundary_only.push_back(stable_face(face));
      for(const auto& face:new_only)
        refused.retriangulation_new_boundary_only.push_back(stable_face(face));
      if(invalid_boundary.second!=0U) {
        refused.retriangulation_nonmanifold_face=
            stable_face(invalid_boundary.first);
        refused.retriangulation_nonmanifold_face_uses=invalid_boundary.second;
      }
      continue;
    }
    long double new_volume{};
    for(const auto& cell:new_cavity)
      new_volume+=std::abs(orient(trial_points[cell[0]],trial_points[cell[1]],
                                 trial_points[cell[2]],trial_points[cell[3]]));
    if(std::abs(old_volume-new_volume)>std::max(1.0L,old_volume)*1e-13L) {
      refused.last_retriangulation_rejection=
          CanonicalBoundaryRetriangulationRejection::cavity_volume_mismatch;
      continue;
    }
    const std::set<std::size_t> removed(
        incident_global.begin(),incident_global.end());
    std::vector<Tet> candidate_mesh;
    for(std::size_t i=0U;i<working_mesh.size();++i)
      if(!removed.contains(i))candidate_mesh.push_back(working_mesh[i]);
    candidate_mesh.insert(candidate_mesh.end(),new_cavity.begin(),new_cavity.end());
    augmented.vertices.erase(augmented.vertices.begin()+steiner);
    for(auto& cell:candidate_mesh)for(auto& vertex:cell) {
      if(vertex==steiner){valid=false;break;}
      if(vertex>steiner)--vertex;
    }
    if(!valid||!has_only_nondegenerate_constraint_facets(augmented)) {
      refused.last_retriangulation_rejection=
          CanonicalBoundaryRetriangulationRejection::invalid_constraint_state;
      continue;
    }
    if(std::any_of(retained_origin.begin(),retained_origin.end(),
                   [](const bool retained){return retained;})) {
      // The recursive source path can deliberately create zero-volume bridge
      // tetrahedra for a region retained at the old edge point.  They exist
      // only until removePnt processes the newly interior relocation points.
      // Validate the resulting mesh, not this transient representation.
      std::vector<std::size_t> disposition_order(component_count);
      std::iota(disposition_order.begin(),disposition_order.end(),0U);
      std::stable_sort(disposition_order.begin(),disposition_order.end(),
          [&](const auto a,const auto b) {
            return retained_origin[a]&&!retained_origin[b];
          });
      for(const auto region:disposition_order) {
        const auto id=relocated_ids[region];
        auto removed_point=remove_canonical_interior_steiner_point(
            augmented,candidate_mesh,id,10U,false);
        augmented=std::move(removed_point.constraints);
        candidate_mesh=std::move(removed_point.tetrahedra);
        if(removed_point.removed())continue;
        auto smoothed=smooth_canonical_interior_steiner_volume(
            augmented,candidate_mesh,id);
        augmented=std::move(smoothed.constraints);
        candidate_mesh=std::move(smoothed.tetrahedra);
      }
      std::vector<Point> completed_points;
      for(const auto& vertex:augmented.vertices) {
        const auto p=vertex.position;
        completed_points.push_back({p.x,p.y,p.z});
      }
      if(std::any_of(candidate_mesh.begin(),candidate_mesh.end(),
                     [&](const auto& cell) {
                       return std::any_of(cell.begin(),cell.end(),[&](const auto corner) {
                                return corner>=completed_points.size();
                              })||robust_orient(completed_points,cell)==0;
                     })) {
        refused.last_retriangulation_rejection=
            CanonicalBoundaryRetriangulationRejection::retained_origin_degenerate;
        continue;
      }
    }
    // Pinned removeEdgStiner re-adds every immediate parent SurTri after
    // removePnt/smoothing and invokes recoverFace(..., 0) when that triangle
    // is not yet a mesh face. This must happen after the boundary point has
    // been cleared or split into interior representatives.
    for(const auto& parent:entry.replaced_facets) {
      const auto recovered=try_recover_wang_facet_by_local_flips(
          augmented,parent.vertices,candidate_mesh);
      if(recovered.edge_removals>0U)
        candidate_mesh=recovered.tetrahedra;
    }
    // recoverFace may still return a warning here. The pinned routine marks
    // the parent SurTri recovered and commits removal anyway; an older
    // boundary split can subsequently replace this intermediate triangle.
    // The final unsplit PLC audit remains authoritative after reverse order
    // has completed.
    CanonicalBoundaryRestorationResult restored;
    restored.failure=CanonicalBoundaryRestorationFailure::none;
    restored.constraints=std::move(augmented);
    restored.tetrahedra=std::move(candidate_mesh);
    restored.restored_points=1U;
    restored.interior_points_inserted=component_count;
    restored.relocation_regions=component_count;
    restored.bridge_tetrahedra=bridge_tetrahedra;
    restored.relocated_positions=std::move(relocated_positions);
    return restored;
  }
  refused.failure=CanonicalBoundaryRestorationFailure::invalid_retriangulation;
  return refused;
}

CanonicalInteriorPointRemovalResult remove_canonical_interior_steiner_point(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<Tet>& tetrahedra,std::uint64_t vertex_id,
    std::size_t maximum_edge_attempts,bool validate_entire_mesh) {
  CanonicalInteriorPointRemovalResult result;
  result.constraints=constraints;
  result.tetrahedra=tetrahedra;
  result.vertex_id=vertex_id;
  const auto registered=std::find_if(
      constraints.interior_steiner_vertices.begin(),
      constraints.interior_steiner_vertices.end(),
      [&](const auto& candidate){return candidate.id==vertex_id;});
  if(registered==constraints.interior_steiner_vertices.end())return result;
  const auto found=std::find_if(
      constraints.vertices.begin(),constraints.vertices.end(),
      [&](const auto& vertex){return vertex.id==vertex_id;});
  if(found==constraints.vertices.end()) {
    result.failure=CanonicalInteriorPointRemovalFailure::missing_or_empty_star;
    return result;
  }
  const auto vertex=static_cast<std::uint32_t>(
      found-constraints.vertices.begin());
  if(std::any_of(constraints.facets.begin(),constraints.facets.end(),
       [&](const auto& facet) {
         return std::find(facet.vertices.begin(),facet.vertices.end(),vertex_id)!=
             facet.vertices.end();
       })) {
    result.failure=CanonicalInteriorPointRemovalFailure::boundary_vertex;
    return result;
  }

  const auto try_four_to_one=[&](const std::vector<Tet>& mesh)
      ->std::optional<CanonicalInteriorPointRemovalResult> {
    std::vector<std::size_t> incident;
    std::set<std::uint32_t> neighbours;
    for(std::size_t i=0U;i<mesh.size();++i) {
      if(std::find(mesh[i].begin(),mesh[i].end(),vertex)==mesh[i].end())continue;
      incident.push_back(i);
      for(const auto corner:mesh[i])if(corner!=vertex)neighbours.insert(corner);
    }
    if(incident.size()!=4U||neighbours.size()!=4U)return std::nullopt;
    Tet replacement{};
    std::copy(neighbours.begin(),neighbours.end(),replacement.begin());
    std::vector<Point> points;
    for(const auto& item:constraints.vertices) {
      const auto p=item.position;points.push_back({p.x,p.y,p.z});
    }
    const auto orientation=robust_orient(points,replacement);
    if(orientation==0)return std::nullopt;
    if(orientation<0)std::swap(replacement[0],replacement[1]);
    const auto boundary_of=[](const auto& cells) {
      std::map<Face,unsigned> uses;
      for(const auto& cell:cells)for(unsigned omitted=0U;omitted<4U;++omitted) {
        Face face{};unsigned cursor{};
        for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];
        ++uses[face_key(face)];
      }
      std::set<Face> boundary;
      for(const auto& [face,count]:uses) {
        if(count==1U)boundary.insert(face);
        else if(count!=2U)return std::set<Face>{};
      }
      return boundary;
    };
    std::vector<Tet> old_star;
    for(const auto i:incident)old_star.push_back(mesh[i]);
    if(boundary_of(old_star)!=boundary_of(std::vector<Tet>{replacement}))
      return std::nullopt;
    long double old_volume{};
    for(const auto& cell:old_star)
      old_volume+=std::abs(orient(points[cell[0]],points[cell[1]],
                                 points[cell[2]],points[cell[3]]));
    const auto new_volume=std::abs(orient(
        points[replacement[0]],points[replacement[1]],
        points[replacement[2]],points[replacement[3]]));
    if(std::abs(old_volume-new_volume)>std::max(1.0L,old_volume)*1e-14L)
      return std::nullopt;
    const std::set<std::size_t> removed(incident.begin(),incident.end());
    std::vector<Tet> candidate;
    for(std::size_t i=0U;i<mesh.size();++i)
      if(!removed.contains(i))candidate.push_back(mesh[i]);
    candidate.push_back(replacement);
    if(validate_entire_mesh&&
       !constraint_mesh_mutation_is_valid(constraints,mesh,candidate))
      return std::nullopt;
    auto updated=constraints;
    updated.vertices.erase(updated.vertices.begin()+vertex);
    updated.interior_steiner_vertices.erase(
        std::remove_if(updated.interior_steiner_vertices.begin(),
                       updated.interior_steiner_vertices.end(),
                       [&](const auto& item){return item.id==vertex_id;}),
        updated.interior_steiner_vertices.end());
    for(auto& cell:candidate)for(auto& corner:cell) {
      if(corner==vertex)return std::nullopt;
      if(corner>vertex)--corner;
    }
    if(!constraint_mesh_is_valid(updated,candidate))return std::nullopt;
    CanonicalInteriorPointRemovalResult removed_result;
    removed_result.failure=CanonicalInteriorPointRemovalFailure::none;
    removed_result.constraints=std::move(updated);
    removed_result.tetrahedra=std::move(candidate);
    removed_result.vertex_id=vertex_id;
    removed_result.retained_vertex_id=0U;
    removed_result.edge_attempts=result.edge_attempts;
    removed_result.edge_removals=result.edge_removals;
    removed_result.used_four_to_one=true;
    return removed_result;
  };

  const auto try_directional_collapse=[&](std::uint32_t neighbour)
      ->std::optional<CanonicalInteriorPointRemovalResult> {
    std::set<std::size_t> shell;
    std::set<std::size_t> remaining_star;
    for(std::size_t i=0U;i<tetrahedra.size();++i) {
      const auto has_vertex=std::find(
          tetrahedra[i].begin(),tetrahedra[i].end(),vertex)!=tetrahedra[i].end();
      if(!has_vertex)continue;
      if(std::find(tetrahedra[i].begin(),tetrahedra[i].end(),neighbour)!=
         tetrahedra[i].end())shell.insert(i);
      else remaining_star.insert(i);
    }
    if(shell.empty())return std::nullopt;

    std::vector<Tet> candidate;
    candidate.reserve(tetrahedra.size()-shell.size());
    std::vector<Point> points;
    for(const auto& item:constraints.vertices) {
      const auto p=item.position;points.push_back({p.x,p.y,p.z});
    }
    for(std::size_t i=0U;i<tetrahedra.size();++i) {
      if(shell.contains(i))continue;
      auto cell=tetrahedra[i];
      if(remaining_star.contains(i)) {
        const auto original_orientation=robust_orient(points,cell);
        *std::find(cell.begin(),cell.end(),vertex)=neighbour;
        const auto orientation=robust_orient(points,cell);
        if(orientation==0||
           (original_orientation!=0&&orientation!=original_orientation))
          return std::nullopt;
        if(original_orientation==0&&orientation<0)
          std::swap(cell[0],cell[1]);
      }
      candidate.push_back(cell);
    }
    if(validate_entire_mesh&&
       !constraint_mesh_mutation_is_valid(constraints,tetrahedra,candidate))
      return std::nullopt;

    auto updated=constraints;
    updated.vertices.erase(updated.vertices.begin()+vertex);
    updated.interior_steiner_vertices.erase(
        std::remove_if(updated.interior_steiner_vertices.begin(),
                       updated.interior_steiner_vertices.end(),
                       [&](const auto& item){return item.id==vertex_id;}),
        updated.interior_steiner_vertices.end());
    for(auto& cell:candidate)for(auto& corner:cell) {
      if(corner==vertex)return std::nullopt;
      if(corner>vertex)--corner;
    }
    if(validate_entire_mesh&&!constraint_mesh_is_valid(updated,candidate))
      return std::nullopt;
    CanonicalInteriorPointRemovalResult removed_result;
    removed_result.failure=CanonicalInteriorPointRemovalFailure::none;
    removed_result.constraints=std::move(updated);
    removed_result.tetrahedra=std::move(candidate);
    removed_result.vertex_id=vertex_id;
    removed_result.retained_vertex_id=constraints.vertices[neighbour].id;
    removed_result.edge_attempts=result.edge_attempts;
    removed_result.edge_removals=1U;
    return removed_result;
  };

  std::map<long double,std::uint32_t> candidates;
  const auto p=constraints.vertices[vertex].position;
  for(const auto& cell:tetrahedra) {
    if(std::find(cell.begin(),cell.end(),vertex)==cell.end())continue;
    for(const auto neighbour:cell) {
      if(neighbour==vertex)continue;
      const auto q=constraints.vertices[neighbour].position;
      const auto dx=static_cast<long double>(q.x)-p.x;
      const auto dy=static_cast<long double>(q.y)-p.y;
      const auto dz=static_cast<long double>(q.z)-p.z;
      candidates.emplace(dx*dx+dy*dy+dz*dz,neighbour);
    }
  }
  if(candidates.empty()) {
    result.failure=CanonicalInteriorPointRemovalFailure::missing_or_empty_star;
    return result;
  }
  for(const auto& [length,neighbour]:candidates) {
    (void)length;
    if(result.edge_attempts++>=maximum_edge_attempts)break;
    if(const auto collapsed=try_directional_collapse(neighbour))return *collapsed;
  }
  if(const auto removed=try_four_to_one(tetrahedra))return *removed;
  result.failure=CanonicalInteriorPointRemovalFailure::not_removable;
  return result;
}

CanonicalInteriorPointSmoothingResult smooth_canonical_interior_steiner_volume(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<Tet>& tetrahedra,std::uint64_t vertex_id) {
  CanonicalInteriorPointSmoothingResult result;
  result.constraints=constraints;
  result.tetrahedra=tetrahedra;
  result.vertex_id=vertex_id;
  const auto registered=std::find_if(
      constraints.interior_steiner_vertices.begin(),
      constraints.interior_steiner_vertices.end(),
      [&](const auto& candidate){return candidate.id==vertex_id;});
  const auto found=std::find_if(
      constraints.vertices.begin(),constraints.vertices.end(),
      [&](const auto& vertex){return vertex.id==vertex_id;});
  if(registered==constraints.interior_steiner_vertices.end()||
     found==constraints.vertices.end())return result;
  if(std::any_of(constraints.facets.begin(),constraints.facets.end(),
       [&](const auto& facet) {
         return std::find(facet.vertices.begin(),facet.vertices.end(),vertex_id)!=
             facet.vertices.end();
       }))return result;
  const auto vertex=static_cast<std::uint32_t>(found-constraints.vertices.begin());
  std::vector<std::size_t> incident;
  std::map<Face,unsigned> global_faces;
  for(std::size_t i=0U;i<tetrahedra.size();++i) {
    if(std::find(tetrahedra[i].begin(),tetrahedra[i].end(),vertex)!=
       tetrahedra[i].end())incident.push_back(i);
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned j=0U;j<4U;++j)if(j!=omitted)face[cursor++]=tetrahedra[i][j];
      ++global_faces[face_key(face)];
    }
  }
  if(incident.empty())return result;
  // The reference refuses a sphere containing a ghost tetrahedron.  Under
  // the prototype's finite-hull adaptation, a boundary face incident to the
  // point is the corresponding observable condition.
  for(const auto cell:incident)for(unsigned omitted=0U;omitted<4U;++omitted) {
    Face face{};unsigned cursor{};
    for(unsigned j=0U;j<4U;++j)if(j!=omitted)face[cursor++]=tetrahedra[cell][j];
    if(std::find(face.begin(),face.end(),vertex)!=face.end()&&
       global_faces[face_key(face)]==1U)return result;
  }
  result.attempted=true;
  result.original_position=found->position;
  result.final_position=found->position;

  std::vector<Point> points;
  for(const auto& item:constraints.vertices) {
    const auto p=item.position;points.push_back({p.x,p.y,p.z});
  }
  auto current=points[vertex];
  const auto solve=[](const std::array<long double,9>& a,Point rhs)
      ->std::optional<Point> {
    const auto det=
        a[0]*(a[4]*a[8]-a[5]*a[7])-
        a[1]*(a[3]*a[8]-a[5]*a[6])+
        a[2]*(a[3]*a[7]-a[4]*a[6]);
    long double scale=1.0L;
    for(const auto value:a)scale=std::max(scale,std::abs(value));
    if(std::abs(det)<=1024.0L*LDBL_EPSILON*scale*scale*scale)
      return std::nullopt;
    const auto determinant=[](std::array<long double,9> m) {
      return m[0]*(m[4]*m[8]-m[5]*m[7])-
             m[1]*(m[3]*m[8]-m[5]*m[6])+
             m[2]*(m[3]*m[7]-m[4]*m[6]);
    };
    auto x=a,y=a,z=a;
    x[0]=rhs.x;x[3]=rhs.y;x[6]=rhs.z;
    y[1]=rhs.x;y[4]=rhs.y;y[7]=rhs.z;
    z[2]=rhs.x;z[5]=rhs.y;z[8]=rhs.z;
    return Point{determinant(x)/det,determinant(y)/det,determinant(z)/det};
  };

  constexpr std::size_t maximum_descents=100U;
  for(std::size_t descent=0U;descent<maximum_descents;++descent) {
    std::array<long double,9> hessian{};
    Point gradient{};
    long double old_energy{};
    struct Term { long double volume{},weight{};Point derivative{};int sign{};Tet cell{}; };
    std::vector<Term> terms;
    for(const auto cell_index:incident) {
      const auto cell=tetrahedra[cell_index];
      const auto sign_now=robust_orient(points,cell);
      if(sign_now==0)return result;
      Face opposite{};unsigned cursor{};
      for(const auto corner:cell)if(corner!=vertex)opposite[cursor++]=corner;
      if(cursor!=3U)return result;
      const auto normal=cross(points[opposite[1]]-points[opposite[0]],
                              points[opposite[2]]-points[opposite[0]]);
      const auto area=std::sqrt(dot(normal,normal))*0.5L;
      if(!(area>0.0L))return result;
      const auto raw=orient(points[cell[0]],points[cell[1]],
                            points[cell[2]],points[cell[3]]);
      const auto volume=std::abs(raw)/6.0L;
      Point derivative{};
      for(unsigned axis=0U;axis<3U;++axis) {
        auto shifted=current;
        if(axis==0U)shifted.x+=1.0L;
        else if(axis==1U)shifted.y+=1.0L;
        else shifted.z+=1.0L;
        auto shifted_points=points;shifted_points[vertex]=shifted;
        const auto shifted_raw=orient(
            shifted_points[cell[0]],shifted_points[cell[1]],
            shifted_points[cell[2]],shifted_points[cell[3]]);
        const auto value=(shifted_raw-raw)*(sign_now>0?1.0L:-1.0L)/6.0L;
        if(axis==0U)derivative.x=value;
        else if(axis==1U)derivative.y=value;
        else derivative.z=value;
      }
      old_energy+=volume*volume/area;
      gradient=gradient+derivative*(2.0L*volume/area);
      const std::array<long double,3> g{{derivative.x,derivative.y,derivative.z}};
      for(unsigned row=0U;row<3U;++row)for(unsigned column=0U;column<3U;++column)
        hessian[row*3U+column]+=2.0L*g[row]*g[column]/area;
      terms.push_back({volume,area,derivative,sign_now,cell});
    }
    const auto newton=solve(hessian,gradient*-1.0L);
    if(!newton) {
      result.converged=result.moved;
      break;
    }
    bool accepted=false;
    Point candidate{};
    long double new_energy{};
    for(long double alpha=1.0L;alpha>=1e-10L;alpha*=0.8L) {
      candidate=current+*newton*alpha;
      auto candidate_points=points;candidate_points[vertex]=candidate;
      new_energy=0.0L;
      bool positive=true;
      for(const auto& term:terms) {
        const auto candidate_sign=robust_orient(candidate_points,term.cell);
        if(candidate_sign!=term.sign){positive=false;break;}
        const auto volume=std::abs(orient(
            candidate_points[term.cell[0]],candidate_points[term.cell[1]],
            candidate_points[term.cell[2]],candidate_points[term.cell[3]]))/6.0L;
        new_energy+=volume*volume/term.weight;
      }
      if(positive&&new_energy<old_energy){accepted=true;break;}
    }
    if(!accepted) {
      result.converged=result.moved;
      break;
    }
    current=candidate;points[vertex]=candidate;
    result.moved=true;
    result.descent_steps=descent+1U;
    result.final_position=as_vec3(current);
    if(std::abs((new_energy-old_energy)/old_energy)<1e-5L) {
      result.converged=true;
      break;
    }
  }
  if(result.moved) {
    result.constraints.vertices[vertex].position=result.final_position;
    if(!constraint_mesh_is_valid(result.constraints,result.tetrahedra)) {
      result.constraints=constraints;
      result.moved=false;
      result.converged=false;
      result.descent_steps=0U;
      result.final_position=result.original_position;
    }
  }
  return result;
}

namespace {
using SusMatrix=std::array<long double,9>;
struct SusTet {
  std::array<Point,4> points;
  unsigned free_index{};
  bool positive{};
};
long double sus_norm(Point value) { return std::sqrt(dot(value,value)); }
Point sus_normalized(Point value) {
  const auto length=sus_norm(value);
  return length>0.0L?value*(1.0L/length):Point{};
}
long double sus_sigma(const std::array<Point,4>& points) {
  return std::sqrt(2.0L)*orient(points[0],points[1],points[2],points[3]);
}
long double sus_norm_squared(const std::array<Point,4>& points) {
  long double result{};
  for(unsigned a=0U;a<3U;++a)for(unsigned b=a+1U;b<4U;++b)
    result+=0.5L*dot(points[a]-points[b],points[a]-points[b]);
  return result;
}
long double sus_signed_quality(const std::array<Point,4>& points) {
  const auto norm=sus_norm_squared(points),det=sus_sigma(points);
  if(!(norm>0.0L)||!std::isfinite(det))return 0.0L;
  return std::copysign(
      std::min(1.0L,3.0L*std::pow(std::abs(det),2.0L/3.0L)/norm),det);
}
long double sus_physical_quality(std::array<Point,4> points) {
  const auto origin=points[0];long double scale{};
  for(auto& point:points) {
    point=point-origin;
    scale=std::max({scale,std::abs(point.x),std::abs(point.y),std::abs(point.z)});
  }
  if(!(scale>0.0L)||!std::isfinite(scale))return 0.0L;
  for(auto& point:points)point=point*(1.0L/scale);
  return sus_signed_quality(points);
}
Point sus_quality_gradient(const SusTet& tet,Point position) {
  auto points=tet.points;points[tet.free_index]=position;
  const auto det=sus_sigma(points),norm=sus_norm_squared(points);
  if(!(norm>0.0L))return {};
  if(std::abs(det)<1e-12L) {
    Point gradient{};
    for(unsigned axis=0U;axis<3U;++axis) {
      auto plus=points,minus=points;
      if(axis==0U){plus[tet.free_index].x+=1e-6L;minus[tet.free_index].x-=1e-6L;}
      else if(axis==1U){plus[tet.free_index].y+=1e-6L;minus[tet.free_index].y-=1e-6L;}
      else {plus[tet.free_index].z+=1e-6L;minus[tet.free_index].z-=1e-6L;}
      const auto value=(sus_signed_quality(plus)-sus_signed_quality(minus))/(2e-6L);
      if(axis==0U)gradient.x=value;else if(axis==1U)gradient.y=value;else gradient.z=value;
    }
    return gradient;
  }
  const auto a=points[1]-points[0],b=points[2]-points[0],c=points[3]-points[0];
  const std::array<Point,4> determinant_gradient{{
      (cross(b,c)+cross(c,a)+cross(a,b))*-1.0L,cross(b,c),cross(c,a),cross(a,b)}};
  Point norm_gradient{};
  for(unsigned i=0U;i<4U;++i)if(i!=tet.free_index)
    norm_gradient=norm_gradient+position-points[i];
  return determinant_gradient[tet.free_index]*
             (2.0L*std::sqrt(2.0L)/(std::cbrt(std::abs(det))*norm))-
         norm_gradient*(sus_signed_quality(points)/norm);
}
Point sus_small_hull_nearest(const std::vector<Point>& gradients) {
  auto best=gradients.front();
  const auto consider=[&](Point candidate) {
    if(dot(candidate,candidate)<dot(best,best))best=candidate;
  };
  for(std::size_t i=0U;i<gradients.size();++i) {
    consider(gradients[i]);
    for(std::size_t j=i+1U;j<gradients.size();++j) {
      const auto u=gradients[j]-gradients[i];
      const auto uu=dot(u,u);
      if(uu>0.0L)consider(gradients[i]+u*std::clamp(-dot(gradients[i],u)/uu,0.0L,1.0L));
      for(std::size_t k=j+1U;k<gradients.size();++k) {
        const auto v=gradients[k]-gradients[i];
        const auto uv=dot(u,v),vv=dot(v,v),det=dot(cross(u,v),cross(u,v));
        if(det<=1e-14L*uu*vv)continue;
        const auto rhs_u=-dot(gradients[i],u),rhs_v=-dot(gradients[i],v);
        const auto beta=(rhs_u*vv-rhs_v*uv)/det;
        const auto gamma=(rhs_v*uu-rhs_u*uv)/det;
        if(beta>=0.0L&&gamma>=0.0L&&beta+gamma<=1.0L)
          consider(gradients[i]+u*beta+v*gamma);
      }
    }
  }
  if(gradients.size()==4U) {
    const auto u=gradients[1]-gradients[0],v=gradients[2]-gradients[0];
    const auto w=gradients[3]-gradients[0],rhs=gradients[0]*-1.0L;
    const auto determinant=dot(u,cross(v,w));
    if(std::abs(determinant)>1e-12L*sus_norm(u)*sus_norm(v)*sus_norm(w)) {
      const auto beta=dot(rhs,cross(v,w))/determinant;
      const auto gamma=dot(u,cross(rhs,w))/determinant;
      const auto delta=dot(u,cross(v,rhs))/determinant;
      if(beta>=0.0L&&gamma>=0.0L&&delta>=0.0L&&beta+gamma+delta<=1.0L)return {};
    }
  }
  return best;
}
Point sus_active_set_direction(const std::vector<SusTet>& tets,Point position) {
  std::vector<long double> qualities;long double worst=LDBL_MAX;
  for(const auto& tet:tets) {
    auto points=tet.points;points[tet.free_index]=position;
    qualities.push_back(sus_signed_quality(points));
    worst=std::min(worst,qualities.back());
  }
  for(const auto relative_band:{1e-3L,1e-5L,0.0L}) {
    std::vector<Point> gradients;long double scale{};
    const auto band=std::max(1e-12L,relative_band*std::max(1e-3L,std::abs(worst)));
    for(std::size_t i=0U;i<tets.size();++i) {
      if(qualities[i]>worst+band)continue;
      const auto gradient=sus_quality_gradient(tets[i],position);
      if(!std::isfinite(gradient.x)||!std::isfinite(gradient.y)||!std::isfinite(gradient.z))return {};
      gradients.push_back(gradient);scale=std::max(scale,sus_norm(gradient));
    }
    if(!(scale>0.0L)||!std::isfinite(scale))return {};
    for(auto& gradient:gradients)gradient=gradient*(1.0L/scale);
    auto nearest=gradients.size()<=4U?sus_small_hull_nearest(gradients):gradients.front();
    for(unsigned iteration=0U;gradients.size()>4U&&iteration<256U;++iteration) {
      std::size_t selected{};
      for(std::size_t i=1U;i<gradients.size();++i)
        if(dot(nearest,gradients[i])<dot(nearest,gradients[selected]))selected=i;
      const auto toward=gradients[selected]-nearest;
      const auto gap=-dot(nearest,toward),distance=dot(toward,toward);
      if(gap<=1e-6L*std::max(1e-12L,dot(nearest,nearest))||distance<=1e-30L)break;
      const auto step=std::clamp(-dot(nearest,toward)/distance,0.0L,1.0L);
      if(step<1e-14L)break;
      nearest=nearest+toward*step;
    }
    if(sus_norm(nearest)<=1e-12L)continue;
    const auto direction=sus_normalized(nearest)*0.5L;
    if(std::all_of(gradients.begin(),gradients.end(),[&](Point gradient) {
         return dot(gradient,direction)>1e-12L;
       }))return direction;
  }
  return {};
}
long double sus_energy(const std::vector<SusTet>& tets,Point position,long double delta,
                       Point* gradient,bool* preserves_validity) {
  constexpr long double epsilon=1e-10L;
  if(gradient)*gradient={};
  if(preserves_validity)*preserves_validity=true;
  long double sum{};
  for(const auto& tet:tets) {
    auto points=tet.points;points[tet.free_index]=position;
    const auto det=sus_sigma(points),norm=sus_norm_squared(points);
    if(preserves_validity&&tet.positive&&det<=0.0L)*preserves_validity=false;
    const auto shifted=det-2.0L*epsilon,root=std::hypot(shifted,2.0L*delta);
    const auto h=det>=0.0L?0.5L*(det+root):
        2.0L*(delta*delta+epsilon*epsilon-epsilon*det)/(root-det);
    if(!(h>0.0L)||!(norm>0.0L)||!std::isfinite(h))
      return std::numeric_limits<long double>::infinity();
    const auto energy=norm/(3.0L*std::pow(h,2.0L/3.0L));sum+=energy;
    if(!gradient)continue;
    const auto a=points[1]-points[0],b=points[2]-points[0],c=points[3]-points[0];
    const std::array<Point,4> determinant_gradient{{
        (cross(b,c)+cross(c,a)+cross(a,b))*-1.0L,cross(b,c),cross(c,a),cross(a,b)}};
    Point norm_gradient{};
    for(unsigned i=0U;i<4U;++i)if(i!=tet.free_index)
      norm_gradient=norm_gradient+position-points[i];
    *gradient=*gradient+norm_gradient*(energy/norm)-
        determinant_gradient[tet.free_index]*
            (energy*(2.0L/3.0L)*((1.0L-epsilon/h)/root)*std::sqrt(2.0L));
  }
  if(gradient)*gradient=*gradient*(1.0L/static_cast<long double>(tets.size()));
  return sum/static_cast<long double>(tets.size());
}
SusMatrix sus_identity() { return {{1,0,0,0,1,0,0,0,1}}; }
Point sus_mat_vec(const SusMatrix& matrix,Point vector) {
  return {matrix[0]*vector.x+matrix[1]*vector.y+matrix[2]*vector.z,
          matrix[3]*vector.x+matrix[4]*vector.y+matrix[5]*vector.z,
          matrix[6]*vector.x+matrix[7]*vector.y+matrix[8]*vector.z};
}
SusMatrix sus_multiply(const SusMatrix& left,const SusMatrix& right) {
  SusMatrix result{};
  for(unsigned row=0U;row<3U;++row)for(unsigned column=0U;column<3U;++column)
    for(unsigned k=0U;k<3U;++k)
      result[row*3U+column]+=left[row*3U+k]*right[k*3U+column];
  return result;
}
SusMatrix sus_transpose(const SusMatrix& matrix) {
  SusMatrix result{};
  for(unsigned row=0U;row<3U;++row)for(unsigned column=0U;column<3U;++column)
    result[row*3U+column]=matrix[column*3U+row];
  return result;
}
SusMatrix sus_bfgs_inverse(const SusMatrix& inverse,Point step,Point gradient_delta) {
  const auto curvature=dot(step,gradient_delta);
  auto transform=sus_identity();
  const std::array<long double,3> s{{step.x,step.y,step.z}};
  const std::array<long double,3> y{{gradient_delta.x,gradient_delta.y,gradient_delta.z}};
  for(unsigned row=0U;row<3U;++row)for(unsigned column=0U;column<3U;++column)
    transform[row*3U+column]-=s[row]*y[column]/curvature;
  auto result=sus_multiply(sus_multiply(transform,inverse),sus_transpose(transform));
  for(unsigned row=0U;row<3U;++row)for(unsigned column=0U;column<3U;++column)
    result[row*3U+column]+=s[row]*s[column]/curvature;
  return result;
}
} // namespace

CanonicalInteriorPointSmoothingResult smooth_canonical_interior_steiner_sus(
    const CanonicalPlcConstraintSet& constraints,const std::vector<Tet>& tetrahedra,
    std::uint64_t vertex_id) {
  CanonicalInteriorPointSmoothingResult result;
  result.constraints=constraints;result.tetrahedra=tetrahedra;result.vertex_id=vertex_id;
  const auto registered=std::find_if(
      constraints.interior_steiner_vertices.begin(),constraints.interior_steiner_vertices.end(),
      [&](const auto& candidate){return candidate.id==vertex_id;});
  const auto found=std::find_if(constraints.vertices.begin(),constraints.vertices.end(),
      [&](const auto& vertex){return vertex.id==vertex_id;});
  if(registered==constraints.interior_steiner_vertices.end()||found==constraints.vertices.end())return result;
  if(std::any_of(constraints.facets.begin(),constraints.facets.end(),[&](const auto& facet) {
       return std::find(facet.vertices.begin(),facet.vertices.end(),vertex_id)!=facet.vertices.end();
     }))return result;
  const auto vertex=static_cast<std::uint32_t>(found-constraints.vertices.begin());
  std::vector<std::size_t> incident;std::set<std::uint32_t> neighbours;
  std::map<Face,unsigned> global_faces;
  for(std::size_t i=0U;i<tetrahedra.size();++i) {
    const auto& cell=tetrahedra[i];
    if(std::find(cell.begin(),cell.end(),vertex)!=cell.end()) {
      incident.push_back(i);for(const auto corner:cell)if(corner!=vertex)neighbours.insert(corner);
    }
    for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned j=0U;j<4U;++j)if(j!=omitted)face[cursor++]=cell[j];
      ++global_faces[face_key(face)];
    }
  }
  if(incident.empty())return result;
  for(const auto cell_index:incident)for(unsigned omitted=0U;omitted<4U;++omitted) {
    Face face{};unsigned cursor{};
    for(unsigned j=0U;j<4U;++j)if(j!=omitted)face[cursor++]=tetrahedra[cell_index][j];
    if(std::find(face.begin(),face.end(),vertex)!=face.end()&&global_faces[face_key(face)]==1U)
      return result;
  }
  result.attempted=true;result.original_position=found->position;result.final_position=found->position;
  std::vector<Point> physical_points;physical_points.reserve(constraints.vertices.size());
  for(const auto& item:constraints.vertices)
    physical_points.push_back({item.position.x,item.position.y,item.position.z});
  const auto origin=physical_points[vertex];long double scale{};
  for(const auto neighbour:neighbours)
    scale=std::max(scale,sus_norm(physical_points[neighbour]-origin));
  if(!(scale>0.0L)||!std::isfinite(scale))return result;

  std::size_t first_quality_check{};
  auto minimum_quality=[&](Point position,long double cutoff) {
    long double minimum=LDBL_MAX;std::size_t worst=first_quality_check;
    const auto first=first_quality_check;
    for(std::size_t offset=0U;offset<incident.size();++offset) {
      const auto k=(first+offset)%incident.size();auto points=std::array<Point,4>{};
      for(unsigned j=0U;j<4U;++j) {
        const auto corner=tetrahedra[incident[k]][j];
        points[j]=corner==vertex?position:physical_points[corner];
      }
      const auto quality=sus_physical_quality(points);
      if(!std::isfinite(quality))return -std::numeric_limits<long double>::infinity();
      if(quality<minimum){minimum=quality;worst=k;}
      if(quality<cutoff){first_quality_check=k;return quality;}
    }
    first_quality_check=worst;return minimum;
  };
  const auto initial_minimum=minimum_quality(origin,-std::numeric_limits<long double>::infinity());
  if(!std::isfinite(initial_minimum))return result;
  auto current_minimum=initial_minimum;
  std::vector<SusTet> tets;long double minimum_det=LDBL_MAX,mean_det{};
  for(const auto cell_index:incident) {
    SusTet tet;bool found_free=false;
    for(unsigned j=0U;j<4U;++j) {
      const auto corner=tetrahedra[cell_index][j];
      tet.points[j]=(physical_points[corner]-origin)*(1.0L/scale);
      if(corner==vertex){tet.free_index=j;found_free=true;}
    }
    if(!found_free)return result;
    const auto determinant=sus_sigma(tet.points);
    if(!std::isfinite(determinant))return result;
    tet.positive=determinant>0.0L;minimum_det=std::min(minimum_det,determinant);
    mean_det+=std::abs(determinant);tets.push_back(tet);
  }
  mean_det/=static_cast<long double>(tets.size());
  const auto effective=std::numeric_limits<double>::epsilon()*1e6L;
  const auto delta=std::max(std::sqrt(std::max(0.0L,effective*(effective-minimum_det))),1e-4L*mean_det);
  Point position{},gradient{};
  auto energy=sus_energy(tets,position,delta,&gradient,nullptr),initial_energy=energy;
  if(!std::isfinite(energy)||!std::isfinite(gradient.x)||!std::isfinite(gradient.y)||!std::isfinite(gradient.z))return result;
  auto inverse=sus_identity();
  constexpr long double quality_tolerance=1e-12L;
  const auto maximum_iterations=initial_minimum>0.0L?12U:60U;
  const auto small_progress_tolerance=initial_minimum>0.0L?1e-4L:1e-6L;
  const auto small_progress_limit=initial_minimum>0.0L?2U:3U;
  const auto progress_block=initial_minimum>0.0L?3U:6U;
  const auto block_quality_tolerance=initial_minimum>0.0L?3e-4L:1e-5L;
  bool active_mode=false;unsigned small_progress_steps{};long double active_step=1.0L;
  Point previous_active_direction{};auto stage_quality=initial_minimum;
  for(unsigned iteration=0U;iteration<maximum_iterations;++iteration) {
    auto direction=sus_mat_vec(inverse,gradient)*-1.0L;
    if(!std::isfinite(direction.x)||!std::isfinite(direction.y)||!std::isfinite(direction.z)||
       dot(direction,gradient)>=0.0L){inverse=sus_identity();direction=gradient*-1.0L;}
    if(sus_norm(direction)>0.5L)direction=direction*(0.5L/sus_norm(direction));
    const auto slope=dot(gradient,direction);
    long double step=1.0L,next_energy=energy,next_minimum=current_minimum;
    auto next_position=position;bool accepted=false,blocked_by_quality=false,used_active_set=false;
    const auto stationary=sus_norm(gradient)<=1e-8L*std::max(1.0L,std::abs(energy))||!(slope<0.0L);
    unsigned quality_rejections{};
    if(!active_mode&&!stationary) {
      for(unsigned search=0U;search<24U;++search,step*=0.5L) {
        next_position=position+direction*step;bool valid=false;
        next_energy=sus_energy(tets,next_position,delta,nullptr,&valid);
        if(valid&&std::isfinite(next_energy)&&next_energy<=energy+1e-4L*step*slope) {
          next_minimum=minimum_quality(origin+(next_position*scale),current_minimum);
          if(next_minimum>=current_minimum){accepted=true;break;}
          blocked_by_quality=true;if(++quality_rejections>=3U)break;
        }
      }
    }
    if(active_mode||stationary||
       (blocked_by_quality&&(!accepted||sus_norm(next_position-position)<=1e-10L))) {
      active_mode=true;direction=sus_active_set_direction(tets,position);accepted=false;
      if(dot(direction,direction)>0.0L) {
        const auto changed=dot(previous_active_direction,previous_active_direction)>0.0L&&
                           dot(direction,previous_active_direction)<0.0L;
        step=std::min(changed?0.125L:1.0L,(changed?8.0L:2.0L)*active_step);
        for(unsigned search=0U;search<24U;++search,step*=0.5L) {
          next_position=position+direction*step;
          next_minimum=minimum_quality(origin+next_position*scale,current_minimum+quality_tolerance);
          if(!(next_minimum>current_minimum+quality_tolerance))continue;
          bool valid=false;next_energy=sus_energy(tets,next_position,delta,nullptr,&valid);
          if(!valid||!std::isfinite(next_energy))continue;
          active_step=step;previous_active_direction=direction;accepted=true;used_active_set=true;break;
        }
      }
    }
    if(!accepted)break;
    auto next_gradient=gradient;
    if(!used_active_set) {
      sus_energy(tets,next_position,delta,&next_gradient,nullptr);
      if(!std::isfinite(next_gradient.x)||!std::isfinite(next_gradient.y)||!std::isfinite(next_gradient.z))break;
    }
    const auto displacement=next_position-position,gradient_delta=next_gradient-gradient;
    const auto curvature=dot(displacement,gradient_delta);
    const auto quality_gain=next_minimum-current_minimum;
    const auto relative_energy_change=std::abs(next_energy-energy)/std::max(1.0L,std::abs(energy));
    const auto small_progress=quality_gain<=small_progress_tolerance&&
                              (used_active_set||relative_energy_change<=small_progress_tolerance);
    small_progress_steps=small_progress?small_progress_steps+1U:0U;
    position=next_position;gradient=next_gradient;energy=next_energy;current_minimum=next_minimum;
    result.descent_steps=iteration+1U;
    if(sus_norm(displacement)<1e-10L||small_progress_steps>=small_progress_limit)break;
    if(active_mode&&(iteration+1U)%progress_block==0U) {
      if(current_minimum-stage_quality<=block_quality_tolerance)break;
      stage_quality=current_minimum;
    }
    if(!used_active_set&&curvature>1e-12L*sus_norm(displacement)*sus_norm(gradient_delta))
      inverse=sus_bfgs_inverse(inverse,displacement,gradient_delta);
    else inverse=sus_identity();
  }
  const auto final_position=origin+position*scale;
  bool preserves_validity=false;
  const auto verified=sus_energy(tets,position,delta,nullptr,&preserves_validity);
  if(!preserves_validity||!std::isfinite(verified))return result;
  const auto final_minimum=minimum_quality(final_position,-std::numeric_limits<long double>::infinity());
  if(final_minimum<initial_minimum)return result;
  const auto improved_quality=final_minimum>initial_minimum+quality_tolerance;
  const auto improved_energy=verified<initial_energy-1e-12L*std::max(1.0L,std::abs(initial_energy));
  if(!improved_quality&&!improved_energy)return result;
  result.final_position=as_vec3(final_position);result.moved=true;result.converged=true;
  result.constraints.vertices[vertex].position=result.final_position;
  // Pinned smooth_sus commits after validating the incident sphere's SUS
  // energy and physical minimum quality above.  It does not rescan unrelated
  // tetrahedra, because removebadtet_addPnt calls it during mesh repair.
  return result;
}

CanonicalBoundaryRestorationResult restore_last_canonical_facet_steiner_point(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra) {
  CanonicalBoundaryRestorationResult result;
  result.constraints=constraints;result.tetrahedra=tetrahedra;
  const auto entry=constraints.recovery_journal.back();
  const auto record=std::find_if(
      constraints.facet_split_vertices.begin(),constraints.facet_split_vertices.end(),
      [&](const auto& candidate){return candidate.id==entry.vertex_id;});
  if(record==constraints.facet_split_vertices.end()) {
    result.failure=CanonicalBoundaryRestorationFailure::missing_split_record;
    return result;
  }
  const auto relocate=[&]() {
    return relocate_last_boundary_steiner_point(constraints,tetrahedra,entry);
  };
  std::map<std::uint64_t,std::uint32_t> index;
  for(std::size_t i=0U;i<constraints.vertices.size();++i)
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
  if(!index.contains(entry.vertex_id)) {
    result.failure=CanonicalBoundaryRestorationFailure::missing_split_record;
    return result;
  }
  std::vector<std::size_t> children;
  for(std::size_t i=0U;i<constraints.facets.size();++i)
    if(constraints.facets[i].parent==record->parent&&
       std::find(constraints.facets[i].vertices.begin(),
                 constraints.facets[i].vertices.end(),entry.vertex_id)!=
           constraints.facets[i].vertices.end())children.push_back(i);
  if(children.size()!=3U) {
    result.failure=CanonicalBoundaryRestorationFailure::constraint_reconstruction_failed;
    return result;
  }
  const auto source=constraints.facets[children.front()].source_vertices;
  std::array<std::uint32_t,3> face{};
  for(unsigned i=0U;i<3U;++i) {
    if(!index.contains(source[i])) {
      result.failure=CanonicalBoundaryRestorationFailure::constraint_reconstruction_failed;
      return result;
    }
    face[i]=index.at(source[i]);
  }
  const auto steiner=index.at(entry.vertex_id);
  std::vector<Tet> incident;
  std::map<std::uint32_t,std::set<std::array<std::uint32_t,2>>> regions;
  for(const auto& cell:tetrahedra) {
    if(std::find(cell.begin(),cell.end(),steiner)==cell.end())continue;
    incident.push_back(cell);
    std::array<std::uint32_t,2> edge{};unsigned edge_count{};
    std::optional<std::uint32_t> opposite;
    for(const auto vertex:cell) {
      if(vertex==steiner)continue;
      if(std::find(face.begin(),face.end(),vertex)!=face.end())edge[edge_count++]=vertex;
      else if(opposite)return relocate();
      else opposite=vertex;
    }
    if(edge_count!=2U||!opposite)return relocate();
    std::sort(edge.begin(),edge.end());regions[*opposite].insert(edge);
  }
  if(incident.empty())return relocate();
  std::set<std::array<std::uint32_t,2>> expected_edges;
  for(const auto pair:std::array<std::array<unsigned,2>,3>{{
      {{0U,1U}},{{1U,2U}},{{0U,2U}}}}) {
    std::array<std::uint32_t,2> edge{{face[pair[0]],face[pair[1]]}};
    std::sort(edge.begin(),edge.end());expected_edges.insert(edge);
  }
  if(std::any_of(regions.begin(),regions.end(),
                 [&](const auto& region){return region.second!=expected_edges;}))
    return relocate();
  std::vector<Point> points;
  for(const auto& vertex:constraints.vertices) {
    const auto p=vertex.position;points.push_back({p.x,p.y,p.z});
  }
  std::vector<Tet> replacement;
  for(const auto& [opposite,edges]:regions) {
    (void)edges;Tet cell{{face[0],face[1],face[2],opposite}};
    const auto orientation=robust_orient(points,cell);
    if(orientation==0)return relocate();
    if(orientation<0)std::swap(cell[0],cell[1]);replacement.push_back(cell);
  }
  const auto boundary_of=[](const auto& cells) {
    std::map<Face,unsigned> counts;
    for(const auto& cell:cells)for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face side{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)side[cursor++]=cell[i];
      ++counts[face_key(side)];
    }
    std::set<Face> boundary;
    for(const auto& [side,count]:counts)if(count==1U)boundary.insert(side);
    return boundary;
  };
  auto before=boundary_of(incident),after=boundary_of(replacement);
  for(auto it=before.begin();it!=before.end();)
    if(std::find(it->begin(),it->end(),steiner)!=it->end())it=before.erase(it);
    else ++it;
  const auto restored_face=face_key(face);
  after.erase(restored_face);
  if(before!=after)return relocate();
  std::set<Tet> removed;
  for(auto cell:incident){std::sort(cell.begin(),cell.end());removed.insert(cell);}
  std::vector<Tet> restored_mesh;
  for(auto cell:tetrahedra) {
    auto key=cell;std::sort(key.begin(),key.end());
    if(!removed.contains(key))restored_mesh.push_back(cell);
  }
  restored_mesh.insert(restored_mesh.end(),replacement.begin(),replacement.end());

  auto merged=constraints.facets[children.front()];merged.vertices=source;
  for(unsigned corner=0U;corner<3U;++corner) {
    bool found=false;
    for(const auto child:children) {
      const auto& facet=constraints.facets[child];
      const auto at=std::find(facet.vertices.begin(),facet.vertices.end(),source[corner]);
      if(at==facet.vertices.end())continue;
      merged.corners[corner]=facet.corners[static_cast<std::size_t>(at-facet.vertices.begin())];
      found=true;break;
    }
    if(!found) {
      result.failure=CanonicalBoundaryRestorationFailure::constraint_reconstruction_failed;
      return result;
    }
  }
  auto restored_constraints=constraints;
  std::sort(children.begin(),children.end(),std::greater<>());
  for(const auto child:children)
    restored_constraints.facets.erase(restored_constraints.facets.begin()+child);
  restored_constraints.facets.push_back(std::move(merged));
  restored_constraints.facet_split_vertices.erase(
      std::remove_if(restored_constraints.facet_split_vertices.begin(),
                     restored_constraints.facet_split_vertices.end(),
                     [&](const auto& candidate){return candidate.id==entry.vertex_id;}),
      restored_constraints.facet_split_vertices.end());
  restored_constraints.recovery_journal.pop_back();
  restored_constraints.vertices.erase(restored_constraints.vertices.begin()+steiner);
  for(auto& cell:restored_mesh)for(auto& vertex:cell) {
    if(vertex==steiner) {
      result.failure=CanonicalBoundaryRestorationFailure::invalid_retriangulation;
      return result;
    }
    if(vertex>steiner)--vertex;
  }
  if(!has_only_nondegenerate_constraint_facets(restored_constraints)||
     !constraint_mesh_is_valid(restored_constraints,restored_mesh)) {
    return relocate();
  }
  result.constraints=std::move(restored_constraints);
  result.tetrahedra=std::move(restored_mesh);
  result.restored_points=1U;
  result.failure=CanonicalBoundaryRestorationFailure::none;
  return result;
}

CanonicalBoundaryRestorationResult
restore_last_canonical_boundary_steiner_point(
    const CanonicalPlcConstraintSet& constraints,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra) {
  CanonicalBoundaryRestorationResult result;
  result.constraints=constraints;result.tetrahedra=tetrahedra;
  if(constraints.recovery_journal.empty())return result;
  const auto entry=constraints.recovery_journal.back();
  if(entry.kind==CanonicalPlcRecoveryInsertionKind::facet_split)
    return restore_last_canonical_facet_steiner_point(constraints,tetrahedra);
  const auto split=std::find_if(
      constraints.split_vertices.begin(),constraints.split_vertices.end(),
      [&](const auto& candidate){return candidate.id==entry.vertex_id;});
  if(split==constraints.split_vertices.end()) {
    result.failure=CanonicalBoundaryRestorationFailure::missing_split_record;
    return result;
  }
  std::map<std::uint64_t,std::uint32_t> index;
  for(std::size_t i=0U;i<constraints.vertices.size();++i)
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
  if(!index.contains(entry.vertex_id)||!index.contains(split->edge[0])||
     !index.contains(split->edge[1])) {
    result.failure=CanonicalBoundaryRestorationFailure::missing_split_record;
    return result;
  }
  const auto relocate=[&]() {
    return relocate_last_boundary_steiner_point(
        constraints,tetrahedra,entry);
  };
  const auto steiner=index.at(entry.vertex_id);
  const auto left=index.at(split->edge[0]);
  const auto right=index.at(split->edge[1]);
  const auto coarse_edge_present=std::any_of(
      tetrahedra.begin(),tetrahedra.end(),[&](const auto& cell) {
        return std::find(cell.begin(),cell.end(),left)!=cell.end()&&
               std::find(cell.begin(),cell.end(),right)!=cell.end();
      });
  if(coarse_edge_present) {
    // Pinned removeEdgStiner handles an accuracy-bypassed split before it
    // colors the point sphere. It first removes the reappeared coarse edge;
    // when that cannot be done, clearbndpnt converts the split point to an
    // ordinary interior Steiner point for removePnt/smooth_volume.
    const auto removed=remove_mesh_edge_in_mesh(
        constraints,{left,right},tetrahedra,nullptr,true,false);
    if(removed.accepted)
      return restore_last_canonical_boundary_steiner_point(
          constraints,removed.tetrahedra);
    if(!entry.facets_before.empty()) {
      auto interior_constraints=constraints;
      interior_constraints.facets=entry.facets_before;
      interior_constraints.split_vertices.erase(
          std::remove_if(interior_constraints.split_vertices.begin(),
                         interior_constraints.split_vertices.end(),
                         [&](const auto& candidate) {
                           return candidate.id==entry.vertex_id;
                         }),
          interior_constraints.split_vertices.end());
      interior_constraints.recovery_journal.pop_back();
      if(std::none_of(interior_constraints.interior_steiner_vertices.begin(),
                      interior_constraints.interior_steiner_vertices.end(),
                      [&](const auto& candidate) {
                        return candidate.id==entry.vertex_id;
                      }))
        interior_constraints.interior_steiner_vertices.push_back(
            {entry.vertex_id,
             CanonicalInteriorSteinerKind::boundary_relocation});
      result.constraints=std::move(interior_constraints);
      result.tetrahedra=tetrahedra;
      result.restored_points=1U;
      result.interior_points_inserted=1U;
      result.relocation_regions=1U;
      result.failure=CanonicalBoundaryRestorationFailure::none;
      return result;
    }
  }
  struct Pair {std::optional<Tet> left;std::optional<Tet> right;};
  std::map<std::array<std::uint32_t,2>,Pair> pairs;
  std::vector<Tet> incident;
  for(const auto& cell:tetrahedra) {
    if(std::find(cell.begin(),cell.end(),steiner)==cell.end())continue;
    incident.push_back(cell);
    const auto has_left=std::find(cell.begin(),cell.end(),left)!=cell.end();
    const auto has_right=std::find(cell.begin(),cell.end(),right)!=cell.end();
    if(has_left==has_right)return relocate();
    std::array<std::uint32_t,2> other{};unsigned cursor{};
    for(const auto vertex:cell)
      if(vertex!=steiner&&vertex!=(has_left?left:right))other[cursor++]=vertex;
    if(cursor!=2U)return relocate();
    std::sort(other.begin(),other.end());
    auto& pair=pairs[other];
    auto& side=has_left?pair.left:pair.right;
    if(side)return relocate();
    side=cell;
  }
  if(incident.empty()||std::any_of(pairs.begin(),pairs.end(),
      [](const auto& item){return !item.second.left||!item.second.right;}))
    return relocate();
  std::vector<Point> points;
  for(const auto& vertex:constraints.vertices) {
    const auto p=vertex.position;points.push_back({p.x,p.y,p.z});
  }
  std::vector<Tet> replacement;
  for(const auto& [other,pair]:pairs) {
    (void)pair;
    Tet cell{{left,right,other[0],other[1]}};
    const auto orientation=robust_orient(points,cell);
    if(orientation==0)return relocate();
    if(orientation<0)std::swap(cell[0],cell[1]);
    replacement.push_back(cell);
  }
  const auto boundary_of=[](const auto& cells) {
    std::map<Face,unsigned> counts;
    for(const auto& cell:cells)for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omitted)face[cursor++]=cell[i];
      ++counts[face_key(face)];
    }
    std::set<Face> boundary;
    for(const auto& [face,count]:counts) {
      if(count==1U)boundary.insert(face);
      else if(count!=2U)return std::set<Face>{};
    }
    return boundary;
  };
  auto old_boundary=boundary_of(incident);
  auto new_boundary=boundary_of(replacement);
  for(auto it=old_boundary.begin();it!=old_boundary.end();) {
    if(std::find(it->begin(),it->end(),steiner)!=it->end())it=old_boundary.erase(it);
    else ++it;
  }
  for(auto it=new_boundary.begin();it!=new_boundary.end();) {
    const auto restores_parent_face=
        std::find(it->begin(),it->end(),left)!=it->end()&&
        std::find(it->begin(),it->end(),right)!=it->end();
    if(restores_parent_face)it=new_boundary.erase(it);else ++it;
  }
  if(old_boundary!=new_boundary)return relocate();
  std::set<Tet> removed;
  for(auto cell:incident){std::sort(cell.begin(),cell.end());removed.insert(cell);}
  std::vector<Tet> restored_mesh;
  for(auto cell:tetrahedra) {
    auto key=cell;std::sort(key.begin(),key.end());
    if(!removed.contains(key))restored_mesh.push_back(cell);
  }
  restored_mesh.insert(restored_mesh.end(),replacement.begin(),replacement.end());

  std::vector<CanonicalPlcConstraintFacet> restored_facets;
  if(!entry.facets_before.empty()) {
    restored_facets=entry.facets_before;
  } else if(!entry.replaced_facets.empty()) {
    for(const auto& facet:constraints.facets)
      if(std::find(facet.vertices.begin(),facet.vertices.end(),entry.vertex_id)==
         facet.vertices.end())restored_facets.push_back(facet);
    restored_facets.insert(restored_facets.end(),entry.replaced_facets.begin(),
                           entry.replaced_facets.end());
  } else {
    std::vector<bool> consumed(constraints.facets.size());
    for(std::size_t i=0U;i<constraints.facets.size();++i) {
    if(consumed[i])continue;
    const auto& first=constraints.facets[i];
    if(std::find(first.vertices.begin(),first.vertices.end(),entry.vertex_id)==
       first.vertices.end()) {
      restored_facets.push_back(first);consumed[i]=true;continue;
    }
    std::optional<std::size_t> partner;
    for(std::size_t j=i+1U;j<constraints.facets.size();++j) {
      if(consumed[j])continue;
      const auto& second=constraints.facets[j];
      if(second.parent==first.parent&&second.source_vertices==first.source_vertices&&
         std::find(second.vertices.begin(),second.vertices.end(),entry.vertex_id)!=
             second.vertices.end()) {partner=j;break;}
    }
    if(!partner) {
      result.failure=CanonicalBoundaryRestorationFailure::constraint_reconstruction_failed;
      return result;
    }
    const auto& second=constraints.facets[*partner];
    auto merged=first;merged.vertices=first.source_vertices;
    for(unsigned corner=0U;corner<3U;++corner) {
      const auto id=merged.vertices[corner];
      const auto take=[&](const auto& facet)->std::optional<FacetBarycentricPoint> {
        const auto found=std::find(facet.vertices.begin(),facet.vertices.end(),id);
        if(found==facet.vertices.end())return std::nullopt;
        return facet.corners[static_cast<std::size_t>(found-facet.vertices.begin())];
      };
      auto value=take(first);if(!value)value=take(second);
      if(!value) {
        result.failure=CanonicalBoundaryRestorationFailure::constraint_reconstruction_failed;
        return result;
      }
      merged.corners[corner]=*value;
    }
    restored_facets.push_back(std::move(merged));
      consumed[i]=true;consumed[*partner]=true;
    }
  }
  auto restored_constraints=constraints;
  restored_constraints.facets=std::move(restored_facets);
  restored_constraints.split_vertices.erase(
      std::remove_if(restored_constraints.split_vertices.begin(),
                     restored_constraints.split_vertices.end(),
                     [&](const auto& candidate){return candidate.id==entry.vertex_id;}),
      restored_constraints.split_vertices.end());
  restored_constraints.recovery_journal.pop_back();
  restored_constraints.vertices.erase(restored_constraints.vertices.begin()+steiner);
  for(auto& cell:restored_mesh)for(auto& vertex:cell) {
    if(vertex==steiner) {
      result.failure=CanonicalBoundaryRestorationFailure::invalid_retriangulation;
      return result;
    }
    if(vertex>steiner)--vertex;
  }
  if(!has_only_nondegenerate_constraint_facets(restored_constraints)||
     !constraint_mesh_is_valid(restored_constraints,restored_mesh)) {
    return relocate();
  }
  result.constraints=std::move(restored_constraints);
  result.tetrahedra=std::move(restored_mesh);
  result.restored_points=1U;
  result.failure=CanonicalBoundaryRestorationFailure::none;
  return result;
}

namespace {
CanonicalFacetCavityResult recover_literal_facet_by_two_sided_cavity_impl(
    const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,3> facet,
    const std::vector<std::array<std::uint32_t,4>>& mesh,
    std::size_t maximum_cavity_vertices,std::size_t maximum_retriangulation_trials,
    std::size_t maximum_cavity_expansions,std::set<std::size_t> forced,
    std::size_t expansion_depth) {
  CanonicalFacetCavityResult result;
  std::sort(facet.begin(),facet.end());
  std::map<std::uint64_t,std::uint32_t> index;
  std::vector<Point> points;
  for(std::size_t i=0;i<constraints.vertices.size();++i) {
    index.emplace(constraints.vertices[i].id,static_cast<std::uint32_t>(i));
    const auto p=constraints.vertices[i].position;points.push_back({p.x,p.y,p.z});
  }
  const auto requested=std::find_if(constraints.facets.begin(),constraints.facets.end(),[&](const auto& candidate){auto key=candidate.vertices;std::sort(key.begin(),key.end());return key==facet;});
  if(requested==constraints.facets.end())return result;
  Face target{};
  for(unsigned i=0;i<3U;++i) {
    const auto found=index.find(requested->parent.vertex_ids[i]);
    if(found==index.end())return result;
    target[i]=found->second;
  }
  target=face_key(target);
  std::set<Face> target_patch;
  std::set<std::uint64_t> parent_point_ids;
  std::map<std::uint64_t,FacetBarycentricPoint> parent_points;
  for(const auto& candidate:constraints.facets)if(candidate.parent==requested->parent) {
    Face face{};for(unsigned i=0U;i<3U;++i){face[i]=index.at(candidate.vertices[i]);parent_point_ids.insert(candidate.vertices[i]);parent_points.emplace(candidate.vertices[i],candidate.corners[i]);}
    target_patch.insert(face_key(face));
  }
  if(target_patch.empty())return result;
  auto conforming_mesh=mesh;bool repaired_bypassed_edge=false;
  std::size_t repaired_cells{};
  for(const auto& split:constraints.split_vertices) {
    const auto repaired=restellarize_bypassed_constraint_edge(
        constraints,split,conforming_mesh);
    if(!repaired.accepted)continue;
    conforming_mesh=repaired.tetrahedra;repaired_bypassed_edge=true;
    repaired_cells+=repaired.intersected_tetrahedra;
  }
  if(repaired_bypassed_edge) {
    auto recovered=recover_literal_facet_by_two_sided_cavity_impl(
        constraints,facet,conforming_mesh,maximum_cavity_vertices,
        maximum_retriangulation_trials,maximum_cavity_expansions,
        std::move(forced),expansion_depth);
    if(recovered.accepted)recovered.already_recovered=false;
    recovered.intersected_tetrahedra+=repaired_cells;
    return recovered;
  }
  const auto a=points[target[0]],u=points[target[1]]-a,v=points[target[2]]-a;
  const auto d00=dot(u,u),d01=dot(u,v),d11=dot(v,v),det=d00*d11-d01*d01;
  if(!(det>0.0L))return result;

  std::map<Face,std::vector<std::size_t>> global_faces;
  for(std::size_t ti=0;ti<mesh.size();++ti)for(unsigned omit=0;omit<4U;++omit) {
    Face face{};unsigned n{};for(unsigned i=0;i<4U;++i)if(i!=omit)face[n++]=mesh[ti][i];
    global_faces[face_key(face)].push_back(ti);
  }
  std::set<std::size_t> coplanar_overlap_cells;
  bool has_overlapping_alternative=false;
  const auto projected=[&](std::uint32_t vertex){const auto relative=points[vertex]-a;const auto d20=dot(relative,u),d21=dot(relative,v);return std::array<long double,2>{{(d11*d20-d01*d21)/det,(d00*d21-d01*d20)/det}};};
  const std::array<std::array<long double,2>,3> target2{{{{0.0L,0.0L}},{{1.0L,0.0L}},{{0.0L,1.0L}}}};
  const auto positive_area_overlap=[&](const std::array<std::array<long double,2>,3>& other){
    const auto separated=[](const auto& left,const auto& right){for(unsigned edge=0U;edge<3U;++edge){const auto dx=left[(edge+1U)%3U][0]-left[edge][0],dy=left[(edge+1U)%3U][1]-left[edge][1];const std::array<long double,2> axis{{-dy,dx}};long double lmin=dot(Point{left[0][0],left[0][1],0.0L},Point{axis[0],axis[1],0.0L}),lmax=lmin,rmin=dot(Point{right[0][0],right[0][1],0.0L},Point{axis[0],axis[1],0.0L}),rmax=rmin;for(unsigned i=1U;i<3U;++i){const auto lp=left[i][0]*axis[0]+left[i][1]*axis[1],rp=right[i][0]*axis[0]+right[i][1]*axis[1];lmin=std::min(lmin,lp);lmax=std::max(lmax,lp);rmin=std::min(rmin,rp);rmax=std::max(rmax,rp);}const auto scale=std::max({1.0L,std::abs(lmin),std::abs(lmax),std::abs(rmin),std::abs(rmax)});if(std::min(lmax,rmax)-std::max(lmin,rmin)<=2048.0L*LDBL_EPSILON*scale)return true;}return false;};
    return !separated(target2,other)&&!separated(other,target2);
  };
  for(const auto& [face,uses]:global_faces) {
    bool coplanar=true;for(const auto vertex:face)coplanar=coplanar&&robust_orient(points[target[0]],points[target[1]],points[target[2]],points[vertex])==0;
    if(!coplanar)continue;
    const std::array<std::array<long double,2>,3> triangle{{projected(face[0]),projected(face[1]),projected(face[2])}};
    if(positive_area_overlap(triangle)) {
      coplanar_overlap_cells.insert(uses.begin(),uses.end());
      has_overlapping_alternative=has_overlapping_alternative||
          !target_patch.contains(face);
    }
  }
  const bool literal_patch_present=std::all_of(
      target_patch.begin(),target_patch.end(),
      [&](const auto& face){return global_faces.contains(face);});
  if(literal_patch_present&&!has_overlapping_alternative) {
    result.accepted=true;result.already_recovered=true;
    result.failure=CanonicalFacetCavityFailure::none;
    result.tetrahedra=mesh;return result;
  }
  // If a temporary boundary point lies strictly inside an older parent
  // edge, every tetrahedron in that coarse edge star must participate. A
  // facet-only cavity otherwise leaves a hanging node on its side boundary
  // and no conforming half-cavity tetrahedralization can exist.
  std::set<std::array<std::uint32_t,2>> bypassed_parent_edges;
  for(auto first=parent_points.begin();first!=parent_points.end();++first)
    for(auto second=std::next(first);second!=parent_points.end();++second) {
      const auto bary2=[](const FacetBarycentricPoint& point) {
        return std::array<long double,2>{{
            static_cast<long double>(point.numerator[1])/point.denominator,
            static_cast<long double>(point.numerator[2])/point.denominator}};
      };
      const auto left=bary2(first->second),right=bary2(second->second);
      const auto dx=right[0]-left[0],dy=right[1]-left[1];
      const auto length2=dx*dx+dy*dy;if(!(length2>0.0L))continue;
      bool bypassed=false;
      for(const auto& [middle_id,middle_point]:parent_points) {
        if(middle_id==first->first||middle_id==second->first)continue;
        const auto middle=bary2(middle_point);
        const auto cross=(middle[0]-left[0])*dy-(middle[1]-left[1])*dx;
        const auto parameter=((middle[0]-left[0])*dx+
                              (middle[1]-left[1])*dy)/length2;
        if(std::abs(cross)<=256.0L*LDBL_EPSILON&&
           parameter>0.0L&&parameter<1.0L){bypassed=true;break;}
      }
      if(bypassed) {
        std::array<std::uint32_t,2> edge{{index.at(first->first),index.at(second->first)}};
        std::sort(edge.begin(),edge.end());bypassed_parent_edges.insert(edge);
      }
    }
  std::vector<std::size_t> selected;
  for(std::size_t ti=0;ti<mesh.size();++ti) {
    const auto& tet=mesh[ti];bool cut=false;
    for(unsigned i=0;i<4U&&!cut;++i)for(unsigned j=i+1U;j<4U&&!cut;++j) {
      const auto op=robust_orient(points[target[0]],points[target[1]],points[target[2]],points[tet[i]]);
      const auto oq=robust_orient(points[target[0]],points[target[1]],points[target[2]],points[tet[j]]);
      if(op==0||oq==0||op==oq)continue;
      const auto dp=orient(points[target[0]],points[target[1]],points[target[2]],points[tet[i]]);
      const auto dq=orient(points[target[0]],points[target[1]],points[target[2]],points[tet[j]]);
      const auto t=dp/(dp-dq);const auto hit=points[tet[i]]+(points[tet[j]]-points[tet[i]])*t;
      const auto relative=hit-a;
      const auto d20=dot(relative,u),d21=dot(relative,v);
      const auto w1=(d11*d20-d01*d21)/det,w2=(d00*d21-d01*d20)/det,w0=1.0L-w1-w2;
      const auto epsilon=2048.0L*LDBL_EPSILON;
      cut=w0>epsilon&&w1>epsilon&&w2>epsilon;
    }
    bool contains_bypassed_edge=false;
    for(const auto& edge:bypassed_parent_edges)
      contains_bypassed_edge=contains_bypassed_edge||
          (std::find(tet.begin(),tet.end(),edge[0])!=tet.end()&&
           std::find(tet.begin(),tet.end(),edge[1])!=tet.end());
    if(cut||forced.contains(ti)||coplanar_overlap_cells.contains(ti)||
       contains_bypassed_edge)selected.push_back(ti);
  }
  result.intersected_tetrahedra=selected.size();
  if(selected.empty()){result.failure=CanonicalFacetCavityFailure::no_intersected_cavity;return result;}

  std::map<Face,unsigned> cavity_uses;
  std::set<std::uint32_t> cavity_vertices;
  long double old_volume{};
  for(const auto ti:selected) {
    const auto& tet=mesh[ti];cavity_vertices.insert(tet.begin(),tet.end());
    old_volume+=std::abs(orient(points[tet[0]],points[tet[1]],points[tet[2]],points[tet[3]]));
    for(unsigned omit=0;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0;i<4U;++i)if(i!=omit)face[n++]=tet[i];++cavity_uses[face_key(face)];}
  }
  if(cavity_vertices.size()>maximum_cavity_vertices){result.failure=CanonicalFacetCavityFailure::retriangulation_failed;return result;}
  std::set<Face> boundary;
  for(const auto& [face,count]:cavity_uses) {
    if(count==1U)boundary.insert(face);
    else if(count!=2U){result.failure=CanonicalFacetCavityFailure::nonmanifold_cavity;return result;}
  }
  std::set<Face> protected_faces;
  for(const auto& constraint:constraints.facets){Face face{};for(unsigned i=0;i<3U;++i)face[i]=index.at(constraint.vertices[i]);protected_faces.insert(face_key(face));}
  for(const auto& face:protected_faces)if(!target_patch.contains(face)&&cavity_uses.contains(face)&&!boundary.contains(face)) {
    result.failure=CanonicalFacetCavityFailure::protected_constraint;return result;
  }

  std::map<std::uint32_t,int> side;
  for(const auto vertex:cavity_vertices)side[vertex]=robust_orient(points[target[0]],points[target[1]],points[target[2]],points[vertex]);
  std::set<Face> replaced_coplanar_boundary;
  for(const auto& face:boundary){bool replace=true;for(const auto vertex:face)replace=replace&&parent_point_ids.contains(constraints.vertices[vertex].id);if(replace)replaced_coplanar_boundary.insert(face);}
  std::set<Face> top_boundary=target_patch,bottom_boundary=target_patch;
  for(const auto& face:boundary) {
    if(replaced_coplanar_boundary.contains(face))continue;
    bool positive=false,negative=false;
    for(const auto vertex:face){positive=positive||side[vertex]>0;negative=negative||side[vertex]<0;}
    if(positive&&negative){result.failure=CanonicalFacetCavityFailure::cavity_crosses_plane;return result;}
    if(!negative)top_boundary.insert(face);
    if(!positive)bottom_boundary.insert(face);
  }

  struct Candidate {Tet tet{};std::array<Face,4> faces{};long double volume{};};
  std::optional<Face> missing_delaunay_boundary;
  bool missing_boundary_is_top{};
  const auto fill_half=[&](bool top,const std::set<Face>& half_boundary,
                           std::vector<Tet>& output,std::size_t& trials) {
    std::vector<std::uint32_t> vertices;
    for(const auto vertex:cavity_vertices)if(top?side[vertex]>=0:side[vertex]<=0)vertices.push_back(vertex);
    // A constrained hull patch has material on only one side.  The opposite
    // half-cavity is the zero-volume empty set bounded solely by the target;
    // it needs no tetrahedron and is a valid half of this transaction.
    if(half_boundary==target_patch)return true;
    // Diazzi/Si cavity recovery re-tetrahedralizes each half from its vertex
    // set, then expands when a required cavity face is absent. Use our exact,
    // deterministic Delaunay kernel first and accept it only when its complete
    // hull is exactly the requested half-cavity boundary.
    if(vertices.size()>=4U) {
      CanonicalDelaunaySeedInput local_input;
      local_input.maximum_vertices=vertices.size();
      local_input.maximum_tetrahedra=std::max<std::size_t>(16U,vertices.size()*vertices.size()*8U);
      for(const auto vertex:vertices) {
        local_input.vertices.push_back(constraints.vertices[vertex].position);
        local_input.stable_vertex_ids.push_back(constraints.vertices[vertex].id);
      }
      const auto local=build_canonical_delaunay_seed(local_input);
      if(local.accepted()) {
        std::map<Face,unsigned> local_uses;
        std::vector<Tet> mapped;
        mapped.reserve(local.tetrahedra.size());
        for(const auto& local_tet:local.tetrahedra) {
          Tet tet{{vertices[local_tet[0]],vertices[local_tet[1]],
                   vertices[local_tet[2]],vertices[local_tet[3]]}};
          mapped.push_back(tet);
          for(unsigned omit=0;omit<4U;++omit){Face face{};unsigned n{};for(unsigned q=0;q<4U;++q)if(q!=omit)face[n++]=tet[q];++local_uses[face_key(face)];}
        }
        std::set<Face> local_boundary;bool manifold=true;
        for(const auto& [face,count]:local_uses) {
          if(count==1U)local_boundary.insert(face);
          else if(count!=2U)manifold=false;
        }
        if(manifold&&local_boundary==half_boundary) {
          output=std::move(mapped);result.used_local_delaunay=true;return true;
        }
        if(manifold) {
          for(const auto& face:half_boundary)if(!local_boundary.contains(face)) {
            missing_delaunay_boundary=face;missing_boundary_is_top=top;break;
          }
        }
      }
    }
    std::vector<Candidate> candidates;
    for(std::size_t i=0;i<vertices.size();++i)for(std::size_t j=i+1;j<vertices.size();++j)
      for(std::size_t k=j+1;k<vertices.size();++k)for(std::size_t l=k+1;l<vertices.size();++l) {
        Tet tet{{vertices[i],vertices[j],vertices[k],vertices[l]}};
        auto volume=orient(points[tet[0]],points[tet[1]],points[tet[2]],points[tet[3]]);
        if(std::abs(volume)<=1e-20L)continue;if(volume<0)std::swap(tet[0],tet[1]);
        Candidate candidate;candidate.tet=tet;candidate.volume=std::abs(volume);
        for(unsigned omit=0;omit<4U;++omit){Face face{};unsigned n{};for(unsigned q=0;q<4U;++q)if(q!=omit)face[n++]=tet[q];candidate.faces[omit]=face_key(face);}
        candidates.push_back(candidate);
      }
    std::map<Face,std::vector<std::size_t>> touching;
    for(std::size_t i=0;i<candidates.size();++i)for(const auto& face:candidates[i].faces)if(half_boundary.contains(face))touching[face].push_back(i);
    std::map<Face,unsigned> uses;std::vector<std::size_t> chosen;std::set<std::size_t> chosen_set;
    std::function<bool()> search=[&]() {
      if(++trials>maximum_retriangulation_trials)return false;
      std::optional<Face> next;
      for(const auto& face:half_boundary)if(uses[face]==0U){next=face;break;}
      if(!next){for(const auto& [face,count]:uses)if(count!=0U&&(half_boundary.contains(face)?count!=1U:count!=2U))return false;return true;}
      for(const auto ci:touching[*next])if(!chosen_set.contains(ci)) {
        bool valid=true;for(const auto& face:candidates[ci].faces){const auto count=uses[face]+1U;if((half_boundary.contains(face)&&count>1U)||(!half_boundary.contains(face)&&count>2U)){valid=false;break;}}
        if(!valid)continue;for(const auto& face:candidates[ci].faces)++uses[face];chosen.push_back(ci);chosen_set.insert(ci);
        if(search())return true;
        chosen_set.erase(ci);chosen.pop_back();for(const auto& face:candidates[ci].faces)--uses[face];
      }
      return false;
    };
    if(!search())return false;
    for(const auto ci:chosen)output.push_back(candidates[ci].tet);
    return true;
  };

  std::vector<Tet> top_tets,bottom_tets;std::size_t trials{};
  if(!fill_half(true,top_boundary,top_tets,trials)||!fill_half(false,bottom_boundary,bottom_tets,trials)) {
    if(missing_delaunay_boundary) {
      if(expansion_depth==maximum_cavity_expansions){result.failure=CanonicalFacetCavityFailure::expansion_limit;result.cavity_expansions=expansion_depth;return result;}
      const auto adjacent=global_faces.find(*missing_delaunay_boundary);
      if(adjacent!=global_faces.end()) {
        const std::set<std::size_t> current(selected.begin(),selected.end());
        for(const auto neighbour:adjacent->second)if(!current.contains(neighbour)) {
          bool correct_side=true;
          for(const auto vertex:mesh[neighbour]) {
            const auto orientation=robust_orient(points[target[0]],points[target[1]],points[target[2]],points[vertex]);
            if(missing_boundary_is_top?orientation<0:orientation>0){correct_side=false;break;}
          }
          if(!correct_side){result.failure=CanonicalFacetCavityFailure::cavity_crosses_plane;result.cavity_expansions=expansion_depth;return result;}
          forced.insert(neighbour);
          auto expanded=recover_literal_facet_by_two_sided_cavity_impl(
              constraints,facet,mesh,maximum_cavity_vertices,
              maximum_retriangulation_trials,maximum_cavity_expansions,
              std::move(forced),expansion_depth+1U);
          expanded.cavity_expansions=std::max(expanded.cavity_expansions,expansion_depth+1U);
          return expanded;
        }
      }
    }
    result.retriangulation_trials=trials;result.failure=CanonicalFacetCavityFailure::retriangulation_failed;return result;
  }
  result.retriangulation_trials=trials;result.top_tetrahedra=top_tets.size();result.bottom_tetrahedra=bottom_tets.size();
  long double new_volume{};for(const auto& tet:top_tets)new_volume+=std::abs(orient(points[tet[0]],points[tet[1]],points[tet[2]],points[tet[3]]));for(const auto& tet:bottom_tets)new_volume+=std::abs(orient(points[tet[0]],points[tet[1]],points[tet[2]],points[tet[3]]));
  result.original_cavity_six_volume=old_volume;
  result.replacement_six_volume=new_volume;
  if(std::abs(new_volume-old_volume)>std::max(1.0L,old_volume)*1e-14L){
    // A topologically closed candidate can still wind around a concave half
    // cavity and therefore enclose the wrong volume.  Treat that exactly like
    // a missing local-Delaunay boundary: enlarge the cavity across the first
    // offending face and retry.  The volume gate remains authoritative.
    if(missing_delaunay_boundary&&expansion_depth==maximum_cavity_expansions) {
      result.failure=CanonicalFacetCavityFailure::expansion_limit;
      result.cavity_expansions=expansion_depth;
      return result;
    }
    if(missing_delaunay_boundary&&expansion_depth<maximum_cavity_expansions) {
      const auto adjacent=global_faces.find(*missing_delaunay_boundary);
      if(adjacent!=global_faces.end()) {
        const std::set<std::size_t> current(selected.begin(),selected.end());
        for(const auto neighbour:adjacent->second)if(!current.contains(neighbour)) {
          bool correct_side=true;
          for(const auto vertex:mesh[neighbour]) {
            const auto orientation=robust_orient(points[target[0]],points[target[1]],points[target[2]],points[vertex]);
            if(missing_boundary_is_top?orientation<0:orientation>0){correct_side=false;break;}
          }
          if(!correct_side)break;
          forced.insert(neighbour);
          auto expanded=recover_literal_facet_by_two_sided_cavity_impl(
              constraints,facet,mesh,maximum_cavity_vertices,
              maximum_retriangulation_trials,maximum_cavity_expansions,
              std::move(forced),expansion_depth+1U);
          expanded.cavity_expansions=std::max(expanded.cavity_expansions,expansion_depth+1U);
          return expanded;
        }
      }
    }
    result.failure=CanonicalFacetCavityFailure::volume_disagreement;return result;
  }
  std::set<std::size_t> removed(selected.begin(),selected.end());for(std::size_t ti=0;ti<mesh.size();++ti)if(!removed.contains(ti))result.tetrahedra.push_back(mesh[ti]);result.tetrahedra.insert(result.tetrahedra.end(),top_tets.begin(),top_tets.end());result.tetrahedra.insert(result.tetrahedra.end(),bottom_tets.begin(),bottom_tets.end());
  std::sort(result.tetrahedra.begin(),result.tetrahedra.end(),[](Tet left,Tet right){std::sort(left.begin(),left.end());std::sort(right.begin(),right.end());return left<right;});
  std::map<Face,unsigned> new_cavity_uses;for(const auto& tet:top_tets)for(unsigned omit=0;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0;i<4U;++i)if(i!=omit)face[n++]=tet[i];++new_cavity_uses[face_key(face)];}for(const auto& tet:bottom_tets)for(unsigned omit=0;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0;i<4U;++i)if(i!=omit)face[n++]=tet[i];++new_cavity_uses[face_key(face)];}
  std::set<Face> new_boundary;for(const auto& [face,count]:new_cavity_uses)if(count==1U)new_boundary.insert(face);else if(count!=2U){result.tetrahedra.clear();result.failure=CanonicalFacetCavityFailure::nonmanifold_cavity;return result;}
  // The recovered patch is the common interface between the two half
  // cavities.  It must occur twice in their union, not on the boundary of
  // the combined replacement cavity.
  auto expected_boundary=boundary;for(const auto& face:replaced_coplanar_boundary)expected_boundary.erase(face);
  if(new_boundary!=expected_boundary){result.tetrahedra.clear();result.failure=CanonicalFacetCavityFailure::changed_boundary;return result;}
  result.accepted=true;result.failure=CanonicalFacetCavityFailure::none;result.cavity_expansions=expansion_depth;return result;
}
} // namespace

CanonicalFacetCavityResult recover_literal_facet_by_two_sided_cavity(
    const CanonicalPlcConstraintSet& constraints,std::array<std::uint64_t,3> facet,
    const std::vector<std::array<std::uint32_t,4>>& tetrahedra,
    std::size_t maximum_cavity_vertices,std::size_t maximum_retriangulation_trials,
    std::size_t maximum_cavity_expansions) {
  return recover_literal_facet_by_two_sided_cavity_impl(
      constraints,facet,tetrahedra,maximum_cavity_vertices,
      maximum_retriangulation_trials,maximum_cavity_expansions,{},0U);
}

CanonicalPlcRecoveryResult recover_canonical_plc_constraints_impl(
    const CanonicalPlcConstraintSet& initial,
    const CanonicalPlcRecoveryOptions& options) {
  CanonicalPlcRecoveryResult result;result.constraints=initial;
  // Algorithm 2 line 1 implementation support: the enclosing tetrahedron is
  // the private super-tetrahedron used by the Delaunay construction. It is not
  // a recovered PLC constraint and is classified as exterior at extraction.
  if(options.use_enclosing_cage&&!result.constraints.vertices.empty()) {
    auto lo=result.constraints.vertices.front().position;
    auto hi=lo;std::uint64_t next_id{};
    for(const auto& vertex:result.constraints.vertices) {
      const auto p=vertex.position;
      lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);lo.z=std::min(lo.z,p.z);
      hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);hi.z=std::max(hi.z,p.z);
      next_id=std::max(next_id,vertex.id);
    }
    const auto extent=std::max({hi.x-lo.x,hi.y-lo.y,hi.z-lo.z});
    if(extent>0.0&&next_id<=std::numeric_limits<std::uint64_t>::max()-4U&&
       result.constraints.vertices.size()+4U<=options.maximum_vertices) {
      const auto centre=(lo+hi)/2.0;
      const auto scale=16.0*extent;
      result.constraints.vertices.push_back({++next_id,centre+Vec3{-scale,-scale,-scale}});
      result.constraints.vertices.push_back({++next_id,centre+Vec3{ scale,-scale,-scale}});
      result.constraints.vertices.push_back({++next_id,centre+Vec3{0.0, scale,-scale}});
      result.constraints.vertices.push_back({++next_id,centre+Vec3{0.0,0.0, scale}});
    }
  }
  const auto is_core_edge=[&](std::array<std::uint64_t,2> edge){return std::any_of(result.constraints.facets.begin(),result.constraints.facets.end(),[edge](const auto& face){
    return face.core_interface&&std::find(face.vertices.begin(),face.vertices.end(),edge[0])!=face.vertices.end()&&
        std::find(face.vertices.begin(),face.vertices.end(),edge[1])!=face.vertices.end();
  });};
  CanonicalDelaunaySeedInput input;input.maximum_vertices=result.constraints.vertices.size();input.maximum_tetrahedra=options.maximum_tetrahedra;input.exact_affine_planes=result.constraints.exact_affine_planes;for(const auto& vertex:result.constraints.vertices){input.vertices.push_back(vertex.position);input.stable_vertex_ids.push_back(vertex.id);}const auto seed=build_canonical_background_seed(input);result.seed_failure=seed.failure;result.seed_invalid_reason=seed.invalid_reason;result.background_stellar_fallback_used=seed.stellar_fallback;if(!seed.accepted()){result.constraints={};result.failure=CanonicalPlcRecoveryFailure::seed_failed;return result;}result.tetrahedra=seed.tetrahedra;if(!constraint_mesh_is_valid(result.constraints,result.tetrahedra)){result.constraints={};result.tetrahedra={};result.failure=CanonicalPlcRecoveryFailure::seed_failed;return result;}
  // Segment recovery is an explicit prerequisite of face recovery. A failed
  // constrained segment is split at its first mesh-face intersection and the
  // same point is inserted into the retained tetrahedralization.
  const auto recover_all_segments=[&]() -> bool {
    for(;;) {
      result.inspection=inspect_canonical_plc_tetrahedra(result.constraints,result.tetrahedra);
      if(result.inspection.missing_edges.empty())return true;
      if(result.attempted_edge_recoveries==options.maximum_edge_recovery_attempts) {
        result.failure=CanonicalPlcRecoveryFailure::resource_limit;
        return false;
      }
      const auto edge=result.inspection.missing_edges.front();
      const std::set<std::array<std::uint64_t,2>> missing_edges_before(
          result.inspection.missing_edges.begin(),
          result.inspection.missing_edges.end());
      ++result.attempted_edge_recoveries;result.last_steiner_attempts=0U;
      result.last_fhc_candidates_generated=0U;
      result.last_fhc_candidates_attempted=0U;
      result.last_fhc_forced_cavity_rejections=0U;
      result.last_fhc_nonmonotonic_rejections=0U;
      result.last_fhc_had_blocking_edge=false;
      result.last_fhc_had_blocking_face=false;
      const auto obstructions_before=constraint_segment_obstruction_count(
          result.constraints,edge,result.tetrahedra);
      const auto preserves_recovered_segments=[&](
          const CanonicalPlcConstraintSet& constraints,
          const std::vector<Tet>& tetrahedra) {
        const auto inspection=inspect_canonical_plc_tetrahedra(
            constraints,tetrahedra);
        return std::none_of(
            inspection.missing_edges.begin(),inspection.missing_edges.end(),
            [&](const auto& missing) {
              return !missing_edges_before.contains(missing);
            });
      };
      const auto valid_candidate=[&](CanonicalLiteralEdgeFlipResult candidate) {
        if(candidate.accepted&&
           !constraint_mesh_mutation_is_valid(
               result.constraints,result.tetrahedra,candidate.tetrahedra)) {
          candidate.accepted=false;
          candidate.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;
          candidate.tetrahedra.clear();
          ++result.invalid_candidate_mesh_rejections;
        }
        if(candidate.accepted&&
           constraint_segment_obstruction_count(
               result.constraints,edge,candidate.tetrahedra)>=
               obstructions_before) {
          // Local face/edge recovery is a transaction on the segment's
          // intersection complex. Accepting an equal-complexity inverse
          // transaction lets two valid bistellar states cycle forever.
          candidate.accepted=false;
          candidate.failure=CanonicalLiteralEdgeFlipFailure::changed_boundary;
          candidate.tetrahedra.clear();
          ++result.invalid_candidate_mesh_rejections;
        }
        if(candidate.accepted&&
           !preserves_recovered_segments(
               result.constraints,candidate.tetrahedra)) {
            // Recovered PLC segments are frozen. Without this check, a valid
            // local flip for the current segment can erase an earlier one and
            // the outer loop cycles among incomplete constraint complexes.
            candidate.accepted=false;
            candidate.failure=CanonicalLiteralEdgeFlipFailure::frozen_cavity_boundary;
            candidate.tetrahedra.clear();
            ++result.invalid_candidate_mesh_rejections;
        }
        return candidate;
      };
      const auto generalized_face=generalized_face_flip_for_segment(
          result.constraints,edge,result.tetrahedra);
      const auto& face_recovery=generalized_face.flip;
      result.mesh_edge_removal_attempts+=generalized_face.edge_removal_attempts;
      result.mesh_edge_retriangulation_trials+=
          generalized_face.edge_retriangulation_trials;
      result.maximum_mesh_edge_degree_attempted=std::max(
          result.maximum_mesh_edge_degree_attempted,
          generalized_face.maximum_edge_degree);
      const auto locked_edge=face_recovery.blocking_mesh_edge;
      auto recovered=valid_candidate(face_recovery);
      if(!recovered.accepted)recovered=valid_candidate(
          try_recover_literal_edge_by_four_to_four(
              result.constraints,edge,result.tetrahedra));
      if(!recovered.accepted)recovered=valid_candidate(endpoint_cone_retriangulation(
          result.constraints,edge,result.tetrahedra,recovered.cavity_tetrahedra));
      if(!recovered.accepted)recovered=valid_candidate(expanded_endpoint_cone_retriangulation(
          result.constraints,edge,result.tetrahedra,recovered.cavity_tetrahedra));
      if(!recovered.accepted)recovered=valid_candidate(segment_kernel_cone_retriangulation(
          result.constraints,edge,result.tetrahedra,recovered.cavity_tetrahedra));
      if(!recovered.accepted)recovered=valid_candidate(bounded_cavity_retriangulation(
          result.constraints,edge,result.tetrahedra,recovered.cavity_tetrahedra));
      if(!recovered.accepted&&
         result.fhc_steiner_insertions<options.maximum_fhc_steiner_insertions&&
         result.constraints.vertices.size()<options.maximum_vertices) {
        struct FhcCandidate {
          Vec3 position;
          std::vector<Tet> forced_cavity;
        };
        std::vector<FhcCandidate> fhc_candidates;
        std::set<PoststallIntersectionKey> excluded;
        const auto edge_hit=find_constraint_mesh_edge_intersection(
            result.constraints,{edge},result.tetrahedra,excluded);
        std::optional<CanonicalLiteralEdgeFlipResult> cascade_insertion;
        if(edge_hit) {
          result.last_fhc_had_blocking_edge=true;
          ++result.fhc_cascade_configurations;
          ++result.fhc_steiner_attempts;
          ++result.last_fhc_candidates_generated;
          ++result.last_fhc_candidates_attempted;
          cascade_insertion=insert_cascade_fhc_vertex(
              result.constraints,edge,edge_hit->mesh_edge,result.tetrahedra,
              options.maximum_fhc_steiner_attempts_per_segment);
          result.last_fhc_insertion_failure=cascade_insertion->failure;
          result.last_fhc_forced_cavity_cells=
              cascade_insertion->cavity_tetrahedra.size();
          if(!cascade_insertion->accepted)
            ++result.last_fhc_forced_cavity_rejections;
        }
        const auto face_hit=find_constraint_edge_mesh_face_intersection(
            result.constraints,{edge},result.tetrahedra,excluded);
        if(face_hit&&locked_edge&&
           std::find(face_hit->mesh_face.begin(),face_hit->mesh_face.end(),
                     (*locked_edge)[0])!=face_hit->mesh_face.end()&&
           std::find(face_hit->mesh_face.begin(),face_hit->mesh_face.end(),
                     (*locked_edge)[1])!=face_hit->mesh_face.end()) {
          result.last_fhc_had_blocking_face=true;
          ++result.fhc_locked_configurations;
          std::vector<Tet> incident_face_cells;
          for(const auto& cell:result.tetrahedra)
            if(std::all_of(face_hit->mesh_face.begin(),face_hit->mesh_face.end(),
                [&](const auto vertex){return std::find(cell.begin(),cell.end(),vertex)!=cell.end();}))
              incident_face_cells.push_back(cell);
          const auto locked=insert_locked_fhc_vertex(
              result.constraints,edge,*locked_edge,result.tetrahedra);
          if(locked.inserted_steiner_vertex)
            fhc_candidates.push_back(
                {*locked.inserted_steiner_vertex,incident_face_cells});
        }
        std::size_t targeted_attempts{};
        result.last_fhc_candidates_generated+=fhc_candidates.size();
        bool targeted_mesh_changed=false;
        if(cascade_insertion&&cascade_insertion->accepted&&
           cascade_insertion->inserted_steiner_vertex) {
          std::uint64_t next{};
          for(const auto& vertex:result.constraints.vertices)
            next=std::max(next,vertex.id);
          if(next!=std::numeric_limits<std::uint64_t>::max()) {
            auto augmented=result.constraints;
            augmented.vertices.push_back(
                {next+1U,*cascade_insertion->inserted_steiner_vertex});
            auto local_recovery=four_to_four_in_mesh(
                augmented,edge,cascade_insertion->tetrahedra);
            result.last_fhc_obstructions_before=
                constraint_segment_obstruction_count(
                    result.constraints,edge,result.tetrahedra);
            result.last_fhc_obstructions_after=local_recovery.accepted?0U:
                constraint_segment_obstruction_count(
                    augmented,edge,cascade_insertion->tetrahedra);
            auto next_mesh=local_recovery.accepted?
                std::move(local_recovery.tetrahedra):
                std::move(cascade_insertion->tetrahedra);
            if(!preserves_recovered_segments(augmented,next_mesh)) {
              ++result.invalid_candidate_mesh_rejections;
            } else {
              result.constraints=std::move(augmented);
              result.tetrahedra=std::move(next_mesh);
              ++result.fhc_steiner_insertions;
              if(local_recovery.accepted)++result.accepted_edge_recoveries;
              targeted_mesh_changed=true;
            }
          }
        }
        for(const auto& candidate:fhc_candidates) {
          if(targeted_mesh_changed)break;
          if(targeted_attempts++==options.maximum_fhc_steiner_attempts_per_segment)break;
          if(result.fhc_steiner_insertions==options.maximum_fhc_steiner_insertions)break;
          ++result.fhc_steiner_attempts;
          ++result.last_fhc_candidates_attempted;
          std::uint64_t next{};for(const auto& vertex:result.constraints.vertices)next=std::max(next,vertex.id);
          if(next==std::numeric_limits<std::uint64_t>::max())break;
          auto augmented=result.constraints;
          augmented.vertices.push_back({next+1U,candidate.position});
          const auto inserted=forced_cavity_insert_constraint_vertex(
              augmented,result.constraints.vertices.size(),result.tetrahedra,
              candidate.forced_cavity);
          result.last_fhc_insertion_failure=inserted.failure;
          result.last_fhc_forced_cavity_cells=candidate.forced_cavity.size();
          if(!inserted.accepted){++result.last_fhc_forced_cavity_rejections;continue;}
          auto local_recovery=four_to_four_in_mesh(augmented,edge,inserted.tetrahedra);
          const auto obstructions_before=constraint_segment_obstruction_count(
              result.constraints,edge,result.tetrahedra);
          const auto obstructions_after=local_recovery.accepted?0U:
              constraint_segment_obstruction_count(augmented,edge,inserted.tetrahedra);
          result.last_fhc_obstructions_before=obstructions_before;
          result.last_fhc_obstructions_after=obstructions_after;
          // This precisely classified Locked-FHC insertion removes the
          // reflex face. The next pass performs the flips it unlocks; the raw
          // number of crossed simplices need not decrease immediately.
          auto next_mesh=local_recovery.accepted?
              std::move(local_recovery.tetrahedra):inserted.tetrahedra;
          if(!preserves_recovered_segments(augmented,next_mesh)) {
            ++result.invalid_candidate_mesh_rejections;
            continue;
          }
          result.constraints=std::move(augmented);
          result.tetrahedra=std::move(next_mesh);
          ++result.fhc_steiner_insertions;
          if(local_recovery.accepted)++result.accepted_edge_recoveries;
          targeted_mesh_changed=true;
          break;
        }
        if(!targeted_mesh_changed&&!recovered.cavity_tetrahedra.empty()&&
           recovered.cavity_tetrahedra.size()<=8U) {
          ++result.fhc_generic_cavity_configurations;
          const auto generic=recover_segment_with_fhc_steiner(
              result.constraints,edge,result.tetrahedra,
              recovered.cavity_tetrahedra,
              options.maximum_fhc_steiner_attempts_per_segment);
          result.fhc_steiner_attempts+=
              options.maximum_fhc_steiner_attempts_per_segment;
          if(generic.accepted&&generic.inserted_steiner_vertex) {
            std::uint64_t next{};
            for(const auto& vertex:result.constraints.vertices)
              next=std::max(next,vertex.id);
            if(next!=std::numeric_limits<std::uint64_t>::max()) {
              auto augmented=result.constraints;
              augmented.vertices.push_back(
                  {next+1U,*generic.inserted_steiner_vertex});
              const auto before=constraint_segment_obstruction_count(
                  result.constraints,edge,result.tetrahedra);
              const auto after=constraint_segment_obstruction_count(
                  augmented,edge,generic.tetrahedra);
              if(after<before&&constraint_mesh_mutation_is_valid(
                     augmented,result.tetrahedra,generic.tetrahedra)&&
                 preserves_recovered_segments(augmented,generic.tetrahedra)) {
                result.constraints=std::move(augmented);
                result.tetrahedra=generic.tetrahedra;
                ++result.fhc_steiner_insertions;
                targeted_mesh_changed=true;
              } else ++result.invalid_candidate_mesh_rejections;
            }
          }
        }
        // Retry the segment only after committing a different mesh.  A
        // rejected candidate leaves every input unchanged; retrying here used
        // to spin forever without consuming either insertion or split budget.
        if(targeted_mesh_changed)continue;
      }
      if(recovered.accepted&&!recovered.recovers_by_constraint_split) {
        if(recovered.inserted_steiner_vertex){std::uint64_t next{};for(const auto& vertex:result.constraints.vertices)next=std::max(next,vertex.id);if(next==std::numeric_limits<std::uint64_t>::max()){result.constraints={};result.tetrahedra={};result.failure=CanonicalPlcRecoveryFailure::resource_limit;return false;}result.constraints.vertices.push_back({next+1U,*recovered.inserted_steiner_vertex});}
        result.tetrahedra=std::move(recovered.tetrahedra);++result.edge_flips;
        result.mesh_edge_removals+=generalized_face.edge_removals;
        ++result.accepted_edge_recoveries;continue;
      }
      // Boundary insertion is temporary and journaled. A midpoint is always
      // interior to the missing segment, minimizes rational-denominator
      // growth, and is located into the retained tetrahedralization by the
      // stellar insertion below. Quantized face-intersection ratios compound
      // by 64 per generation and can overflow exact parent provenance.
      std::optional<std::pair<std::uint32_t,std::uint32_t>> split_ratio=
          std::pair<std::uint32_t,std::uint32_t>{1U,2U};
      if(!split_ratio)split_ratio=endpoint_star_split_ratio(result.constraints,edge,result.tetrahedra);
      result.first_unrecovered_edge=edge;result.first_unrecovered_edge_is_core=is_core_edge(edge);result.last_edge_failure=recovered.failure;result.last_cavity_cell_count=recovered.cavity_tetrahedra.size();result.last_retriangulation_trials=recovered.retriangulation_trials;result.last_cavity_touches_frozen_facet=recovered.touches_frozen_facet;
      // Keep the private checkpoint for reproducible diagnostics.  Callers
      // may publish cells only when accepted(), so clearing it adds no safety
      // and previously made the first paper-stage refusal impossible to
      // inspect.
      const auto failed=[&](CanonicalPlcRecoveryFailure failure){result.failure=failure;return false;};
      const auto split_failure=result.first_unrecovered_edge_is_core?CanonicalPlcRecoveryFailure::core_refinement_required:CanonicalPlcRecoveryFailure::segment_recovery_failed;
      if(!split_ratio)return failed(split_failure);
      if(result.edge_splits==options.maximum_edge_splits)return failed(CanonicalPlcRecoveryFailure::resource_limit);
      result.last_split_numerator=split_ratio->first;result.last_split_denominator=split_ratio->second;
      auto split=split_canonical_plc_constraint_edge_at_ratio(result.constraints,edge,split_ratio->first,split_ratio->second,options.maximum_vertices,options.maximum_facets);
      // Deep recovery chains can make an otherwise useful intersection ratio
      // exceed the compact 64-bit boundary-provenance representation.  Do
      // not approximate it.  A constrained segment may be split at any
      // interior point, so try exact Farey mediants of its existing endpoint
      // coordinates; these often remain compact where another dyadic split
      // would add yet another denominator bit.
      if(!split.accepted()&&
         split.failure==CanonicalPlcConstraintFailure::rational_overflow) {
        std::set<std::pair<std::uint32_t,std::uint32_t>> alternatives;
        alternatives.emplace(1U,2U);
        for(const auto& face:result.constraints.facets) {
          std::optional<std::size_t> first,second;
          for(std::size_t i=0U;i<3U;++i) {
            if(face.vertices[i]==edge[0])first=i;
            if(face.vertices[i]==edge[1])second=i;
          }
          if(!first||!second)continue;
          const auto left=face.corners[*first].denominator;
          const auto right=face.corners[*second].denominator;
          if(left>std::numeric_limits<std::uint32_t>::max()-right)continue;
          auto candidate_numerator=right;
          auto candidate_denominator=left+right;
          const auto divisor=greatest_common_divisor_64(
              candidate_numerator,candidate_denominator);
          candidate_numerator/=divisor;candidate_denominator/=divisor;
          if(candidate_numerator<=std::numeric_limits<std::uint32_t>::max()&&
             candidate_denominator<=std::numeric_limits<std::uint32_t>::max())
            alternatives.emplace(static_cast<std::uint32_t>(candidate_numerator),
                                 static_cast<std::uint32_t>(candidate_denominator));
        }
        alternatives.erase(*split_ratio);
        for(const auto alternative:alternatives) {
          auto exact=split_canonical_plc_constraint_edge_at_ratio(
              result.constraints,edge,alternative.first,alternative.second,
              options.maximum_vertices,options.maximum_facets);
          if(!exact.accepted())continue;
          split_ratio=alternative;
          result.last_split_numerator=alternative.first;
          result.last_split_denominator=alternative.second;
          split=std::move(exact);break;
        }
      }
      if(!split.accepted()) {
        result.last_constraint_split_failure=split.failure;
        const auto restored=restore_last_canonical_boundary_steiner_point(
            result.constraints,result.tetrahedra);
        ++result.early_boundary_restoration_attempts;
        result.last_early_boundary_restoration_failure=restored.failure;
        if(restored.accepted()&&restored.interior_points_inserted>0U) {
          result.constraints=restored.constraints;
          result.tetrahedra=restored.tetrahedra;
          result.early_boundary_restorations+=restored.restored_points;
          result.early_boundary_relocation_points+=
              restored.interior_points_inserted;
          continue;
        }
        return failed(split_failure);
      }
      const auto inserted=stellar_insert_constraint_vertex(split.constraints,result.constraints.vertices.size(),result.tetrahedra);
      if(!inserted.accepted){result.last_split_insertion_failure=inserted.failure;return failed(split_failure);}
      result.constraints=split.constraints;result.tetrahedra=inserted.tetrahedra;++result.edge_splits;++result.segment_intersection_splits;
    }
  };
  if(!recover_all_segments())return result;
  // Segment recovery establishes the two child subsegments, but subsequent
  // local flips may recreate their coarse parent edge. Reapply the exact
  // stellar edge split before recovering facets so no hanging boundary point
  // remains hidden behind an apparently present child patch.
  const auto repair_bypassed_constraint_edges=[&] {
    bool changed=false;
    for(const auto& split:result.constraints.split_vertices) {
      const auto repaired=restellarize_bypassed_constraint_edge(
          result.constraints,split,result.tetrahedra);
      if(!repaired.accepted)continue;
      result.tetrahedra=repaired.tetrahedra;changed=true;
    }
    return changed;
  };
  repair_bypassed_constraint_edges();
  result.inspection=inspect_canonical_plc_tetrahedra(result.constraints,result.tetrahedra);
  result.edges_recovered_before_facet_stage=result.inspection.missing_edges.empty();
  if(!result.edges_recovered_before_facet_stage){result.constraints={};result.tetrahedra={};result.failure=CanonicalPlcRecoveryFailure::segment_recovery_failed;return result;}
  // Primary face path: recover a complete triangular constraint in one
  // transaction by independently filling the two sides of its cut cavity.
  // Later face operations may disturb an earlier face only if that face was
  // internal to the removed cavity; the kernel explicitly refuses that case.
  for(;;) {
    if(repair_bypassed_constraint_edges())continue;
    result.inspection=inspect_canonical_plc_tetrahedra(result.constraints,result.tetrahedra);
    if(result.inspection.accepted()){result.failure=CanonicalPlcRecoveryFailure::none;return result;}
    if(!result.inspection.missing_edges.empty()){result.constraints={};result.tetrahedra={};result.failure=CanonicalPlcRecoveryFailure::segment_recovery_failed;return result;}
    if(result.inspection.missing_facets.empty())break;
    std::set<std::array<std::uint64_t,3>> mesh_faces;
    for(const auto& cell:result.tetrahedra)for(unsigned omit=0U;omit<4U;++omit) {
      std::array<std::uint64_t,3> face{};unsigned cursor{};
      for(unsigned i=0U;i<4U;++i)if(i!=omit)
        face[cursor++]=result.constraints.vertices[cell[i]].id;
      std::sort(face.begin(),face.end());mesh_faces.insert(face);
    }
    auto literal=std::find_if(
        result.constraints.facets.begin(),result.constraints.facets.end(),
        [&](const auto& facet) {
          auto face=facet.vertices;std::sort(face.begin(),face.end());
          return !mesh_faces.contains(face);
        });
    // A parent can still be nonconforming when every refined child face is
    // present on one side but the other side retains an overlapping coarser
    // face. The patch audit reports the parent identity in that case; run a
    // cavity transaction through any one of its current children.
    if(literal==result.constraints.facets.end()) {
      auto missing_parent=result.inspection.missing_facets.front();
      std::sort(missing_parent.begin(),missing_parent.end());
      literal=std::find_if(
          result.constraints.facets.begin(),result.constraints.facets.end(),
          [&](const auto& facet) {
            auto parent=facet.parent.vertex_ids;
            std::sort(parent.begin(),parent.end());
            return parent==missing_parent;
          });
    }
    if(literal==result.constraints.facets.end())break;
    ++result.two_sided_facet_attempts;
    const auto recovered=recover_literal_facet_by_two_sided_cavity(
        result.constraints,literal->vertices,result.tetrahedra,
        options.maximum_facet_cavity_vertices,
        options.maximum_facet_retriangulation_trials,
        options.maximum_facet_cavity_expansions);
    result.last_two_sided_intersected_tetrahedra=recovered.intersected_tetrahedra;
    result.last_two_sided_top_tetrahedra=recovered.top_tetrahedra;
    result.last_two_sided_bottom_tetrahedra=recovered.bottom_tetrahedra;
    result.last_two_sided_retriangulation_trials=recovered.retriangulation_trials;
    result.two_sided_cavity_expansions+=recovered.cavity_expansions;
    if(!recovered.accepted){result.last_two_sided_facet_failure=recovered.failure;break;}
    result.tetrahedra=recovered.tetrahedra;
    ++result.two_sided_facets_recovered;
  }
  // Legacy face recovery remains temporarily as a bounded comparison after
  // the segment gate. It will be replaced by two-sided cavity remeshing; it
  // must never run with a missing constrained edge.
  using StableEdge=std::array<std::uint64_t,2>;using StableFace=std::array<std::uint64_t,3>;
  using IntersectionKey=PoststallIntersectionKey;
  struct IntersectionCheckpoint {
    CanonicalPlcConstraintSet constraints;
    std::vector<Tet> tetrahedra;
    std::size_t missing_facets{};
    std::size_t ridge_attempts{};
    std::size_t ridge_insertions{};
    std::array<std::size_t,static_cast<std::size_t>(CanonicalAdvancingRidgeFailure::count)> ridge_refusals{};
    IntersectionKey key{};
  };
  std::optional<IntersectionCheckpoint> checkpoint;
  std::set<IntersectionKey> rejected_intersections;
  for(;;) {
    std::map<StableFace,unsigned> advancing_ridge_attempt_count;
    for(;;) {
      if(result.advancing_ridge_insertions==options.maximum_advancing_ridge_insertions)break;
      result.inspection=inspect_canonical_plc_tetrahedra(result.constraints,result.tetrahedra);
      if(result.inspection.accepted()){result.failure=CanonicalPlcRecoveryFailure::none;return result;}
      std::set<std::array<std::uint64_t,3>> mesh_faces,required_faces;
      for(const auto& cell:result.tetrahedra)for(unsigned omit=0U;omit<4U;++omit){std::array<std::uint64_t,3> face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=result.constraints.vertices[cell[i]].id;std::sort(face.begin(),face.end());mesh_faces.insert(face);}
      for(const auto& facet:result.constraints.facets){auto face=facet.vertices;std::sort(face.begin(),face.end());required_faces.insert(face);}
      std::map<StableEdge,std::vector<StableFace>> incident;
      for(const auto& face:required_faces)for(const auto pair:std::array<std::array<unsigned,2>,3>{{{{0U,1U}},{{1U,2U}},{{0U,2U}}}})incident[{face[pair[0]],face[pair[1]]}].push_back(face);
      std::vector<std::pair<StableFace,StableEdge>> front;
      for(const auto& target:required_faces)if(!mesh_faces.contains(target))for(const auto pair:std::array<std::array<unsigned,2>,3>{{{{0U,1U}},{{1U,2U}},{{0U,2U}}}}){const StableEdge ridge{{target[pair[0]],target[pair[1]]}};if(std::any_of(incident[ridge].begin(),incident[ridge].end(),[&](const auto& neighbour){return neighbour!=target&&mesh_faces.contains(neighbour);}))front.push_back({target,ridge});}
      std::sort(front.begin(),front.end());front.erase(std::unique(front.begin(),front.end()),front.end());bool advanced=false;
      for(const auto& [target,ridge]:front){if(advancing_ridge_attempt_count[target]>=3U)continue;++advancing_ridge_attempt_count[target];++result.advancing_ridge_attempts;const auto insertion=advancing_ridge_insert_facet(result.constraints,target,ridge,result.tetrahedra);if(!insertion.accepted){++result.advancing_ridge_refusals[static_cast<std::size_t>(insertion.advancing_ridge_failure)];continue;}result.tetrahedra=insertion.tetrahedra;++result.advancing_ridge_insertions;advanced=true;break;}
      if(!advanced)break;
    }
    result.inspection=inspect_canonical_plc_tetrahedra(result.constraints,result.tetrahedra);
    if(checkpoint) {
      if(result.inspection.accepted()||result.inspection.missing_facets.size()<checkpoint->missing_facets) {
        ++result.intersection_steiner_insertions;
        if(checkpoint->key[0]==0U)++result.intersection_edge_insertions;
        else if(checkpoint->key[0]==1U)++result.intersection_facet_insertions;
        else ++result.intersection_edge_edge_insertions;
      } else {
        result.constraints=std::move(checkpoint->constraints);
        result.tetrahedra=std::move(checkpoint->tetrahedra);
        result.advancing_ridge_attempts=checkpoint->ridge_attempts;
        result.advancing_ridge_insertions=checkpoint->ridge_insertions;
        result.advancing_ridge_refusals=checkpoint->ridge_refusals;
        rejected_intersections.insert(checkpoint->key);
        result.inspection=inspect_canonical_plc_tetrahedra(result.constraints,result.tetrahedra);
      }
      checkpoint.reset();
    }
    if(result.inspection.accepted()){result.failure=CanonicalPlcRecoveryFailure::none;return result;}
    if(result.intersection_steiner_insertions==options.maximum_intersection_steiner_insertions||
       result.intersection_steiner_attempts==options.maximum_intersection_steiner_attempts){
      if(checkpoint){result.constraints=std::move(checkpoint->constraints);result.tetrahedra=std::move(checkpoint->tetrahedra);result.advancing_ridge_attempts=checkpoint->ridge_attempts;result.advancing_ridge_insertions=checkpoint->ridge_insertions;result.advancing_ridge_refusals=checkpoint->ridge_refusals;result.inspection=inspect_canonical_plc_tetrahedra(result.constraints,result.tetrahedra);checkpoint.reset();}
      break;
    }
    std::optional<ConstraintMeshIntersection> edge_intersection;
    std::optional<MeshEdgeConstraintFacetIntersection> facet_intersection;
    std::optional<ConstraintMeshEdgeIntersection> edge_edge_intersection;
    const auto primary_budget=options.maximum_intersection_steiner_attempts/2U;
    if(result.intersection_steiner_attempts<primary_budget) {
      if(result.intersection_steiner_attempts%2U==0U)facet_intersection=find_mesh_edge_constraint_facet_intersection(result.constraints,result.inspection.missing_facets,result.tetrahedra,rejected_intersections);
      else edge_intersection=find_constraint_edge_mesh_face_intersection(result.constraints,result.inspection.missing_edges,result.tetrahedra,rejected_intersections);
    } else {
      edge_edge_intersection=find_constraint_mesh_edge_intersection(result.constraints,result.inspection.missing_edges,result.tetrahedra,rejected_intersections);
    }
    if(!facet_intersection&&!edge_intersection&&!edge_edge_intersection)facet_intersection=find_mesh_edge_constraint_facet_intersection(result.constraints,result.inspection.missing_facets,result.tetrahedra,rejected_intersections);
    if(!facet_intersection&&!edge_intersection&&!edge_edge_intersection)edge_intersection=find_constraint_edge_mesh_face_intersection(result.constraints,result.inspection.missing_edges,result.tetrahedra,rejected_intersections);
    if(!facet_intersection&&!edge_intersection&&!edge_edge_intersection)edge_edge_intersection=find_constraint_mesh_edge_intersection(result.constraints,result.inspection.missing_edges,result.tetrahedra,rejected_intersections);
    if(!facet_intersection&&!edge_intersection&&!edge_edge_intersection){
      if(checkpoint){result.constraints=std::move(checkpoint->constraints);result.tetrahedra=std::move(checkpoint->tetrahedra);result.advancing_ridge_attempts=checkpoint->ridge_attempts;result.advancing_ridge_insertions=checkpoint->ridge_insertions;result.advancing_ridge_refusals=checkpoint->ridge_refusals;result.inspection=inspect_canonical_plc_tetrahedra(result.constraints,result.tetrahedra);checkpoint.reset();}
      break;
    }
    ++result.intersection_steiner_attempts;
    const IntersectionKey key=edge_intersection
        ? IntersectionKey{{0U,edge_intersection->constraint_edge[0],edge_intersection->constraint_edge[1],
                           edge_intersection->numerator,edge_intersection->denominator,0U,0U,0U}}
        : facet_intersection ? IntersectionKey{{1U,facet_intersection->constraint_facet[0],facet_intersection->constraint_facet[1],
                           facet_intersection->constraint_facet[2],facet_intersection->barycentric[0],
                           facet_intersection->barycentric[1],facet_intersection->barycentric[2],
                           facet_intersection->denominator}}
        : IntersectionKey{{2U,edge_edge_intersection->constraint_edge[0],edge_edge_intersection->constraint_edge[1],
                           edge_edge_intersection->mesh_edge[0],edge_edge_intersection->mesh_edge[1],
                           edge_edge_intersection->numerator,edge_edge_intersection->denominator,0U}};
    checkpoint=IntersectionCheckpoint{result.constraints,result.tetrahedra,
        result.inspection.missing_facets.size(),result.advancing_ridge_attempts,
        result.advancing_ridge_insertions,result.advancing_ridge_refusals,key};
    const auto split=edge_intersection
        ? split_canonical_plc_constraint_edge_at_ratio(
            result.constraints,edge_intersection->constraint_edge,edge_intersection->numerator,
            edge_intersection->denominator,options.maximum_vertices,options.maximum_facets)
        : facet_intersection ? split_canonical_plc_constraint_facet(
            result.constraints,facet_intersection->constraint_facet,facet_intersection->barycentric,
            facet_intersection->denominator,options.maximum_vertices,options.maximum_facets)
        : split_canonical_plc_constraint_edge_at_ratio(
            result.constraints,edge_edge_intersection->constraint_edge,edge_edge_intersection->numerator,
            edge_edge_intersection->denominator,options.maximum_vertices,options.maximum_facets);
    if(!split.accepted()){result.last_constraint_split_failure=split.failure;rejected_intersections.insert(key);checkpoint.reset();continue;}
    const auto inserted=stellar_insert_constraint_vertex(
        split.constraints,result.constraints.vertices.size(),result.tetrahedra);
    if(!inserted.accepted){result.last_split_insertion_failure=inserted.failure;rejected_intersections.insert(key);checkpoint.reset();continue;}
    result.constraints=split.constraints;
    result.tetrahedra=inserted.tetrahedra;
  }
  // Describe the stalled front before any fallback mutates the constraints.
  // Components are derived only from PLC ridge adjacency, so the diagnostic
  // remains invariant under tetrahedron ordering and rigid transforms.
  {
    result.inspection=inspect_canonical_plc_tetrahedra(result.constraints,result.tetrahedra);
    std::set<StableFace> mesh_faces,required_faces,core_faces;
    for(const auto& cell:result.tetrahedra)for(unsigned omit=0U;omit<4U;++omit){StableFace face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=result.constraints.vertices[cell[i]].id;std::sort(face.begin(),face.end());mesh_faces.insert(face);}
    for(const auto& facet:result.constraints.facets){auto face=facet.vertices;std::sort(face.begin(),face.end());required_faces.insert(face);if(facet.core_interface)core_faces.insert(face);}
    std::map<StableEdge,std::vector<StableFace>> incident;
    for(const auto& face:required_faces)for(const auto pair:std::array<std::array<unsigned,2>,3>{{{{0U,1U}},{{1U,2U}},{{0U,2U}}}})incident[{face[pair[0]],face[pair[1]]}].push_back(face);
    std::set<StableFace> missing(result.inspection.missing_facets.begin(),result.inspection.missing_facets.end());
    for(const auto& face:missing){if(core_faces.contains(face))++result.stalled_core_facets;else ++result.stalled_outer_facets;bool has_front=false;for(const auto pair:std::array<std::array<unsigned,2>,3>{{{{0U,1U}},{{1U,2U}},{{0U,2U}}}}){const StableEdge edge{{face[pair[0]],face[pair[1]]}};has_front=has_front||std::any_of(incident[edge].begin(),incident[edge].end(),[&](const auto& neighbour){return neighbour!=face&&mesh_faces.contains(neighbour);});}if(!has_front)++result.advancing_ridge_front_unavailable_facets;}
    std::set<StableFace> visited;
    for(const auto& seed:required_faces)if(!visited.contains(seed)){
      std::vector<StableFace> pending{{seed}};visited.insert(seed);bool has_missing=false,has_recovered=false;
      while(!pending.empty()){const auto face=pending.back();pending.pop_back();has_missing=has_missing||missing.contains(face);has_recovered=has_recovered||mesh_faces.contains(face);for(const auto pair:std::array<std::array<unsigned,2>,3>{{{{0U,1U}},{{1U,2U}},{{0U,2U}}}}){const StableEdge edge{{face[pair[0]],face[pair[1]]}};for(const auto& neighbour:incident[edge])if(visited.insert(neighbour).second)pending.push_back(neighbour);}}
      if(has_missing){++result.stalled_constraint_components;if(!has_recovered)++result.stalled_components_without_recovered_seed;}
    }
  }
  for(;;){result.inspection=inspect_canonical_plc_tetrahedra(result.constraints,result.tetrahedra);if(result.inspection.accepted()){result.failure=CanonicalPlcRecoveryFailure::none;return result;}if(result.inspection.missing_edges.empty()){result.failure=CanonicalPlcRecoveryFailure::facet_recovery_required;return result;}
    const auto edge=result.inspection.missing_edges.front();
    ++result.attempted_edge_recoveries;
    result.last_steiner_attempts=0U;
    auto flipped=try_recover_literal_edge_by_four_to_four(result.constraints,edge,result.tetrahedra);
    if(!flipped.accepted){
      flipped=endpoint_cone_retriangulation(result.constraints,edge,result.tetrahedra,flipped.cavity_tetrahedra);
    }
    if(!flipped.accepted){
      flipped=segment_kernel_cone_retriangulation(result.constraints,edge,result.tetrahedra,flipped.cavity_tetrahedra);
      if(flipped.accepted&&flipped.recovers_by_constraint_split) {
        result.last_split_numerator=flipped.constraint_split_numerator;result.last_split_denominator=flipped.constraint_split_denominator;
        const auto split=split_canonical_plc_constraint_edge_at_ratio(result.constraints,edge,flipped.constraint_split_numerator,flipped.constraint_split_denominator,options.maximum_vertices,options.maximum_facets);
        if(!split.accepted()){result.last_constraint_split_failure=split.failure;result.constraints={};result.tetrahedra={};result.failure=CanonicalPlcRecoveryFailure::core_refinement_required;return result;}
        const auto split_position=split.constraints.vertices.back().position;
        if(!flipped.inserted_steiner_vertex||split_position.x!=flipped.inserted_steiner_vertex->x||split_position.y!=flipped.inserted_steiner_vertex->y||split_position.z!=flipped.inserted_steiner_vertex->z){result.constraints={};result.tetrahedra={};result.failure=CanonicalPlcRecoveryFailure::core_refinement_required;return result;}
        result.constraints=split.constraints;result.tetrahedra=flipped.tetrahedra;++result.edge_splits;continue;
      }
    }
    if(!flipped.accepted){
      flipped=bounded_cavity_retriangulation(result.constraints,edge,result.tetrahedra,flipped.cavity_tetrahedra);
      if(!flipped.accepted&&!flipped.cavity_tetrahedra.empty()&&result.constraints.vertices.size()<options.maximum_vertices){
        std::vector<Point> steiner_candidates;Point weighted{};long double weight{};
        for(const auto& cell:flipped.cavity_tetrahedra){
          const auto point=[&](std::uint32_t i){const auto p=result.constraints.vertices[i].position;return Point{p.x,p.y,p.z};};
          const auto volume=std::abs(orient(point(cell[0]),point(cell[1]),point(cell[2]),point(cell[3])));
          const auto centroid=(point(cell[0])+point(cell[1])+point(cell[2])+point(cell[3]))*.25L;
          steiner_candidates.push_back(centroid);weighted=weighted+centroid*volume;weight+=volume;
        }
        const auto cell_centroid_count=steiner_candidates.size();
        for(std::size_t a=0;a<cell_centroid_count;++a)for(std::size_t b=a+1;b<cell_centroid_count;++b)
          steiner_candidates.push_back((steiner_candidates[a]+steiner_candidates[b])*.5L);
        if(weight>0.)steiner_candidates.push_back(weighted*(1.L/weight));
        std::sort(steiner_candidates.begin(),steiner_candidates.end(),[](Point a,Point b){return std::tie(a.x,a.y,a.z)<std::tie(b.x,b.y,b.z);});
        steiner_candidates.erase(std::unique(steiner_candidates.begin(),steiner_candidates.end(),[](Point a,Point b){return a.x==b.x&&a.y==b.y&&a.z==b.z;}),steiner_candidates.end());
        for(std::size_t candidate_index=0U;candidate_index<std::min<std::size_t>(4U,steiner_candidates.size());++candidate_index){
          ++result.last_steiner_attempts;
          const auto attempt=bounded_cavity_retriangulation(result.constraints,edge,result.tetrahedra,flipped.cavity_tetrahedra,steiner_candidates[candidate_index]);
          if(attempt.accepted){flipped=attempt;break;}
        }
      }
    }
    if(!flipped.accepted) {
      result.first_unrecovered_edge=edge;
      result.first_unrecovered_edge_is_core=is_core_edge(edge);
      result.last_edge_failure=flipped.failure;
      result.last_cavity_cell_count=flipped.cavity_tetrahedra.size();
      result.last_retriangulation_trials=flipped.retriangulation_trials;
      result.last_cavity_touches_frozen_facet=flipped.touches_frozen_facet;
      if(!result.first_unrecovered_edge_is_core){result.constraints={};result.tetrahedra={};result.failure=CanonicalPlcRecoveryFailure::facet_recovery_required;return result;}
      if(result.edge_splits==options.maximum_edge_splits){result.constraints={};result.tetrahedra={};result.failure=CanonicalPlcRecoveryFailure::resource_limit;return result;}
      // A geometric core facet may be subdivided, provided the split has exact
      // provenance and the selected core is rebuilt from that same record.
      // Restarting the private seed is required: retaining cells from before a
      // new constrained vertex would silently create a nonconforming interface.
      const auto ratio=endpoint_star_split_ratio(result.constraints,edge,result.tetrahedra);
      if(!ratio){result.constraints={};result.tetrahedra={};result.failure=CanonicalPlcRecoveryFailure::core_refinement_required;return result;}
      result.last_split_numerator=ratio->first;result.last_split_denominator=ratio->second;
      const auto split=split_canonical_plc_constraint_edge_at_ratio(result.constraints,edge,ratio->first,ratio->second,options.maximum_vertices,options.maximum_facets);
      if(!split.accepted()){result.last_constraint_split_failure=split.failure;result.constraints={};result.tetrahedra={};result.failure=CanonicalPlcRecoveryFailure::core_refinement_required;return result;}
      const auto inserted=stellar_insert_constraint_vertex(split.constraints,result.constraints.vertices.size(),result.tetrahedra);
      if(!inserted.accepted){result.last_split_insertion_failure=inserted.failure;result.constraints={};result.tetrahedra={};result.failure=CanonicalPlcRecoveryFailure::core_refinement_required;return result;}
      result.constraints=split.constraints;result.tetrahedra=inserted.tetrahedra;++result.edge_splits;continue;
    }
    if(flipped.inserted_steiner_vertex){std::uint64_t next{};for(const auto& vertex:result.constraints.vertices)next=std::max(next,vertex.id);if(next==std::numeric_limits<std::uint64_t>::max()){result.constraints={};result.failure=CanonicalPlcRecoveryFailure::resource_limit;return result;}result.constraints.vertices.push_back({next+1U,*flipped.inserted_steiner_vertex});}result.tetrahedra=flipped.tetrahedra;++result.edge_flips;++result.accepted_edge_recoveries;
  }
}

CanonicalPlcRecoveryResult recover_canonical_plc_edges(
    const NonmatchingPlcManifestResult& manifest,
    const CanonicalPlcRecoveryOptions& options) {
  const auto initial=materialize_canonical_plc_constraints(manifest);
  if(!initial.accepted())return {};
  return recover_canonical_plc_constraints_impl(initial.constraints,options);
}

CanonicalPlcRecoveryResult recover_canonical_plc_edges(
    const CanonicalPlcConstraintSet& constraints,
    const CanonicalPlcRecoveryOptions& options) {
  if(!has_only_nondegenerate_constraint_facets(constraints))return {};
  return recover_canonical_plc_constraints_impl(constraints,options);
}

CanonicalPlcRecoveryResult recover_wang_constraints(
    const CanonicalPlcConstraintSet& initial,
    const CanonicalPlcRecoveryOptions& options) {
  // This entry deliberately exposes the independently built Delaunay state
  // while the owned scheduler is being connected.  It must never fall back to
  // either the author DT or the retired generic recovery transaction below.
  CanonicalPlcRecoveryResult result;
  result.constraints=initial;
  if(!has_only_nondegenerate_constraint_facets(result.constraints))
    return result;
  std::sort(result.constraints.vertices.begin(),result.constraints.vertices.end(),
            [](const auto& left,const auto& right){return left.id<right.id;});
  // The author scheduler discovers boundary edges in the supplied facet
  // order.  This ordering is mutable control state (not merely presentation),
  // so retain the materialized PLC order rather than replacing it with a
  // project-specific stable-ID sort.
  const auto original_vertex_count=result.constraints.vertices.size();
  if(options.use_enclosing_cage&&!result.constraints.vertices.empty()) {
    auto low=result.constraints.vertices.front().position;
    auto high=low;
    std::uint64_t next_id{};
    for(const auto& vertex:result.constraints.vertices) {
      low.x=std::min(low.x,vertex.position.x);low.y=std::min(low.y,vertex.position.y);
      low.z=std::min(low.z,vertex.position.z);high.x=std::max(high.x,vertex.position.x);
      high.y=std::max(high.y,vertex.position.y);high.z=std::max(high.z,vertex.position.z);
      next_id=std::max(next_id,vertex.id);
    }
    if(next_id>std::numeric_limits<std::uint64_t>::max()-8U||
       result.constraints.vertices.size()+8U>options.maximum_vertices) {
      result.resource_limit=WangRecoveryResourceLimit::initial_vertices;
      result.resource_limit_observed=result.constraints.vertices.size()+8U;
      result.resource_limit_configured=options.maximum_vertices;
      result.failure=CanonicalPlcRecoveryFailure::resource_limit;
      return result;
    }
    const auto centre=(low+high)/2.0;
    const auto half=(high-low)/2.0;
    low=centre-half*2.0;high=centre+half*2.0;
    for(const auto point:std::array<Vec3,8>{{
            {low.x,low.y,low.z},{high.x,low.y,low.z},{high.x,high.y,low.z},{low.x,high.y,low.z},
            {low.x,low.y,high.z},{high.x,low.y,high.z},{high.x,high.y,high.z},{low.x,high.y,high.z}}})
      result.constraints.vertices.push_back({++next_id,point});
  }
  CanonicalDelaunaySeedInput input;
  input.maximum_vertices=result.constraints.vertices.size();
  input.maximum_tetrahedra=options.maximum_tetrahedra;
  input.exact_affine_planes=result.constraints.exact_affine_planes;
  for(const auto& vertex:result.constraints.vertices) {
    input.vertices.push_back(vertex.position);
    input.stable_vertex_ids.push_back(vertex.id);
  }
  const auto seed_started=std::chrono::steady_clock::now();
  const auto seed=build_wang_reference_seed_for_recovery(
      input,original_vertex_count);
  result.seed_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-seed_started).count();
  result.seed_failure=seed.result.failure;
  result.seed_invalid_reason=seed.result.invalid_reason;
  if(!seed.result.accepted()||seed.stages.empty()) {
    result.constraints={};
    result.failure=CanonicalPlcRecoveryFailure::seed_failed;
    return result;
  }
  WangOrderedTetMesh mesh(result.constraints.vertices.size(),seed.stages.back());
  const auto segment_started=std::chrono::steady_clock::now();
  if(!mesh.set_point_incidence(seed.point_to_tetrahedron)||!mesh.audit().accepted()) {
    result.constraints={};
    result.failure=CanonicalPlcRecoveryFailure::seed_failed;
    return result;
  }
  // The reference DT retains its ghost-hull connectivity throughout both the
  // local-flip scheduler and addinnerSteiner_Edge.  Construct that owned
  // traversal representation before the scheduler, not as an FHC-only view.
  if(result.constraints.vertices.size()>
         static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    result.resource_limit=WangRecoveryResourceLimit::initial_vertices;
    result.resource_limit_observed=result.constraints.vertices.size()+1U;
    result.resource_limit_configured=options.maximum_vertices;
    result.failure=CanonicalPlcRecoveryFailure::resource_limit;
    return result;
  }
  // DT::DelaunayTetrahedralization creates `ghost` immediately after the
  // input surface nodes, before AddBox appends its eight vertices.  The seed
  // trace uses a compact input numbering (surface nodes followed by box
  // nodes), so transcribe it here into DT's physical node layout:
  //
  //   compact: [ surface ... ][ box ... ][ ghost ]
  //   source:  [ surface ... ][ ghost ][ box ... ]
  //
  // This is not presentation-only. `getP2T` stores physical `Elems.form`
  // indices, which `finddirection` subsequently reads as its starting tet.
  const auto scheduler_ghost=static_cast<std::uint32_t>(original_vertex_count);
  std::uint64_t scheduler_ghost_id{};
  for(const auto& vertex:result.constraints.vertices)
    scheduler_ghost_id=std::max(scheduler_ghost_id,vertex.id);
  if(scheduler_ghost_id==std::numeric_limits<std::uint64_t>::max()) {
    result.resource_limit=WangRecoveryResourceLimit::initial_vertices;
    result.resource_limit_observed=result.constraints.vertices.size()+1U;
    result.resource_limit_configured=options.maximum_vertices;
    result.failure=CanonicalPlcRecoveryFailure::resource_limit;
    return result;
  }
  result.constraints.vertices.insert(
      result.constraints.vertices.begin()+static_cast<std::ptrdiff_t>(scheduler_ghost),
      {scheduler_ghost_id+1U,{0,0,0}});
  // The recovery mesh is not a re-created finite seed plus an inferred hull.
  // DT continues from BndPntInst's sparse `Elems` allocation and its FIFO
  // `Evacancy`; those identities determine later flip/P2T state.  The seed
  // trace records the owned transcription of precisely that state.
  const auto compact_ghost=static_cast<std::uint32_t>(input.vertices.size());
  if(seed.ghost_vertex!=compact_ghost||seed.final_live_slots.empty()||
     seed.final_slot_count<seed.final_live_slots.size()||
     seed.point_to_tetrahedron.size()!=compact_ghost) {
    result.constraints.vertices.erase(
        result.constraints.vertices.begin()+static_cast<std::ptrdiff_t>(scheduler_ghost));
    result.failure=CanonicalPlcRecoveryFailure::seed_failed;
    return result;
  }
  const auto source_node=[&](std::uint32_t compact_node) {
    if(compact_node==compact_ghost)return scheduler_ghost;
    return compact_node>=scheduler_ghost?compact_node+1U:compact_node;
  };
  const auto source_cell=[&](WangOrderedTetMesh::Tet cell) {
    for(auto& node:cell)node=source_node(node);
    return cell;
  };
  std::vector<WangOrderedTetMesh::Tet> scheduler_cells;
  std::vector<std::pair<std::size_t,WangOrderedTetMesh::Tet>> scheduler_slots;
  scheduler_cells.reserve(seed.final_live_slots.size());
  scheduler_slots.reserve(seed.final_live_slots.size());
  for(const auto& slot:seed.final_live_slots) {
    const auto cell=source_cell(slot.cell);
    scheduler_cells.push_back(cell);
    scheduler_slots.push_back({slot.index,cell});
  }
  std::vector<WangOrderedTetMesh::Tet> scheduler_incidence(
      result.constraints.vertices.size());
  for(std::uint32_t compact_node=0;compact_node<compact_ghost;++compact_node)
    scheduler_incidence[source_node(compact_node)]=
        source_cell(seed.point_to_tetrahedron[compact_node]);
  const auto ghost_cell=std::find_if(scheduler_cells.begin(),scheduler_cells.end(),
      [&](const auto& cell) {
        return std::find(cell.begin(),cell.end(),scheduler_ghost)!=cell.end();
      });
  if(ghost_cell==scheduler_cells.end()) {
    result.constraints.vertices.erase(
        result.constraints.vertices.begin()+static_cast<std::ptrdiff_t>(scheduler_ghost));
    result.failure=CanonicalPlcRecoveryFailure::seed_failed;
    return result;
  }
  scheduler_incidence[scheduler_ghost]=*ghost_cell;
  mesh=WangOrderedTetMesh(result.constraints.vertices.size(),scheduler_cells,
                          static_cast<std::int32_t>(scheduler_ghost));
  std::vector<std::uint64_t> ordered_mesh_ids;
  ordered_mesh_ids.reserve(result.constraints.vertices.size());
  for(const auto& vertex:result.constraints.vertices)
    ordered_mesh_ids.push_back(vertex.id);
  if(!mesh.set_exact_affine_planes(std::move(ordered_mesh_ids),
                                   result.constraints.exact_affine_planes)) {
    result.constraints.vertices.erase(
        result.constraints.vertices.begin()+static_cast<std::ptrdiff_t>(scheduler_ghost));
    result.failure=CanonicalPlcRecoveryFailure::seed_failed;
    return result;
  }
  if(!mesh.set_source_slot_layout(scheduler_slots,seed.final_slot_count,
                                  seed.final_vacancy_slots)) {
    result.constraints.vertices.erase(
        result.constraints.vertices.begin()+static_cast<std::ptrdiff_t>(scheduler_ghost));
    result.failure=CanonicalPlcRecoveryFailure::seed_failed;
    return result;
  }
  if(!mesh.set_point_incidence(scheduler_incidence)||!mesh.audit().accepted()) {
    result.constraints.vertices.erase(
        result.constraints.vertices.begin()+static_cast<std::ptrdiff_t>(scheduler_ghost));
    result.failure=CanonicalPlcRecoveryFailure::seed_failed;
    return result;
  }
  // The ordered mesh validates reciprocal connectivity, but the source's
  // post-BW FHC relocation also assumes that its finite tetrahedron star
  // remains embedded.  This trace-only audit identifies the first scheduler
  // transaction that violates that geometric invariant on a real PLC.
  const auto trace_finite_embedding=[&](const char* stage) {
    if(std::getenv("WANG_OWNED_GEOMETRY_TRACE")==nullptr)return;
    std::map<Face,std::vector<std::uint32_t>> opposites;
    std::size_t semantic_cells{};
    const auto ghost=mesh.ghost_vertex();
    for(const auto& cell:mesh.cells()) {
      if(cell.deleted||(ghost>=0&&std::find(cell.vertices.begin(),
          cell.vertices.end(),static_cast<std::uint32_t>(ghost))!=
          cell.vertices.end()))continue;
      std::array<std::uint64_t,4> ids{};
      bool ids_valid=true;
      for(unsigned corner=0U;corner<4U;++corner) {
        if(cell.vertices[corner]>=result.constraints.vertices.size()) {
          ids_valid=false;break;
        }
        ids[corner]=result.constraints.vertices[cell.vertices[corner]].id;
      }
      if(ids_valid&&is_semantically_coplanar(
             ids,result.constraints.exact_affine_planes))++semantic_cells;
      for(unsigned omitted=0U;omitted<4U;++omitted) {
        Face face{};unsigned cursor{};
        for(unsigned corner=0U;corner<4U;++corner)
          if(corner!=omitted)face[cursor++]=cell.vertices[corner];
        opposites[face_key(face)].push_back(cell.vertices[omitted]);
      }
    }
    if(semantic_cells>0U)
      std::cerr<<"owned_semantic_invalid "<<stage<<" cells "
               <<semantic_cells<<" fhc "
               <<result.owned_segment_fhc_insertions<<'\n';
    for(const auto& [face,uses]:opposites) {
      if(uses.size()!=2U)continue;
      const auto& a=result.constraints.vertices[face[0]].position;
      const auto point=[&](std::uint32_t vertex) {
        const auto& p=result.constraints.vertices[vertex].position;
        return Point{p.x-a.x,p.y-a.y,p.z-a.z};
      };
      const auto normal=cross(point(face[1]),point(face[2]));
      const auto left=dot(normal,point(uses[0]));
      const auto right=dot(normal,point(uses[1]));
      if(left==0.0L||right==0.0L||(left>0.0L)==(right>0.0L)) {
        std::cerr<<"owned_geometry_invalid "<<stage<<" face "
                 <<face[0]<<' '<<face[1]<<' '<<face[2]<<" opposites "
                 <<uses[0]<<' '<<uses[1]<<" fhc "
                 <<result.owned_segment_fhc_insertions<<'\n';
        return;
      }
    }
  };
  const auto append_scheduler_attempts=[&](
      const WangOwnedSegmentSchedulerResult& source) {
    const auto first=result.segment_scheduler_attempt_trace.size();
    for(const auto& attempt:source.attempts) {
      const auto outcome=attempt.outcome==WangOwnedSchedulerAttemptOutcome::recovered?
          CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::recovered:
          CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::failed;
      result.segment_scheduler_attempt_trace.push_back(
          {attempt.edge,attempt.round,attempt.info_before,attempt.search_depth,
           attempt.full_search,attempt.steiner_mode,outcome});
    }
    return source.attempts.empty()?result.segment_scheduler_attempt_trace.size():
        first+source.attempts.size()-1U;
  };
  // Drive any live recoverEdge queue through the owned local/full-search and
  // FHC continuation without recreating the ordered ghost-hull mesh.  The
  // initial AutorecoverEdges pass and recoverFace's prerequisite-edge calls
  // deliberately share this operation.
  const auto drive_segment_scheduler=[&](WangOwnedSegmentSchedulerResult scheduler,
                                          bool requeue_mode_one_failure) {
  std::size_t scheduler_trace_begin=append_scheduler_attempts(scheduler);
  std::optional<CanonicalPlcRecoveryFailure> owned_fhc_terminal_failure;
  while((scheduler.stop==WangOwnedSegmentSchedulerStop::steiner_insertion_required||
         scheduler.stop==WangOwnedSegmentSchedulerStop::interior_vertex_obstruction)&&
        !scheduler.lost_edges.empty()) {
    if(scheduler.stop==WangOwnedSegmentSchedulerStop::interior_vertex_obstruction) {
      if(!scheduler.continuation) {
        owned_fhc_terminal_failure=
            CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
        break;
      }
      const auto retry_current_edge=[&]() {
        auto continuation=*scheduler.continuation;
        continuation.remaining_round.insert(continuation.remaining_round.begin(),
                                             scheduler.lost_edges.front());
        scheduler=resume_wang_segment_scheduler_after_fhc(
            result.constraints,mesh,continuation);
        scheduler_trace_begin=append_scheduler_attempts(scheduler);
      };
      result.owned_segment_obstructing_vertex=scheduler.obstructing_vertex;
      result.owned_segment_obstructing_edge=
          scheduler.surface_edges[scheduler.lost_edges.front()].vertices;
      if(const auto point=std::find_if(result.constraints.vertices.begin(),
          result.constraints.vertices.end(),[&](const auto& vertex) {
            return vertex.id==scheduler.obstructing_vertex;
          });point!=result.constraints.vertices.end()) {
        result.owned_segment_obstructing_vertex_position=point->position;
        result.owned_segment_obstructing_vertex_position_known=true;
      }
      result.owned_segment_obstructing_vertex_is_constraint_vertex=
          std::any_of(result.constraints.facets.begin(),result.constraints.facets.end(),
              [&](const auto& facet) {
                return std::find(facet.vertices.begin(),facet.vertices.end(),
                                 scheduler.obstructing_vertex)!=facet.vertices.end();
              });
      const auto disposable=std::find_if(
          result.constraints.interior_steiner_vertices.begin(),
          result.constraints.interior_steiner_vertices.end(),
          [&](const auto& vertex) {
            return vertex.id==scheduler.obstructing_vertex;
          });
      // `removePnt` is legal only for a generated, non-boundary node.  The
      // source walk must not turn a ghost or an original PLC vertex into a
      // negative-point split candidate merely because it was selected by an
      // incomplete traversal transcription.
      if(disposable==result.constraints.interior_steiner_vertices.end()) {
        owned_fhc_terminal_failure=
            CanonicalPlcRecoveryFailure::owned_segment_original_vertex_promotion_required;
        break;
      }
      // This is the source's `Across Vertex` order in recoverEdgebyFlip:
      // removePnt, then disturbPnt only when removal failed, and only then
      // splitBndEdge(edge,-point).  The removal helper supplies the same
      // directional, short-edge-first admissibility decision; the ordered
      // mesh retains DT-like physical slots, so commit it without compacting
      // all later vertex indices.
      std::vector<Tet> finite_cells;
      const auto ghost=mesh.ghost_vertex();
      for(const auto& cell:mesh.cells())
        if(!cell.deleted&&(ghost<0||std::find(cell.vertices.begin(),cell.vertices.end(),
                                              static_cast<std::uint32_t>(ghost))==
                                      cell.vertices.end()))
          finite_cells.push_back(cell.vertices);
      ++result.owned_segment_remove_point_attempts;
      const auto removed=remove_canonical_interior_steiner_point(
          result.constraints,finite_cells,scheduler.obstructing_vertex,10U);
      bool ordered_removal_committed=false;
      const auto obstructing_index=std::find_if(
          result.constraints.vertices.begin(),result.constraints.vertices.end(),
          [&](const auto& vertex) { return vertex.id==scheduler.obstructing_vertex; });
      if(removed.removed()&&obstructing_index!=result.constraints.vertices.end()) {
        const auto vertex=static_cast<std::uint32_t>(
            obstructing_index-result.constraints.vertices.begin());
        if(removed.used_four_to_one) {
          ordered_removal_committed=mesh.remove_vertex_four_to_one(vertex);
        } else {
          const auto retained=std::find_if(
              result.constraints.vertices.begin(),result.constraints.vertices.end(),
              [&](const auto& candidate) {
                return candidate.id==removed.retained_vertex_id;
              });
          if(retained!=result.constraints.vertices.end())
            ordered_removal_committed=mesh.collapse_vertex_into(
                vertex,static_cast<std::uint32_t>(retained-
                                                   result.constraints.vertices.begin()));
        }
      }
      if(ordered_removal_committed) {
        result.constraints.interior_steiner_vertices.erase(
            std::remove_if(result.constraints.interior_steiner_vertices.begin(),
                           result.constraints.interior_steiner_vertices.end(),
                           [&](const auto& vertex) {
                             return vertex.id==scheduler.obstructing_vertex;
                           }),
            result.constraints.interior_steiner_vertices.end());
        ++result.owned_segment_remove_point_successes;
        retry_current_edge();
        continue;
      }

      ++result.owned_segment_disturbance_attempts;
      const auto disturbance=disturb_wang_owned_interior_vertex(
          result.constraints,scheduler.obstructing_vertex,mesh);
      if(disturbance.moved) {
        ++result.owned_segment_disturbance_successes;
        retry_current_edge();
        continue;
      }
      if(!options.allow_incomplete_obstruction_promotion) {
        owned_fhc_terminal_failure=
            CanonicalPlcRecoveryFailure::owned_segment_remove_or_disturb_point_required;
        break;
      }
      // This is the negative-point arm of DT::splitBndEdge.  Unlike an
      // ordinary edge split, the point already occupies a mesh slot, so no
      // BW insertion or mesh reindexing is permitted here.
      const auto parent=scheduler.surface_edges[scheduler.lost_edges.front()];
      std::vector<CanonicalPlcConstraintFacet> parent_facets;
      for(const auto& facet:result.constraints.facets)
        if(std::find(facet.vertices.begin(),facet.vertices.end(),parent.vertices[0])!=
               facet.vertices.end()&&
           std::find(facet.vertices.begin(),facet.vertices.end(),parent.vertices[1])!=
               facet.vertices.end())parent_facets.push_back(facet);
      const auto promoted=promote_canonical_interior_steiner_point_to_segment(
          result.constraints,parent.vertices,scheduler.obstructing_vertex,
          options.maximum_facets);
      result.owned_segment_obstruction_promotion_failure=promoted.failure;
      if(!promoted.accepted()) {
        owned_fhc_terminal_failure=
            CanonicalPlcRecoveryFailure::segment_recovery_failed;
        break;
      }
      result.constraints=promoted.constraints;
      ++result.edge_splits;
      ++result.segment_intersection_splits;
      auto continuation=*scheduler.continuation;
      continuation.surface_edges[scheduler.lost_edges.front()].info=1;
      std::unordered_map<std::uint64_t,std::uint32_t> index_for_id;
      for(std::size_t i=0;i<result.constraints.vertices.size();++i)
        index_for_id.emplace(result.constraints.vertices[i].id,
                             static_cast<std::uint32_t>(i));
      std::vector<std::size_t> children;
      const auto append_child=[&](std::array<std::uint64_t,2> directed) {
        const auto first=index_for_id.find(directed[0]);
        const auto second=index_for_id.find(directed[1]);
        if(first==index_for_id.end()||second==index_for_id.end())return false;
        children.push_back(continuation.surface_edges.size());
        continuation.surface_edges.push_back({directed,{{first->second,second->second}},0});
        return true;
      };
      const auto split_id=scheduler.obstructing_vertex;
      if(!append_child({{parent.vertices[0],split_id}})||
         !append_child({{parent.vertices[1],split_id}})) {
        owned_fhc_terminal_failure=
            CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
        break;
      }
      for(const auto& facet:parent_facets) {
        const auto other=std::find_if(facet.vertices.begin(),facet.vertices.end(),
            [&](const auto vertex) {
              return vertex!=parent.vertices[0]&&vertex!=parent.vertices[1];
            });
        if(other==facet.vertices.end()||!append_child({{*other,split_id}})) {
          owned_fhc_terminal_failure=
              CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
          break;
        }
      }
      if(owned_fhc_terminal_failure)break;
      std::vector<std::array<std::uint64_t,2>> child_calls;
      std::vector<int> child_info;
      for(const auto child:children) {
        auto& child_edge=continuation.surface_edges[child];
        child_calls.push_back(child_edge.vertices);
        auto recovered=recover_wang_segment_by_local_flips(
            result.constraints,child_edge.vertices,false,1000U,mesh).recovered;
        if(!recovered)recovered=recover_wang_segment_by_local_flips(
            result.constraints,child_edge.vertices,true,1000U,mesh).recovered;
        if(!recovered)recovered=recover_wang_segment_by_full_search(
            result.constraints,child_edge.vertices,1000U,mesh).recovered;
        result.segment_scheduler_attempt_trace.push_back(
            {child_edge.vertices,continuation.round,-3,1000U,true,0U,
             recovered?CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::recovered:
                       CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::failed});
        child_edge.info=recovered?1:-3;
        if(!recovered)continuation.failed_earlier_this_round.push_back(child);
        child_info.push_back(child_edge.info);
      }
      result.segment_post_split_child_edge_calls.push_back(std::move(child_calls));
      result.segment_post_split_child_info.push_back(std::move(child_info));
      if(scheduler_trace_begin<result.segment_scheduler_attempt_trace.size())
        result.segment_scheduler_attempt_trace[scheduler_trace_begin].outcome=
            CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::split;
      scheduler=resume_wang_segment_scheduler_after_fhc(
          result.constraints,mesh,continuation);
      scheduler_trace_begin=append_scheduler_attempts(scheduler);
      continue;
    }
    // Algorithm 2 advances from the complete local-flip pass to the owned
    // addinnerSteiner_Edge mode. That routine performs the source-defined
    // face sequence, constrained BW commits, and the two directional retries
    // before the later boundary-split fallback is considered.
    if(result.owned_segment_fhc_insertions>=
       options.maximum_fhc_steiner_insertions) {
      result.resource_limit=WangRecoveryResourceLimit::segment_fhc_insertions;
      result.resource_limit_observed=result.owned_segment_fhc_insertions;
      result.resource_limit_configured=options.maximum_fhc_steiner_insertions;
      owned_fhc_terminal_failure=CanonicalPlcRecoveryFailure::resource_limit;
      break;
    }
    const auto fhc_vertex_begin=result.constraints.vertices.size();
    const auto edge=scheduler.surface_edges[scheduler.lost_edges.front()].vertices;
    const auto fhc=recover_wang_segment_with_interior_steiner_mode1(
        result.constraints,edge,mesh,
        options.maximum_fhc_steiner_attempts_per_segment,
        options.maximum_fhc_steiner_insertions-
            result.owned_segment_fhc_insertions);
    trace_finite_embedding("cascade-fhc");
    result.owned_segment_fhc_invoked=true;
    result.owned_segment_fhc_insertions+=fhc.inserted_points.size();
    result.owned_segment_fhc_recovered=fhc.recovered;
    result.owned_segment_fhc_failure_code=static_cast<std::uint8_t>(fhc.failure);
    result.owned_segment_fhc_walk_failure_code=
        static_cast<std::uint8_t>(fhc.walk_failure);
    result.owned_segment_fhc_walk_failure_step=fhc.walk_failure_step;
    // addinnerSteiner_Edge creates disposable interior vertices.  Keep their
    // creation order and FHC provenance for the later Algorithm 2 removal
    // phase; boundary split vertices have separate journal ownership.
    for(std::size_t vertex=fhc_vertex_begin;
        vertex<result.constraints.vertices.size();++vertex) {
      const auto& point=result.constraints.vertices[vertex];
      const auto locked=std::any_of(fhc.placements.begin(),fhc.placements.end(),
          [&](const auto& placement) {
            return placement.point.x==point.position.x&&
                placement.point.y==point.position.y&&
                placement.point.z==point.position.z;
          });
      result.constraints.interior_steiner_vertices.push_back(
          {point.id,locked?CanonicalInteriorSteinerKind::locked_fhc:
                           CanonicalInteriorSteinerKind::cascade_fhc});
    }
    if(fhc.failure==WangOwnedInteriorSteinerFailure::walk_failed&&
       fhc.walk_failure==
           WangOwnedFullSearchFailure::vertex_obstruction_requires_remove_point) {
      // addinnerSteiner_Edge re-enters recoverEdgebyFlip's Across-Vertex arm
      // here.  Reuse the driver-owned R5 transaction above instead of
      // converting this source walk result into an unsupported terminal.
      if(!scheduler.continuation) {
        owned_fhc_terminal_failure=
            CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
      } else {
        scheduler.stop=WangOwnedSegmentSchedulerStop::interior_vertex_obstruction;
        scheduler.obstructing_vertex=fhc.obstructing_vertex;
        continue;
      }
    }
    else if(fhc.failure==WangOwnedInteriorSteinerFailure::walk_failed&&
            fhc.walk_failure==WangOwnedFullSearchFailure::unsupported_edge_contact)
      owned_fhc_terminal_failure=
          CanonicalPlcRecoveryFailure::owned_segment_contact_unsupported;
    else if(fhc.failure==WangOwnedInteriorSteinerFailure::walk_failed&&
            fhc.walk_failure==WangOwnedFullSearchFailure::walk_failed)
      owned_fhc_terminal_failure=
          CanonicalPlcRecoveryFailure::owned_segment_full_search_walk_required;
    else if(fhc.failure==WangOwnedInteriorSteinerFailure::resource_limit) {
      result.resource_limit=WangRecoveryResourceLimit::segment_fhc_insertions;
      result.resource_limit_observed=result.owned_segment_fhc_insertions;
      result.resource_limit_configured=options.maximum_fhc_steiner_insertions;
      owned_fhc_terminal_failure=CanonicalPlcRecoveryFailure::resource_limit;
    }
    else if(fhc.failure==WangOwnedInteriorSteinerFailure::edge_feature_not_implemented)
      owned_fhc_terminal_failure=
          CanonicalPlcRecoveryFailure::owned_segment_cascade_fhc_required;
    else if(fhc.failure!=WangOwnedInteriorSteinerFailure::none)
      owned_fhc_terminal_failure=
          CanonicalPlcRecoveryFailure::owned_segment_fhc_failed;
    else if(!fhc.recovered&&scheduler.pending_steiner_mode==1U) {
      // recoverEdge(..., info=1) returns zero after addinnerSteiner_Edge.
      // AutorecoverEdges puts this edge back into the live queue; only the
      // later info=2 invocation may continue into splitBndEdge.
      if(!requeue_mode_one_failure) {
        scheduler.stop=WangOwnedSegmentSchedulerStop::complete;
        break;
      }
      if(!scheduler.continuation) {
        owned_fhc_terminal_failure=
            CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
        break;
      }
      auto continuation=*scheduler.continuation;
      continuation.failed_earlier_this_round.push_back(
          scheduler.lost_edges.front());
      scheduler=resume_wang_segment_scheduler_after_fhc(
          result.constraints,mesh,continuation);
      if(std::getenv("WANG_OWNED_GEOMETRY_TRACE")!=nullptr&&
         !scheduler.attempts.empty()) {
        const auto& attempt=scheduler.attempts.back();
        std::cerr<<"owned_scheduler_resume edge "<<attempt.edge[0]<<' '
                 <<attempt.edge[1]<<" round "<<attempt.round<<'\n';
      }
      trace_finite_embedding("post-fhc-scheduler");
      scheduler_trace_begin=append_scheduler_attempts(scheduler);
      continue;
    }
    else if(!fhc.recovered&&scheduler.pending_steiner_mode==2U) {
      // DT::recoverEdge(..., info=2) calls addinnerSteiner_Edge first and
      // enters splitBndEdge only when that exact retry still returns zero.
      // Snapshot the parent facets before their replacement: their order is
      // the order used by splitBndEdge to append radial [p3,newp] edges.
      if(!scheduler.continuation) {
        owned_fhc_terminal_failure=
            CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
        break;
      }
      if(result.edge_splits==options.maximum_edge_splits) {
        result.resource_limit=WangRecoveryResourceLimit::segment_boundary_splits;
        result.resource_limit_observed=result.edge_splits;
        result.resource_limit_configured=options.maximum_edge_splits;
        owned_fhc_terminal_failure=CanonicalPlcRecoveryFailure::resource_limit;
        break;
      }
      if(result.constraints.vertices.size()>=options.maximum_vertices) {
        result.resource_limit=WangRecoveryResourceLimit::segment_boundary_splits;
        result.resource_limit_observed=result.constraints.vertices.size();
        result.resource_limit_configured=options.maximum_vertices;
        owned_fhc_terminal_failure=CanonicalPlcRecoveryFailure::resource_limit;
        break;
      }
      const auto parent=scheduler.surface_edges[scheduler.lost_edges.front()];
      std::vector<CanonicalPlcConstraintFacet> parent_facets;
      for(const auto& facet:result.constraints.facets) {
        if(std::find(facet.vertices.begin(),facet.vertices.end(),parent.vertices[0])!=
               facet.vertices.end()&&
           std::find(facet.vertices.begin(),facet.vertices.end(),parent.vertices[1])!=
               facet.vertices.end())
          parent_facets.push_back(facet);
      }
      std::vector<std::array<std::uint32_t,4>> finite_cells;
      for(const auto& cell:mesh.cells())
        if(!cell.deleted&&std::find(cell.vertices.begin(),cell.vertices.end(),
            static_cast<std::uint32_t>(mesh.ghost_vertex()))==cell.vertices.end())
          finite_cells.push_back(cell.vertices);
      const auto inserted=insert_wang_segment_boundary_steiner_point(
          result.constraints,parent.vertices,finite_cells,options.maximum_vertices,
          options.maximum_facets,&mesh);
      result.last_wang_segment_boundary_failure=inserted.failure;
      result.last_constraint_split_failure=inserted.constraint_failure;
      result.last_split_insertion_failure=inserted.insertion_failure;
      result.last_split_numerator=inserted.split_numerator;
      result.last_split_denominator=inserted.split_denominator;
      if(!inserted.accepted()) {
        owned_fhc_terminal_failure=
            CanonicalPlcRecoveryFailure::segment_recovery_failed;
        break;
      }
      const auto committed=mesh.replace_cavity_with_appended_vertex(
          inserted.ordered_cavity_tetrahedra,inserted.replacement_tetrahedra,
          inserted.constraints.vertices.back().id,
          inserted.constraints.exact_affine_planes);
      if(!committed.accepted) {
        owned_fhc_terminal_failure=
            CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
        break;
      }
      result.constraints=inserted.constraints;
      ++result.edge_splits;
      ++result.segment_intersection_splits;

      // splitBndEdge retires the parent and appends its two halves, followed
      // by one radial edge per old incident facet.  Do not canonicalize these
      // directed records: the order is consumed immediately by recoverEdge.
      auto continuation=*scheduler.continuation;
      continuation.surface_edges[scheduler.lost_edges.front()].info=1;
      const auto split_id=result.constraints.vertices.back().id;
      std::unordered_map<std::uint64_t,std::uint32_t> index_for_id;
      for(std::size_t i=0;i<result.constraints.vertices.size();++i)
        index_for_id.emplace(result.constraints.vertices[i].id,
                             static_cast<std::uint32_t>(i));
      std::vector<std::size_t> children;
      const auto append_child=[&](std::array<std::uint64_t,2> directed) {
        const auto first=index_for_id.find(directed[0]);
        const auto second=index_for_id.find(directed[1]);
        if(first==index_for_id.end()||second==index_for_id.end())return false;
        children.push_back(continuation.surface_edges.size());
        continuation.surface_edges.push_back(
            {directed,{{first->second,second->second}},0});
        return true;
      };
      if(!append_child({{parent.vertices[0],split_id}})||
         !append_child({{parent.vertices[1],split_id}})) {
        owned_fhc_terminal_failure=
            CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
        break;
      }
      for(const auto& facet:parent_facets) {
        const auto other=std::find_if(facet.vertices.begin(),facet.vertices.end(),
            [&](const auto vertex) {
              return vertex!=parent.vertices[0]&&vertex!=parent.vertices[1];
            });
        if(other==facet.vertices.end()||!append_child({{*other,split_id}})) {
          owned_fhc_terminal_failure=
              CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
          break;
        }
      }
      if(owned_fhc_terminal_failure)break;
      std::vector<std::array<std::uint64_t,2>> child_calls;
      for(const auto child:children)
        child_calls.push_back(continuation.surface_edges[child].vertices);
      result.segment_post_split_child_edge_calls.push_back(child_calls);
      std::vector<int> child_info;

      // AutorecoverEdges calls recoverEdge(child, 1, 0) across the newly
      // appended range.  Thus each child gets forward, reverse, and full
      // search, but never a nested FHC/split in this immediate pass.
      for(const auto child:children) {
        auto& child_edge=continuation.surface_edges[child];
        // Incidental adjacency is not a successful recoverEdge call.  The
        // source makes the directional flip transactions decide this.
        auto recovered=recover_wang_segment_by_local_flips(
            result.constraints,child_edge.vertices,false,1000U,mesh).recovered;
        if(!recovered)recovered=recover_wang_segment_by_local_flips(
            result.constraints,child_edge.vertices,true,1000U,mesh).recovered;
        if(!recovered) {
          const auto full=recover_wang_segment_by_full_search(
              result.constraints,child_edge.vertices,1000U,mesh);
          recovered=full.failure==WangOwnedFullSearchFailure::none&&full.recovered;
        }
        result.segment_scheduler_attempt_trace.push_back(
            {child_edge.vertices,continuation.round,-3,1000U,true,0U,
             recovered?CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::recovered:
                       CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::failed});
        if(recovered)child_edge.info=1;
        else {
          child_edge.info=-3;
          continuation.failed_earlier_this_round.push_back(child);
        }
        child_info.push_back(child_edge.info);
      }
      result.segment_post_split_child_info.push_back(std::move(child_info));
      if(scheduler_trace_begin<result.segment_scheduler_attempt_trace.size())
        result.segment_scheduler_attempt_trace[scheduler_trace_begin].outcome=
            CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::split;
      scheduler=resume_wang_segment_scheduler_after_fhc(
          result.constraints,mesh,continuation);
      scheduler_trace_begin=append_scheduler_attempts(scheduler);
      continue;
    }
    else if(!fhc.recovered)
      owned_fhc_terminal_failure=
          CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
    else if(!scheduler.continuation)
      owned_fhc_terminal_failure=
          CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
    else {
      // FHC succeeded for the popped edge.  Continue the still-live source
      // queue at its next entry, retaining failed earlier entries until the
      // current round finishes and updateFliptype can run.
      scheduler=resume_wang_segment_scheduler_after_fhc(
          result.constraints,mesh,*scheduler.continuation);
      if(std::getenv("WANG_OWNED_GEOMETRY_TRACE")!=nullptr&&
         !scheduler.attempts.empty()) {
        const auto& attempt=scheduler.attempts.back();
        std::cerr<<"owned_scheduler_resume edge "<<attempt.edge[0]<<' '
                 <<attempt.edge[1]<<" round "<<attempt.round<<'\n';
      }
      trace_finite_embedding("post-fhc-scheduler");
      if(scheduler_trace_begin<result.segment_scheduler_attempt_trace.size())
        result.segment_scheduler_attempt_trace[scheduler_trace_begin].outcome=
            CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::recovered;
      scheduler_trace_begin=append_scheduler_attempts(scheduler);
      continue;
    }
    break;
  }
  return std::pair{std::move(scheduler),owned_fhc_terminal_failure};
  };
  auto scheduler=run_wang_segment_scheduler_pre_steiner(
      result.constraints,mesh);
  trace_finite_embedding("pre-steiner");
  result.owned_segment_scheduler_invoked=true;
  auto driven_scheduler=drive_segment_scheduler(std::move(scheduler),true);
  scheduler=std::move(driven_scheduler.first);
  auto owned_fhc_terminal_failure=driven_scheduler.second;
  const auto mesh_has_stable_edge=[&](std::array<std::uint64_t,2> edge) {
    std::array<std::uint32_t,2> indices{};
    for(unsigned endpoint=0U;endpoint<2U;++endpoint) {
      const auto found=std::find_if(result.constraints.vertices.begin(),
          result.constraints.vertices.end(),[&](const auto& vertex) {
            return vertex.id==edge[endpoint];
          });
      if(found==result.constraints.vertices.end())return false;
      indices[endpoint]=static_cast<std::uint32_t>(
          found-result.constraints.vertices.begin());
    }
    return std::any_of(mesh.cells().begin(),mesh.cells().end(),
        [&](const auto& cell) {
          return !cell.deleted&&
              std::find(cell.vertices.begin(),cell.vertices.end(),indices[0])!=
                  cell.vertices.end()&&
              std::find(cell.vertices.begin(),cell.vertices.end(),indices[1])!=
                  cell.vertices.end();
        });
  };
  // Source-shaped recoverFace prerequisite: inspect the target's three
  // cyclic edges and re-enter recoverEdge against this same ordered mesh.
  // Newly split boundary edges update both the live surface-edge table and
  // the PLC facets before the caller resumes its lost-face queue.
  const auto recover_facet_prerequisite_edges=[&](
      const std::array<std::uint64_t,3>& facet,std::uint8_t info) {
    for(unsigned corner=0U;corner<3U;++corner) {
      const std::array<std::uint64_t,2> cyclic_edge{
          {facet[corner],facet[(corner+1U)%3U]}};
      const bool missing=!mesh_has_stable_edge(cyclic_edge);
      result.facet_prerequisite_edge_calls.push_back(
          {facet,cyclic_edge,missing});
      if(!missing)continue;

      const auto same_edge=[&](const WangOwnedSurfaceEdge& candidate) {
        auto left=candidate.vertices,right=cyclic_edge;
        std::sort(left.begin(),left.end());
        std::sort(right.begin(),right.end());
        return left==right;
      };
      auto found=std::find_if(scheduler.surface_edges.begin(),
                              scheduler.surface_edges.end(),same_edge);
      std::size_t edge_index{};
      if(found==scheduler.surface_edges.end()) {
        std::array<std::uint32_t,2> indices{};
        for(unsigned endpoint=0U;endpoint<2U;++endpoint) {
          const auto vertex=std::find_if(result.constraints.vertices.begin(),
              result.constraints.vertices.end(),[&](const auto& candidate) {
                return candidate.id==cyclic_edge[endpoint];
              });
          if(vertex==result.constraints.vertices.end())return false;
          indices[endpoint]=static_cast<std::uint32_t>(
              vertex-result.constraints.vertices.begin());
        }
        edge_index=scheduler.surface_edges.size();
        scheduler.surface_edges.push_back({cyclic_edge,indices,0});
      } else edge_index=static_cast<std::size_t>(
          found-scheduler.surface_edges.begin());

      auto& surface_edge=scheduler.surface_edges[edge_index];
      if(info==0U) {
        bool recovered=recover_wang_segment_by_local_flips(
            result.constraints,surface_edge.vertices,false,1000U,mesh).recovered;
        if(!recovered)recovered=recover_wang_segment_by_local_flips(
            result.constraints,surface_edge.vertices,true,1000U,mesh).recovered;
        if(!recovered) {
          const auto full=recover_wang_segment_by_full_search(
              result.constraints,surface_edge.vertices,1000U,mesh);
          recovered=full.failure==WangOwnedFullSearchFailure::none&&full.recovered;
        }
        result.segment_scheduler_attempt_trace.push_back(
            {surface_edge.vertices,1U,-3,1000U,true,0U,
             recovered?CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::recovered:
                       CanonicalPlcRecoveryResult::SegmentSchedulerOutcome::failed});
        surface_edge.info=recovered?1:-3;
        if(!recovered)return false;
        continue;
      }

      WangOwnedSegmentSchedulerState operation;
      operation.surface_edges=scheduler.surface_edges;
      operation.surface_edges[edge_index].info=info>=2U?-5:-4;
      operation.remaining_round.push_back(edge_index);
      operation.previous_lost_count={
          {operation.surface_edges[edge_index].vertices[0],1},
          {operation.surface_edges[edge_index].vertices[1],1}};
      operation.round=1U;
      auto edge_scheduler=resume_wang_segment_scheduler_after_fhc(
          result.constraints,mesh,operation);
      auto driven=drive_segment_scheduler(std::move(edge_scheduler),false);
      scheduler=std::move(driven.first);
      if(driven.second) {
        owned_fhc_terminal_failure=driven.second;
        return false;
      }
      if(!mesh_has_stable_edge(cyclic_edge))return false;
    }
    return true;
  };
  result.owned_segment_scheduler_round=scheduler.next_round;
  result.owned_segment_scheduler_steiner_mode=scheduler.pending_steiner_mode;
  const auto snapshot_segment_stage=[&]() {
    // The public snapshot excludes the private scheduler ghost exactly as the
    // final result does.  Capture it before recoverFacesPass begins: facet
    // flips and facet Steiner insertion are allowed to change the same
    // ordered mesh, so a later snapshot cannot serve as segment evidence.
    auto constraints=result.constraints;
    constraints.vertices.erase(constraints.vertices.begin()+scheduler_ghost);
    std::vector<std::array<std::uint32_t,4>> cells;
    for(const auto& cell:mesh.cells()) {
      if(cell.deleted||std::find(cell.vertices.begin(),cell.vertices.end(),
          static_cast<std::uint32_t>(mesh.ghost_vertex()))!=cell.vertices.end())
        continue;
      auto finite=cell.vertices;
      for(auto& vertex:finite)
        if(mesh.ghost_vertex()>=0&&vertex>
            static_cast<std::uint32_t>(mesh.ghost_vertex()))--vertex;
      cells.push_back(finite);
    }
    result.segment_stage_constraints=std::move(constraints);
    result.segment_stage_tetrahedra=std::move(cells);
    result.segment_recovery_milliseconds=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-segment_started).count();
  };
  // DT::recoverFacesPass begins while the enclosing hull is still present.
  // Keep this ordered state intact for the source-shaped flip-only facet arm;
  // stripping the ghost first would change the edge-ring traversal.
  const auto facet_started=std::chrono::steady_clock::now();
  if(!owned_fhc_terminal_failure&&scheduler.stop==WangOwnedSegmentSchedulerStop::complete) {
    snapshot_segment_stage();
    const auto finite_before_facet=[&]() {
      std::vector<std::array<std::uint32_t,4>> cells;
      for(const auto& cell:mesh.cells())
        if(!cell.deleted&&std::find(cell.vertices.begin(),cell.vertices.end(),
            static_cast<std::uint32_t>(mesh.ghost_vertex()))==cell.vertices.end())
          cells.push_back(cell.vertices);
      return cells;
    };
    const auto missing_facets_in_surface_order=[&]() {
      using GeometryPoint=std::array<double,3>;
      using GeometryFace=std::array<GeometryPoint,3>;
      using StableFace=std::array<std::uint64_t,3>;
      const auto geometry_point=[](const Vec3& point) {
        return GeometryPoint{{point.x,point.y,point.z}};
      };
      const auto finite=finite_before_facet();
      const auto inspection=inspect_canonical_plc_tetrahedra(
          result.constraints,finite);
      std::map<std::uint64_t,GeometryPoint> current_geometry;
      for(const auto& vertex:result.constraints.vertices)
        current_geometry.emplace(vertex.id,geometry_point(vertex.position));
      const auto face_geometry=[&](const StableFace& face,
                                   const auto& positions)
          -> std::optional<GeometryFace> {
        GeometryFace geometry{};
        for(unsigned corner=0U;corner<3U;++corner) {
          const auto found=positions.find(face[corner]);
          if(found==positions.end())return std::nullopt;
          geometry[corner]=found->second;
        }
        std::sort(geometry.begin(),geometry.end());
        return geometry;
      };
      // Validation identifies the actual unrecovered parent patches.  It is
      // deliberately keyed canonically, whereas recoverFacesPass consumes
      // SurTris in the input surface order.  Join those two views by literal
      // geometry: stable identities can differ after coincident-node remap,
      // but the author's observable queue still follows mesh.F order.
      std::multimap<GeometryFace,StableFace> missing_by_geometry;
      for(const auto& face:inspection.missing_facets) {
        const auto geometry=face_geometry(face,current_geometry);
        if(geometry)missing_by_geometry.emplace(*geometry,face);
      }
      std::map<std::uint64_t,GeometryPoint> initial_geometry;
      for(const auto& vertex:initial.vertices)
        initial_geometry.emplace(vertex.id,geometry_point(vertex.position));
      std::vector<StableFace> missing;
      missing.reserve(inspection.missing_facets.size());
      for(const auto& facet:initial.facets) {
        const auto geometry=face_geometry(facet.vertices,initial_geometry);
        if(!geometry)continue;
        const auto found=missing_by_geometry.find(*geometry);
        if(found==missing_by_geometry.end())continue;
        missing.push_back(found->second);
        missing_by_geometry.erase(found);
      }
      // A split facet may have no literal counterpart in the initial stream.
      // Preserve it rather than silently weakening the recovery obligation.
      for(const auto& [geometry,face]:missing_by_geometry) {
        (void)geometry;
        missing.push_back(face);
      }
      return missing;
    };
    // recoverFacesPass scans SurTris in its stored surface order.  The PLC
    // inspection report groups parents in a map and is deliberately
    // canonical for validation, so using its missing_facets list here changes
    // the author's mutation schedule even when the segment mesh is identical.
    std::vector<std::array<std::uint64_t,3>> pending=
        missing_facets_in_surface_order();
    const auto snapshot_facet_cells=[&]() {
      std::vector<std::array<std::uint64_t,4>> cells;
      for(const auto& cell:mesh.cells()) {
        if(cell.deleted||std::find(cell.vertices.begin(),cell.vertices.end(),
            static_cast<std::uint32_t>(mesh.ghost_vertex()))!=cell.vertices.end())
          continue;
        std::array<std::uint64_t,4> ids{};
        for(unsigned corner=0U;corner<4U;++corner)
          ids[corner]=result.constraints.vertices[cell.vertices[corner]].id;
        std::sort(ids.begin(),ids.end());
        cells.push_back(ids);
      }
      std::sort(cells.begin(),cells.end());
      return cells;
    };
    for(std::size_t round=0U;!pending.empty()&&round<=100U;++round) {
      const auto count=pending.size();
      std::size_t successes{};
      for(std::size_t item=0U;item<count;++item) {
        const auto key=pending.front();pending.erase(pending.begin());
        const auto facet=std::find_if(result.constraints.facets.begin(),
            result.constraints.facets.end(),[&](const auto& candidate) {
              auto candidate_key=candidate.vertices;std::sort(candidate_key.begin(),candidate_key.end());
              return candidate_key==key;
            });
        if(facet==result.constraints.facets.end()) {++successes;continue;}
        ++result.attempted_facet_recoveries;
        result.facet_recovery_attempt_trace.push_back({key,0U});
        const auto recovered=recover_wang_facet_by_flip_split(
            result.constraints,facet->vertices,0U,mesh);
        result.initial_facet_cells_after_attempt.push_back(
            snapshot_facet_cells());
        result.facet_local_edge_removal_attempts+=recovered.edge_removal_attempts;
        result.facet_local_edge_removals+=recovered.edge_removals;
        if(recovered.recovered)++successes;
        else pending.push_back(key);
      }
      if(successes==0U)break;
      // recoverFaces retains the failed entries in the same FIFO.  It does
      // not rebuild or canonically sort the queue after a successful round.
    }
    // recoverFacesPass makes one final level-1000 pass after a zero-success
    // flip-only round. recoverFace first retries recoverFacebyFlip_Split and
    // then grows its local intersecting-edge patch for recoverFacebyLocalFlips.
    if(!pending.empty()) {
      const auto count=pending.size();
      for(std::size_t item=0U;item<count;++item) {
        const auto key=pending.front();pending.erase(pending.begin());
        const auto facet=std::find_if(result.constraints.facets.begin(),
            result.constraints.facets.end(),[&](const auto& candidate) {
              auto candidate_key=candidate.vertices;
              std::sort(candidate_key.begin(),candidate_key.end());
              return candidate_key==key;
            });
        if(facet==result.constraints.facets.end())continue;
        ++result.attempted_facet_recoveries;
        result.facet_recovery_attempt_trace.push_back({key,0U});
        auto recovered=recover_wang_facet_by_flip_split(
            result.constraints,facet->vertices,0U,mesh);
        result.facet_local_edge_removal_attempts+=recovered.edge_removal_attempts;
        result.facet_local_edge_removals+=recovered.edge_removals;
        if(!recovered.recovered) {
          recovered=recover_wang_facet_by_local_flips(
              result.constraints,facet->vertices,1000U,mesh);
          result.facet_local_edge_removal_attempts+=recovered.edge_removal_attempts;
          result.facet_local_edge_removals+=recovered.edge_removals;
        }
        if(!recovered.recovered)pending.push_back(key);
      }
    }
  }
  // `recoverFacesPass` now advances into its info==1 pass without exporting
  // the ordered hull. In particular, addInteriorFacePoints commits each
  // constrained-BW cavity and immediately retries recoverFace here.
  if(!owned_fhc_terminal_failure&&
     scheduler.stop==WangOwnedSegmentSchedulerStop::complete) {
    const auto finite_for_insertion=[&]() {
      std::vector<std::array<std::uint32_t,4>> cells;
      for(const auto& cell:mesh.cells())
        if(!cell.deleted&&std::find(cell.vertices.begin(),cell.vertices.end(),
            static_cast<std::uint32_t>(mesh.ghost_vertex()))==cell.vertices.end())
          cells.push_back(cell.vertices);
      return cells;
    };
    auto facet_state=inspect_canonical_plc_tetrahedra(
        result.constraints,finite_for_insertion());
    for(const auto facet:facet_state.missing_facets) {
      if(result.facet_interior_steiner_insertions+2U>
             options.maximum_facet_interior_steiner_insertions||
         result.constraints.vertices.size()+2U>options.maximum_vertices)break;
      ++result.attempted_facet_recoveries;
      result.facet_recovery_attempt_trace.push_back({facet,1U});
      ++result.facet_interior_steiner_attempts;
      auto interior=insert_wang_facet_interior_points(
          result.constraints,facet,finite_for_insertion(),
          options.maximum_facet_local_flip_passes,&mesh);
      result.last_facet_interior_failure=interior.failure;
      result.facet_interior_bw_attempts+=interior.bowyer_watson_attempts;
      if(!interior.changed())continue;
      result.facet_interior_steiner_insertions+=interior.inserted_points.size();
      result.constraints=std::move(interior.constraints);
    }
    // recoverFacesPass follows its single info==1 queue traversal with one
    // ordinary info==0 queue traversal.  That pass may recover a different
    // still-lost facet after a neighbour's interior insertion, but it may
    // only use recoverFacebyFlip_Split (no new Steiner points or generic
    // retriangulation).
    const auto finite_after_insertion=[&]() {
      std::vector<std::array<std::uint32_t,4>> cells;
      for(const auto& cell:mesh.cells())
        if(!cell.deleted&&std::find(cell.vertices.begin(),cell.vertices.end(),
            static_cast<std::uint32_t>(mesh.ghost_vertex()))==cell.vertices.end())
          cells.push_back(cell.vertices);
      return cells;
    };
    const auto after_insertion=inspect_canonical_plc_tetrahedra(
        result.constraints,finite_after_insertion());
    for(const auto facet:after_insertion.missing_facets) {
      const auto found=std::find_if(result.constraints.facets.begin(),
          result.constraints.facets.end(),[&](const auto& candidate) {
            auto key=candidate.vertices;std::sort(key.begin(),key.end());
            return key==facet;
          });
      if(found==result.constraints.facets.end())continue;
      ++result.attempted_facet_recoveries;
      result.facet_recovery_attempt_trace.push_back({facet,0U});
      const auto recovered=recover_wang_facet_by_flip_split(
          result.constraints,found->vertices,0U,mesh);
      result.facet_local_edge_removal_attempts+=recovered.edge_removal_attempts;
      result.facet_local_edge_removals+=recovered.edge_removals;
    }
  }
  // Algorithm 2 lines 18--20 / recoverFaces(info=2): after the bounded
  // flip-only and interior-point passes above, a still-missing facet receives
  // the source-defined boundary split.  `recoverFaces` does not stop at the
  // first split: splitBndTri appends the three children to its lost-face
  // queue, and the caller follows each info==2 traversal with an info==0
  // retry traversal.  Keep that queue live against the ordered mesh.  In
  // particular, do not retain the finite vector from before the first split:
  // it no longer represents either the child facets or their edge shells.
  // A failed split stays visible through `last_facet_boundary_failure`; it is
  // never replaced by a general cavity remesher.
  if(!owned_fhc_terminal_failure&&
     scheduler.stop==WangOwnedSegmentSchedulerStop::complete) {
    const auto finite_mesh=[&]() {
      std::vector<std::array<std::uint32_t,4>> cells;
      for(const auto& cell:mesh.cells())
        if(!cell.deleted&&std::find(cell.vertices.begin(),cell.vertices.end(),
            static_cast<std::uint32_t>(mesh.ghost_vertex()))==cell.vertices.end())
          cells.push_back(cell.vertices);
      return cells;
    };
    const auto find_facet=[&](std::array<std::uint64_t,3> key) {
      std::sort(key.begin(),key.end());
      return std::find_if(result.constraints.facets.begin(),
          result.constraints.facets.end(),[&](const auto& candidate) {
            auto candidate_key=candidate.vertices;
            std::sort(candidate_key.begin(),candidate_key.end());
            return candidate_key==key;
          });
    };
    auto inspection=inspect_canonical_plc_tetrahedra(
        result.constraints,finite_mesh());
    std::vector<std::array<std::uint64_t,3>> pending=inspection.missing_facets;
    for(std::size_t round=0U;!pending.empty()&&round<=options.maximum_facets;
        ++round) {
      const auto count=pending.size();
      std::size_t changed{};
      for(std::size_t item=0U;item<count;++item) {
        const auto facet=pending.front();pending.erase(pending.begin());
        const auto found=find_facet(facet);
        // A parent consumed by splitBndTri is complete as far as its stale
        // queue entry is concerned; its appended children carry the work.
        if(found==result.constraints.facets.end()) {++changed;continue;}
        // Prerequisite recovery can split a boundary edge and replace the
        // surface-facet vector.  Keep the source queue identity by value;
        // an iterator into the old vector is not valid after that operation.
        const auto facet_vertices=found->vertices;
        ++result.attempted_facet_recoveries;
        result.facet_recovery_attempt_trace.push_back({facet,2U});
        const auto facets_before_edge_recovery=result.constraints.facets;
        const bool prerequisites_ready=
            recover_facet_prerequisite_edges(facet_vertices,2U);
        std::vector<std::array<std::uint64_t,3>> edge_split_children;
        for(const auto& child:result.constraints.facets)
          if(std::find(facets_before_edge_recovery.begin(),
                       facets_before_edge_recovery.end(),child)==
             facets_before_edge_recovery.end())
            edge_split_children.push_back(child.vertices);
        if(!edge_split_children.empty()) {
          pending.insert(pending.end(),edge_split_children.begin(),
                         edge_split_children.end());
          ++changed;
          continue;
        }
        if(!prerequisites_ready) {
          if(owned_fhc_terminal_failure)break;
          pending.push_back(facet);
          continue;
        }
        // Edge recovery may have completed or retired this exact literal
        // triangle incidentally.  Re-resolve it in the current live PLC.
        const auto current=find_facet(facet);
        if(current==result.constraints.facets.end()) {++changed;continue;}
        auto recovered=recover_wang_facet_by_flip_split(
            result.constraints,current->vertices,0U,mesh);
        result.facet_local_edge_removal_attempts+=recovered.edge_removal_attempts;
        result.facet_local_edge_removals+=recovered.edge_removals;
        if(!recovered.recovered) {
          recovered=recover_wang_facet_by_local_flips(
              result.constraints,current->vertices,1000U,mesh);
          result.facet_local_edge_removal_attempts+=recovered.edge_removal_attempts;
          result.facet_local_edge_removals+=recovered.edge_removals;
        }
        if(recovered.recovered) {++changed;continue;}
        // DT::recoverFace(info=2) runs addInteriorFacePoints before its
        // splitBndTri fallback, just as it does in the info=1 pass.  The
        // insertion commits against the same ordered ghost-hull mesh and
        // immediately retries the local recovery inside the owned helper.
        if(result.facet_interior_steiner_insertions+2U<=
               options.maximum_facet_interior_steiner_insertions&&
           result.constraints.vertices.size()+2U<=options.maximum_vertices) {
          ++result.attempted_facet_recoveries;
          result.facet_recovery_attempt_trace.push_back({facet,2U});
          ++result.facet_interior_steiner_attempts;
          auto interior=insert_wang_facet_interior_points(
              result.constraints,current->vertices,finite_mesh(),
              options.maximum_facet_local_flip_passes,&mesh);
          result.last_facet_interior_failure=interior.failure;
          result.facet_interior_bw_attempts+=interior.bowyer_watson_attempts;
          if(interior.changed()) {
            result.facet_interior_steiner_insertions+=
                interior.inserted_points.size();
            result.constraints=std::move(interior.constraints);
            const auto retry=find_facet(facet);
            if(retry==result.constraints.facets.end()) {++changed;continue;}
            recovered=recover_wang_facet_by_flip_split(
                result.constraints,retry->vertices,0U,mesh);
            result.facet_local_edge_removal_attempts+=
                recovered.edge_removal_attempts;
            result.facet_local_edge_removals+=recovered.edge_removals;
            if(!recovered.recovered) {
              recovered=recover_wang_facet_by_local_flips(
                  result.constraints,retry->vertices,1000U,mesh);
              result.facet_local_edge_removal_attempts+=
                  recovered.edge_removal_attempts;
              result.facet_local_edge_removals+=recovered.edge_removals;
            }
            if(recovered.recovered) {++changed;continue;}
          }
        }
        if(result.facet_splits>=options.maximum_facet_splits) {
          result.resource_limit=WangRecoveryResourceLimit::facet_boundary_splits;
          result.resource_limit_observed=result.facet_splits;
          result.resource_limit_configured=options.maximum_facet_splits;
          pending.push_back(facet);
          continue;
        }
        result.last_facet_boundary_facet=facet;
        const auto inserted=insert_wang_facet_boundary_steiner_point(
            result.constraints,facet,finite_mesh(),options.maximum_vertices,
            options.maximum_facets,&mesh);
        result.last_facet_boundary_failure=inserted.failure;
        // recoverFaces preserves this literal queue item after a failed
        // recoverFace.  Let the paired info=0 pass and the next bounded
        // round observe the same live state before declaring no progress.
        if(!inserted.accepted()) {pending.push_back(facet);continue;}
        result.constraints=inserted.constraints;
        ++result.facet_splits;
        ++changed;
        std::vector<std::array<std::uint64_t,3>> children;
        for(auto child=result.constraints.facets.end()-3;
            child!=result.constraints.facets.end();++child) {
          children.push_back(child->vertices);
          pending.push_back(child->vertices);
        }
        result.facet_post_split_child_calls.push_back(std::move(children));
      }
      if(owned_fhc_terminal_failure)break;
      // The paired recoverFaces(lost, 0) pass sees the topology generated by
      // every split in this round before another boundary insertion is tried.
      const auto retry_count=pending.size();
      bool deferred_prerequisite_edge=false;
      for(std::size_t item=0U;item<retry_count;++item) {
        const auto facet=pending.front();pending.erase(pending.begin());
        const auto found=find_facet(facet);
        if(found==result.constraints.facets.end()) {++changed;continue;}
        ++result.attempted_facet_recoveries;
        result.facet_recovery_attempt_trace.push_back({facet,0U});
        if(!recover_facet_prerequisite_edges(found->vertices,0U)) {
          deferred_prerequisite_edge=true;
          pending.push_back(facet);
          continue;
        }
        const auto recovered=recover_wang_facet_by_flip_split(
            result.constraints,found->vertices,0U,mesh);
        result.facet_local_edge_removal_attempts+=recovered.edge_removal_attempts;
        result.facet_local_edge_removals+=recovered.edge_removals;
        if(recovered.recovered)++changed;
        else pending.push_back(facet);
      }
      inspection=inspect_canonical_plc_tetrahedra(result.constraints,finite_mesh());
      // `inspection.missing_facets` is parent-patch based and therefore
      // cannot replace recoverFaces' literal child queue.  Keep every
      // explicit failed/appended child until its own queued recoverFace turn;
      // otherwise an incomplete parent patch can hide one missing radial
      // edge behind a different representative child.
      // A flip-only prerequisite failure is not a stalled facet round: the
      // next recoverFaces(...,2) call must retry that literal edge with its
      // FHC/boundary-split continuation.
      if((changed==0U&&!deferred_prerequisite_edge)||
         result.resource_limit!=WangRecoveryResourceLimit::none)
        break;
    }
  }
  result.facet_recovery_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-facet_started).count();
  const auto finalization_started=std::chrono::steady_clock::now();
  // Ghost connectivity is an owned scheduler implementation detail. Remove
  // its constraint record only after all scheduler/FHC operations finish;
  // emitted finite cells are remapped below.
  result.constraints.vertices.erase(
      result.constraints.vertices.begin()+scheduler_ghost);
  result.tetrahedra.clear();
  for(const auto& cell:mesh.cells())
    if(!cell.deleted&&std::find(cell.vertices.begin(),cell.vertices.end(),
                                static_cast<std::uint32_t>(mesh.ghost_vertex()))==cell.vertices.end()) {
      auto finite=cell.vertices;
      for(auto& vertex:finite)
        if(mesh.ghost_vertex()>=0&&vertex>static_cast<std::uint32_t>(mesh.ghost_vertex()))--vertex;
      result.tetrahedra.push_back(finite);
    }
  if(!constraint_mesh_is_valid(result.constraints,result.tetrahedra)) {
    result.finalization_milliseconds=std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-finalization_started).count();
    result.failure=CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
    return result;
  }
  // The scheduler mutates this owned state in place; preserve the seed before
  // it can be confused with its post-flip segment stage.
  result.initial_tetrahedra=seed.stages.back();
  result.inspection=inspect_canonical_plc_tetrahedra(
      result.constraints,result.tetrahedra);
  result.edges_recovered_before_facet_stage=
      result.inspection.missing_edges.empty();
  if(owned_fhc_terminal_failure) {
    result.failure=*owned_fhc_terminal_failure;
    return result;
  }
  switch(scheduler.stop) {
    case WangOwnedSegmentSchedulerStop::complete:
      result.failure=result.inspection.missing_facets.empty()?
          CanonicalPlcRecoveryFailure::none:
          CanonicalPlcRecoveryFailure::facet_recovery_required;
      break;
    case WangOwnedSegmentSchedulerStop::steiner_insertion_required:
      result.failure=scheduler.pending_steiner_mode>=2U?
          CanonicalPlcRecoveryFailure::owned_segment_boundary_split_required:
          CanonicalPlcRecoveryFailure::owned_segment_fhc_required;
      break;
    case WangOwnedSegmentSchedulerStop::unsupported_contact:
      result.failure=CanonicalPlcRecoveryFailure::owned_segment_contact_unsupported;
      break;
    default:
      result.failure=CanonicalPlcRecoveryFailure::owned_segment_scheduler_failed;
      break;
  }
  result.finalization_milliseconds=std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-finalization_started).count();
  return result;

#if 0 // Retired prototype Wang scheduler; retained temporarily for migration archaeology.
  CanonicalPlcRecoveryResult result;
  result.constraints=initial;
  // Stable IDs, rather than container insertion order, are the prototype's
  // representation of the pinned node/surface identities. Canonicalize the
  // storage view before BndPntInst/AddBox so an equivalent PLC permutation
  // reaches the same Delaunay seed and the same recovery queues.
  std::sort(result.constraints.vertices.begin(),result.constraints.vertices.end(),
            [](const auto& left,const auto& right){return left.id<right.id;});
  std::sort(result.constraints.facets.begin(),result.constraints.facets.end(),
            [](const auto& left,const auto& right) {
              auto left_key=left.vertices,right_key=right.vertices;
              std::sort(left_key.begin(),left_key.end());
              std::sort(right_key.begin(),right_key.end());
              if(left_key!=right_key)return left_key<right_key;
              if(left.parent!=right.parent)return left.parent<right.parent;
              return left.source_vertices<right.source_vertices;
            });
  if(!has_only_nondegenerate_constraint_facets(result.constraints))return result;
  const auto original_vertex_count=result.constraints.vertices.size();

  if(options.use_enclosing_cage&&!result.constraints.vertices.empty()) {
    auto lo=result.constraints.vertices.front().position;
    auto hi=lo;
    std::uint64_t next_id{};
    for(const auto& vertex:result.constraints.vertices) {
      const auto p=vertex.position;
      lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);lo.z=std::min(lo.z,p.z);
      hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);hi.z=std::max(hi.z,p.z);
      next_id=std::max(next_id,vertex.id);
    }
    if(next_id>std::numeric_limits<std::uint64_t>::max()-8U||
       result.constraints.vertices.size()+8U>options.maximum_vertices) {
      result.resource_limit=WangRecoveryResourceLimit::initial_vertices;
      result.resource_limit_observed=result.constraints.vertices.size()+8U;
      result.resource_limit_configured=options.maximum_vertices;
      result.failure=CanonicalPlcRecoveryFailure::resource_limit;
      return result;
    }
    // Pinned DT::AddBox(2.0): C +/- 2*H, where H is the input AABB's half
    // extent. Insert in the reference's standard AABB corner order.
    const auto centre=(lo+hi)/2.0;
    const auto half=(hi-lo)/2.0;
    const Vec3 low=centre-half*2.0,high=centre+half*2.0;
    for(const auto position:std::array<Vec3,8>{{
            {low.x,low.y,low.z},{high.x,low.y,low.z},
            {high.x,high.y,low.z},{low.x,high.y,low.z},
            {low.x,low.y,high.z},{high.x,low.y,high.z},
            {high.x,high.y,high.z},{low.x,high.y,high.z}}})
      result.constraints.vertices.push_back({++next_id,position});
  }

  // Wang et al. (2026), Algorithm 2 line 1: the initial mesh is Delaunay.
  // A failed Delaunay construction is a visible failure on this path; the
  // legacy non-Delaunay stellar seed is deliberately not an alternative.
  CanonicalDelaunaySeedInput input;
  input.maximum_vertices=result.constraints.vertices.size();
  input.maximum_tetrahedra=options.maximum_tetrahedra;
  input.exact_affine_planes=result.constraints.exact_affine_planes;
  for(const auto& vertex:result.constraints.vertices) {
    input.vertices.push_back(vertex.position);
    input.stable_vertex_ids.push_back(vertex.id);
  }
  const auto seed=build_wang_reference_seed(input,original_vertex_count);
  result.seed_failure=seed.failure;
  result.seed_invalid_reason=seed.invalid_reason;
  result.background_stellar_fallback_used=false;
  if(!seed.accepted()) {
    result.constraints={};
    result.failure=CanonicalPlcRecoveryFailure::seed_failed;
    return result;
  }
  result.tetrahedra=seed.tetrahedra;
  result.initial_tetrahedra=result.tetrahedra;
  if(!constraint_mesh_is_valid(result.constraints,result.tetrahedra)) {
    result.constraints={};
    result.tetrahedra={};
    result.failure=CanonicalPlcRecoveryFailure::seed_failed;
    return result;
  }

  // Algorithm 2 lines 3-12: complete segment recovery before entering the
  // facet loop. Each pass uses only line-4 local flips, lines 5-7 classified
  // FHC insertion, and lines 8-10 journaled boundary insertion, in that order.
  bool segment_full_search_enabled=false;
  std::size_t segment_easy_search_depth=1U;
  std::vector<std::array<std::uint64_t,2>> preferred_segment_order;
  const auto recover_all_missing_segments=[&](std::uint8_t recovery_info) {
  for(;;) {
    // Pinned recoverEdgesPass/recoverEdges queue discipline: every currently
    // lost edge gets one local-flip turn before an edge is allowed to advance
    // to Steiner insertion. Successful topology changes start another full
    // round; only a complete zero-progress round opens the FHC stage.
    std::map<std::array<std::uint64_t,2>,CanonicalLiteralEdgeFlipFailure>
        stalled_local_failures;
    bool segment_attempt_limit_reached=false;
    const auto ordered_missing_edges=[&](
        const std::vector<std::array<std::uint64_t,2>>& missing) {
      std::vector<std::array<std::uint64_t,2>> ordered;
      for(const auto edge:preferred_segment_order)
        if(std::find(missing.begin(),missing.end(),edge)!=missing.end()&&
           std::find(ordered.begin(),ordered.end(),edge)==ordered.end())
          ordered.push_back(edge);
      for(const auto edge:missing)
        if(std::find(ordered.begin(),ordered.end(),edge)==ordered.end())
          ordered.push_back(edge);
      return ordered;
    };
    const auto run_local_round=[&](bool full_search,std::size_t search_depth) {
      result.inspection=inspect_canonical_plc_tetrahedra(
          result.constraints,result.tetrahedra);
      if(result.inspection.missing_edges.empty())return false;
      const auto round=ordered_missing_edges(result.inspection.missing_edges);
      result.segment_local_flip_round_attempts.emplace_back();
      bool round_changed=false;
      std::vector<std::array<std::uint64_t,2>> next_preferred_segment_order;
      const auto prefer_next=[&](std::array<std::uint64_t,2> child) {
        std::sort(child.begin(),child.end());
        if(std::find(next_preferred_segment_order.begin(),
                     next_preferred_segment_order.end(),child)==
           next_preferred_segment_order.end())
          next_preferred_segment_order.push_back(child);
      };
      for(const auto edge:round) {
        // Pinned recoverEdges pops one SurEdg and recoverEdge begins with the
        // direct isRecBndEdg test.  Rebuilding the complete parent-facet audit
        // here was observationally equivalent but made every queued edge pay
        // for all facet recovery.  Query this edge in the current mesh
        // directly; accepted mutations are already committed before the next
        // queue item, so the result follows the same source queue semantics.
        if(mesh_contains_stable_edge(
               result.constraints,edge,result.tetrahedra))continue;
        if(result.attempted_edge_recoveries==
           options.maximum_edge_recovery_attempts) {
          result.resource_limit=WangRecoveryResourceLimit::segment_attempts;
          result.resource_limit_observed=result.attempted_edge_recoveries;
          result.resource_limit_configured=options.maximum_edge_recovery_attempts;
          result.failure=CanonicalPlcRecoveryFailure::resource_limit;
          segment_attempt_limit_reached=true;
          return false;
        }
        result.segment_local_flip_round_attempts.back().push_back(edge);
        ++result.attempted_edge_recoveries;
        WangSegmentLocalFlipResult local;
        const auto attempt=[&](WangSegmentFlipSearchMode mode,bool reverse) {
          result.segment_local_flip_attempt_trace.push_back(
              {edge,mode,reverse,search_depth});
          auto candidate=try_recover_wang_segment_by_local_flips(
              result.constraints,edge,result.tetrahedra,mode,reverse,search_depth);
          result.mesh_edge_removal_attempts+=candidate.edge_removal_attempts;
          result.mesh_edge_retriangulation_trials+=
              candidate.edge_retriangulation_trials;
          result.maximum_mesh_edge_degree_attempted=std::max(
              result.maximum_mesh_edge_degree_attempted,
              candidate.maximum_edge_degree);
          if(!candidate.preserved_recovered_constraints)
            ++result.invalid_candidate_mesh_rejections;
          if(candidate.flip.accepted&&!candidate.flip.recovers_by_constraint_split) {
            local=std::move(candidate);
            return true;
          }
          local=std::move(candidate);
          return false;
        };
        bool changed=attempt(WangSegmentFlipSearchMode::easy,false);
        if(!changed)changed=attempt(WangSegmentFlipSearchMode::easy,true);
        if(!changed&&full_search)
          changed=attempt(WangSegmentFlipSearchMode::full,false);
        if(changed) {
          if(local.boundary_vertex_attached) {
            result.constraints=std::move(local.constraints);
            ++result.boundary_vertex_attachments;
            prefer_next({edge[0],local.attached_boundary_vertex});
            prefer_next({local.attached_boundary_vertex,edge[1]});
          }
          result.tetrahedra=local.flip.tetrahedra;
          if(!local.boundary_vertex_attached)++result.edge_flips;
          result.mesh_edge_removals+=local.edge_removals;
          if(local.segment_recovered)++result.accepted_edge_recoveries;
          round_changed=true;
        } else stalled_local_failures[edge]=local.flip.failure;
      }
      preferred_segment_order=std::move(next_preferred_segment_order);
      return round_changed;
    };

    // recoverEdgesPass first exhausts increasing-depth easy rounds. Once one
    // complete round stalls, it permanently enables the info&1 full search
    // with the reference's depth floor of 1000.
    if(!segment_full_search_enabled) {
      for(;;) {
        result.inspection=inspect_canonical_plc_tetrahedra(
            result.constraints,result.tetrahedra);
        if(result.inspection.missing_edges.empty())break;
        if(!run_local_round(false,segment_easy_search_depth)) {
          segment_full_search_enabled=true;
          break;
        }
        ++segment_easy_search_depth;
      }
    }
    if(segment_full_search_enabled) {
      for(;;) {
        result.inspection=inspect_canonical_plc_tetrahedra(
            result.constraints,result.tetrahedra);
        if(result.inspection.missing_edges.empty())break;
        if(!run_local_round(true,std::max<std::size_t>(1000U,
                                                       segment_easy_search_depth)))
          break;
      }
    }
    if(segment_attempt_limit_reached)return false;

    result.inspection=inspect_canonical_plc_tetrahedra(
        result.constraints,result.tetrahedra);
    if(result.inspection.missing_edges.empty())break;
    // recoverEdge info=0 is a flip-only call. The containing recoverFace is
    // requeued when that prerequisite still cannot be recovered.
    if(recovery_info==0U)return true;

    const auto stalled_edges=result.inspection.missing_edges;
    bool fhc_round_changed=false;
    for(const auto edge:stalled_edges) {
      const auto current=inspect_canonical_plc_tetrahedra(
          result.constraints,result.tetrahedra);
      if(std::find(current.missing_edges.begin(),current.missing_edges.end(),edge)==
         current.missing_edges.end())continue;
      const std::set<std::array<std::uint64_t,2>> missing_edges_before(
          current.missing_edges.begin(),current.missing_edges.end());
      const std::set<std::array<std::uint64_t,3>> missing_facets_before(
          current.missing_facets.begin(),current.missing_facets.end());
      result.last_fhc_had_blocking_edge=false;
      result.last_fhc_had_blocking_face=false;
      result.last_fhc_candidates_generated=0U;
      result.last_fhc_candidates_attempted=0U;
      result.last_fhc_forced_cavity_rejections=0U;

    // Definition 3.4: a local transaction may not invalidate any constraint
    // that was recovered before the transaction began.
    const auto preserves_recovered_constraints=[&](
        const CanonicalPlcConstraintSet& constraints,
        const std::vector<Tet>& tetrahedra) {
      const auto inspection=inspect_canonical_plc_tetrahedra(constraints,tetrahedra);
      const auto introduced_missing_edge=std::any_of(
          inspection.missing_edges.begin(),inspection.missing_edges.end(),
          [&](const auto& missing){return !missing_edges_before.contains(missing);});
      const auto introduced_missing_facet=std::any_of(
          inspection.missing_facets.begin(),inspection.missing_facets.end(),
          [&](const auto& missing){return !missing_facets_before.contains(missing);});
      return !introduced_missing_edge&&!introduced_missing_facet;
    };
    // Algorithm 2 lines 5-7 and Section 4.2. Only the named Cascade-FHC and
    // Locked-FHC placements are reachable here; no generic cavity centroid is
    // treated as an FHC.
    bool fhc_inserted=false;
    const auto retry_after_fhc_insertion=[&](
        const CanonicalPlcConstraintSet& augmented,
        const std::vector<Tet>& inserted_mesh) {
      WangSegmentLocalFlipResult last;
      const auto depth=std::max<std::size_t>(1000U,segment_easy_search_depth);
      for(const bool reverse:std::array<bool,2>{{false,true}}) {
        result.segment_local_flip_attempt_trace.push_back(
            {edge,WangSegmentFlipSearchMode::easy,reverse,depth});
        auto candidate=try_recover_wang_segment_by_local_flips(
            augmented,edge,inserted_mesh,WangSegmentFlipSearchMode::easy,
            reverse,depth);
        result.mesh_edge_removal_attempts+=candidate.edge_removal_attempts;
        result.mesh_edge_retriangulation_trials+=
            candidate.edge_retriangulation_trials;
        result.maximum_mesh_edge_degree_attempted=std::max(
            result.maximum_mesh_edge_degree_attempted,
            candidate.maximum_edge_degree);
        if(!candidate.preserved_recovered_constraints)
          ++result.invalid_candidate_mesh_rejections;
        if(candidate.flip.accepted&&!candidate.flip.recovers_by_constraint_split)
          return candidate;
        last=std::move(candidate);
      }
      return last;
    };
    if(result.fhc_steiner_insertions<options.maximum_fhc_steiner_insertions&&
       result.constraints.vertices.size()<options.maximum_vertices) {
      const auto fhc_schedule=schedule_wang_fhc_candidates(
          result.constraints,edge,result.tetrahedra);
      const auto& cascade=fhc_schedule.cascade;
      const auto& locked=fhc_schedule.locked;
      const auto commit_insertion=[&](CanonicalLiteralEdgeFlipResult inserted,
                                      CanonicalInteriorSteinerKind kind) {
        result.last_fhc_insertion_failure=inserted.failure;
        result.last_fhc_forced_cavity_cells=inserted.cavity_tetrahedra.size();
        if(!inserted.accepted||!inserted.inserted_steiner_vertex) {
          ++result.last_fhc_forced_cavity_rejections;
          return false;
        }
        std::uint64_t next_id{};
        for(const auto& vertex:result.constraints.vertices)
          next_id=std::max(next_id,vertex.id);
        if(next_id==std::numeric_limits<std::uint64_t>::max()) {
          ++result.last_fhc_forced_cavity_rejections;
          return false;
        }
        auto augmented=result.constraints;
        augmented.vertices.push_back(
            {next_id+1U,*inserted.inserted_steiner_vertex});
        augmented.interior_steiner_vertices.push_back({next_id+1U,kind});
        auto after_flips=retry_after_fhc_insertion(
            augmented,inserted.tetrahedra);
        auto next_mesh=after_flips.flip.accepted?
            after_flips.flip.tetrahedra:inserted.tetrahedra;
        if(!constraint_mesh_mutation_is_valid(
               augmented,result.tetrahedra,next_mesh)||
           !preserves_recovered_constraints(augmented,next_mesh)) {
          ++result.invalid_candidate_mesh_rejections;
          return false;
        }
        result.constraints=std::move(augmented);
        result.tetrahedra=std::move(next_mesh);
        ++result.fhc_steiner_insertions;
        result.mesh_edge_removals+=after_flips.edge_removals;
        if(after_flips.segment_recovered)++result.accepted_edge_recoveries;
        return true;
      };
      const auto attempt_cascade=[&]() {
        if(!cascade.classified())return false;
        result.last_fhc_had_blocking_edge=true;
        ++result.fhc_cascade_configurations;
        ++result.fhc_steiner_attempts;
        ++result.last_fhc_candidates_generated;
        ++result.last_fhc_candidates_attempted;
        return commit_insertion(insert_cascade_fhc_vertex(
            result.constraints,edge,cascade.main_intersecting_edge,result.tetrahedra,
            options.maximum_fhc_steiner_attempts_per_segment),
            CanonicalInteriorSteinerKind::cascade_fhc);
      };
      const auto attempt_locked=[&]() {
        if(!locked.classified())return false;
        result.last_fhc_had_blocking_face=true;
        ++result.fhc_locked_configurations;
        ++result.fhc_steiner_attempts;
        ++result.last_fhc_candidates_generated;
        ++result.last_fhc_candidates_attempted;
        return commit_insertion(insert_locked_fhc_vertex(
            result.constraints,edge,locked.locking_mesh_edge,
            result.tetrahedra),CanonicalInteriorSteinerKind::locked_fhc);
      };

      if(fhc_schedule.first==WangFhcCandidateKind::cascade) {
        fhc_inserted=attempt_cascade();
        if(!fhc_inserted)fhc_inserted=attempt_locked();
      } else {
        fhc_inserted=attempt_locked();
        if(!fhc_inserted)fhc_inserted=attempt_cascade();
      }
    }
      if(fhc_inserted)fhc_round_changed=true;
    }
    if(fhc_round_changed)continue;
    // recoverEdge info=1 permits the prescribed interior FHC insertion but
    // does not split the PLC segment boundary.
    if(recovery_info==1U)return true;

    result.inspection=inspect_canonical_plc_tetrahedra(
        result.constraints,result.tetrahedra);
    if(result.inspection.missing_edges.empty())continue;
    const auto edge=result.inspection.missing_edges.front();
    CanonicalLiteralEdgeFlipResult recovered;
    if(const auto failure=stalled_local_failures.find(edge);
       failure!=stalled_local_failures.end())recovered.failure=failure->second;

    // Algorithm 2 lines 8-10 and the pinned splitBndEdge path. The boundary
    // transaction refines every incident PLC facet, journals the insertion,
    // and commits a valid Bowyer-Watson cavity that preserves recovered
    // constraints. Immediate child-edge recovery is diagnostic only: pinned
    // recoverEdges queues both new children even when neither exists yet.
    // Definition 3.4's global measure remains diagnostic: the reference does
    // not veto a split of a temporary radial facet edge on that value.
    if(result.edge_splits==options.maximum_edge_splits) {
      result.resource_limit=WangRecoveryResourceLimit::segment_boundary_splits;
      result.resource_limit_observed=result.edge_splits;
      result.resource_limit_configured=options.maximum_edge_splits;
      result.failure=CanonicalPlcRecoveryFailure::resource_limit;
      return false;
    }
    result.first_unrecovered_edge=edge;
    result.last_edge_failure=recovered.failure;
    const auto inserted=insert_wang_segment_boundary_steiner_point(
        result.constraints,edge,result.tetrahedra,options.maximum_vertices,
        options.maximum_facets);
    result.last_wang_segment_boundary_failure=inserted.failure;
    result.last_constraint_split_failure=inserted.constraint_failure;
    result.last_split_insertion_failure=inserted.insertion_failure;
    result.last_split_numerator=inserted.split_numerator;
    result.last_split_denominator=inserted.split_denominator;
    if(!inserted.accepted()) {
      result.failure=CanonicalPlcRecoveryFailure::segment_recovery_failed;
      return false;
    }
    const auto split_id=inserted.constraints.vertices.back().id;
    std::vector<std::array<std::uint64_t,2>> child_calls;
    const auto append_child=[&](std::array<std::uint64_t,2> child) {
      std::sort(child.begin(),child.end());
      if(std::find(child_calls.begin(),child_calls.end(),child)==child_calls.end())
        child_calls.push_back(child);
    };
    // Pinned splitBndEdge appends the two halves first, then [newp,p3] in
    // incident-facet order. recoverEdges immediately calls recoverEdge on
    // that entire appended range. Stable-ID canonicalization is the only
    // representation adaptation here; it does not alter the queue order.
    append_child({edge[0],split_id});
    append_child({edge[1],split_id});
    for(const auto& facet:result.constraints.facets) {
      const auto has_left=std::find(facet.vertices.begin(),facet.vertices.end(),
                                    edge[0])!=facet.vertices.end();
      const auto has_right=std::find(facet.vertices.begin(),facet.vertices.end(),
                                     edge[1])!=facet.vertices.end();
      if(!has_left||!has_right)continue;
      const auto other=std::find_if(facet.vertices.begin(),facet.vertices.end(),
          [&](std::uint64_t id){return id!=edge[0]&&id!=edge[1];});
      if(other!=facet.vertices.end())append_child({split_id,*other});
    }
    result.segment_post_split_child_edge_calls.push_back(child_calls);
    preferred_segment_order=child_calls;
    result.constraints=inserted.constraints;
    result.tetrahedra=inserted.tetrahedra;
    ++result.edge_splits;
    ++result.segment_intersection_splits;
  }
  return true;
  };

  // R1 selects the pinned AutorecoverEdges driver.  Unlike recoverEdgesPass,
  // it escalates each queued surface edge independently through info 0..-5.
  // Keep recover_all_missing_segments above for facet-prerequisite calls,
  // whose recoverFace info contract is separate from this initial scheduler.
  const auto recover_autorecover_segments=[&]() {
    using Outcome=CanonicalPlcRecoveryResult::SegmentSchedulerOutcome;
    struct AttemptResult {
      Outcome outcome{Outcome::failed};
      std::vector<std::array<std::uint64_t,2>> appended_edges;
    };
    std::size_t scheduler_round=1U;
    result.inspection=inspect_canonical_plc_tetrahedra(
        result.constraints,result.tetrahedra);
    std::vector<std::array<std::uint64_t,2>> queue;
    std::set<std::array<std::uint64_t,2>> queued;
    const std::set<std::array<std::uint64_t,2>> initially_missing(
        result.inspection.missing_edges.begin(),
        result.inspection.missing_edges.end());
    // buildBndInfo inserts a facet's cyclic opposite edges into SurEdgs and
    // AutorecoverEdges scans that stable surface-edge order.
    for(const auto& facet:result.constraints.facets)
      for(unsigned corner=0;corner<3U;++corner) {
        std::array<std::uint64_t,2> edge{{
            facet.vertices[(corner+1U)%3U],
            facet.vertices[(corner+2U)%3U]}};
        std::sort(edge.begin(),edge.end());
        if(initially_missing.contains(edge)&&queued.insert(edge).second)
          queue.push_back(edge);
      }
    std::map<std::array<std::uint64_t,2>,int> edge_info;
    std::map<std::uint64_t,std::size_t> previous_lost_at_vertex;
    for(auto edge:queue) {
      std::sort(edge.begin(),edge.end());
      edge_info[edge]=0;
      ++previous_lost_at_vertex[edge[0]];
      ++previous_lost_at_vertex[edge[1]];
    }

    std::function<AttemptResult(std::array<std::uint64_t,2>,bool,
                                std::uint8_t,std::size_t,std::size_t,int)>
        attempt_edge;
    attempt_edge=[&](std::array<std::uint64_t,2> edge,bool full_search,
                     std::uint8_t steiner_mode,std::size_t depth,
                     std::size_t round,int info_before) -> AttemptResult {
      std::sort(edge.begin(),edge.end());
      CanonicalPlcRecoveryResult::SegmentSchedulerAttempt scheduler_event{
          edge,round,info_before,depth,full_search,steiner_mode,
          Outcome::failed};
      const auto finish=[&](AttemptResult attempt) {
        scheduler_event.outcome=attempt.outcome;
        result.segment_scheduler_attempt_trace.push_back(scheduler_event);
        return attempt;
      };
      if(mesh_contains_stable_edge(result.constraints,edge,result.tetrahedra))
        return finish({Outcome::recovered,{}});
      if(result.attempted_edge_recoveries==
         options.maximum_edge_recovery_attempts) {
        result.resource_limit=WangRecoveryResourceLimit::segment_attempts;
        result.resource_limit_observed=result.attempted_edge_recoveries;
        result.resource_limit_configured=options.maximum_edge_recovery_attempts;
        result.failure=CanonicalPlcRecoveryFailure::resource_limit;
        return finish({});
      }
      ++result.attempted_edge_recoveries;
      bool attached_boundary_vertex=false;
      const auto local_attempt=[&](WangSegmentFlipSearchMode mode,
                                   bool reverse) {
        result.segment_local_flip_attempt_trace.push_back(
            {edge,mode,reverse,depth});
        auto candidate=try_recover_wang_segment_by_local_flips(
            result.constraints,edge,result.tetrahedra,mode,reverse,depth);
        result.mesh_edge_removal_attempts+=candidate.edge_removal_attempts;
        result.mesh_edge_retriangulation_trials+=
            candidate.edge_retriangulation_trials;
        result.maximum_mesh_edge_degree_attempted=std::max(
            result.maximum_mesh_edge_degree_attempted,
            candidate.maximum_edge_degree);
        if(!candidate.preserved_recovered_constraints)
          ++result.invalid_candidate_mesh_rejections;
        const auto accepted=candidate.flip.accepted&&
            !candidate.flip.recovers_by_constraint_split;
        if(!accepted)return false;
        if(candidate.boundary_vertex_attached) {
          attached_boundary_vertex=true;
          result.constraints=std::move(candidate.constraints);
          ++result.boundary_vertex_attachments;
        }
        result.tetrahedra=std::move(candidate.flip.tetrahedra);
        if(!candidate.boundary_vertex_attached)++result.edge_flips;
        result.mesh_edge_removals+=candidate.edge_removals;
        if(candidate.segment_recovered)++result.accepted_edge_recoveries;
        // AttachPnt2Seg deletes the parent surface edge.  Its positive return
        // value is the first appended edge index, not proof that the deleted
        // parent geometric edge remains in the mesh.  The caller must treat
        // the attachment itself as success and immediately recover its new
        // children, just as AutorecoverEdges does for ret > 1.
        if(candidate.boundary_vertex_attached)return true;
        return mesh_contains_stable_edge(
            result.constraints,edge,result.tetrahedra);
      };
      bool recovered=local_attempt(WangSegmentFlipSearchMode::easy,false);
      if(!recovered)
        recovered=local_attempt(WangSegmentFlipSearchMode::easy,true);
      if(!recovered&&full_search)
        recovered=local_attempt(WangSegmentFlipSearchMode::full,false);
      if(recovered) {
        if(attached_boundary_vertex) {
          std::set<std::array<std::uint64_t,2>> parent_edges;
          for(const auto& facet:constraints.facets)
            for(unsigned corner=0U;corner<3U;++corner) {
              std::array<std::uint64_t,2> child{{
                  facet.vertices[corner],facet.vertices[(corner+1U)%3U]}};
              std::sort(child.begin(),child.end());parent_edges.insert(child);
            }
          std::vector<std::array<std::uint64_t,2>> appended_edges;
          for(const auto& facet:result.constraints.facets)
            for(unsigned corner=0U;corner<3U;++corner) {
              std::array<std::uint64_t,2> child{{
                  facet.vertices[corner],facet.vertices[(corner+1U)%3U]}};
              std::sort(child.begin(),child.end());
              if(!parent_edges.contains(child)&&
                 std::find(appended_edges.begin(),appended_edges.end(),child)==
                     appended_edges.end())
                appended_edges.push_back(child);
            }
          return finish({Outcome::split,std::move(appended_edges)});
        }
        return finish({Outcome::recovered,{}});
      }

      if(steiner_mode>=1U) {
        const auto before=inspect_canonical_plc_tetrahedra(
            result.constraints,result.tetrahedra);
        const std::set<std::array<std::uint64_t,2>> missing_edges_before(
            before.missing_edges.begin(),before.missing_edges.end());
        const std::set<std::array<std::uint64_t,3>> missing_facets_before(
            before.missing_facets.begin(),before.missing_facets.end());
        const auto preserves=[&](const CanonicalPlcConstraintSet& constraints,
                                 const std::vector<Tet>& tetrahedra) {
          const auto after=inspect_canonical_plc_tetrahedra(
              constraints,tetrahedra);
          return std::none_of(after.missing_edges.begin(),
              after.missing_edges.end(),[&](const auto& missing) {
                return !missing_edges_before.contains(missing);
              })&&std::none_of(after.missing_facets.begin(),
              after.missing_facets.end(),[&](const auto& missing) {
                return !missing_facets_before.contains(missing);
              });
        };
        if(result.fhc_steiner_insertions<
               options.maximum_fhc_steiner_insertions&&
           result.constraints.vertices.size()<options.maximum_vertices) {
          const auto schedule=schedule_wang_fhc_candidates(
              result.constraints,edge,result.tetrahedra);
          const auto commit=[&](CanonicalLiteralEdgeFlipResult inserted,
                                CanonicalInteriorSteinerKind kind) {
            result.last_fhc_insertion_failure=inserted.failure;
            result.last_fhc_forced_cavity_cells=
                inserted.cavity_tetrahedra.size();
            if(!inserted.accepted||!inserted.inserted_steiner_vertex) {
              ++result.last_fhc_forced_cavity_rejections;
              return false;
            }
            std::uint64_t next_id{};
            for(const auto& vertex:result.constraints.vertices)
              next_id=std::max(next_id,vertex.id);
            if(next_id==std::numeric_limits<std::uint64_t>::max())return false;
            auto augmented=result.constraints;
            augmented.vertices.push_back(
                {next_id+1U,*inserted.inserted_steiner_vertex});
            augmented.interior_steiner_vertices.push_back({next_id+1U,kind});
            auto next_mesh=inserted.tetrahedra;
            if(!constraint_mesh_mutation_is_valid(
                   augmented,result.tetrahedra,next_mesh)||
               !preserves(augmented,next_mesh)) {
              ++result.invalid_candidate_mesh_rejections;
              return false;
            }
            result.constraints=std::move(augmented);
            result.tetrahedra=std::move(next_mesh);
            ++result.fhc_steiner_insertions;
            return true;
          };
          const auto attempt_cascade=[&]() {
            if(!schedule.cascade.classified())return false;
            result.last_fhc_had_blocking_edge=true;
            ++result.fhc_cascade_configurations;
            ++result.fhc_steiner_attempts;
            ++result.last_fhc_candidates_generated;
            ++result.last_fhc_candidates_attempted;
            return commit(insert_cascade_fhc_vertex(
                result.constraints,edge,
                schedule.cascade.main_intersecting_edge,result.tetrahedra,
                options.maximum_fhc_steiner_attempts_per_segment),
                CanonicalInteriorSteinerKind::cascade_fhc);
          };
          const auto attempt_locked=[&]() {
            if(!schedule.locked.classified())return false;
            result.last_fhc_had_blocking_face=true;
            ++result.fhc_locked_configurations;
            ++result.fhc_steiner_attempts;
            ++result.last_fhc_candidates_generated;
            ++result.last_fhc_candidates_attempted;
            return commit(insert_locked_fhc_vertex(
                result.constraints,edge,schedule.locked.locking_mesh_edge,
                result.tetrahedra),CanonicalInteriorSteinerKind::locked_fhc);
          };
          bool inserted=false;
          if(schedule.first==WangFhcCandidateKind::cascade) {
            inserted=attempt_cascade();
            if(!inserted)inserted=attempt_locked();
          } else {
            inserted=attempt_locked();
            if(!inserted)inserted=attempt_cascade();
          }
          if(inserted&&mesh_contains_stable_edge(
                 result.constraints,edge,result.tetrahedra)) {
            ++result.accepted_edge_recoveries;
            return finish({Outcome::recovered,{}});
          }
        }
      }

      if(steiner_mode<2U)return finish({});
      if(result.edge_splits==options.maximum_edge_splits) {
        result.resource_limit=WangRecoveryResourceLimit::segment_boundary_splits;
        result.resource_limit_observed=result.edge_splits;
        result.resource_limit_configured=options.maximum_edge_splits;
        result.failure=CanonicalPlcRecoveryFailure::resource_limit;
        return finish({});
      }
      result.first_unrecovered_edge=edge;
      const auto inserted=insert_wang_segment_boundary_steiner_point(
          result.constraints,edge,result.tetrahedra,options.maximum_vertices,
          options.maximum_facets);
      result.last_wang_segment_boundary_failure=inserted.failure;
      result.last_constraint_split_failure=inserted.constraint_failure;
      result.last_split_insertion_failure=inserted.insertion_failure;
      result.last_split_numerator=inserted.split_numerator;
      result.last_split_denominator=inserted.split_denominator;
      if(!inserted.accepted()) {
        result.failure=CanonicalPlcRecoveryFailure::segment_recovery_failed;
        return finish({});
      }
      const auto split_id=inserted.constraints.vertices.back().id;
      std::vector<std::array<std::uint64_t,2>> children;
      const auto append=[&](std::array<std::uint64_t,2> child) {
        std::sort(child.begin(),child.end());
        if(std::find(children.begin(),children.end(),child)==children.end())
          children.push_back(child);
      };
      append({edge[0],split_id});
      append({edge[1],split_id});
      for(const auto& facet:result.constraints.facets) {
        if(std::find(facet.vertices.begin(),facet.vertices.end(),edge[0])==
               facet.vertices.end()||
           std::find(facet.vertices.begin(),facet.vertices.end(),edge[1])==
               facet.vertices.end())continue;
        const auto other=std::find_if(facet.vertices.begin(),
            facet.vertices.end(),[&](auto id) {
              return id!=edge[0]&&id!=edge[1];
            });
        if(other!=facet.vertices.end())append({split_id,*other});
      }
      result.segment_post_split_child_edge_calls.push_back(children);
      result.constraints=inserted.constraints;
      result.tetrahedra=inserted.tetrahedra;
      ++result.edge_splits;
      ++result.segment_intersection_splits;
      return finish({Outcome::split,children});
    };

    while(!queue.empty()) {
      if(scheduler_round>1000U) {
        result.failure=CanonicalPlcRecoveryFailure::segment_recovery_failed;
        return false;
      }
      const auto round_queue=queue;
      queue.clear();
      result.segment_local_flip_round_attempts.emplace_back();
      for(auto edge:round_queue) {
        std::sort(edge.begin(),edge.end());
        if(mesh_contains_stable_edge(result.constraints,edge,result.tetrahedra))
          continue;
        result.segment_local_flip_round_attempts.back().push_back(edge);
        const auto info=edge_info[edge];
        const auto full_search=info<=-3;
        const auto steiner_mode=static_cast<std::uint8_t>(
            info<=-5?2:(info<=-4?1:0));
        const auto escalated_depth=scheduler_round+
            static_cast<std::size_t>(-info)*10U;
        const auto depth=full_search?
            std::max<std::size_t>(1000U,escalated_depth):escalated_depth;
        auto attempt=attempt_edge(edge,full_search,steiner_mode,depth,
                                  scheduler_round,info);
        if(result.failure==CanonicalPlcRecoveryFailure::resource_limit||
           result.failure==CanonicalPlcRecoveryFailure::segment_recovery_failed)
          return false;
        if(attempt.outcome==Outcome::failed)queue.push_back(edge);
        if(attempt.outcome==Outcome::split) {
          for(auto child:attempt.appended_edges) {
            auto immediate=attempt_edge(child,true,0U,depth,
                                        scheduler_round,-3);
            if(immediate.outcome==Outcome::failed) {
              edge_info[child]=-3;
              queue.push_back(child);
            }
          }
        }
      }

      std::map<std::uint64_t,std::size_t> current_lost_at_vertex;
      for(const auto edge:queue) {
        ++current_lost_at_vertex[edge[0]];
        ++current_lost_at_vertex[edge[1]];
      }
      for(auto& edge:queue) {
        auto& info=edge_info[edge];
        const auto old_a=previous_lost_at_vertex[edge[0]];
        const auto old_b=previous_lost_at_vertex[edge[1]];
        const auto new_a=current_lost_at_vertex[edge[0]];
        const auto new_b=current_lost_at_vertex[edge[1]];
        if(old_a<=new_a&&old_b<=new_b)info=std::max(info-1,-90);
        else info=std::min(info+1,0);
      }
      previous_lost_at_vertex=std::move(current_lost_at_vertex);
      ++scheduler_round;
    }
    return true;
  };

  if(!recover_autorecover_segments())return result;

  result.inspection=inspect_canonical_plc_tetrahedra(
      result.constraints,result.tetrahedra);
  result.edges_recovered_before_facet_stage=result.inspection.missing_edges.empty();
  if(!result.edges_recovered_before_facet_stage) {
    result.failure=CanonicalPlcRecoveryFailure::segment_recovery_failed;
    return result;
  }
  result.segment_stage_constraints=result.constraints;
  result.segment_stage_tetrahedra=result.tetrahedra;
  // recoverFacesPass sets the edge flip level to 1000 before any
  // recoverFace prerequisite call and passes fullsearch=1 to recoverEdge.
  segment_full_search_enabled=true;
  segment_easy_search_depth=std::max<std::size_t>(
      segment_easy_search_depth,1000U);

  // Algorithm 2 line 14 and the author-reference recoverFacebyLocalFlips:
  // remove free mesh edges intersecting the target facet, restarting the
  // intersection walk after each topology change. Later facet FHC and
  // boundary-split stages remain a visible refusal rather than substituting
  // the legacy two-sided/advancing-ridge solvers.
  const auto literal_facet_is_mesh_face=[&](
      std::array<std::uint64_t,3> target) {
    std::sort(target.begin(),target.end());
    for(const auto& cell:result.tetrahedra)
      for(unsigned omitted=0U;omitted<4U;++omitted) {
        std::array<std::uint64_t,3> face{};
        unsigned next{};
        for(unsigned i=0U;i<4U;++i)if(i!=omitted)
          face[next++]=result.constraints.vertices[cell[i]].id;
        std::sort(face.begin(),face.end());
        if(face==target)return true;
      }
    return false;
  };
  std::vector<std::array<std::uint64_t,3>> facet_queue=
      result.inspection.missing_facets;
  bool facet_stage_failed=false;
  const auto recover_one_facet=[&](std::array<std::uint64_t,3> facet,
                                   std::uint8_t info) {
    ++result.attempted_facet_recoveries;
    std::sort(facet.begin(),facet.end());
    result.facet_recovery_attempt_trace.push_back({facet,info});
    auto constraint_facet=std::find_if(
        result.constraints.facets.begin(),result.constraints.facets.end(),
        [&](const auto& candidate) {
          auto key=candidate.vertices;
          std::sort(key.begin(),key.end());
          return key==facet;
        });
    if(constraint_facet==result.constraints.facets.end()) {
      // A queued parent triangle is deleted when a prerequisite edge or the
      // triangle itself is split. recoverFaces treats that old entry as done
      // and works on the appended child range instead.
      return 1;
    }
    const auto oriented_facet=constraint_facet->vertices;
    bool prerequisite_changed_constraints=false;
    for(unsigned j=0U;j<3U;++j) {
      std::array<std::uint64_t,2> edge{{
          oriented_facet[j],oriented_facet[(j+1U)%3U]}};
      std::sort(edge.begin(),edge.end());
      const auto before=inspect_canonical_plc_tetrahedra(
          result.constraints,result.tetrahedra);
      const bool missing=std::find(before.missing_edges.begin(),
          before.missing_edges.end(),edge)!=before.missing_edges.end();
      result.facet_prerequisite_edge_calls.push_back(
          {oriented_facet,edge,missing});
      if(!missing)continue;
      std::set<std::array<std::uint64_t,3>> facets_before;
      for(const auto& before_facet:result.constraints.facets) {
        auto key=before_facet.vertices;
        std::sort(key.begin(),key.end());
        facets_before.insert(key);
      }
      preferred_segment_order={edge};
      if(!recover_all_missing_segments(info)) {
        facet_stage_failed=true;
        return -1;
      }
      for(const auto& after_facet:result.constraints.facets) {
        auto key=after_facet.vertices;
        std::sort(key.begin(),key.end());
        if(facets_before.contains(key))continue;
        facet_queue.push_back(after_facet.vertices);
        prerequisite_changed_constraints=true;
      }
      const auto after=inspect_canonical_plc_tetrahedra(
          result.constraints,result.tetrahedra);
      if(std::find(after.missing_edges.begin(),after.missing_edges.end(),edge)!=
         after.missing_edges.end()) {
        // Pinned recoverFace returns 0 and remains in the current phase's
        // queue when recoverEdge cannot satisfy a prerequisite.
        return 0;
      }
      // recoverFace returns the first new surface-triangle index immediately
      // when recoverEdge split a prerequisite edge. Its caller queues that
      // newly appended range instead of continuing with the stale triangle.
      if(prerequisite_changed_constraints)break;
    }
    result.inspection=inspect_canonical_plc_tetrahedra(
        result.constraints,result.tetrahedra);
    if(prerequisite_changed_constraints||literal_facet_is_mesh_face(facet))
      return 1;
    auto local=try_recover_wang_facet_by_local_flips(
        result.constraints,facet,result.tetrahedra,
        options.maximum_facet_local_flip_passes);
    result.facet_local_intersecting_edges+=local.intersecting_edges;
    result.facet_local_edge_removal_attempts+=local.edge_removal_attempts;
    result.facet_local_edge_removals+=local.edge_removals;
    result.facet_local_retriangulation_trials+=local.edge_retriangulation_trials;
    if(!local.preserved_recovered_constraints)
      ++result.invalid_candidate_mesh_rejections;
    if(local.edge_removals>0U)result.tetrahedra=local.tetrahedra;
    if(local.facet_recovered) {
      result.tetrahedra=std::move(local.tetrahedra);
      result.inspection=inspect_canonical_plc_tetrahedra(
          result.constraints,result.tetrahedra);
      return 1;
    }
    if(info==0U)return 0;

    // Author-reference recoverFacebyaddinSt/addInteriorFacePoints: after the
    // local flip pass stalls, insert at most one interior point on each side
    // of the facet and retry the same local operation. These are ordinary
    // interior Steiner points and therefore are not boundary-journal entries.
    if(result.facet_interior_steiner_insertions+2U<=
           options.maximum_facet_interior_steiner_insertions&&
       result.constraints.vertices.size()+2U<=options.maximum_vertices) {
      ++result.facet_interior_steiner_attempts;
      auto interior=insert_wang_facet_interior_points(
          result.constraints,facet,result.tetrahedra,
          options.maximum_facet_local_flip_passes);
      result.last_facet_interior_failure=interior.failure;
      result.facet_interior_bw_attempts+=interior.bowyer_watson_attempts;
      if(interior.changed()) {
        result.facet_interior_steiner_insertions+=interior.inserted_points.size();
        result.constraints=std::move(interior.constraints);
        result.tetrahedra=std::move(interior.tetrahedra);
        result.inspection=inspect_canonical_plc_tetrahedra(
            result.constraints,result.tetrahedra);
        if(literal_facet_is_mesh_face(facet))return 1;
      }
    }
    if(info<2U)return 0;

    // Author-reference recoverFacebyFlip_Split(...,2) / splitBndTri(...,-2):
    // after the interior pair and flip retry stall, split the facet at a
    // residual free-edge intersection and seed Bowyer-Watson with that edge's
    // complete shell.
    if(result.facet_splits==options.maximum_facet_splits) {
      result.resource_limit=WangRecoveryResourceLimit::facet_boundary_splits;
      result.resource_limit_observed=result.facet_splits;
      result.resource_limit_configured=options.maximum_facet_splits;
      result.failure=CanonicalPlcRecoveryFailure::resource_limit;
      facet_stage_failed=true;
      return -1;
    }
    auto boundary=insert_wang_facet_boundary_steiner_point(
        result.constraints,facet,result.tetrahedra,options.maximum_vertices,
        options.maximum_facets);
    result.last_facet_boundary_failure=boundary.failure;
    if(!boundary.accepted()) {
      result.failure=CanonicalPlcRecoveryFailure::facet_recovery_required;
      facet_stage_failed=true;
      return -1;
    }
    std::vector<std::array<std::uint64_t,3>> child_calls;
    if(boundary.constraints.facets.size()>=3U) {
      const auto first=boundary.constraints.facets.end()-3;
      for(auto child=first;child!=boundary.constraints.facets.end();++child)
        child_calls.push_back(child->vertices);
    }
    result.facet_post_split_child_calls.push_back(std::move(child_calls));
    facet_queue.insert(
        facet_queue.end(),
        result.facet_post_split_child_calls.back().begin(),
        result.facet_post_split_child_calls.back().end());
    result.constraints=std::move(boundary.constraints);
    result.tetrahedra=std::move(boundary.tetrahedra);
    ++result.facet_splits;
    result.inspection=inspect_canonical_plc_tetrahedra(
        result.constraints,result.tetrahedra);
    return 1;
  };
  const auto recover_facet_round=[&](std::uint8_t info) {
    const auto count=facet_queue.size();
    std::size_t successes{};
    for(std::size_t i=0U;i<count;++i) {
      const auto facet=facet_queue.front();
      facet_queue.erase(facet_queue.begin());
      const auto recovered=recover_one_facet(facet,info);
      if(recovered<0)return successes;
      if(recovered==0)facet_queue.push_back(facet);
      else ++successes;
    }
    return successes;
  };

  // Pinned recoverFacesPass: exhaust flip-only rounds first. A complete
  // zero-success round raises fliplevel_face to its full-search value and
  // therefore produces exactly one further flip-only round before leaving
  // this phase.
  std::size_t facet_flip_level{};
  while(!facet_queue.empty()) {
    const auto success=recover_facet_round(0U);
    if(facet_stage_failed)return result;
    if(facet_flip_level++>100U)break;
    if(success==0U)facet_flip_level=std::max<std::size_t>(facet_flip_level,1000U);
  }

  // The reference then gives each remaining face one interior-enabled turn,
  // followed by a flip-only retry, before boundary splitting is enabled.
  if(!facet_queue.empty()) {
    (void)recover_facet_round(1U);
    if(facet_stage_failed)return result;
    (void)recover_facet_round(0U);
    if(facet_stage_failed)return result;
  }

  std::size_t facet_split_rounds{};
  while(!facet_queue.empty()) {
    (void)recover_facet_round(2U);
    if(facet_stage_failed)return result;
    (void)recover_facet_round(0U);
    if(facet_stage_failed)return result;
    if(++facet_split_rounds>1000U) {
      result.failure=CanonicalPlcRecoveryFailure::resource_limit;
      return result;
    }
  }
  result.inspection=inspect_canonical_plc_tetrahedra(
      result.constraints,result.tetrahedra);
  result.failure=CanonicalPlcRecoveryFailure::none;
  return result;
#endif
}

namespace {
bool point_in_tet(Point p,const std::array<std::uint32_t,4>& tet,const std::vector<Point>& points){
  for(unsigned omit=0;omit<4U;++omit){std::array<std::uint32_t,3> face{};unsigned n=0;for(unsigned i=0;i<4U;++i)if(i!=omit)face[n++]=tet[i];const auto reference=orient(points[face[0]],points[face[1]],points[face[2]],points[tet[omit]]);const auto value=orient(points[face[0]],points[face[1]],points[face[2]],p);if(reference*value<-1e-18L)return false;}return true;
}
std::uint64_t canonical_regular_edge_id(std::array<std::uint64_t,2> edge) {
  if(edge[1]<edge[0])std::swap(edge[0],edge[1]);std::uint64_t value=1469598103934665603ULL;
  for(const auto word:edge){value^=word;value*=1099511628211ULL;}return value;
}
struct RefinedCanonicalCore { bool accepted{};std::vector<FrozenFacetVertex> vertices;std::vector<std::array<std::uint64_t,4>> tetrahedra; };
} // namespace

CanonicalPlcPublicationRepairResult
repair_canonical_plc_publication_degeneracies(
    const CanonicalPlcConstraintSet& constraints,const std::vector<Tet>& tetrahedra,
    double coordinate_scale,std::size_t maximum_mutations) {
  CanonicalPlcPublicationRepairResult result;
  result.constraints=constraints;
  result.tetrahedra=tetrahedra;
  if(!std::isfinite(coordinate_scale)||coordinate_scale<=0.0||
     !constraint_mesh_is_repairable(constraints,tetrahedra)) return result;
  const double floor=coordinate_scale*coordinate_scale*coordinate_scale*1.0e-13;
  const auto semantically_coplanar=[&](const CanonicalPlcConstraintSet& current,
                                       const Tet& tet) {
    for(const auto& plane:current.exact_affine_planes) {
      if(std::ranges::all_of(tet,[&](std::uint32_t vertex) {
           return vertex<current.vertices.size()&&std::binary_search(
               plane.vertex_ids.begin(),plane.vertex_ids.end(),
               current.vertices[vertex].id);
         })) return true;
    }
    return false;
  };
  const auto six_volume=[&](const CanonicalPlcConstraintSet& current,
                            const Tet& tet) {
    const auto& a=current.vertices[tet[0]].position;
    const auto& b=current.vertices[tet[1]].position;
    const auto& c=current.vertices[tet[2]].position;
    const auto& d=current.vertices[tet[3]].position;
    return std::abs((b.x-a.x)*((c.y-a.y)*(d.z-a.z)-(c.z-a.z)*(d.y-a.y))-
                    (b.y-a.y)*((c.x-a.x)*(d.z-a.z)-(c.z-a.z)*(d.x-a.x))+
                    (b.z-a.z)*((c.x-a.x)*(d.y-a.y)-(c.y-a.y)*(d.x-a.x)));
  };
  const auto count_bad=[&](const CanonicalPlcConstraintSet& current,
                           const std::vector<Tet>& cells) {
    return static_cast<std::size_t>(std::count_if(cells.begin(),cells.end(),
        [&](const auto& cell) {
          return six_volume(current,cell)<=floor||
              semantically_coplanar(current,cell);
        }));
  };
  result.initial_degenerate_tetrahedra=count_bad(result.constraints,result.tetrahedra);
  result.remaining_degenerate_tetrahedra=result.initial_degenerate_tetrahedra;
  // A rejected local move is evidence about this particular mesh, not a
  // reason to abandon every other bad cell.  Stable IDs make the visited set
  // independent of transient tetrahedron ordering.  A successful mutation
  // strictly lowers count_bad, so clearing it cannot introduce a cycle.
  std::set<std::array<std::uint64_t,4>> attempted_bad_cells;
  const auto stable_cell_key=[&](const Tet& cell) {
    std::array<std::uint64_t,4> key{{
        result.constraints.vertices[cell[0]].id,
        result.constraints.vertices[cell[1]].id,
        result.constraints.vertices[cell[2]].id,
        result.constraints.vertices[cell[3]].id}};
    std::sort(key.begin(),key.end());
    return key;
  };
  constexpr std::array<std::array<unsigned,2>,6> edge_corners{{
      {{0U,1U}},{{0U,2U}},{{0U,3U}},{{1U,2U}},{{1U,3U}},{{2U,3U}}}};
  while(result.accepted_mutations<maximum_mutations&&
        result.remaining_degenerate_tetrahedra>0U) {
    std::optional<std::size_t> bad;
    for(std::size_t cell=0U;cell<result.tetrahedra.size();++cell)
      if(six_volume(result.constraints,result.tetrahedra[cell])<=floor||
         semantically_coplanar(result.constraints,result.tetrahedra[cell]))
        if(!attempted_bad_cells.contains(stable_cell_key(result.tetrahedra[cell]))) {
        bad=cell;break;
        }
    if(!bad) {
      // Every currently bad stable cell has received the full local search.
      // Keep one deterministic witness for a reduced external diagnostic.
      for(const auto& candidate:result.tetrahedra)
        if(six_volume(result.constraints,candidate)<=floor||
           semantically_coplanar(result.constraints,candidate)) {
          result.first_unrepaired_vertex_ids=stable_cell_key(candidate);
          result.has_first_unrepaired_tetrahedron=true;
          for(const auto& incident:result.tetrahedra) {
            bool shares_vertex=false;
            for(const auto vertex:incident)
              shares_vertex=shares_vertex||std::binary_search(
                  result.first_unrepaired_vertex_ids.begin(),
                  result.first_unrepaired_vertex_ids.end(),
                  result.constraints.vertices[vertex].id);
            if(!shares_vertex) continue;
            std::array<std::uint64_t,4> ids{{
                result.constraints.vertices[incident[0]].id,
                result.constraints.vertices[incident[1]].id,
                result.constraints.vertices[incident[2]].id,
                result.constraints.vertices[incident[3]].id}};
            std::sort(ids.begin(),ids.end());
            result.first_unrepaired_incident_tetrahedra.push_back(ids);
          }
          for(const auto& facet:result.constraints.facets) {
            bool touches=false;
            for(const auto id:facet.vertices)
              touches=touches||std::binary_search(
                  result.first_unrepaired_vertex_ids.begin(),
                  result.first_unrepaired_vertex_ids.end(),id);
            if(touches) result.first_unrepaired_constrained_facets.push_back(
                facet.vertices);
          }
          break;
        }
      break;
    }
    const auto original=result.remaining_degenerate_tetrahedra;
    std::optional<std::vector<Tet>> improved;
    const auto accept=[&](const CanonicalLiteralEdgeFlipResult& candidate,
                          bool bounded_cavity=false) {
      if(!candidate.accepted||improved) return;
      if(!constraint_mesh_mutation_is_valid(
             result.constraints,result.tetrahedra,candidate.tetrahedra)) {
        ++result.invalid_candidate_mesh_rejections;
        return;
      }
      if(count_bad(result.constraints,candidate.tetrahedra)>=original) {
        if(bounded_cavity) ++result.bounded_cavity_non_improving_rejections;
        return;
      }
      const auto inspection=inspect_canonical_plc_tetrahedra(
          result.constraints,candidate.tetrahedra);
      if(!inspection.accepted()) {
        if(bounded_cavity) ++result.bounded_cavity_inspection_rejections;
        return;
      }
      improved=candidate.tetrahedra;
    };
    const auto& cell=result.tetrahedra[*bad];
    for(const auto corners:edge_corners) {
      accept(remove_mesh_edge_in_mesh(result.constraints,
          {{cell[corners[0]],cell[corners[1]]}},result.tetrahedra));
      if(improved)break;
    }
    if(!improved) for(unsigned omitted=0U;omitted<4U;++omitted) {
      Face face{};unsigned cursor{};
      for(unsigned corner=0U;corner<4U;++corner)
        if(corner!=omitted)face[cursor++]=cell[corner];
      accept(face_flip_for_specific_face(result.constraints,result.tetrahedra,face));
      if(improved)break;
    }
    if(!improved) {
      // A single-cell flip cannot remove a flat four-vertex simplex. Expand
      // through one non-frozen shared face and cone its unchanged boundary to
      // a deterministic interior point. This is a local stellar repair, not
      // a boundary edit: every constrained face remains in the candidate.
      std::map<Face,std::vector<std::size_t>> uses;
      for(std::size_t ci=0U;ci<result.tetrahedra.size();++ci)
        for(unsigned omitted=0U;omitted<4U;++omitted) {
          Face face{};unsigned cursor{};
          for(unsigned corner=0U;corner<4U;++corner)
            if(corner!=omitted)face[cursor++]=result.tetrahedra[ci][corner];
          uses[face_key(face)].push_back(ci);
        }
      std::set<Face> frozen;
      for(const auto& facet:result.constraints.facets) {
        Face face{};
        for(unsigned corner=0U;corner<3U;++corner) {
          const auto found=std::find_if(result.constraints.vertices.begin(),
              result.constraints.vertices.end(),[&](const auto& vertex) {
                return vertex.id==facet.vertices[corner];
              });
          if(found==result.constraints.vertices.end()) {face={};break;}
          face[corner]=static_cast<std::uint32_t>(found-result.constraints.vertices.begin());
        }
        frozen.insert(face_key(face));
      }
      // A greedy maximal component can be non-ball even when one of its
      // smaller connected subcavities is a legal ball. Enumerate a fixed,
      // deterministic prefix of such non-frozen subcavities (up to four
      // cells) rather than conflating that selection failure with a failure
      // of the exact-boundary retriangulator itself.
      std::vector<std::set<std::size_t>> cavity_sets{{std::set<std::size_t>{{*bad}}}};
      for(std::size_t cursor=0U;cursor<cavity_sets.size()&&
          cavity_sets.size()<32U;++cursor) {
        const auto selected=cavity_sets[cursor];
        if(selected.size()>=4U) continue;
        std::set<std::size_t> neighbours;
        for(const auto ci:selected) for(unsigned omitted=0U;omitted<4U;++omitted) {
          Face face{};unsigned face_cursor{};
          for(unsigned corner=0U;corner<4U;++corner)
            if(corner!=omitted) face[face_cursor++]=result.tetrahedra[ci][corner];
          const auto key=face_key(face);
          if(frozen.contains(key)) continue;
          const auto adjacent=uses.find(key);
          if(adjacent==uses.end()||adjacent->second.size()!=2U) continue;
          const auto neighbour=adjacent->second[0]==ci?
              adjacent->second[1]:adjacent->second[0];
          if(!selected.contains(neighbour)) neighbours.insert(neighbour);
        }
        for(const auto neighbour:neighbours) {
          auto expanded=selected;expanded.insert(neighbour);
          if(std::find(cavity_sets.begin(),cavity_sets.end(),expanded)==
             cavity_sets.end()) cavity_sets.push_back(std::move(expanded));
          if(cavity_sets.size()>=32U) break;
        }
      }
      for(const auto corners:edge_corners) for(const auto& selected:cavity_sets) {
        if(improved) break;
        if(selected.size()<2U) continue;
        std::vector<Tet> cavity;
        for(const auto ci:selected) cavity.push_back(result.tetrahedra[ci]);
        const std::array<std::uint64_t,2> stable_edge{{
            result.constraints.vertices[cell[corners[0]]].id,
            result.constraints.vertices[cell[corners[1]]].id}};
        ++result.bounded_cavity_attempts;
        const auto bounded=bounded_cavity_retriangulation(result.constraints,
            stable_edge,result.tetrahedra,cavity,std::nullopt,500U);
        if(!bounded.accepted) {
          if(bounded.failure==CanonicalLiteralEdgeFlipFailure::invalid_endpoint)
            ++result.bounded_cavity_missing_edge_rejections;
          else if(bounded.failure==CanonicalLiteralEdgeFlipFailure::incompatible_cavity_star)
            ++result.bounded_cavity_incompatible_rejections;
          else if(bounded.failure==CanonicalLiteralEdgeFlipFailure::retriangulation_trial_limit)
            ++result.bounded_cavity_trial_limit_rejections;
        }
        accept(bounded,true);
      }
      std::uint64_t next_id{};
      for(const auto& vertex:result.constraints.vertices)
        next_id=std::max(next_id,vertex.id);
      if(next_id==std::numeric_limits<std::uint64_t>::max()) break;
      for(unsigned omitted=0U;omitted<4U&&!improved;++omitted) {
        Face shared{};unsigned cursor{};
        for(unsigned corner=0U;corner<4U;++corner)
          if(corner!=omitted)shared[cursor++]=cell[corner];
        const auto found=uses.find(face_key(shared));
        if(found==uses.end()||found->second.size()!=2U||
           frozen.contains(face_key(shared))) continue;
        const auto other=found->second[0]==*bad?found->second[1]:found->second[0];
        Vec3 centroid{};
        std::set<std::uint32_t> vertices(cell.begin(),cell.end());
        vertices.insert(result.tetrahedra[other].begin(),result.tetrahedra[other].end());
        if(vertices.size()!=5U) continue;
        for(const auto vertex:vertices)
          centroid=centroid+result.constraints.vertices[vertex].position;
        centroid=centroid/static_cast<double>(vertices.size());
        // A declared source plane is stronger than the evaluated coordinates:
        // direct the disposable apex toward the adjoining cell's off-plane
        // vertex, without perturbing any frozen vertex.  The midpoint remains
        // strictly inside the two-cell convex combination and avoids coning a
        // planar boundary face from an almost-planar arithmetic centroid.
        for(const auto& plane:result.constraints.exact_affine_planes) {
          const auto on_plane=[&](std::uint32_t vertex) {
            return std::binary_search(plane.vertex_ids.begin(),plane.vertex_ids.end(),
                                      result.constraints.vertices[vertex].id);
          };
          if(!std::ranges::all_of(cell,on_plane)||
             !std::ranges::all_of(shared,on_plane)) continue;
          const auto& neighbour=result.tetrahedra[other];
          const auto off_plane=std::find_if(neighbour.begin(),neighbour.end(),
              [&](std::uint32_t vertex) { return !on_plane(vertex); });
          if(off_plane==neighbour.end()) continue;
          centroid=(centroid+result.constraints.vertices[*off_plane].position)*0.5;
          break;
        }
        auto augmented=result.constraints;
        const auto apex=static_cast<std::uint32_t>(augmented.vertices.size());
        augmented.vertices.push_back({next_id+1U,centroid});
        augmented.interior_steiner_vertices.push_back(
            {next_id+1U,CanonicalInteriorSteinerKind::topology_repair});
        std::map<Face,unsigned> boundary;
        for(const auto ci:found->second)
          for(unsigned opposite=0U;opposite<4U;++opposite) {
            Face face{};unsigned n{};
            for(unsigned corner=0U;corner<4U;++corner)
              if(corner!=opposite)face[n++]=result.tetrahedra[ci][corner];
            ++boundary[face_key(face)];
          }
        std::vector<Tet> replacement;bool positive=true;
        for(const auto& [face,count]:boundary) if(count==1U) {
          Tet candidate{{apex,face[0],face[1],face[2]}};
          replacement.push_back(candidate);
        }
        std::vector<Point> points;
        for(const auto& vertex:augmented.vertices)
          points.push_back({vertex.position.x,vertex.position.y,vertex.position.z});
        for(auto& candidate:replacement) {
          const auto orientation=robust_orient(points,candidate);
          if(orientation==0) {positive=false;break;}
          if(orientation<0)std::swap(candidate[0],candidate[1]);
        }
        if(!positive)continue;
        std::vector<Tet> candidate_mesh;
        for(std::size_t ci=0U;ci<result.tetrahedra.size();++ci)
          if(ci!=*bad&&ci!=other)candidate_mesh.push_back(result.tetrahedra[ci]);
        candidate_mesh.insert(candidate_mesh.end(),replacement.begin(),replacement.end());
        if(!constraint_mesh_mutation_is_valid(augmented,result.tetrahedra,candidate_mesh)||
           !inspect_canonical_plc_tetrahedra(augmented,candidate_mesh).accepted()||
           count_bad(augmented,candidate_mesh)>=original) continue;
        result.constraints=std::move(augmented);
        improved=std::move(candidate_mesh);
      }
    }
    if(!improved) {
      attempted_bad_cells.insert(stable_cell_key(cell));
      continue;
    }
    result.tetrahedra=std::move(*improved);
    result.remaining_degenerate_tetrahedra=count_bad(result.constraints,result.tetrahedra);
    ++result.accepted_mutations;
    attempted_bad_cells.clear();
  }
  return result;
}

CanonicalPlcRegionResult classify_canonical_plc_regions(const CanonicalPlcRegionInput& input) {
  CanonicalPlcRegionResult result;
  if(!std::isfinite(input.coordinate_scale)||input.coordinate_scale<=0.) return result;
  using Incident=std::pair<std::uint32_t,std::uint32_t>;
  std::map<Face,std::vector<Incident>> faces;
  for(std::uint32_t cell=0;cell<input.tetrahedra.size();++cell) {
    const auto& tet=input.tetrahedra[cell];
    for(const auto vertex:tet) if(vertex>=input.vertices.size()) {result.failure=CanonicalPlcRegionFailure::invalid_index;return result;}
    // Recovery and the pinned implementation classify tetrahedra with an
    // orientation predicate, not a scale-dependent minimum-volume quality
    // threshold.  A very small nonzero tetrahedron is still topologically
    // usable for the flood classification and is handled later by quality
    // evaluation; only an exact zero is degenerate here.
    if(exact_orientation_3d(input.vertices[tet[0]],input.vertices[tet[1]],
                            input.vertices[tet[2]],input.vertices[tet[3]])==
       ExactPredicateSign::zero) {
      result.failure=CanonicalPlcRegionFailure::degenerate_tetrahedron;
      return result;
    }
    for(std::uint32_t omit=0;omit<4U;++omit) {Face face{};std::uint32_t n{};for(std::uint32_t i=0;i<4U;++i)if(i!=omit)face[n++]=tet[i];faces[face_key(face)].push_back({cell,omit});}
  }
  for(const auto& [face,uses]:faces) { (void)face;if(uses.empty()||uses.size()>2U) {result.failure=CanonicalPlcRegionFailure::nonmanifold_mesh;return result;} }
  std::set<Face> constraints;
  const auto add_constraints=[&](const std::vector<std::array<std::uint32_t,3>>& source) {
    for(const auto raw:source) {
      for(const auto vertex:raw)if(vertex>=input.vertices.size())return false;
      const auto face=face_key(raw);const auto found=faces.find(face);
      // A constraint on the background hull is legal: it separates its sole
      // incident material cell from exterior space. Interior constraints must
      // have a cell on both sides.
      if(found==faces.end()||found->second.empty())return false;
      constraints.insert(face);
    }
    return true;
  };
  if(!add_constraints(input.outer_faces)||!add_constraints(input.core_faces)) {result.failure=CanonicalPlcRegionFailure::missing_constraint_face;return result;}
  result.regions.assign(input.tetrahedra.size(),CanonicalPlcCellRegion::shell);
  std::vector<std::uint32_t> queue;
  for(const auto& [face,uses]:faces)if(uses.size()==1U&&!constraints.contains(face)) {
    const auto cell=uses.front().first;if(result.regions[cell]!=CanonicalPlcCellRegion::outside) {result.regions[cell]=CanonicalPlcCellRegion::outside;queue.push_back(cell);}
  }
  const auto flood=[&](CanonicalPlcCellRegion region) {
    for(std::size_t cursor=0;cursor<queue.size();++cursor) {
      const auto cell=queue[cursor];const auto& tet=input.tetrahedra[cell];
      for(std::uint32_t omit=0;omit<4U;++omit) {Face face{};std::uint32_t n{};for(std::uint32_t i=0;i<4U;++i)if(i!=omit)face[n++]=tet[i];const auto key=face_key(face);if(constraints.contains(key))continue;const auto& uses=faces.at(key);if(uses.size()!=2U)continue;const auto neighbour=uses[0].first==cell?uses[1].first:uses[0].first;if(result.regions[neighbour]==CanonicalPlcCellRegion::shell){result.regions[neighbour]=region;queue.push_back(neighbour);}}
    }
  };
  flood(CanonicalPlcCellRegion::outside);
  const auto closed_contains=[&](const std::array<std::uint32_t,4>& tet,Vec3 p) {
    bool on_constraint=false;
    for(std::uint32_t omit=0;omit<4U;++omit){std::array<std::uint32_t,3> face{};std::uint32_t n{};for(std::uint32_t i=0;i<4U;++i)if(i!=omit)face[n++]=tet[i];const auto a=input.vertices[face[0]],b=input.vertices[face[1]],c=input.vertices[face[2]],opposite=input.vertices[tet[omit]];
      const auto reference=exact_orientation_3d(a,b,c,opposite);
      const auto value=exact_orientation_3d(a,b,c,p);
      if(reference==ExactPredicateSign::zero||
         (value!=ExactPredicateSign::zero&&reference!=value))
        return std::pair{false,false};
      if(value==ExactPredicateSign::zero&&constraints.contains(face_key(face)))
        on_constraint=true;
    }return std::pair{true,on_constraint};
  };
  queue.clear();
  for(const auto witness:input.core_witnesses) {
    if(!std::isfinite(witness.x)||!std::isfinite(witness.y)||!std::isfinite(witness.z)){result.failure=CanonicalPlcRegionFailure::ambiguous_core_witness;return result;}
    std::vector<std::uint32_t> owners;
    bool on_constraint{};
    for(std::uint32_t cell=0;cell<input.tetrahedra.size();++cell) {
      const auto [contains,on_boundary]=closed_contains(
          input.tetrahedra[cell],witness);
      if(contains)owners.push_back(cell);
      on_constraint=on_constraint||on_boundary;
    }
    if(owners.empty()){result.failure=CanonicalPlcRegionFailure::core_witness_outside_mesh;return result;}
    // A witness on an ordinary internal face legitimately has both incident
    // cells as owners; seed both and the unconstrained flood joins them.  A
    // witness on a recovered PLC face is genuinely ambiguous because its two
    // sides have different region meanings.
    if(on_constraint){result.failure=CanonicalPlcRegionFailure::ambiguous_core_witness;return result;}
    if(std::any_of(owners.begin(),owners.end(),[&](const auto owner) {
         return result.regions[owner]==CanonicalPlcCellRegion::outside;
       })) {result.failure=CanonicalPlcRegionFailure::core_witness_in_exterior;return result;}
    for(const auto owner:owners)
      if(result.regions[owner]==CanonicalPlcCellRegion::shell){result.regions[owner]=CanonicalPlcCellRegion::core;queue.push_back(owner);}
  }
  flood(CanonicalPlcCellRegion::core);
  for(const auto region:result.regions)if(region==CanonicalPlcCellRegion::outside)++result.outside_cells;else if(region==CanonicalPlcCellRegion::shell)++result.shell_cells;else ++result.core_cells;
  result.failure=CanonicalPlcRegionFailure::none;return result;
}

namespace {
RefinedCanonicalCore refine_canonical_core(const NonmatchingPlcManifestResult& manifest,const CanonicalPlcConstraintSet& constraints) {
  RefinedCanonicalCore result;std::map<std::uint64_t,Vec3> positions;for(const auto& vertex:constraints.vertices)if(!positions.emplace(vertex.id,vertex.position).second)return result;
  std::vector<RegularCoreGeometricParent> parents;std::set<std::uint64_t> core_vertices;
  for(std::size_t i=0;i<manifest.manifest.materialized_core_tetrahedra.size();++i){const auto tet=manifest.manifest.materialized_core_tetrahedra[i];for(const auto id:tet)if(!positions.contains(id))return result;core_vertices.insert(tet.begin(),tet.end());parents.push_back({static_cast<RegularCoreParentId>(i+1U),tet});}
  std::map<std::array<std::uint64_t,2>,RegularCoreEdgeId> edge_ids;for(const auto& parent:parents)for(std::size_t a=0;a<4U;++a)for(std::size_t b=a+1U;b<4U;++b){auto edge=std::array<std::uint64_t,2>{{parent.vertices[a],parent.vertices[b]}};if(edge[1]<edge[0])std::swap(edge[0],edge[1]);edge_ids.emplace(edge,RegularCoreEdgeId{canonical_regular_edge_id(edge)});}
  struct EdgeParameter { std::array<std::uint64_t,2> edge{};std::uint64_t numerator{},denominator{}; };
  // Only splits occurring on the core interface belong to this local core
  // refinement. Outer-sheet recovery can introduce independent split
  // vertices which necessarily have no parent-core edge provenance.
  std::set<std::uint64_t> core_split_ids;
  for(const auto& facet:constraints.facets)if(facet.core_interface)
    for(const auto id:facet.vertices)core_split_ids.insert(id);
  std::map<std::uint64_t,CanonicalPlcSplitVertex> pending;
  for(const auto& split:constraints.split_vertices)if(core_split_ids.contains(split.id)&&!pending.emplace(split.id,split).second)return result;
  std::map<std::uint64_t,EdgeParameter> resolved;
  const auto endpoint_parameter=[&](std::uint64_t id,const std::array<std::uint64_t,2>& edge)->std::optional<EdgeParameter>{
    if(id==edge[0])return EdgeParameter{edge,0U,1U};if(id==edge[1])return EdgeParameter{edge,1U,1U};
    const auto found=resolved.find(id);if(found==resolved.end()||found->second.edge!=edge)return std::nullopt;return found->second;
  };
  while(!pending.empty()) { bool progressed{};
    for(auto it=pending.begin();it!=pending.end();) { const auto& split=it->second;bool found{};EdgeParameter value{};
      for(const auto& [edge,unused]:edge_ids) { (void)unused;const auto a=endpoint_parameter(split.edge[0],edge),b=endpoint_parameter(split.edge[1],edge);if(!a||!b)continue;if(found)return {};found=true;const auto common_divisor=std::gcd(a->denominator,b->denominator);const auto lcm=a->denominator/common_divisor*b->denominator;if(lcm>std::numeric_limits<std::uint32_t>::max()/2U)return {};const auto numerator=a->numerator*(lcm/a->denominator)+b->numerator*(lcm/b->denominator);const auto denominator=lcm*2U;const auto reduce=std::gcd(numerator,denominator);value={edge,numerator/reduce,denominator/reduce}; }
      if(!found){++it;continue;}if(value.numerator==0U||value.numerator>=value.denominator)return {};resolved.emplace(split.id,value);it=pending.erase(it);progressed=true;
    }
    if(!progressed)return {};
  }
  std::vector<RegularCoreArbitraryEdgeSplitRequest> requests;std::map<std::uint64_t,RegularCoreArbitraryEdgeSplitRequest> request_by_actual;
  for(const auto& [actual,value]:resolved){
    // Structural midpoint provenance is not enough: a vertex can occur in
    // several materialized-child edge stars.  Before aliasing it into the
    // refined core, prove that the chosen root edge reconstructs its actual
    // PLC position.  Otherwise a valid shell facet could be silently bent or
    // collapsed by an ambiguous local-edge interpretation.
    const auto& left=positions.at(value.edge[0]);const auto& right=positions.at(value.edge[1]);
    const auto fraction=static_cast<double>(value.numerator)/static_cast<double>(value.denominator);
    const Vec3 reconstructed{left.x+(right.x-left.x)*fraction,left.y+(right.y-left.y)*fraction,left.z+(right.z-left.z)*fraction};
    const auto delta=positions.at(actual)-reconstructed;
    const auto magnitude=std::max({1.0,std::abs(left.x),std::abs(left.y),std::abs(left.z),std::abs(right.x),std::abs(right.y),std::abs(right.z)});
    if(std::abs(delta.x)>magnitude*1e-12||std::abs(delta.y)>magnitude*1e-12||std::abs(delta.z)>magnitude*1e-12)return result;
    const auto request=RegularCoreArbitraryEdgeSplitRequest{edge_ids.at(value.edge),{static_cast<std::uint32_t>(value.numerator),static_cast<std::uint32_t>(value.denominator)}};requests.push_back(request);request_by_actual.emplace(actual,request);
  }
  const auto plan=plan_regular_core_arbitrary_edge_splits(requests,{requests.size()});if(!plan.accepted())return result;
  std::map<RegularCoreEdgeId,std::array<std::uint64_t,2>> edge_roots;
  for(const auto& [edge,id]:edge_ids)edge_roots.emplace(id,edge);
  std::map<std::uint64_t,Vec3> refined_positions=positions;
  for(const auto& split:plan.vertices){const auto roots=edge_roots.at(split.edge);const auto& left=positions.at(roots[0]);const auto& right=positions.at(roots[1]);const auto fraction=static_cast<double>(split.parameter.numerator)/static_cast<double>(split.parameter.denominator);refined_positions.emplace(split.id,Vec3{left.x+(right.x-left.x)*fraction,left.y+(right.y-left.y)*fraction,left.z+(right.z-left.z)*fraction});}
  const auto request_key=[](const RegularCoreArbitraryEdgeSplitRequest& request){return std::pair{request.edge,request.parameter};};
  std::map<std::pair<RegularCoreEdgeId,RegularCoreRational>,std::uint64_t> plan_ids;for(const auto& split:plan.vertices)plan_ids.emplace(std::pair{split.edge,split.parameter},split.id);
  std::map<std::uint64_t,std::uint64_t> actual_to_plan;for(const auto& [actual,request]:request_by_actual){const auto found=plan_ids.find(request_key(request));if(found==plan_ids.end())return result;actual_to_plan.emplace(actual,found->second);}
  std::vector<RegularCoreGeometryVertex> roots;for(const auto id:core_vertices)roots.push_back({id,{positions.at(id).x,positions.at(id).y,positions.at(id).z}});
  using FaceIds=std::array<std::uint64_t,3>;std::map<FaceIds,unsigned> core_face_uses;for(const auto& parent:parents)for(std::uint8_t omit=0;omit<4U;++omit){FaceIds face{};std::size_t n{};for(std::size_t i=0;i<4U;++i)if(i!=omit)face[n++]=parent.vertices[i];std::sort(face.begin(),face.end());++core_face_uses[face];}
  std::map<FaceIds,std::vector<const CanonicalPlcConstraintFacet*>> constrained_faces;for(const auto& facet:constraints.facets)if(facet.core_interface){auto source=facet.source_vertices;std::sort(source.begin(),source.end());constrained_faces[source].push_back(&facet);}
  std::map<RegularCoreEdgeId,std::vector<RegularCoreArbitrarySplitVertex>> splits;for(const auto& split:plan.vertices)splits[split.edge].push_back(split);
  const auto cross_vec=[](Vec3 left,Vec3 right){return Vec3{left.y*right.z-left.z*right.y,left.z*right.x-left.x*right.z,left.x*right.y-left.y*right.x};};
  const auto dot_vec=[](Vec3 left,Vec3 right){return left.x*right.x+left.y*right.y+left.z*right.z;};
  std::vector<RegularCoreArbitraryFaceTopology> topologies;
  for(const auto& parent:parents)for(std::uint8_t omit=0;omit<4U;++omit){
    RegularCoreArbitraryFaceTopology topology;topology.face={parent.id,omit};std::size_t n{};for(std::size_t i=0;i<4U;++i)if(i!=omit)topology.corners[n++]=parent.vertices[i];
    for(std::size_t i=0,k=0;i<3U;++i)for(std::size_t j=i+1U;j<3U;++j,++k){auto edge=std::array<std::uint64_t,2>{{topology.corners[i],topology.corners[j]}};if(edge[1]<edge[0])std::swap(edge[0],edge[1]);topology.edges[k]={edge_ids.at(edge),edge};}
    auto canonical=topology.corners;std::sort(canonical.begin(),canonical.end());
    if(core_face_uses.at(canonical)==1U){const auto found=constrained_faces.find(canonical);if(found==constrained_faces.end())return {};for(const auto* facet:found->second){auto triangle=facet->vertices;for(auto& vertex:triangle)if(const auto replacement=actual_to_plan.find(vertex);replacement!=actual_to_plan.end())vertex=replacement->second;topology.triangles.push_back(triangle);}}
    else {
      // An internal shared face has no external facet record.  Derive the
      // same diagonalisation from its sorted stable IDs in both parents;
      // deriving a fan from parent-local winding would disagree on a shared
      // face even when its split edge set is identical.
      std::vector<std::uint64_t> boundary;
      for(std::size_t i=0;i<3U;++i){
        const auto first=canonical[i],second=canonical[(i+1U)%3U];
        boundary.push_back(first);
        auto edge=std::array<std::uint64_t,2>{{first,second}};
        if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
        auto entries=splits[edge_ids.at(edge)];
        std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.parameter<b.parameter;});
        if(first!=edge[0])std::reverse(entries.begin(),entries.end());
        for(const auto& split:entries)boundary.push_back(split.id);
      }
      // A plain fan is invalid when its first three vertices are an original
      // edge endpoint, an edge split, and the other endpoint.  Clip only
      // non-collinear ears instead.  Every point lies on the boundary of one
      // convex parent triangle, so this is a complete deterministic
      // triangulation and preserves the exact boundary sequence.
      const auto face_normal=cross_vec(refined_positions.at(topology.corners[1])-refined_positions.at(topology.corners[0]),refined_positions.at(topology.corners[2])-refined_positions.at(topology.corners[0]));
      if(dot_vec(face_normal,face_normal)<=1e-24)return {};
      long double winding{};
      for(std::size_t i=0;i<boundary.size();++i){const auto previous=boundary[(i+boundary.size()-1U)%boundary.size()];const auto current=boundary[i];const auto next=boundary[(i+1U)%boundary.size()];const auto normal=cross_vec(refined_positions.at(current)-refined_positions.at(previous),refined_positions.at(next)-refined_positions.at(previous));winding=dot_vec(normal,face_normal);if(std::abs(winding)>1e-24)break;}
      if(std::abs(winding)<=1e-24)return {};
      while(boundary.size()>3U){
        bool clipped{};
        for(std::size_t i=0;i<boundary.size();++i){
          const auto previous=boundary[(i+boundary.size()-1U)%boundary.size()];
          const auto current=boundary[i];const auto next=boundary[(i+1U)%boundary.size()];
          const auto normal=cross_vec(refined_positions.at(current)-refined_positions.at(previous),refined_positions.at(next)-refined_positions.at(previous));
          const auto signed_area=dot_vec(normal,face_normal);
          if(std::abs(signed_area)<=1e-24||signed_area*winding<0.)continue;
          // Do not consume the sole off-edge corner of a triangle-boundary
          // polygon: that produces a legal-looking first ear but leaves only
          // collinear vertices for the final triangle.
          std::vector<std::uint64_t> remainder;remainder.reserve(boundary.size()-1U);
          for(std::size_t j=0;j<boundary.size();++j)if(j!=i)remainder.push_back(boundary[j]);
          bool remainder_has_area{};
          for(std::size_t a=0;a<remainder.size()&&!remainder_has_area;++a)for(std::size_t b=a+1U;b<remainder.size()&&!remainder_has_area;++b)for(std::size_t c=b+1U;c<remainder.size();++c){
            const auto remainder_normal=cross_vec(refined_positions.at(remainder[b])-refined_positions.at(remainder[a]),refined_positions.at(remainder[c])-refined_positions.at(remainder[a]));
            if(std::abs(dot_vec(remainder_normal,face_normal))>1e-24){remainder_has_area=true;break;}
          }
          if(!remainder_has_area)continue;
          topology.triangles.push_back({{previous,current,next}});boundary.erase(boundary.begin()+static_cast<std::ptrdiff_t>(i));clipped=true;break;
        }
        if(!clipped)return {};
      }
      const auto final_normal=cross_vec(refined_positions.at(boundary[1])-refined_positions.at(boundary[0]),refined_positions.at(boundary[2])-refined_positions.at(boundary[0]));
      if(dot_vec(final_normal,final_normal)<=1e-24)return {};
      topology.triangles.push_back({{boundary[0],boundary[1],boundary[2]}});
    }
    topologies.push_back(std::move(topology));
  }
  const auto refined=materialize_regular_core_arbitrary_face_refinement(parents,roots,plan,topologies);if(!refined.accepted())return result;
  const auto canonical_id=[&](std::uint64_t id){for(const auto& [actual,planned]:actual_to_plan)if(planned==id)return actual;return id;};
  for(const auto& vertex:refined.vertices)result.vertices.push_back({canonical_id(vertex.id),{vertex.point.x,vertex.point.y,vertex.point.z}});
  for(auto tet:refined.tetrahedra){for(auto& vertex:tet)vertex=canonical_id(vertex);result.tetrahedra.push_back(tet);}result.accepted=true;return result;
}
Vec3 vector_cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double vector_dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
std::pair<double,double> dihedral_range(const std::vector<Vec3>& vertices,const std::vector<std::array<std::uint32_t,4>>& cells){
  double lo=180.,hi=0.;for(const auto& t:cells)for(unsigned u=0;u<4U;++u)for(unsigned v=u+1U;v<4U;++v){unsigned a=4U,b=4U;for(unsigned i=0;i<4U;++i)if(i!=u&&i!=v){if(a==4U)a=i;else b=i;}const auto n1=vector_cross(vertices[t[v]]-vertices[t[u]],vertices[t[a]]-vertices[t[u]]),n2=vector_cross(vertices[t[u]]-vertices[t[v]],vertices[t[b]]-vertices[t[v]]);const auto den=std::sqrt(vector_dot(n1,n1)*vector_dot(n2,n2));if(den<=1e-20)return {0.,180.};const auto angle=(std::acos(-1.)-std::acos(std::clamp(vector_dot(n1,n2)/den,-1.,1.)))*180./std::acos(-1.);lo=std::min(lo,angle);hi=std::max(hi,angle);}return {lo,hi};
}
}

CanonicalPlcVolumeResult construct_canonical_plc_volume(const NonmatchingPlcManifestResult& manifest,const CanonicalPlcRecoveryOptions& options){
  CanonicalPlcVolumeResult result;const auto recovery=recover_canonical_plc_edges(manifest,options);if(!recovery.accepted()){result.failure=recovery.failure==CanonicalPlcRecoveryFailure::core_refinement_required?CanonicalPlcVolumeFailure::core_refinement_required:CanonicalPlcVolumeFailure::constraint_recovery_failed;return result;}
  if(recovery.tetrahedra.empty()){result.failure=CanonicalPlcVolumeFailure::seed_failed;return result;}
  const auto refined_core=refine_canonical_core(manifest,recovery.constraints);if(!refined_core.accepted){result.failure=CanonicalPlcVolumeFailure::core_refinement_required;return result;}
  auto all_vertices=recovery.constraints.vertices;std::map<std::uint64_t,std::uint32_t> index;for(std::size_t i=0;i<all_vertices.size();++i)if(!index.emplace(all_vertices[i].id,static_cast<std::uint32_t>(i)).second){result.failure=CanonicalPlcVolumeFailure::geometry_rejected;return result;}
  for(const auto& vertex:refined_core.vertices)if(!index.contains(vertex.id)){index.emplace(vertex.id,static_cast<std::uint32_t>(all_vertices.size()));all_vertices.push_back(vertex);}
  std::vector<Point> points;for(const auto& vertex:all_vertices)points.push_back({vertex.position.x,vertex.position.y,vertex.position.z});
  std::vector<std::array<std::uint32_t,4>> core;for(const auto& tet:refined_core.tetrahedra){std::array<std::uint32_t,4> mapped{};for(unsigned i=0;i<4U;++i){const auto it=index.find(tet[i]);if(it==index.end())return result;mapped[i]=it->second;}core.push_back(mapped);}
  using StableFace=std::array<std::uint64_t,3>;
  std::set<StableFace> recovered_mesh_faces;
  for(const auto& tet:recovery.tetrahedra)for(unsigned omit=0U;omit<4U;++omit){StableFace face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=recovery.constraints.vertices[tet[i]].id;std::sort(face.begin(),face.end());recovered_mesh_faces.insert(face);}
  std::map<FrozenFacetIdentity,std::vector<StableFace>> recovered_patch_faces;
  const auto recovered_patches=recovered_parent_patches(recovery.constraints,recovered_mesh_faces,&recovered_patch_faces);
  if(std::any_of(recovered_patches.begin(),recovered_patches.end(),[](const auto& item){return !item.second;})||core.empty())return result;
  std::map<FrozenFacetIdentity,bool> parent_is_core;
  std::map<FrozenFacetIdentity,std::map<std::uint64_t,FacetBarycentricPoint>> parent_points;
  for(const auto& facet:recovery.constraints.facets){parent_is_core.emplace(facet.parent,facet.core_interface);for(unsigned i=0U;i<3U;++i)parent_points[facet.parent].emplace(facet.vertices[i],facet.corners[i]);}
  // The regular core is generated independently, so its initial boundary can
  // use a different coplanar diagonal from the recovered PLC patch. Conform
  // that explicit core boundary transactionally to the actual recovered
  // patch before classification/assembly; never publish mismatched fronts.
  CanonicalPlcConstraintSet core_constraints;core_constraints.vertices=all_vertices;
  for(const auto& [parent,faces]:recovered_patch_faces)if(parent_is_core.at(parent))for(const auto& face:faces){CanonicalPlcConstraintFacet facet;facet.parent=parent;facet.vertices=face;facet.source_vertices=parent.vertex_ids;facet.core_interface=true;for(unsigned i=0U;i<3U;++i)facet.corners[i]=parent_points.at(parent).at(face[i]);core_constraints.facets.push_back(facet);}
  for(const auto& [parent,faces]:recovered_patch_faces)if(parent_is_core.at(parent)&&!faces.empty()) {
    const auto conformed=recover_literal_facet_by_two_sided_cavity(
        core_constraints,faces.front(),core,
        options.maximum_facet_cavity_vertices,
        options.maximum_facet_retriangulation_trials,
        options.maximum_facet_cavity_expansions);
    if(!conformed.accepted){result.failure=CanonicalPlcVolumeFailure::core_refinement_required;return result;}
    core=conformed.tetrahedra;
  }
  CanonicalPlcRegionInput region_input;region_input.tetrahedra=recovery.tetrahedra;region_input.coordinate_scale=1.;
  for(const auto& vertex:all_vertices){region_input.vertices.push_back(vertex.position);region_input.coordinate_scale=std::max({region_input.coordinate_scale,std::abs(vertex.position.x),std::abs(vertex.position.y),std::abs(vertex.position.z)});}
  region_input.coordinate_scale*=2.;
  for(const auto& [parent,faces]:recovered_patch_faces)for(const auto& stable_face:faces){std::array<std::uint32_t,3> face{};for(unsigned i=0;i<3U;++i)face[i]=index.at(stable_face[i]);(parent_is_core.at(parent)?region_input.core_faces:region_input.outer_faces).push_back(face);}
  // Seed-cell centroids are strictly interior to their own background cells.
  // Use the known retained-core geometry only to nominate core-side cells;
  // classification itself then spreads through unconstrained faces. A refined
  // core-tet centroid can coincide with a Delaunay face in this deliberately
  // degenerate regular control and is not a safe background-cell witness.
  for(const auto& tet:recovery.tetrahedra){const auto witness=(points[tet[0]]+points[tet[1]]+points[tet[2]]+points[tet[3]])*.25L;bool inside_core=false;for(const auto& core_tet:core)inside_core=inside_core||point_in_tet(witness,core_tet,points);if(inside_core)region_input.core_witnesses.push_back({static_cast<double>(witness.x),static_cast<double>(witness.y),static_cast<double>(witness.z)});}
  if(region_input.core_witnesses.empty()){result.failure=CanonicalPlcVolumeFailure::domain_classification_failed;result.region_failure=CanonicalPlcRegionFailure::core_witness_outside_mesh;return result;}
  const auto regions=classify_canonical_plc_regions(region_input);if(!regions.accepted()){result.failure=CanonicalPlcVolumeFailure::domain_classification_failed;result.region_failure=regions.failure;return result;}
  // Keep the mutable shell separate from the retained core.  Quality moves
  // replace shell cells, but must neither interleave replacement cells with
  // the core nor make index ranges an implicit ownership mechanism.
  std::vector<std::array<std::uint32_t,4>> shell;for(std::size_t i=0;i<recovery.tetrahedra.size();++i)if(regions.regions[i]==CanonicalPlcCellRegion::shell)shell.push_back(recovery.tetrahedra[i]);
  result.shell_tetrahedra=shell.size();result.core_tetrahedra=core.size();
  auto assemble_shell_before_core=[&](const std::vector<std::array<std::uint32_t,4>>& candidate_shell){auto combined=candidate_shell;combined.insert(combined.end(),core.begin(),core.end());return combined;};
  auto shell_from_assembled=[&](const std::vector<std::array<std::uint32_t,4>>& candidate){std::vector<std::array<std::uint32_t,4>> candidate_shell;candidate_shell.reserve(candidate.size()-core.size());for(const auto& tet:candidate)if(std::find(core.begin(),core.end(),tet)==core.end())candidate_shell.push_back(tet);return candidate_shell;};
  std::vector<std::array<std::uint32_t,4>> assembled=assemble_shell_before_core(shell);
  SurfaceCoreTransitionInput input;for(const auto& vertex:all_vertices){input.vertices.push_back(vertex.position);input.stable_vertex_ids.push_back(vertex.id);}input.retained_core_tetrahedra=core;double scale=1.;for(const auto& p:input.vertices)scale=std::max({scale,std::abs(p.x),std::abs(p.y),std::abs(p.z)});input.coordinate_scale=scale*2.;
  std::map<FrozenFacetIdentity,std::size_t> ordinals;for(std::size_t i=0;i<manifest.manifest.outer_parent_coverage.size();++i){const auto& split=manifest.manifest.outer_parent_coverage[i];const auto winding=manifest.manifest.outer_parent_windings[i];std::array<std::uint32_t,3> face{};for(unsigned j=0;j<3U;++j)face[j]=index.at(winding[j]);input.outer_faces.push_back(face);input.outer_parent_facets.push_back({split.parent,FacetPreservationMode::geometric});}
  SurfaceCoreTransitionOutput output;output.tetrahedra=assembled;
  for(const auto& [parent,faces]:recovered_patch_faces)if(!parent_is_core.at(parent))for(const auto& face:faces){OutputFacetSubface record;record.exact.parent=parent;record.exact.ordinal=static_cast<std::uint32_t>(ordinals[parent]++);record.exact.owner_chunk=0U;record.exact.emitted_by_local_chunk=true;for(unsigned i=0;i<3U;++i){record.vertices[i]=index.at(face[i]);record.exact.corners[i]=parent_points.at(parent).at(face[i]);}const auto& a=record.exact.corners[0];const auto& b=record.exact.corners[1];const auto& c=record.exact.corners[2];const auto ax=static_cast<long double>(a.numerator[1])/a.denominator,ay=static_cast<long double>(a.numerator[2])/a.denominator;const auto bx=static_cast<long double>(b.numerator[1])/b.denominator,by=static_cast<long double>(b.numerator[2])/b.denominator;const auto cx=static_cast<long double>(c.numerator[1])/c.denominator,cy=static_cast<long double>(c.numerator[2])/c.denominator;if((bx-ax)*(cy-ay)-(by-ay)*(cx-ax)<0.0L){std::swap(record.vertices[1],record.vertices[2]);std::swap(record.exact.corners[1],record.exact.corners[2]);}output.outer_preserved_facets.push_back(record);}
  // Bounded final-assembly repair: inspect every free shell face and every
  // degree-three free shell edge. A 2→3 or 3→2 flip leaves all frozen
  // outer/core faces and the refined core untouched. The complete validator
  // remains the sole authority for accepting a candidate.
  auto current_range=dihedral_range(input.vertices,assembled);
  std::map<Face,std::vector<std::pair<std::size_t,std::uint32_t>>> shell_faces;
  for(std::size_t ti=0;ti<result.shell_tetrahedra;++ti)for(unsigned omit=0;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0;i<4U;++i)if(i!=omit)face[n++]=assembled[ti][i];shell_faces[face_key(face)].push_back({ti,assembled[ti][omit]});}
  std::set<Face> frozen_faces;for(const auto& [parent,faces]:recovered_patch_faces){(void)parent;for(const auto& stable_face:faces){Face face{};for(unsigned i=0;i<3U;++i)face[i]=index.at(stable_face[i]);frozen_faces.insert(face_key(face));}}
  std::vector<std::array<std::uint32_t,4>> best=assembled;bool best_changed{};
  for(std::size_t selected_shell=0U;selected_shell<result.shell_tetrahedra;++selected_shell)for(unsigned omit=0;omit<4U;++omit){Face shared{};unsigned n{};for(unsigned i=0;i<4U;++i)if(i!=omit)shared[n++]=assembled[selected_shell][i];const auto key=face_key(shared);const auto found=shell_faces.find(key);if(found==shell_faces.end()||found->second.size()!=2U||frozen_faces.contains(key))continue;
    const auto left=found->second[0].first,right=found->second[1].first;if(left==right)continue;const auto left_apex=found->second[0].second,right_apex=found->second[1].second;
    std::array<std::array<std::uint32_t,4>,3> replacement{{{{left_apex,right_apex,shared[0],shared[1]}},{{left_apex,right_apex,shared[1],shared[2]}},{{left_apex,right_apex,shared[2],shared[0]}}}};
    bool positive=true;for(auto& tet:replacement){const auto volume=orient(points[tet[0]],points[tet[1]],points[tet[2]],points[tet[3]]);if(std::abs(volume)<=1e-20L){positive=false;break;}if(volume<0.)std::swap(tet[0],tet[1]);}if(!positive)continue;++result.quality_repair_candidates;
    std::vector<std::array<std::uint32_t,4>> candidate;candidate.reserve(assembled.size()+1U);for(std::size_t i=0;i<assembled.size();++i)if(i!=left&&i!=right)candidate.push_back(assembled[i]);candidate.insert(candidate.end(),replacement.begin(),replacement.end());
    SurfaceCoreTransitionOutput candidate_output=output;candidate_output.tetrahedra=candidate;const auto validation=validate_surface_core_transition_output(input,candidate_output);const auto candidate_range=dihedral_range(input.vertices,candidate);
    if(validation.valid&&candidate_range.first>current_range.first+1e-9&&(candidate_range.second<=current_range.second+1e-9)){best=std::move(candidate);current_range=candidate_range;best_changed=true;}
  }
  constexpr std::array<std::array<unsigned,2>,6> edge_corners{{{{0,1}},{{0,2}},{{0,3}},{{1,2}},{{1,3}},{{2,3}}}};
  for(std::size_t selected_shell=0U;selected_shell<result.shell_tetrahedra;++selected_shell)for(const auto corner:edge_corners){
    auto edge=std::array<std::uint32_t,2>{{assembled[selected_shell][corner[0]],assembled[selected_shell][corner[1]]}};if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
    std::vector<std::size_t> star;for(std::size_t ti=0;ti<result.shell_tetrahedra;++ti)if(std::find(assembled[ti].begin(),assembled[ti].end(),edge[0])!=assembled[ti].end()&&std::find(assembled[ti].begin(),assembled[ti].end(),edge[1])!=assembled[ti].end())star.push_back(ti);
    if(star.size()!=3U)continue;
    std::vector<std::uint32_t> rim;for(const auto ti:star)for(const auto vertex:assembled[ti])if(vertex!=edge[0]&&vertex!=edge[1]&&std::find(rim.begin(),rim.end(),vertex)==rim.end())rim.push_back(vertex);
    if(rim.size()!=3U)continue;
    std::sort(rim.begin(),rim.end());
    std::array<std::array<std::uint32_t,4>,2> replacement{{{{rim[0],rim[1],rim[2],edge[0]}},{{rim[0],rim[2],rim[1],edge[1]}}}};
    bool positive=true;for(auto& tet:replacement){const auto volume=orient(points[tet[0]],points[tet[1]],points[tet[2]],points[tet[3]]);if(std::abs(volume)<=1e-20L){positive=false;break;}if(volume<0.)std::swap(tet[0],tet[1]);}if(!positive)continue;++result.quality_repair_candidates;
    std::vector<std::array<std::uint32_t,4>> candidate;candidate.reserve(assembled.size()-1U);for(std::size_t i=0;i<assembled.size();++i)if(std::find(star.begin(),star.end(),i)==star.end())candidate.push_back(assembled[i]);candidate.insert(candidate.end(),replacement.begin(),replacement.end());
    SurfaceCoreTransitionOutput candidate_output=output;candidate_output.tetrahedra=candidate;const auto validation=validate_surface_core_transition_output(input,candidate_output);const auto candidate_range=dihedral_range(input.vertices,candidate);
    if(validation.valid&&candidate_range.first>current_range.first+1e-9&&(candidate_range.second<=current_range.second+1e-9)){best=std::move(candidate);current_range=candidate_range;best_changed=true;}
  }
  // A degree-four edge star admits two 4→4 diagonal exchanges.  Recover the
  // cyclic rim from the actual star, rather than stable-ID order, so the
  // replacement has exactly the old cavity boundary.
  for(std::size_t selected_shell=0U;selected_shell<result.shell_tetrahedra;++selected_shell)for(const auto corner:edge_corners){
    auto axis=std::array<std::uint32_t,2>{{assembled[selected_shell][corner[0]],assembled[selected_shell][corner[1]]}};if(axis[1]<axis[0])std::swap(axis[0],axis[1]);
    std::vector<std::size_t> star;for(std::size_t ti=0;ti<result.shell_tetrahedra;++ti)if(std::find(assembled[ti].begin(),assembled[ti].end(),axis[0])!=assembled[ti].end()&&std::find(assembled[ti].begin(),assembled[ti].end(),axis[1])!=assembled[ti].end())star.push_back(ti);
    if(star.size()!=4U)continue;
    std::map<std::uint32_t,std::vector<std::uint32_t>> rim_adjacency;
    for(const auto ti:star){std::array<std::uint32_t,2> pair{};unsigned count{};for(const auto vertex:assembled[ti])if(vertex!=axis[0]&&vertex!=axis[1])pair[count++]=vertex;if(count!=2U)continue;rim_adjacency[pair[0]].push_back(pair[1]);rim_adjacency[pair[1]].push_back(pair[0]);}
    if(rim_adjacency.size()!=4U||std::any_of(rim_adjacency.begin(),rim_adjacency.end(),[](const auto& entry){return entry.second.size()!=2U;}))continue;
    std::vector<std::uint32_t> rim{rim_adjacency.begin()->first};
    while(rim.size()<4U){const auto& choices=rim_adjacency.at(rim.back());const auto next=choices[0]==(rim.size()>1U?rim[rim.size()-2U]:std::numeric_limits<std::uint32_t>::max())?choices[1]:choices[0];if(std::find(rim.begin(),rim.end(),next)!=rim.end())break;rim.push_back(next);}
    if(rim.size()!=4U||std::find(rim_adjacency.at(rim.back()).begin(),rim_adjacency.at(rim.back()).end(),rim.front())==rim_adjacency.at(rim.back()).end())continue;
    for(const auto diagonal:std::array<std::array<unsigned,2>,2>{{{{0U,2U}},{{1U,3U}}}}){const auto left=rim[diagonal[0]],right=rim[diagonal[1]];const auto middle_a=rim[(diagonal[0]+1U)%4U],middle_b=rim[(diagonal[0]+3U)%4U];std::array<std::array<std::uint32_t,4>,4> replacement{{{{left,right,axis[0],middle_a}},{{left,right,middle_a,axis[1]}},{{left,right,axis[1],middle_b}},{{left,right,middle_b,axis[0]}}}};
      bool positive=true;for(auto& tet:replacement){const auto volume=orient(points[tet[0]],points[tet[1]],points[tet[2]],points[tet[3]]);if(std::abs(volume)<=1e-20L){positive=false;break;}if(volume<0.)std::swap(tet[0],tet[1]);}if(!positive)continue;++result.quality_repair_candidates;
      std::vector<std::array<std::uint32_t,4>> candidate;candidate.reserve(assembled.size());for(std::size_t i=0;i<assembled.size();++i)if(std::find(star.begin(),star.end(),i)==star.end())candidate.push_back(assembled[i]);candidate.insert(candidate.end(),replacement.begin(),replacement.end());
      SurfaceCoreTransitionOutput candidate_output=output;candidate_output.tetrahedra=candidate;const auto validation=validate_surface_core_transition_output(input,candidate_output);const auto candidate_range=dihedral_range(input.vertices,candidate);
      if(validation.valid&&candidate_range.first>current_range.first+1e-9&&(candidate_range.second<=current_range.second+1e-9)){best=std::move(candidate);current_range=candidate_range;best_changed=true;}
    }
  }
  if(best_changed){
    // Candidates above are assembled for validation.  Rebuild the public
    // ordering from the selected shell portion so even a count-preserving
    // 4→4 exchange is installed and later stages see the true shell.
    shell=shell_from_assembled(best);
    assembled=assemble_shell_before_core(shell);output.tetrahedra=assembled;
    result.shell_tetrahedra=shell.size();++result.quality_repair_accepted;
  }
  // Test deterministic interior Steiner candidates transactionally.  Each
  // centroid split preserves its parent tet boundary, hence cannot alter a
  // frozen face; the full validator remains the acceptance authority.
  current_range=dihedral_range(input.vertices,assembled);std::vector<std::array<std::uint32_t,4>> steiner_best;std::optional<FrozenFacetVertex> steiner_vertex;std::uint64_t next_id{};for(const auto id:input.stable_vertex_ids)next_id=std::max(next_id,id);
  if(next_id!=std::numeric_limits<std::uint64_t>::max()){++next_id;for(std::size_t selected_shell=0U;selected_shell<result.shell_tetrahedra;++selected_shell){const auto& tet=assembled[selected_shell];const auto centroid=(input.vertices[tet[0]]+input.vertices[tet[1]]+input.vertices[tet[2]]+input.vertices[tet[3]])/4.0;SurfaceCoreTransitionInput candidate_input=input;const auto apex=static_cast<std::uint32_t>(candidate_input.vertices.size());candidate_input.vertices.push_back(centroid);candidate_input.stable_vertex_ids.push_back(next_id);std::array<std::array<std::uint32_t,4>,4> replacement{{{{apex,tet[1],tet[2],tet[3]}},{{tet[0],apex,tet[2],tet[3]}},{{tet[0],tet[1],apex,tet[3]}},{{tet[0],tet[1],tet[2],apex}}}};bool positive=true;for(auto& child:replacement){const auto a=candidate_input.vertices[child[0]],b=candidate_input.vertices[child[1]],c=candidate_input.vertices[child[2]],d=candidate_input.vertices[child[3]];const auto volume=orient({a.x,a.y,a.z},{b.x,b.y,b.z},{c.x,c.y,c.z},{d.x,d.y,d.z});if(std::abs(volume)<=1e-20L){positive=false;break;}if(volume<0.)std::swap(child[0],child[1]);}if(!positive)continue;++result.quality_repair_candidates;std::vector<std::array<std::uint32_t,4>> candidate;candidate.reserve(assembled.size()+3U);for(std::size_t index=0U;index<assembled.size();++index)if(index!=selected_shell)candidate.push_back(assembled[index]);candidate.insert(candidate.end(),replacement.begin(),replacement.end());SurfaceCoreTransitionOutput candidate_output=output;candidate_output.tetrahedra=candidate;const auto validation=validate_surface_core_transition_output(candidate_input,candidate_output);const auto candidate_range=dihedral_range(candidate_input.vertices,candidate);if(validation.valid&&candidate_range.first>current_range.first+1e-9&&(candidate_range.second<=current_range.second+1e-9)){steiner_best=std::move(candidate);steiner_vertex=FrozenFacetVertex{next_id,centroid};current_range=candidate_range;}}}
  if(steiner_vertex){all_vertices.push_back(*steiner_vertex);input.vertices.push_back(steiner_vertex->position);input.stable_vertex_ids.push_back(steiner_vertex->id);shell=shell_from_assembled(steiner_best);assembled=assemble_shell_before_core(shell);output.tetrahedra=assembled;result.shell_tetrahedra=shell.size();++result.quality_repair_accepted;}
  // Two shell tets joined across a free face form the smallest multi-tet
  // cavity. Cone its unchanged six-face boundary to a deterministic interior
  // point and submit the entire expanded transaction to the validator.
  current_range=dihedral_range(input.vertices,assembled);std::vector<std::array<std::uint32_t,4>> cavity_best;std::optional<FrozenFacetVertex> cavity_vertex;std::uint64_t cavity_id{};for(const auto id:input.stable_vertex_ids)cavity_id=std::max(cavity_id,id);
  if(cavity_id!=std::numeric_limits<std::uint64_t>::max()){++cavity_id;std::map<Face,std::vector<std::size_t>> face_uses;for(std::size_t ti=0U;ti<result.shell_tetrahedra;++ti)for(unsigned omit=0U;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=assembled[ti][i];face_uses[face_key(face)].push_back(ti);}for(const auto& [shared,uses]:face_uses){if(uses.size()!=2U||frozen_faces.contains(shared))continue;std::map<Face,unsigned> cavity_faces;std::set<std::uint32_t> cavity_points;for(const auto ti:uses){cavity_points.insert(assembled[ti].begin(),assembled[ti].end());for(unsigned omit=0U;omit<4U;++omit){Face face{};unsigned n{};for(unsigned i=0U;i<4U;++i)if(i!=omit)face[n++]=assembled[ti][i];++cavity_faces[face_key(face)];}}Vec3 centroid{};for(const auto vertex:cavity_points)centroid=centroid+input.vertices[vertex];centroid=centroid/static_cast<double>(cavity_points.size());SurfaceCoreTransitionInput candidate_input=input;const auto apex=static_cast<std::uint32_t>(candidate_input.vertices.size());candidate_input.vertices.push_back(centroid);candidate_input.stable_vertex_ids.push_back(cavity_id);std::vector<std::array<std::uint32_t,4>> replacement;bool positive=true;for(const auto& [face,count]:cavity_faces)if(count==1U){std::array<std::uint32_t,4> child{{apex,face[0],face[1],face[2]}};const auto a=candidate_input.vertices[child[0]],b=candidate_input.vertices[child[1]],c=candidate_input.vertices[child[2]],d=candidate_input.vertices[child[3]];const auto volume=orient({a.x,a.y,a.z},{b.x,b.y,b.z},{c.x,c.y,c.z},{d.x,d.y,d.z});if(std::abs(volume)<=1e-20L){positive=false;break;}if(volume<0.)std::swap(child[0],child[1]);replacement.push_back(child);}if(!positive||replacement.size()!=6U)continue;++result.quality_repair_candidates;std::vector<std::array<std::uint32_t,4>> candidate;candidate.reserve(assembled.size()+4U);for(std::size_t ti=0U;ti<assembled.size();++ti)if(ti!=uses[0]&&ti!=uses[1])candidate.push_back(assembled[ti]);candidate.insert(candidate.end(),replacement.begin(),replacement.end());SurfaceCoreTransitionOutput candidate_output=output;candidate_output.tetrahedra=candidate;const auto validation=validate_surface_core_transition_output(candidate_input,candidate_output);const auto candidate_range=dihedral_range(candidate_input.vertices,candidate);if(validation.valid&&candidate_range.first>current_range.first+1e-9&&(candidate_range.second<=current_range.second+1e-9)){cavity_best=std::move(candidate);cavity_vertex=FrozenFacetVertex{cavity_id,centroid};current_range=candidate_range;}}}
  if(cavity_vertex){all_vertices.push_back(*cavity_vertex);input.vertices.push_back(cavity_vertex->position);input.stable_vertex_ids.push_back(cavity_vertex->id);shell=shell_from_assembled(cavity_best);assembled=assemble_shell_before_core(shell);output.tetrahedra=assembled;result.shell_tetrahedra=shell.size();++result.quality_repair_accepted;}
  result.validation=validate_surface_core_transition_output(input,output);if(!result.validation.valid){result.failure=CanonicalPlcVolumeFailure::geometry_rejected;return result;}const auto range=dihedral_range(input.vertices,assembled);result.minimum_dihedral_degrees=range.first;result.maximum_dihedral_degrees=range.second;if(range.first<5.||range.second>175.){result.failure=CanonicalPlcVolumeFailure::quality_rejected;return result;}
  result.vertices=all_vertices;for(const auto& tet:assembled){std::array<std::uint64_t,4> ids{};for(unsigned i=0;i<4U;++i)ids[i]=input.stable_vertex_ids[tet[i]];result.tetrahedra.push_back(ids);}result.failure=CanonicalPlcVolumeFailure::none;return result;
}
} // namespace tetra::probes
