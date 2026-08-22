#include "tetra_core/whole_cell_surface.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>

namespace tetra {
namespace {

constexpr std::array<std::array<std::size_t,3>,4> face_corners{{
    {{0,1,2}},{{0,1,3}},{{0,2,3}},{{1,2,3}}}};

struct FaceRecord {
  std::array<VertexId,3> key{};
  std::array<VertexId,3> vertices{};
  std::uint32_t leaf{};
};

double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
double length(Vec3 value){return std::sqrt(dot(value,value));}

struct Arc {std::uint32_t to{}; std::uint32_t next{}; double capacity{};};

class PackedDinic {
 public:
  explicit PackedDinic(std::size_t nodes):heads_(nodes,none),levels_(nodes),current_(nodes){}
  void reserve(std::size_t arcs){arcs_.reserve(arcs);}
  void add(std::uint32_t from,std::uint32_t to,double capacity){
    arcs_.push_back({to,heads_[from],capacity}); heads_[from]=static_cast<std::uint32_t>(arcs_.size()-1);
    arcs_.push_back({from,heads_[to],0.0}); heads_[to]=static_cast<std::uint32_t>(arcs_.size()-1);
  }
  void add_pair(std::uint32_t a,std::uint32_t b,double capacity){add(a,b,capacity);add(b,a,capacity);}
  void solve(std::uint32_t source,std::uint32_t sink){
    while(bfs(source,sink)){
      current_=heads_;
      while(push(source,sink,std::numeric_limits<double>::infinity())>epsilon){}
    }
  }
  [[nodiscard]] std::vector<std::uint8_t> source_set(std::uint32_t source) const {
    std::vector<std::uint8_t> seen(heads_.size());
    std::vector<std::uint32_t> queue{source}; seen[source]=1;
    for(std::size_t begin=0;begin<queue.size();++begin){
      const auto node=queue[begin];
      for(auto arc=heads_[node];arc!=none;arc=arcs_[arc].next)
        if(arcs_[arc].capacity>epsilon&&!seen[arcs_[arc].to]){seen[arcs_[arc].to]=1;queue.push_back(arcs_[arc].to);}
    }
    return seen;
  }
 private:
  bool bfs(std::uint32_t source,std::uint32_t sink){
    std::fill(levels_.begin(),levels_.end(),-1); levels_[source]=0;
    std::vector<std::uint32_t> queue{source}; queue.reserve(heads_.size());
    for(std::size_t begin=0;begin<queue.size();++begin){
      const auto node=queue[begin];
      for(auto arc=heads_[node];arc!=none;arc=arcs_[arc].next)if(arcs_[arc].capacity>epsilon&&levels_[arcs_[arc].to]<0){
        levels_[arcs_[arc].to]=levels_[node]+1; queue.push_back(arcs_[arc].to);
      }
    }
    return levels_[sink]>=0;
  }
  double push(std::uint32_t node,std::uint32_t sink,double amount){
    if(node==sink)return amount;
    for(auto& arc=current_[node];arc!=none;arc=arcs_[arc].next){
      auto& edge=arcs_[arc];
      if(edge.capacity<=epsilon||levels_[edge.to]!=levels_[node]+1)continue;
      const double sent=push(edge.to,sink,std::min(amount,edge.capacity));
      if(sent>epsilon){edge.capacity-=sent;arcs_[arc^1U].capacity+=sent;return sent;}
    }
    return 0.0;
  }
  static constexpr std::uint32_t none=std::numeric_limits<std::uint32_t>::max();
  static constexpr double epsilon=1.0e-12;
  std::vector<std::uint32_t> heads_;
  std::vector<Arc> arcs_;
  std::vector<int> levels_;
  std::vector<std::uint32_t> current_;
};

std::uint64_t mix(std::uint64_t hash,std::uint64_t value){
  value^=value>>30U;value*=0xbf58476d1ce4e5b9ULL;value^=value>>27U;
  value*=0x94d049bb133111ebULL;value^=value>>31U;
  return hash^(value+0x9e3779b97f4a7c15ULL+(hash<<6U)+(hash>>2U));
}

} // namespace

WholeCellCut build_whole_cell_cut(const TetMesh& mesh,const Sphere& sphere,const WholeCellOptions& options){
  const auto start=std::chrono::steady_clock::now();
  const auto& leaves=mesh.active_leaves();
  WholeCellCut result;
  result.selected_words.assign((leaves.size()+63U)/64U,0);
  std::vector<FaceRecord> faces;faces.reserve(leaves.size()*4U);
  std::vector<SurfaceRelation> relations;relations.reserve(leaves.size());
  std::vector<double> centroids(leaves.size()),scales(leaves.size());
  for(std::size_t index=0;index<leaves.size();++index){
    const auto& ids=mesh.tetrahedron(leaves[index]).vertices;
    Vec3 centre{};double longest=0.0;
    for(auto id:ids)centre=centre+mesh.vertices()[id];centre=centre/4.0;
    for(std::size_t a=0;a<4;++a)for(std::size_t b=a+1;b<4;++b)
      longest=std::max(longest,length(mesh.vertices()[ids[a]]-mesh.vertices()[ids[b]]));
    centroids[index]=sphere.signed_distance(centre);scales[index]=longest;
    relations.push_back(classify_tetrahedron(mesh,leaves[index],sphere));
    for(const auto& face:face_corners){FaceRecord record;record.leaf=static_cast<std::uint32_t>(index);
      for(std::size_t c=0;c<3;++c)record.vertices[c]=ids[face[c]];record.key=record.vertices;
      std::sort(record.key.begin(),record.key.end());faces.push_back(record);}
  }
  std::sort(faces.begin(),faces.end(),[](const auto& a,const auto& b){return a.key<b.key;});
  std::vector<std::uint8_t> labels(leaves.size());
  if(options.method==WholeCellSelectionMethod::variational){
    std::vector<std::uint32_t> graph_nodes(leaves.size(),std::numeric_limits<std::uint32_t>::max());
    std::uint32_t graph_node_count=0;
    for(std::size_t i=0;i<leaves.size();++i){
      if(relations[i]==SurfaceRelation::intersecting)graph_nodes[i]=graph_node_count++;
      else labels[i]=relations[i]==SurfaceRelation::inside?1U:0U;
    }
    const auto source=graph_node_count,sink=source+1U;
    PackedDinic graph(static_cast<std::size_t>(graph_node_count)+2U);
    graph.reserve(static_cast<std::size_t>(graph_node_count)*12U);
    for(std::size_t i=0;i<leaves.size();++i){
      if(relations[i]!=SurfaceRelation::intersecting)continue;
      const double normalized=centroids[i]/std::max(scales[i],1.0e-12);
      const double inside=options.data_weight*std::max(normalized,0.0)*std::max(normalized,0.0);
      const double outside=options.data_weight*std::max(-normalized,0.0)*std::max(-normalized,0.0);
      graph.add(source,graph_nodes[i],outside);graph.add(graph_nodes[i],sink,inside);
    }
    for(std::size_t begin=0;begin<faces.size();){std::size_t end=begin+1;while(end<faces.size()&&faces[end].key==faces[begin].key)++end;
      if(end-begin==2){const auto& f=faces[begin];const Vec3 a=mesh.vertices()[f.vertices[0]],b=mesh.vertices()[f.vertices[1]],c=mesh.vertices()[f.vertices[2]];
        const Vec3 n=cross(b-a,c-a);const double twice_area=length(n);const Vec3 centre=(a+b+c)/3.0;
        const Vec3 field_normal=sphere.normal(centre);
        const double alignment=twice_area>1.0e-12?std::abs(dot(n/twice_area,field_normal)):0.0;
        const double scale=0.5*(scales[faces[begin].leaf]+scales[faces[begin+1].leaf]);
        const double distance=std::abs(sphere.signed_distance(centre))/std::max(scale,1.0e-12);
        const double weight=0.5*twice_area*(options.area_weight+options.distance_weight*distance*distance+options.normal_weight*(1.0-alignment)*(1.0-alignment));
        const auto first=faces[begin].leaf,second=faces[begin+1].leaf;
        const bool first_free=relations[first]==SurfaceRelation::intersecting;
        const bool second_free=relations[second]==SurfaceRelation::intersecting;
        if(first_free&&second_free)graph.add_pair(graph_nodes[first],graph_nodes[second],weight);
        else if(first_free){if(labels[second])graph.add(source,graph_nodes[first],weight);else graph.add(graph_nodes[first],sink,weight);}
        else if(second_free){if(labels[first])graph.add(source,graph_nodes[second],weight);else graph.add(graph_nodes[second],sink,weight);}}
      begin=end;}
    graph.solve(source,sink);const auto reachable=graph.source_set(source);
    for(std::size_t i=0;i<leaves.size();++i)if(relations[i]==SurfaceRelation::intersecting)labels[i]=reachable[graph_nodes[i]];
  }else{
    for(std::size_t i=0;i<leaves.size();++i){const auto& ids=mesh.tetrahedron(leaves[i]).vertices;std::size_t inside=0;
      for(auto id:ids)inside+=sphere.signed_distance(mesh.vertices()[id])<=0.0?1U:0U;
      labels[i]=options.method==WholeCellSelectionMethod::all_vertices_inside?inside==4:
        options.method==WholeCellSelectionMethod::centroid_inside?centroids[i]<=0.0:
        options.method==WholeCellSelectionMethod::majority_vertices_inside?inside>=3:
        relations[i]!=SurfaceRelation::outside;}
  }

  const auto extract_boundary=[&] {
    std::vector<WholeCellBoundaryFace> boundary;
    for(std::size_t begin=0;begin<faces.size();){
      std::size_t end=begin+1;while(end<faces.size()&&faces[end].key==faces[begin].key)++end;
      const bool first=labels[faces[begin].leaf]!=0;
      const bool exposed=end-begin==1?first:
          (end-begin==2&&first!=(labels[faces[begin+1].leaf]!=0));
      if(exposed){const auto& owner=first?faces[begin]:faces[begin+1];
        boundary.push_back({owner.vertices,owner.leaf});}
      begin=end;
    }
    return boundary;
  };
  // A binary cell cut is closed, but alternating labels around a primal edge
  // can create several sheets meeting at that edge. Repair only ambiguous
  // cells in such stars; hard inside/outside evidence remains untouched.
  for(std::size_t iteration=0;iteration<8;++iteration){
    const auto boundary=extract_boundary();
    std::vector<std::array<VertexId,2>> boundary_edges;boundary_edges.reserve(boundary.size()*3U);
    for(const auto& face:boundary)for(std::size_t edge=0;edge<3;++edge){
      auto key=std::array{face.vertices[edge],face.vertices[(edge+1)%3]};
      if(key[1]<key[0])std::swap(key[0],key[1]);boundary_edges.push_back(key);}
    std::sort(boundary_edges.begin(),boundary_edges.end());
    std::vector<std::array<VertexId,2>> bad_edges;
    for(std::size_t begin=0;begin<boundary_edges.size();){std::size_t end=begin+1;
      while(end<boundary_edges.size()&&boundary_edges[end]==boundary_edges[begin])++end;
      if(end-begin!=2)bad_edges.push_back(boundary_edges[begin]);begin=end;}
    if(bad_edges.empty())break;
    bool changed=false;
    for(const auto edge:bad_edges){
      std::vector<std::size_t> free_cells;std::size_t inside=0,outside=0;
      for(std::size_t leaf=0;leaf<leaves.size();++leaf){const auto& ids=mesh.tetrahedron(leaves[leaf]).vertices;
        if(std::ranges::find(ids,edge[0])==ids.end()||std::ranges::find(ids,edge[1])==ids.end())continue;
        labels[leaf]?++inside:++outside;
        if(relations[leaf]==SurfaceRelation::intersecting)free_cells.push_back(leaf);}
      if(free_cells.empty())continue;
      const std::uint8_t target=inside>outside?1U:0U;
      for(const auto leaf:free_cells)if(labels[leaf]!=target){labels[leaf]=target;changed=true;}
    }
    if(!changed)break;
  }
  for(std::size_t i=0;i<labels.size();++i)if(labels[i]){result.selected_words[i/64U]|=std::uint64_t{1}<<(i%64U);++result.selected_cells;result.selected_volume+=mesh.signed_volume(leaves[i]);result.hash=mix(result.hash,leaves[i]);}
  result.boundary_faces=extract_boundary();
  for(const auto& face:result.boundary_faces){auto key=face.vertices;std::sort(key.begin(),key.end());for(auto id:key)result.hash=mix(result.hash,id);}
  struct BoundaryEdge{std::array<VertexId,2> key;std::uint32_t face;};
  std::vector<BoundaryEdge> edges;edges.reserve(result.boundary_faces.size()*3U);
  for(std::size_t face_index=0;face_index<result.boundary_faces.size();++face_index){
    const auto& face=result.boundary_faces[face_index];
    for(std::size_t e=0;e<3;++e){auto edge=std::array{face.vertices[e],face.vertices[(e+1)%3]};if(edge[1]<edge[0])std::swap(edge[0],edge[1]);edges.push_back({edge,static_cast<std::uint32_t>(face_index)});}}
  std::sort(edges.begin(),edges.end(),[](const auto& a,const auto& b){return a.key<b.key;});
  std::vector<std::uint32_t> parents(result.boundary_faces.size());
  for(std::size_t i=0;i<parents.size();++i)parents[i]=static_cast<std::uint32_t>(i);
  const auto root=[&parents](std::uint32_t value){while(parents[value]!=value){parents[value]=parents[parents[value]];value=parents[value];}return value;};
  for(std::size_t begin=0;begin<edges.size();){std::size_t end=begin+1;while(end<edges.size()&&edges[end].key==edges[begin].key)++end;
    ++result.boundary_edges;if(end-begin!=2)++result.nonmanifold_boundary_edges;
    for(std::size_t i=begin+1;i<end;++i){const auto a=root(edges[begin].face),b=root(edges[i].face);if(a!=b)parents[b]=a;}begin=end;}
  for(std::size_t i=0;i<parents.size();++i)if(root(static_cast<std::uint32_t>(i))==i)++result.boundary_components;
  result.solve_milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  return result;
}

AdaptiveResult refine_to_whole_cell_surface(TetMesh& mesh,const Sphere& sphere,
                                             const Camera& camera,double pixel_threshold,
                                             unsigned int maximum_depth,
                                             const WholeCellOptions& options){
  AdaptiveResult result=refine_to_sphere(mesh,sphere,camera,pixel_threshold,maximum_depth);
  const unsigned int increment=subdivision_depth_increment(mesh.subdivision_method());
  while(true){
    const auto cut=build_whole_cell_cut(mesh,sphere,options);
    std::vector<TetId> marked;marked.reserve(cut.boundary_faces.size());
    bool blocked_by_depth=false;
    for(const auto& face:cut.boundary_faces){
      const TetId id=mesh.active_leaves()[face.inside_leaf];
      if(projected_tetrahedron_diameter(mesh,id,camera)<=pixel_threshold)continue;
      if(mesh.refinement_depth(id)+increment<=maximum_depth)marked.push_back(id);
      else blocked_by_depth=true;
    }
    std::sort(marked.begin(),marked.end());marked.erase(std::unique(marked.begin(),marked.end()),marked.end());
    if(marked.empty()){result.reached_depth_limit|=blocked_by_depth;return result;}
    result.refined_leaves+=marked.size();++result.iterations;mesh.refine_selected_binary(marked);
  }
}

} // namespace tetra
