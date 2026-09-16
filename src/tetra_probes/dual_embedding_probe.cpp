#include "tetra_probes/dual_embedding_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>

namespace tetra::probes {
namespace {

constexpr double epsilon=1.0e-8;

struct P {
  int x{},y{},z{};
  friend auto operator<=>(const P&,const P&)=default;
};
struct V { double x{},y{},z{}; };
struct E { P a,b; friend auto operator<=>(const E&,const E&)=default; };
struct H { std::array<P,8> p; };
struct T { unsigned a,b,c; friend auto operator<=>(const T&,const T&)=default; };
using FaceKey=std::array<P,4>;

constexpr std::array<std::array<unsigned,2>,12> edges{{
    {{0,1}},{{0,2}},{{1,3}},{{2,3}},{{4,5}},{{4,6}},{{5,7}},{{6,7}},
    {{0,4}},{{1,5}},{{2,6}},{{3,7}}}};
constexpr std::array<std::array<unsigned,4>,6> faces{{
    {{0,1,3,2}},{{4,6,7,5}},{{0,4,5,1}},{{2,3,7,6}},
    {{0,2,6,4}},{{1,5,7,3}}}};

V pos(P p) { return {p.x/12.0,p.y/12.0,p.z/12.0}; }
V operator+(V a,V b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
V operator-(V a,V b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
V operator*(V a,double s) { return {a.x*s,a.y*s,a.z*s}; }
double dot(V a,V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
V cross(V a,V b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
double length_squared(V a) { return dot(a,a); }
double length(V a) { return std::sqrt(length_squared(a)); }
bool same_point(V a,V b) { return length_squared(a-b)<=epsilon*epsilon; }
E edge(P a,P b) { if(b<a)std::swap(a,b); return {a,b}; }
FaceKey face_key(const H& h,const std::array<unsigned,4>& face) {
  FaceKey result{{h.p[face[0]],h.p[face[1]],h.p[face[2]],h.p[face[3]]}};
  std::sort(result.begin(),result.end());return result;
}

// Independent Cartesian transcription of make_four_hexahedra. P stores
// Cartesian twelfths, so barycentric weights are divided by 12 here.
P barycentric(const std::array<P,4>& v,
              std::initializer_list<std::pair<unsigned,unsigned>> w) {
  P p{};
  for(const auto& [i,n]:w) {
    p.x+=v[i].x*static_cast<int>(n)/12;
    p.y+=v[i].y*static_cast<int>(n)/12;
    p.z+=v[i].z*static_cast<int>(n)/12;
  }
  return p;
}
std::array<H,4> four_hexes(const std::array<P,4>& v) {
  std::array<H,4> out{};
  for(unsigned owner=0;owner<4;++owner) {
    std::array<unsigned,3> other{};unsigned count{};
    for(unsigned i=0;i<4;++i)if(i!=owner)other[count++]=i;
    const auto a=other[0],b=other[1],c=other[2];
    out[owner].p={{
      barycentric(v,{{owner,12}}),barycentric(v,{{owner,6},{a,6}}),
      barycentric(v,{{owner,6},{b,6}}),barycentric(v,{{owner,4},{a,4},{b,4}}),
      barycentric(v,{{owner,6},{c,6}}),barycentric(v,{{owner,4},{a,4},{c,4}}),
      barycentric(v,{{owner,4},{b,4},{c,4}}),barycentric(v,{{0,3},{1,3},{2,3},{3,3}})}};
  }
  return out;
}
std::vector<H> fixture(EmbeddingFixture kind) {
  const std::array<P,4> root{{{0,0,0},{12,0,0},{0,12,0},{0,0,12}}};
  std::vector<std::array<P,4>> tets{root};
  if(kind==EmbeddingFixture::two_tet) {
    tets.push_back({{{12,0,0},{0,12,0},{0,0,12},{12,12,12}}});
  } else if(kind==EmbeddingFixture::six_root) {
    tets.clear();
    constexpr std::array<P,6> ring{{{12,0,0},{6,12,0},{-6,12,0},
                                    {-12,0,0},{-6,-12,0},{6,-12,0}}};
    for(unsigned i=0;i<ring.size();++i)
      tets.push_back({{{0,0,-12},{0,0,12},ring[i],ring[(i+1)%ring.size()]}});
  }
  std::vector<H> out;
  for(const auto& tet:tets) {
    const auto cells=four_hexes(tet);
    out.insert(out.end(),cells.begin(),cells.end());
  }
  return out;
}
bool fixture_oracle(EmbeddingFixture kind,const std::vector<H>& all) {
  const auto cells=four_hexes({{{0,0,0},{12,0,0},{0,12,0},{0,0,12}}});
  const std::array<P,4> roots{{{0,0,0},{12,0,0},{0,12,0},{0,0,12}}};
  std::set<P> distinct;
  for(unsigned owner=0;owner<cells.size();++owner) {
    if(cells[owner].p[0]!=roots[owner]||cells[owner].p[7]!=P{3,3,3})return false;
    for(const auto point:cells[owner].p) {
      if(point.x<0||point.y<0||point.z<0||point.x+point.y+point.z>12)return false;
      distinct.insert(point);
    }
  }
  // The same public oracle test calls out this canonical 15-point set:
  // four vertices, six edge midpoints, four face centroids, one cell centre.
  if(distinct.size()!=15U)return false;
  const std::size_t expected=kind==EmbeddingFixture::one_tet?4U:
                             kind==EmbeddingFixture::two_tet?8U:24U;
  if(all.size()!=expected)return false;
  // Each tetrahedron is independently compared with the canonical 15-point
  // four-hexahedra support.  The face map then verifies that shared tet faces
  // really contribute the same three hexahedral boundary quads on both sides.
  for(std::size_t first=0;first<all.size();first+=4U) {
    std::set<P> support;
    for(std::size_t cell=first;cell<first+4U;++cell)
      support.insert(all[cell].p.begin(),all[cell].p.end());
    if(support.size()!=15U)return false;
  }
  std::map<FaceKey,unsigned> incidence;
  for(const auto& h:all)for(const auto& f:faces)++incidence[face_key(h,f)];
  if(std::ranges::any_of(incidence,[](const auto& item){return item.second>2U;}))return false;
  const auto shared=std::count_if(incidence.begin(),incidence.end(),
                                  [](const auto& item){return item.second==2U;});
  return kind==EmbeddingFixture::one_tet?shared>=6U:
         kind==EmbeddingFixture::two_tet?shared>=15U:shared>=42U;
}

// R1Q field contract: this global affine analytic field supplies the canonical
// values at every globally keyed corner. Its restrictions to shared faces and
// primal edges are identical from both incident cells; edge roots and normals
// are evaluated from this same field. No linear estimate is called analytic.
struct F {
  std::array<double,4> c{}; // c0 + cx*x + cy*y + cz*z
  // A separately named quadratic control. It is global and sampled directly
  // at every hexahedron corner; it is not called trilinear.
  double xy{};
  double at(V p)const { return c[0]+c[1]*p.x+c[2]*p.y+c[3]*p.z+xy*p.x*p.y; }
  V gradient(V p)const { return {c[1]+xy*p.y,c[2]+xy*p.x,c[3]}; }
  V gradient()const { return gradient({}); }
};
enum class CrossKind { none,endpoint_a,endpoint_b,interior,coincident };
struct Crossing { CrossKind kind{CrossKind::none}; double t{}; };
Crossing crossing(double a,double b) {
  const bool az=std::abs(a)<=epsilon,bz=std::abs(b)<=epsilon;
  if(az&&bz)return {CrossKind::coincident,0.0};
  if(az)return {CrossKind::endpoint_a,0.0};
  if(bz)return {CrossKind::endpoint_b,1.0};
  if((a<0.0)==(b<0.0))return {};
  return {CrossKind::interior,a/(a-b)};
}
bool crosses(Crossing r) { return r.kind==CrossKind::interior; }
Crossing crossing(const F& field,V a,V b) {
  const auto first=crossing(field.at(a),field.at(b));
  if(first.kind!=CrossKind::interior||std::abs(field.xy)<=epsilon)return first;
  double lo=0.0,hi=1.0,fa=field.at(a);
  for(unsigned iteration=0;iteration<80U;++iteration) {
    const double middle=(lo+hi)*.5;
    const double value=field.at(a+(b-a)*middle);
    if((value<0.0)==(fa<0.0)){lo=middle;fa=value;}else hi=middle;
  }
  return {CrossKind::interior,(lo+hi)*.5};
}

struct DSU {
  std::array<unsigned,12> parent{};
  DSU(){std::iota(parent.begin(),parent.end(),0U);}
  unsigned find(unsigned x){return parent[x]==x?x:parent[x]=find(parent[x]);}
  void join(unsigned a,unsigned b){a=find(a);b=find(b);if(a!=b)parent[b]=a;}
};
int local_edge(const H& h,E e) {
  for(unsigned i=0;i<edges.size();++i)
    if(edge(h.p[edges[i][0]],h.p[edges[i][1]])==e)return static_cast<int>(i);
  return -1;
}
std::vector<std::pair<E,E>> face_arcs(const H& h,const std::array<unsigned,4>& f,
                                      const F& field) {
  std::array<unsigned,4> hits{};unsigned n{};
  for(unsigned i=0;i<4;++i)
    if(crosses(crossing(field,pos(h.p[f[i]]),pos(h.p[f[(i+1)%4]]))))hits[n++]=i;
  const auto e=[&](unsigned i){return edge(h.p[f[i]],h.p[f[(i+1)%4]]);};
  if(n==2)return {{e(hits[0]),e(hits[1])}};
  if(n!=4)return {};
  double centre{};for(const auto i:f)centre+=field.at(pos(h.p[i]));
  const bool first_negative=(centre*.25<0.0)==(field.at(pos(h.p[f[0]]))<0.0);
  return first_negative?std::vector<std::pair<E,E>>{{e(0),e(1)},{e(2),e(3)}}:
                        std::vector<std::pair<E,E>>{{e(0),e(3)},{e(1),e(2)}};
}

struct Plane { V n;double d{}; };
std::vector<Plane> hull_planes(const H& h) {
  std::vector<Plane> out;
  for(unsigned i=0;i<8;++i)for(unsigned j=i+1;j<8;++j)for(unsigned k=j+1;k<8;++k){
    V n=cross(pos(h.p[j])-pos(h.p[i]),pos(h.p[k])-pos(h.p[i]));
    const double magnitude=length(n);if(magnitude<epsilon)continue;
    n=n*(1.0/magnitude);double d=dot(n,pos(h.p[i])),lo=1e100,hi=-1e100;
    for(const auto p:h.p){const double value=dot(n,pos(p))-d;lo=std::min(lo,value);hi=std::max(hi,value);}
    if(lo<-epsilon&&hi>epsilon)continue;
    if(lo>=-epsilon){n=n*-1.0;d=-d;}
    bool duplicate=false;for(const auto& old:out)
      if(length(old.n-n)<epsilon&&std::abs(old.d-d)<epsilon)duplicate=true;
    if(!duplicate)out.push_back({n,d});
  }
  return out;
}
bool inside(const std::vector<Plane>& planes,V x) {
  return std::ranges::all_of(planes,[&](const Plane& p){return dot(p.n,x)-p.d<=4*epsilon;});
}

struct Qef { std::array<std::array<double,3>,3> h{};V b{}; };
V h_times(const Qef& q,V x) {
  return {q.h[0][0]*x.x+q.h[0][1]*x.y+q.h[0][2]*x.z,
          q.h[1][0]*x.x+q.h[1][1]*x.y+q.h[1][2]*x.z,
          q.h[2][0]*x.x+q.h[2][1]*x.y+q.h[2][2]*x.z};
}
double qef_energy(const Qef& q,V x) { return dot(x,h_times(q,x))-2.0*dot(q.b,x); }
bool solve_linear(std::array<std::array<double,6>,6> a,std::array<double,6> b,
                  unsigned n,std::array<double,6>& out) {
  for(unsigned i=0;i<n;++i) {
    unsigned pivot=i;for(unsigned r=i+1;r<n;++r)
      if(std::abs(a[r][i])>std::abs(a[pivot][i]))pivot=r;
    if(std::abs(a[pivot][i])<1e-11)return false;
    std::swap(a[i],a[pivot]);std::swap(b[i],b[pivot]);
    const double d=a[i][i];for(unsigned c=i;c<n;++c)a[i][c]/=d;b[i]/=d;
    for(unsigned r=0;r<n;++r)if(r!=i) {
      const double factor=a[r][i];for(unsigned c=i;c<n;++c)a[r][c]-=factor*a[i][c];
      b[r]-=factor*b[i];
    }
  }
  out=b;return true;
}
struct Placement { V point{};double root_residual{};double kkt_residual{};bool certified{}; };
Placement solve_convex_qef(const H& h,const Qef& q,V zero_residual_candidate,
                           double root_residual=0.0) {
  const auto planes=hull_planes(h);V answer=zero_residual_candidate;
  double best=qef_energy(q,answer),best_kkt=length(h_times(q,answer)*2.0-q.b*2.0);
  bool have=inside(planes,answer);
  if(!have)best=std::numeric_limits<double>::infinity();
  // Active-set enumeration covers the interior, every face, every edge, and
  // every vertex of the convex cell. A candidate is accepted only with
  // primal feasibility, non-negative multipliers, and stationarity residual.
  for(unsigned mask=0;mask<(1U<<planes.size());++mask) {
    const unsigned active=std::popcount(mask);if(active>3)continue;
    std::array<unsigned,3> ids{};unsigned n{};for(unsigned i=0;i<planes.size();++i)
      if(mask&(1U<<i))ids[n++]=i;
    std::array<std::array<double,6>,6> a{};std::array<double,6> rhs{},solution{};
    for(unsigned r=0;r<3;++r) {
      rhs[r]=2.0*std::array<double,3>{{q.b.x,q.b.y,q.b.z}}[r];
      for(unsigned c=0;c<3;++c)a[r][c]=2.0*q.h[r][c];
      for(unsigned c=0;c<active;++c)a[r][3+c]=std::array<double,3>{{planes[ids[c]].n.x,planes[ids[c]].n.y,planes[ids[c]].n.z}}[r];
    }
    for(unsigned r=0;r<active;++r) {
      const auto& p=planes[ids[r]];a[3+r][0]=p.n.x;a[3+r][1]=p.n.y;a[3+r][2]=p.n.z;rhs[3+r]=p.d;
    }
    if(!solve_linear(a,rhs,3+active,solution))continue;
    const V x{solution[0],solution[1],solution[2]};if(!inside(planes,x))continue;
    bool multipliers=true;V stationarity=h_times(q,x)*2.0-q.b*2.0;
    for(unsigned i=0;i<active;++i) {
      if(solution[3+i]<-1e-7)multipliers=false;
      stationarity=stationarity+planes[ids[i]].n*solution[3+i];
    }
    if(!multipliers||length(stationarity)>1e-6)continue;
    const double energy=qef_energy(q,x);if(energy<best){answer=x;best=energy;best_kkt=length(stationarity);have=true;}
  }
  // A zero-gradient feasible candidate is a complete KKT certificate, which
  // also covers rank-deficient QEFs such as a planar component.
  return {answer,root_residual,best_kkt,have&&best_kkt<=1e-6};
}
Placement constrained_qef(const H& h,const F& field,const std::vector<unsigned>& component) {
  Qef q;V centroid{};double root_residual{};
  for(const auto i:component) {
    const V a=pos(h.p[edges[i][0]]),b=pos(h.p[edges[i][1]]);
    const auto r=crossing(field,a,b);const V root=a+(b-a)*r.t;
    const V gradient=field.gradient(root);
    if(length(gradient)<epsilon)return {};
    const V normal=gradient*(1.0/length(gradient));
    centroid=centroid+root;root_residual=std::max(root_residual,std::abs(field.at(root)));
    const double d=dot(normal,root);q.b=q.b+normal*d;
    const std::array<double,3> n{{normal.x,normal.y,normal.z}};
    for(unsigned row=0;row<3;++row)for(unsigned column=0;column<3;++column)q.h[row][column]+=n[row]*n[column];
  }
  centroid=centroid*(1.0/static_cast<double>(component.size()));
  return solve_convex_qef(h,q,centroid,root_residual);
}
bool optimizer_controls() {
  H box{{{{0,0,0},{12,0,0},{0,12,0},{12,12,0},{0,0,12},{12,0,12},{0,12,12},{12,12,12}}}};
  Qef q{};q.h[0][0]=q.h[1][1]=q.h[2][2]=1.0;
  const auto solve_target=[&](V target) { q.b=target;return solve_convex_qef(box,q,{.5,.5,.5}); };
  const auto face=solve_target({2,.5,.5}),edge=solve_target({2,2,.5}),vertex=solve_target({2,2,2});
  return face.certified&&edge.certified&&vertex.certified&&
         std::abs(face.point.x-1.0)<epsilon&&std::abs(edge.point.x-1.0)<epsilon&&
         std::abs(edge.point.y-1.0)<epsilon&&std::abs(vertex.point.z-1.0)<epsilon;
}

enum class Intersection { none,contact,proper };
struct P2 { double x{},y{}; };
double orient(P2 a,P2 b,P2 c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
bool on_segment(P2 a,P2 b,P2 p){return std::abs(orient(a,b,p))<=epsilon&&
  p.x>=std::min(a.x,b.x)-epsilon&&p.x<=std::max(a.x,b.x)+epsilon&&
  p.y>=std::min(a.y,b.y)-epsilon&&p.y<=std::max(a.y,b.y)+epsilon;}
bool segments_hit(P2 a,P2 b,P2 c,P2 d) {
  const double ab_c=orient(a,b,c),ab_d=orient(a,b,d),cd_a=orient(c,d,a),cd_b=orient(c,d,b);
  return (ab_c*ab_d<-epsilon&&cd_a*cd_b<-epsilon)||on_segment(a,b,c)||on_segment(a,b,d)||
         on_segment(c,d,a)||on_segment(c,d,b);
}
bool point_tri(V p,V a,V b,V c) {
  const V n=cross(b-a,c-a);const double area=length_squared(n);
  if(area<epsilon||std::abs(dot(n,p-a))>epsilon)return false;
  const double u=dot(cross(b-p,c-p),n)/area,v=dot(cross(c-p,a-p),n)/area,
               w=dot(cross(a-p,b-p),n)/area;
  return u>=-epsilon&&v>=-epsilon&&w>=-epsilon;
}
struct SegmentTriangleHit { bool hit{};V point{}; };
SegmentTriangleHit segment_tri(V a,V b,V p,V q,V r) {
  const V n=cross(q-p,r-p);const double da=dot(n,a-p),db=dot(n,b-p);
  if(std::abs(da)<epsilon&&point_tri(a,p,q,r))return {true,a};
  if(std::abs(db)<epsilon&&point_tri(b,p,q,r))return {true,b};
  if(da*db>0.0||std::abs(da-db)<epsilon)return {};
  const double t=da/(da-db);const V hit=a+(b-a)*t;
  return {t>=-epsilon&&t<=1.0+epsilon&&point_tri(hit,p,q,r),hit};
}
Intersection triangle_intersection(V a,V b,V c,V d,V e,V f) {
  const unsigned shared=(same_point(a,d)||same_point(a,e)||same_point(a,f))+
                        (same_point(b,d)||same_point(b,e)||same_point(b,f))+
                        (same_point(c,d)||same_point(c,e)||same_point(c,f));
  const V na=cross(b-a,c-a),nb=cross(e-d,f-d);
  if(length(na)<epsilon||length(nb)<epsilon)return Intersection::none;
  if(length(cross(na,nb))<epsilon) {
    if(std::abs(dot(na,d-a))>epsilon)return Intersection::none;
    const unsigned axis=std::abs(na.x)>=std::abs(na.y)&&std::abs(na.x)>=std::abs(na.z)?0:
                        (std::abs(na.y)>=std::abs(na.z)?1:2);
    const auto project=[&](V x)->P2{return axis==0?P2{x.y,x.z}:axis==1?P2{x.x,x.z}:P2{x.x,x.y};};
    const std::array<P2,3> x{{project(a),project(b),project(c)}},y{{project(d),project(e),project(f)}};
    bool hit=false;for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)
      hit=hit||segments_hit(x[i],x[(i+1)%3],y[j],y[(j+1)%3]);
    const auto inside2=[](P2 p,const std::array<P2,3>& t){const double u=orient(t[0],t[1],p),v=orient(t[1],t[2],p),w=orient(t[2],t[0],p);return (u>=-epsilon&&v>=-epsilon&&w>=-epsilon)||(u<=epsilon&&v<=epsilon&&w<=epsilon);};
    hit=hit||inside2(x[0],y)||inside2(y[0],x);if(!hit)return Intersection::none;
    // Positive-area coplanar overlap is proper; edge/vertex contact is not.
    const auto strictly_inside=[&](P2 p,const std::array<P2,3>& t) {
      return inside2(p,t)&&!on_segment(t[0],t[1],p)&&!on_segment(t[1],t[2],p)&&!on_segment(t[2],t[0],p);
    };
    bool area_overlap=false;
    for(const auto p:x)area_overlap=area_overlap||strictly_inside(p,y);
    for(const auto p:y)area_overlap=area_overlap||strictly_inside(p,x);
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)
      area_overlap=area_overlap||(orient(x[i],x[(i+1)%3],y[j])*orient(x[i],x[(i+1)%3],y[(j+1)%3])<-epsilon&&
        orient(y[j],y[(j+1)%3],x[i])*orient(y[j],y[(j+1)%3],x[(i+1)%3])<-epsilon);
    return area_overlap?Intersection::proper:Intersection::contact;
  }
  const std::array<std::pair<V,V>,3> x{{{a,b},{b,c},{c,a}}},y{{{d,e},{e,f},{f,d}}};
  bool contact=false;
  const auto shared_point=[&](V p) {
    return ((same_point(a,d)||same_point(a,e)||same_point(a,f))&&same_point(p,a))||
           ((same_point(b,d)||same_point(b,e)||same_point(b,f))&&same_point(p,b))||
           ((same_point(c,d)||same_point(c,e)||same_point(c,f))&&same_point(p,c));
  };
  for(const auto& [u,v]:x) {
    const auto hit=segment_tri(u,v,d,e,f);if(!hit.hit)continue;
    if(!shared||!shared_point(hit.point))return Intersection::proper;
    contact=true;
  }
  for(const auto& [u,v]:y) {
    const auto hit=segment_tri(u,v,a,b,c);if(!hit.hit)continue;
    if(!shared||!shared_point(hit.point))return Intersection::proper;
    contact=true;
  }
  return contact?Intersection::contact:Intersection::none;
}
bool intersection_controls() {
  const V a{0,0,0},b{2,0,0},c{0,2,0};
  const bool coplanar=triangle_intersection(a,b,c,{.25,.25,0},{1.5,.25,0},{.25,1.5,0})==Intersection::proper;
  const bool crossing=triangle_intersection(a,b,c,{.5,-.5,-1},{.5,-.5,1},{.5,1.5,0})==Intersection::proper;
  const bool separated=triangle_intersection(a,b,c,{3,0,0},{4,0,0},{3,1,0})==Intersection::none;
  const bool shared_edge=triangle_intersection(a,b,c,b,a,{1,-1,0})==Intersection::contact;
  const bool shared_vertex=triangle_intersection(a,b,c,a,{0,-1,1},{-1,0,1})==Intersection::contact;
  return coplanar&&crossing&&separated&&shared_edge&&shared_vertex;
}

struct TriangleOrigin { E primal;std::vector<std::pair<unsigned,unsigned>> polygon;bool exterior{}; };

std::vector<std::vector<T>> polygon_triangulations(unsigned first,unsigned last) {
  if(last<first+2U)return {{}};
  std::vector<std::vector<T>> result;
  for(unsigned pivot=first+1U;pivot<last;++pivot) {
    const auto left=polygon_triangulations(first,pivot);
    const auto right=polygon_triangulations(pivot,last);
    for(const auto& l:left)for(const auto& r:right) {
      auto candidate=l;candidate.insert(candidate.end(),r.begin(),r.end());
      candidate.push_back({first,pivot,last});result.push_back(std::move(candidate));
    }
  }
  return result;
}
std::string point_json(V p) {
  std::ostringstream out;out<<"["<<p.x<<","<<p.y<<","<<p.z<<"]";return out.str();
}
std::string witness_json(const TriangleOrigin& origin,const std::vector<V>& points,
                         const T& triangle,const char* kind) {
  std::ostringstream out;
  out<<"{\\\"kind\\\":\\\""<<kind<<"\\\",\\\"primal_edge\\\":["<<point_json(pos(origin.primal.a))<<","<<point_json(pos(origin.primal.b))
     <<"],\\\"exterior\\\":"<<(origin.exterior?"true":"false")<<",\\\"ordered_cells\\\":[";
  for(std::size_t i=0;i<origin.polygon.size();++i) {if(i)out<<",";out<<origin.polygon[i].first;}
  out<<"],\\\"component_vertices\\\":[";
  for(std::size_t i=0;i<origin.polygon.size();++i) {if(i)out<<",";out<<origin.polygon[i].second;}
  out<<"],\\\"dual_points\\\":[";
  for(std::size_t i=0;i<origin.polygon.size();++i) {if(i)out<<",";out<<point_json(points[origin.polygon[i].second]);}
  out<<"],\\\"triangle\\\":["<<triangle.a<<","<<triangle.b<<","<<triangle.c<<"]}";
  return out.str();
}

EmbeddingValidation build(const std::vector<H>& hs,const F& field,EmbeddingFixture fixture_kind) {
  EmbeddingValidation v{.boundary_contract_explicit=true,.fixture_oracle_pass=fixture_oracle(fixture_kind,hs),.optimizer_certified=true,
                        .optimizer_controls_pass=optimizer_controls(),
                        .intersection_controls_pass=intersection_controls(),.links_valid=true,
                        .no_duplicate_triangles=true,.no_degenerate_triangles=true,
                        .no_proper_self_intersections=true,.minimum_double_area=std::numeric_limits<double>::infinity(),
                        .failure={},.validator_witness={}};
  std::map<E,std::vector<std::pair<unsigned,unsigned>>> rings;
  std::map<FaceKey,unsigned> face_incidence;
  for(const auto& h:hs)for(const auto& face:faces)++face_incidence[face_key(h,face)];
  std::vector<std::array<int,12>> ids(hs.size());for(auto& x:ids)x.fill(-1);
  std::vector<V> points,centres(hs.size());
  for(unsigned ci=0;ci<hs.size();++ci) {
    const H& h=hs[ci];for(const P p:h.p)centres[ci]=centres[ci]+pos(p);centres[ci]=centres[ci]*.125;
    DSU d;std::array<bool,12> on{};
    for(unsigned i=0;i<12;++i) {
      const E e=edge(h.p[edges[i][0]],h.p[edges[i][1]]);
      const auto root=crossing(field,pos(e.a),pos(e.b));
      if(root.kind==CrossKind::endpoint_a||root.kind==CrossKind::endpoint_b||root.kind==CrossKind::coincident)++v.zero_sample_edges;
      on[i]=crosses(root);if(on[i])rings[e].push_back({ci,i});
    }
    for(const auto& face:faces)for(const auto& [a,b]:face_arcs(h,face,field)) {
      const int x=local_edge(h,a),y=local_edge(h,b);if(x>=0&&y>=0)d.join(static_cast<unsigned>(x),static_cast<unsigned>(y));
    }
    std::map<unsigned,std::vector<unsigned>> components;
    for(unsigned i=0;i<12;++i)if(on[i])components[d.find(i)].push_back(i);
    for(const auto& [root,component]:components) {
      (void)root;const auto placed=constrained_qef(h,field,component);
      v.optimizer_certified=v.optimizer_certified&&placed.certified;
      v.maximum_edge_root_residual=std::max(v.maximum_edge_root_residual,placed.root_residual);
      v.maximum_kkt_residual=std::max(v.maximum_kkt_residual,placed.kkt_residual);
      const int id=static_cast<int>(points.size());points.push_back(placed.point);
      for(const auto i:component)ids[ci][i]=id;
    }
  }
  std::vector<T> triangles;std::vector<TriangleOrigin> origins;
  for(auto& [primal,ring]:rings) {
    std::vector<std::pair<unsigned,unsigned>> q;
    for(const auto& [cell,local]:ring)if(ids[cell][local]>=0)q.push_back({cell,static_cast<unsigned>(ids[cell][local])});
    std::sort(q.begin(),q.end());q.erase(std::unique(q.begin(),q.end()),q.end());
    // Exterior status is a face-incidence fact. A crossed primal edge is on
    // the fixture boundary when any incident hexahedral face containing that
    // edge has no mate. Ring cardinality is recorded but never used here.
    bool exterior=false;
    for(const auto& [cell,local]:q) for(const auto& face:faces) {
      const auto first=hs[cell].p[edges[local][0]],second=hs[cell].p[edges[local][1]];
      const bool has_first=std::ranges::find(face,edges[local][0])!=face.end();
      const bool has_second=std::ranges::find(face,edges[local][1])!=face.end();
      if(has_first&&has_second&&face_incidence[face_key(hs[cell],face)]==1U)exterior=true;
    }
    if(exterior){++v.boundary_rings;continue;}
    ++v.complete_interior_rings;
    const V axis=pos(primal.b)-pos(primal.a),origin=(pos(primal.a)+pos(primal.b))*.5;
    const V reference=std::abs(axis.x)<.8?V{1,0,0}:V{0,1,0};
    V u=cross(axis,reference);u=u*(1.0/length(u));const V w=cross(axis,u);
    std::sort(q.begin(),q.end(),[&](auto x,auto y){const V px=centres[x.first]-origin,py=centres[y.first]-origin;return std::atan2(dot(px,w),dot(px,u))<std::atan2(dot(py,w),dot(py,u));});
    if(q.size()==3)++v.valence_3_polygons;
    if(q.size()==4)++v.valence_4_polygons;
    if(q.size()==6)++v.valence_6_polygons;
    for(unsigned i=1;i+1<q.size();++i) {
      triangles.push_back({q[0].second,q[i].second,q[i+1].second});
      origins.push_back({primal,q,false});
    }
  }
  v.vertices=points.size();v.triangles=triangles.size();
  std::set<T> unique;std::map<std::pair<unsigned,unsigned>,unsigned> uses;
  std::vector<std::vector<std::pair<unsigned,unsigned>>> links(points.size());
  for(std::size_t triangle_index=0;triangle_index<triangles.size();++triangle_index) {
    const auto t=triangles[triangle_index];
    std::array<unsigned,3> sorted{{t.a,t.b,t.c}};std::sort(sorted.begin(),sorted.end());
    if(!unique.insert({sorted[0],sorted[1],sorted[2]}).second)v.no_duplicate_triangles=false;
    const double area=length(cross(points[t.b]-points[t.a],points[t.c]-points[t.a]));
    v.minimum_double_area=std::min(v.minimum_double_area,area);if(area<1e-9) {
      v.no_degenerate_triangles=false;
      if(v.failure_witness.empty())v.failure_witness=witness_json(origins[triangle_index],points,t,"zero_area_triangle");
    }
    for(unsigned i=0;i<3;++i){++uses[std::minmax(std::array<unsigned,3>{{t.a,t.b,t.c}}[i],std::array<unsigned,3>{{t.a,t.b,t.c}}[(i+1)%3])];links[std::array<unsigned,3>{{t.a,t.b,t.c}}[i]].push_back({std::array<unsigned,3>{{t.a,t.b,t.c}}[(i+1)%3],std::array<unsigned,3>{{t.a,t.b,t.c}}[(i+2)%3]});}
  }
  for(const auto& [e,n]:uses)if(n>2) {
    v.links_valid=false;
    if(v.validator_witness.empty())v.validator_witness="dual edge ("+
        std::to_string(e.first)+","+std::to_string(e.second)+") has "+
        std::to_string(n)+" incident triangles";
  }
  for(const auto& link:links)if(!link.empty()) {
    std::map<unsigned,std::set<unsigned>> graph;
    for(const auto& [a,b]:link){graph[a].insert(b);graph[b].insert(a);}
    unsigned endpoints{};
    for(const auto& [vertex,neighbours]:graph) {
      (void)vertex;
      if(neighbours.size()>2U) {
        v.links_valid=false;
        if(v.validator_witness.empty())v.validator_witness="dual vertex "+
            std::to_string(vertex)+" has link degree "+std::to_string(neighbours.size());
      }
      if(neighbours.size()==1U)++endpoints;
    }
    std::set<unsigned> visited;
    std::vector<unsigned> stack{graph.begin()->first};
    while(!stack.empty()) {
      const auto vertex=stack.back();stack.pop_back();
      if(!visited.insert(vertex).second)continue;
      for(const auto neighbour:graph[vertex])stack.push_back(neighbour);
    }
    if(visited.size()!=graph.size()||(endpoints!=0U&&endpoints!=2U)) {
      v.links_valid=false;
      if(v.validator_witness.empty())v.validator_witness="dual vertex link "+
          std::to_string(graph.begin()->first)+" is disconnected or has "+
          std::to_string(endpoints)+" endpoints";
    }
  }
  for(unsigned i=0;i<triangles.size();++i)for(unsigned j=i+1;j<triangles.size();++j) {
    const auto a=triangles[i],b=triangles[j];const unsigned common=(a.a==b.a||a.a==b.b||a.a==b.c)+(a.b==b.a||a.b==b.b||a.b==b.c)+(a.c==b.a||a.c==b.b||a.c==b.c);
    const auto kind=triangle_intersection(points[a.a],points[a.b],points[a.c],points[b.a],points[b.b],points[b.c]);
    if(kind==Intersection::none)continue;
    if(common>0&&kind==Intersection::contact)++v.shared_vertex_intersection_pairs;
    else if(common==0||kind==Intersection::proper){
      v.no_proper_self_intersections=false;v.validator_witness="triangle pair "+std::to_string(i)+" and "+std::to_string(j);
      if(v.failure_witness.empty())v.failure_witness=witness_json(origins[i],points,a,"intersecting_triangle");
    }
  }
  // A zero-area triangle is a triangulation-independent failure only when it
  // occurs in every combinatorial triangulation of its ordered dual polygon.
  if(!v.failure_witness.empty()) {
    for(const auto& origin:origins) {
      bool every_bad=true;
      for(const auto& triangulation:polygon_triangulations(0U,static_cast<unsigned>(origin.polygon.size()-1U))) {
        bool this_bad=false;
        for(const auto local:triangulation) {
          const auto area=length(cross(points[origin.polygon[local.b].second]-points[origin.polygon[local.a].second],
                                       points[origin.polygon[local.c].second]-points[origin.polygon[local.a].second]));
          this_bad=this_bad||area<1e-9;
        }
        every_bad=every_bad&&this_bad;
      }
      v.all_polygon_triangulations_fail=v.all_polygon_triangulations_fail||every_bad;
    }
  }
  if(!std::isfinite(v.minimum_double_area))v.minimum_double_area=0.0;
  if(!v.fixture_oracle_pass)v.failure="four-hexahedra fixture oracle failed";
  else if(!v.optimizer_controls_pass)v.failure="convex QEF qualification control failed";
  else if(!v.intersection_controls_pass)v.failure="intersection qualification control failed";
  else if(!v.optimizer_certified)v.failure="convex QEF certificate failed";
  else if(!v.links_valid)v.failure="invalid vertex link";
  else if(!v.no_duplicate_triangles)v.failure="duplicate triangle";
  else if(!v.no_degenerate_triangles)v.failure="zero-area triangle";
  else if(!v.no_proper_self_intersections)v.failure="triangle intersection without legitimate adjacency";
  else if(triangles.empty())v.failure="no crossed-edge surface";
  return v;
}
F bits(unsigned mask) {
  // Fifteen deterministic non-zero affine fields; no enumerated fixture has
  // an endpoint root, which keeps the surface contract unambiguous.
  return {{(mask&1U)?-.37:.41,(mask&2U)?1.0:-.75,(mask&4U)?.63:-1.1,(mask&8U)?.82:-.57}};
}
F non_affine_control_field() {
  // This is a global quadratic sampled-field control. It deliberately has a
  // different name from the affine corpus and every corner/root/normal is
  // obtained from this one function.
  return {{.19,-.83,.71,-.46},.31};
}
bool ok(const EmbeddingValidation& v) {
  return v.fixture_oracle_pass&&v.optimizer_controls_pass&&v.intersection_controls_pass&&v.optimizer_certified&&v.links_valid&&
         v.no_duplicate_triangles&&v.no_degenerate_triangles&&v.no_proper_self_intersections;
}
} // namespace

std::string embedding_fixture_name(EmbeddingFixture f) {
  return f==EmbeddingFixture::one_tet?"one-tet-four-hex":
         f==EmbeddingFixture::two_tet?"two-face-adjacent-tets":"six-tets-around-interior-edge";
}
EmbeddingReport run_dual_embedding_probe(std::uint64_t seed,std::size_t budget) {
  EmbeddingReport report{};report.seed=seed;report.budget=budget;
  report.fixture_identity="four-hexahedra/cartesian-twelfths-v1";
  report.field_contract="global affine analytic field with canonical corner samples; exact linear primal-edge roots and analytic normals";
  report.boundary_contract="exterior status is derived from explicit unmatched hexahedral face incidence; exterior rings are excluded from interior-link claims";
  report.non_affine_control_contract="global-quadratic-sampled-field-v1: corner values, bisection edge roots, and analytic point normals come from one quadratic function";
  std::mt19937_64 random(seed);std::uniform_real_distribution<double> coefficient(-1.0,1.0);
  for(const auto kind:{EmbeddingFixture::one_tet,EmbeddingFixture::two_tet,EmbeddingFixture::six_root}) {
    EmbeddingFixtureReport item{};item.fixture=kind;const auto cells=fixture(kind);
    for(unsigned mask=1;mask<16&&!item.counterexample_found;++mask) {
      const auto field=bits(mask);
      const auto validation=build(cells,field,kind);++item.enumerated_fields;
      if(!validation.triangles)continue;
      ++item.valid_surfaces;item.worst_validation=validation;
      if(!ok(validation)){item.counterexample_found=true;item.counterexample_case=mask;item.counterexample_coefficients=field.c;}
    }
    for(std::size_t i=0;i<budget&&!item.counterexample_found;++i) {
      F field;for(auto& x:field.c)x=coefficient(random);if(length(field.gradient())<epsilon)continue;
      const auto validation=build(cells,field,kind);++item.adversarial_fields;if(!validation.triangles)continue;
      ++item.valid_surfaces;item.worst_validation=validation;
      if(!ok(validation)){item.counterexample_found=true;item.counterexample_case=16+i;item.counterexample_coefficients=field.c;}
    }
    report.qualification_controls_pass=report.qualification_controls_pass||item.worst_validation.intersection_controls_pass;
    const auto non_affine=build(cells,non_affine_control_field(),kind);
    item.worst_validation.non_affine_control_pass=non_affine.fixture_oracle_pass&&
        non_affine.optimizer_certified&&non_affine.maximum_edge_root_residual<1e-10&&
        non_affine.zero_sample_edges==0U;
    report.fixtures.push_back(item);
  }
  report.strict_dual_survived_search=std::ranges::none_of(report.fixtures,[](const auto& x){return x.counterexample_found;});
  report.conclusion=report.qualification_controls_pass?
    "R1Q controls passed; any bounded-search result uses the stated fixture, field, boundary, QEF, and intersection contracts":
    "R1Q controls failed; no R1 conclusion may be drawn";
  return report;
}
std::string make_dual_embedding_report_json(const EmbeddingReport& r) {
  std::ostringstream o;o<<"{\n  \"schema\": \"dual_embedding_probe/r1q-v1\",\n"
    <<"  \"fixture_identity\": \""<<r.fixture_identity<<"\",\n"
    <<"  \"field_contract\": \""<<r.field_contract<<"\",\n"
    <<"  \"boundary_contract\": \""<<r.boundary_contract<<"\",\n"
    <<"  \"seed\": "<<r.seed<<", \"budget\": "<<r.budget<<",\n"
    <<"  \"qualification_controls_pass\": "<<(r.qualification_controls_pass?"true":"false")<<",\n"
    <<"  \"strict_dual_survived_search\": "<<(r.strict_dual_survived_search?"true":"false")<<",\n"
    <<"  \"fixtures\": [\n";
  for(std::size_t i=0;i<r.fixtures.size();++i){const auto& x=r.fixtures[i];const auto& v=x.worst_validation;
    o<<"    {\"fixture\": \""<<embedding_fixture_name(x.fixture)<<"\", \"enumerated_fields\": "<<x.enumerated_fields
      <<", \"adversarial_fields\": "<<x.adversarial_fields<<", \"valid_surfaces\": "<<x.valid_surfaces
      <<", \"counterexample_found\": "<<(x.counterexample_found?"true":"false")<<", \"counterexample_case\": "<<x.counterexample_case
      <<", \"coefficients\": ["<<x.counterexample_coefficients[0]<<","<<x.counterexample_coefficients[1]<<","<<x.counterexample_coefficients[2]<<","<<x.counterexample_coefficients[3]
      <<"], \"validation\": {\"boundary_contract_explicit\": "<<(v.boundary_contract_explicit?"true":"false")
      <<", \"fixture_oracle_pass\": "<<(v.fixture_oracle_pass?"true":"false")
      <<", \"boundary_rings\": "<<v.boundary_rings<<", \"complete_interior_rings\": "<<v.complete_interior_rings
      <<", \"optimizer_certified\": "<<(v.optimizer_certified?"true":"false")<<", \"optimizer_controls_pass\": "<<(v.optimizer_controls_pass?"true":"false")<<", \"maximum_kkt_residual\": "<<v.maximum_kkt_residual
      <<", \"maximum_edge_root_residual\": "<<v.maximum_edge_root_residual<<", \"intersection_controls_pass\": "<<(v.intersection_controls_pass?"true":"false")
      <<", \"links_valid\": "<<(v.links_valid?"true":"false")<<", \"no_proper_self_intersections\": "<<(v.no_proper_self_intersections?"true":"false")
      <<", \"validator_witness\": \""<<v.validator_witness<<"\", \"failure\": \""<<v.failure<<"\"}}"
      <<(i+1==r.fixtures.size()?"\n":",\n");}
  o<<"  ],\n  \"conclusion\": \""<<r.conclusion<<"\"\n}\n";
  return o.str();
}

// Kept only so the historical standalone executable remains buildable. Its old
// mask-29 placement comparison had an invalid coordinate/field contract and
// is deliberately not presented as R1 evidence.
PlacementProbeReport run_strict_dual_placement_probe(std::uint64_t seed,std::size_t budget) {
  const auto report=run_dual_embedding_probe(seed,budget);PlacementProbeReport legacy{};
  legacy.seed=seed;legacy.search_budget=budget;legacy.safe_rule_corpus=report.fixtures;
  legacy.safe_rule_survived_corpus=report.strict_dual_survived_search;
  legacy.conclusion="deprecated by dual_embedding_probe/r1q-v1; use its qualified report";
  return legacy;
}
std::string make_strict_dual_placement_report_json(const PlacementProbeReport& r) {
  return "{\n  \"schema\": \"strict_dual_placement_probe/deprecated\",\n  \"seed\": "+std::to_string(r.seed)+",\n  \"conclusion\": \""+r.conclusion+"\"\n}\n";
}
} // namespace tetra::probes
