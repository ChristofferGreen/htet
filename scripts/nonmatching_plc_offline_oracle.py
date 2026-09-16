#!/usr/bin/env python3
"""Offline-only TetGen PLC exporter and independent output auditor.

It deliberately has no project/runtime dependency and never launches TetGen.
The companion shell runner receives a caller-supplied executable.
"""
import argparse, itertools, json, math, pathlib, sys

OUTER=[(-3,-3,-3),(3,-3,-3),(-3,3,-3),(-3,-3,3),(0,-3,-3),(0,0,-3),(-3,0,-3),(-3,-3,0),(0,-3,0),(-3,0,0)]
OUTER_FACES=[(0,6,4),(6,2,5),(4,5,1),(6,5,4),(0,4,7),(4,1,8),(7,8,3),(4,8,7),(0,7,6),(7,3,9),(6,9,2),(7,9,6),(1,5,8),(5,2,9),(8,9,3),(5,9,8)]
CORE=[(-2,-2,-2),(-1.5,-2,-2),(-2,-1.5,-2),(-2,-2,-2.5),(-2.6,-2,-1.5)]
PARENTS=[(10,12,11,13),(10,11,12,14)] # oriented, share the first three

def add(a,b): return tuple(x+y for x,y in zip(a,b))
def sub(a,b): return tuple(x-y for x,y in zip(a,b))
def dot(a,b): return sum(x*y for x,y in zip(a,b))
def cross(a,b): return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
def scale(a,s): return tuple(x*s for x in a)
def vol(a,b,c,d): return dot(sub(b,a),cross(sub(c,a),sub(d,a)))
def key(p): return tuple(round(x,12) for x in p)
def fkey(points,face): return tuple(sorted(key(points[i]) for i in face))

def red_face(points, face, lookup):
    a,b,c=face
    def mid(i,j):
        p=scale(add(points[i],points[j]),.5); k=key(p)
        if k not in lookup: lookup[k]=len(points); points.append(p)
        return lookup[k]
    ab,bc,ca=mid(a,b),mid(b,c),mid(c,a)
    return [(a,ab,ca),(ab,b,bc),(ca,bc,c),(ab,bc,ca)]

def red_tetra(points, tet, lookup):
    a,b,c,d=tet
    def mid(i,j):
        p=scale(add(points[i],points[j]),.5); k=key(p)
        if k not in lookup: lookup[k]=len(points); points.append(p)
        return lookup[k]
    ab,ac,ad,bc,bd,cd=mid(a,b),mid(a,c),mid(a,d),mid(b,c),mid(b,d),mid(c,d)
    return [(a,ab,ac,ad),(ab,b,bc,bd),(ac,bc,c,cd),(ad,bd,cd,d),
            (ab,ac,ad,cd),(ab,ac,bc,cd),(ab,ad,bd,cd),(ab,bc,bd,cd)]

def fixture():
    points=list(map(lambda p:tuple(map(float,p)),OUTER+CORE)); lookup={key(p):i for i,p in enumerate(points)}
    # The six core boundary parent faces are independently red-refined; the
    # common parent face is internal and is refined only by core materialization.
    incidence={}
    for t in PARENTS:
        for n in range(4):
            face=tuple(t[i] for i in range(4) if i!=n)
            incidence.setdefault(tuple(sorted(face)),[]).append(face)
    inner=[]
    for uses in incidence.values():
        if len(uses)==1: inner.extend(red_face(points,uses[0],lookup))
    core=[]
    for t in PARENTS: core.extend(red_tetra(points,t,lookup))
    return points, list(OUTER_FACES), inner, core

def export(prefix):
    points,outer,inner,core=fixture(); prefix=pathlib.Path(prefix); prefix.parent.mkdir(parents=True,exist_ok=True)
    facets=outer+inner
    with prefix.with_suffix('.poly').open('w') as f:
        f.write(f'{len(points)} 3 0 0\n')
        for i,p in enumerate(points,1): f.write(f'{i} {p[0]:.17g} {p[1]:.17g} {p[2]:.17g}\n')
        f.write(f'{len(facets)} 0\n')
        # TetGen .poly has a facet header (one polygon, no facet holes), then
        # the polygon's vertex-count/index record.  Writing ``3 0 0`` here
        # would incorrectly declare three polygons and can make TetGen crash
        # while reading this otherwise valid PLC.
        for tri in facets: f.write('1 0 0\n3 '+' '.join(str(i+1) for i in tri)+'\n')
        # Strictly inside the materialized core: TetGen must leave this as a hole.
        h=scale(add(add(points[10],points[11]),add(points[12],points[13])),.25)
        f.write(f'1\n1 {h[0]:.17g} {h[1]:.17g} {h[2]:.17g}\n0\n')
    with prefix.with_suffix('.manifest.json').open('w') as f:
        json.dump({'schema':1,'points':points,'outer_facets':outer,'inner_refined_core_facets':inner,'materialized_core_tetrahedra':core},f,indent=2)
    print(prefix.with_suffix('.poly'))

def read_nodes(path):
    lines=[x.split('#',1)[0].split() for x in open(path)]; lines=[x for x in lines if x]
    n=int(lines[0][0]); result={}
    for x in lines[1:1+n]: result[int(x[0])]=tuple(map(float,x[1:4]))
    return result
def read_elements(path):
    lines=[x.split('#',1)[0].split() for x in open(path)]; lines=[x for x in lines if x]
    n=int(lines[0][0]); corners=int(lines[0][1]);
    if corners!=4: raise ValueError('expected tetrahedral .ele')
    return [tuple(map(int,x[1:5])) for x in lines[1:1+n]]
def faces(t): return [tuple(t[j] for j in range(4) if j!=i) for i in range(4)]
def boundary(points,tets):
    ledger={}
    for ti,t in enumerate(tets):
        for fi,face in enumerate(faces(t)): ledger.setdefault(fkey(points,face),[]).append((ti,fi,face))
    return ledger
def same_side(points,t0,f0,t1,f1):
    a,b,c=(points[i] for i in f0); n=cross(sub(b,a),sub(c,a)); d=dot(n,sub(points[t0[f1]],a))
    # caller supplies opposite vertex indexes separately; this helper is unused
    return d
def dihedral(points,t):
    result=[]
    for i,j in itertools.combinations(range(4),2):
        other=[k for k in range(4) if k not in (i,j)]
        n=cross(sub(points[t[j]],points[t[i]]),sub(points[t[other[0]]],points[t[i]]))
        m=cross(sub(points[t[i]],points[t[j]]),sub(points[t[other[1]]],points[t[j]]))
        result.append(math.degrees(math.acos(max(-1,min(1,dot(n,m)/(math.sqrt(dot(n,n))*math.sqrt(dot(m,m))))))))
    return result
def inside_tet(p, points, tet):
    signs=[]
    for face in faces(tet):
        a,b,c=(points[i] for i in face); opposite=next(i for i in tet if i not in face)
        ref=vol(a,b,c,points[opposite]); value=vol(a,b,c,p)
        signs.append(value/ref)
    return min(signs)>1e-10
def segment_triangle(a,b,p,q,r):
    direction=sub(b,a); e1=sub(q,p); e2=sub(r,p); h=cross(direction,e2); determinant=dot(e1,h)
    if abs(determinant)<1e-12: return False
    inv=1/determinant; u=dot(sub(a,p),h)*inv
    if not 1e-10<u<1-1e-10: return False
    v=dot(direction,cross(sub(a,p),e1))*inv
    if not 1e-10<v<1-u-1e-10: return False
    t=dot(e2,cross(sub(a,p),e1))*inv
    return 1e-10<t<1-1e-10
def strict_overlap(points,left,right):
    if any(inside_tet(points[i],points,right) for i in left): return True
    if any(inside_tet(points[i],points,left) for i in right): return True
    for a,b in itertools.combinations(left,2):
        for f in faces(right):
            if segment_triangle(points[a],points[b],*(points[i] for i in f)): return True
    for a,b in itertools.combinations(right,2):
        for f in faces(left):
            if segment_triangle(points[a],points[b],*(points[i] for i in f)): return True
    return False
def audit(prefix):
    prefix=pathlib.Path(prefix); manifest=json.load(prefix.with_suffix('.manifest.json').open()); nodes=read_nodes(prefix.with_suffix('.1.node')); imported=read_elements(prefix.with_suffix('.1.ele'))
    points=[nodes[i] for i in sorted(nodes)]; index={old:new for new,old in enumerate(sorted(nodes))}; imported=[tuple(index[i] for i in t) for t in imported]
    expected_outer={fkey(manifest['points'],x) for x in manifest['outer_facets']}; expected_inner={fkey(manifest['points'],x) for x in manifest['inner_refined_core_facets']}
    imported_ledger=boundary(points,imported); imported_boundary={k for k,v in imported_ledger.items() if len(v)==1}
    # Resolve each materialized core coordinate against TetGen nodes: no hidden
    # node renumbering or coordinate drift is permitted.
    by_coord={key(p):i for i,p in enumerate(points)}
    try: core=[tuple(by_coord[key(manifest['points'][i])] for i in t) for t in manifest['materialized_core_tetrahedra']]
    except KeyError: raise ValueError('TetGen output omitted a required materialized-core vertex')
    all_tets=imported+core; ledger=boundary(points,all_tets); combined_boundary={k for k,v in ledger.items() if len(v)==1}
    problems=[]
    if imported_boundary != expected_outer|expected_inner: problems.append('imported boundary is not exactly outer + refined core facets')
    if combined_boundary != expected_outer: problems.append('combined boundary has scaffold/core leakage or missing outer facet')
    if any(len(v)>2 for v in ledger.values()): problems.append('non-manifold face incidence')
    for uses in ledger.values():
        if len(uses)==2:
            ti,_,face0=uses[0]; tj,_,face1=uses[1]
            opposite0=next(v for v in all_tets[ti] if v not in face0); opposite1=next(v for v in all_tets[tj] if v not in face1)
            a,b,c=(points[i] for i in face0); normal=cross(sub(b,a),sub(c,a))
            if dot(normal,sub(points[opposite0],a))*dot(normal,sub(points[opposite1],a)) >= -1e-12:
                problems.append('same-sided shared face'); break
    if any(abs(vol(*(points[i] for i in t)))<1e-12 for t in all_tets): problems.append('nonpositive/degenerate tetrahedron')
    if len({tuple(sorted(key(points[i]) for i in t)) for t in all_tets})!=len(all_tets): problems.append('duplicate tetrahedron')
    if any(strict_overlap(points,a,b) for a,b in itertools.combinations(all_tets,2) if len(set(a)&set(b))<3): problems.append('strict tetrahedron overlap')
    angles=[a for t in all_tets for a in dihedral(points,t)]
    if any(a<5 or a>175 for a in angles): problems.append('S4 dihedral gate')
    report={'accepted':not problems,'problems':problems,'imported_tetrahedra':len(imported),'combined_tetrahedra':len(all_tets),'minimum_dihedral_degrees':min(angles),'maximum_dihedral_degrees':max(angles),'exact_imported_boundary':imported_boundary==expected_outer|expected_inner,'exact_combined_outer_boundary':combined_boundary==expected_outer}
    print(json.dumps(report,indent=2)); return 0 if report['accepted'] else 1
def quality_score(points,tets):
    angles=[a for t in tets for a in dihedral(points,t)]
    return min(min(angles),180-max(angles)),min(angles),max(angles)

def combined_geometry_problems(points, shell, core, manifest):
    """Full interface-preserving audit, except that S4 is reported by caller.

    This deliberately receives only geometry plus the immutable PLC manifest;
    it has no TetGen cell identities or fixture repair information.
    """
    combined=shell+core
    expected_outer={fkey(manifest['points'],x) for x in manifest['outer_facets']}
    expected_inner={fkey(manifest['points'],x) for x in manifest['inner_refined_core_facets']}
    ledger=boundary(points,combined); outer={k for k,v in ledger.items() if len(v)==1}
    problems=[]
    if outer != expected_outer: problems.append('combined boundary differs from frozen outer PLC')
    if any(len(v)>2 for v in ledger.values()): problems.append('non-manifold face incidence')
    # Core-interface facets must be internal after the shell/core join, while
    # exterior PLC facets must remain the exact final boundary.
    if any(len(ledger.get(k,[]))!=2 for k in expected_inner): problems.append('refined core facet was not retained as a two-sided interface')
    for uses in ledger.values():
        if len(uses)==2:
            ti,_,face0=uses[0]; tj,_,face1=uses[1]
            opposite0=next(v for v in combined[ti] if v not in face0); opposite1=next(v for v in combined[tj] if v not in face1)
            a,b,c=(points[i] for i in face0); normal=cross(sub(b,a),sub(c,a))
            if dot(normal,sub(points[opposite0],a))*dot(normal,sub(points[opposite1],a)) >= -1e-12:
                problems.append('same-sided shared face'); break
    if any(abs(vol(*(points[i] for i in t)))<1e-12 for t in combined): problems.append('nonpositive/degenerate tetrahedron')
    if len({tuple(sorted(key(points[i]) for i in t)) for t in combined})!=len(combined): problems.append('duplicate tetrahedron')
    if any(strict_overlap(points,a,b) for a,b in itertools.combinations(combined,2) if len(set(a)&set(b))<3): problems.append('strict tetrahedron overlap')
    return problems

def steiner(prefix):
    """Bounded, generic cavity-cone feasibility search around the worst free tet.

    This is an offline experiment.  It never changes a frozen PLC face and
    does not export a repaired TetGen mesh as a production result.
    """
    prefix=pathlib.Path(prefix); manifest=json.load(prefix.with_suffix('.manifest.json').open())
    nodes=read_nodes(prefix.with_suffix('.1.node')); node_ids=sorted(nodes)
    points=[nodes[i] for i in node_ids]; old_to_new={old:new for new,old in enumerate(node_ids)}
    shell=[tuple(old_to_new[i] for i in t) for t in read_elements(prefix.with_suffix('.1.ele'))]
    by_coord={key(p):i for i,p in enumerate(points)}
    core=[tuple(by_coord[key(manifest['points'][i])] for i in t) for t in manifest['materialized_core_tetrahedra']]
    fixed={fkey(manifest['points'],x) for x in manifest['outer_facets']+manifest['inner_refined_core_facets']}
    shell_ledger=boundary(points,shell)
    free={i for i,t in enumerate(shell) if all(fkey(points,f) not in fixed for f in faces(t))}
    if not free:
        print(json.dumps({'accepted':False,'reason':'no free-interior shell tetrahedron'},indent=2)); return 1
    worst=min(free,key=lambda i:quality_score(points,[shell[i]])[0])
    neighbours={i:[] for i in free}
    for uses in shell_ledger.values():
        if len(uses)==2 and uses[0][0] in free and uses[1][0] in free:
            a,b=uses[0][0],uses[1][0]; neighbours[a].append(b); neighbours[b].append(a)
    # Enumerate connected free-cell cavities containing the dynamic seed,
    # capped before work becomes a surrogate general mesher.
    max_cavities=8
    regions={(worst,)}; frontier=[(worst,)]
    for _ in range(3):
        next_frontier=[]
        for region in frontier:
            choices=sorted({n for cell in region for n in neighbours[cell]}-set(region))
            for n in choices:
                candidate=tuple(sorted(region+(n,)))
                if candidate not in regions and len(regions)<max_cavities:
                    regions.add(candidate); next_frontier.append(candidate)
        frontier=next_frontier
        if not frontier or len(regions)>=max_cavities: break
    before,lo,hi=quality_score(points,shell+core); trials=0; valid=[]
    for region in sorted(regions,key=lambda r:(len(r),r)):
        region_set=set(region); face_uses={}
        for cell in region:
            for face in faces(shell[cell]): face_uses.setdefault(fkey(points,face),face)
        # A cavity boundary face occurs once among its selected cells.  Its
        # cone replaces the removed region exactly when all validation gates
        # below accept it.
        internal={fkey(points,f) for cell in region for f in faces(shell[cell])
                  if len(shell_ledger[fkey(points,f)])==2 and all(u[0] in region_set for u in shell_ledger[fkey(points,f)])}
        boundary_faces=[f for k,f in face_uses.items() if k not in internal]
        vertices=sorted({v for f in boundary_faces for v in f})
        candidate_points=[]
        candidate_points.append(tuple(sum(points[v][axis] for v in vertices)/len(vertices) for axis in range(3)))
        # Deduplicate coordinate candidates deterministically.
        seen=set()
        for p in candidate_points:
            if key(p) in seen or key(p) in {key(x) for x in points}: continue
            seen.add(key(p)); trials+=1; trial_points=points+[p]; apex=len(points)
            replacement=[tuple(face)+(apex,) for face in boundary_faces]
            trial_shell=[t for i,t in enumerate(shell) if i not in region_set]+replacement
            problems=combined_geometry_problems(trial_points,trial_shell,core,manifest)
            if problems: continue
            score,new_lo,new_hi=quality_score(trial_points,trial_shell+core)
            if score>before+1e-12: valid.append((score,new_lo,new_hi,region,p))
    report={'accepted':False,'worst_free_shell_tet':worst,'cavities_considered':len(regions),'steiner_candidates_considered':trials,'before_score':before,'minimum_dihedral_degrees':lo,'maximum_dihedral_degrees':hi}
    if valid:
        best=max(valid,key=lambda x:x[0]); report.update({'best_geometry_valid_improvement':True,'after_score':best[0],'after_minimum_dihedral_degrees':best[1],'after_maximum_dihedral_degrees':best[2],'cavity_shell_tets':best[3],'steiner_point':best[4],'s4_qualified':best[0]>=5})
    else: report['reason']='no geometry-valid quality improvement in bounded free-interior Steiner cavities'
    print(json.dumps(report,indent=2)); return 0 if valid and max(valid,key=lambda x:x[0])[0]>=5 else 1
def repair(prefix):
    """Bounded generic 2<->3 search around the worst free shell tetrahedron."""
    prefix=pathlib.Path(prefix); manifest=json.load(prefix.with_suffix('.manifest.json').open()); nodes=read_nodes(prefix.with_suffix('.1.node')); imported_raw=read_elements(prefix.with_suffix('.1.ele'))
    points=[nodes[i] for i in sorted(nodes)]; old_to_new={old:new for new,old in enumerate(sorted(nodes))}; shell=[tuple(old_to_new[i] for i in t) for t in imported_raw]
    by_coord={key(p):i for i,p in enumerate(points)}; core=[tuple(by_coord[key(manifest['points'][i])] for i in t) for t in manifest['materialized_core_tetrahedra']]
    fixed={fkey(manifest['points'],x) for x in manifest['outer_facets']+manifest['inner_refined_core_facets']}
    ledger=boundary(points,shell); free=[]
    for i,t in enumerate(shell):
        if all(fkey(points,f) not in fixed for f in faces(t)): free.append(i)
    before,lo,hi=quality_score(points,shell+core)
    if not free:
        print(json.dumps({'accepted':False,'reason':'no free-interior shell tetrahedron','before_score':before},indent=2)); return 1
    worst=min(free,key=lambda i:quality_score(points,[shell[i]])[0])
    candidates=[]
    # 2->3: only a pair sharing a face with the chosen tet, wholly free.
    for face_uses in ledger.values():
        if len(face_uses)!=2 or worst not in [x[0] for x in face_uses]: continue
        left,right=face_uses[0][0],face_uses[1][0]
        if left not in free or right not in free: continue
        common=list(set(shell[left])&set(shell[right])); opposite=[next(v for v in shell[i] if v not in common) for i in (left,right)]
        a,b,c=common; d,e=opposite
        candidates.append(('2-3',(left,right),[(d,e,a,b),(d,e,b,c),(d,e,c,a)]))
    # 3->2: inspect each one-ring edge of the chosen tet. It must have exactly
    # three incident free shell tetrahedra and no core/materialized member.
    for edge in itertools.combinations(shell[worst],2):
        star=[i for i,t in enumerate(shell) if set(edge)<=set(t)]
        if len(star)!=3 or any(i not in free for i in star): continue
        rim=sorted(set().union(*(set(shell[i]) for i in star))-set(edge))
        if len(rim)==3: candidates.append(('3-2',tuple(star),[(rim[0],rim[1],rim[2],edge[0]),(rim[0],rim[1],rim[2],edge[1])]))
    legal=[]
    for kind,remove,replacement in candidates:
        trial=[t for i,t in enumerate(shell) if i not in remove]+replacement
        combined=trial+core; trial_ledger=boundary(points,combined)
        if any(len(v)>2 for v in trial_ledger.values()): continue
        if {k for k,v in trial_ledger.items() if len(v)==1}!={fkey(manifest['points'],x) for x in manifest['outer_facets']}: continue
        if any(abs(vol(*(points[i] for i in t)))<1e-12 for t in combined): continue
        if any(strict_overlap(points,a,b) for a,b in itertools.combinations(combined,2) if len(set(a)&set(b))<3): continue
        score,new_lo,new_hi=quality_score(points,combined)
        if score>before+1e-12: legal.append((score,kind,remove,replacement,new_lo,new_hi,trial))
    if not legal:
        print(json.dumps({'accepted':False,'reason':'no legal one-ring 2-3 or 3-2 quality improvement','worst_free_shell_tet':worst,'candidates_considered':len(candidates),'before_score':before,'minimum_dihedral_degrees':lo,'maximum_dihedral_degrees':hi},indent=2)); return 1
    best=max(legal,key=lambda x:x[0]); out=prefix.with_suffix('.repair.ele')
    with out.open('w') as f:
        f.write(f'{len(best[6])} 4 0\n')
        for i,t in enumerate(best[6],1): f.write(f'{i} '+' '.join(str(sorted(nodes)[v]) for v in t)+'\n')
    print(json.dumps({'accepted':True,'move':best[1],'replaced_shell_tets':best[2],'before_score':before,'after_score':best[0],'minimum_dihedral_degrees':best[4],'maximum_dihedral_degrees':best[5],'output':str(out)},indent=2)); return 0
def main():
    p=argparse.ArgumentParser(); s=p.add_subparsers(dest='mode',required=True)
    for name in ('export','audit','repair','steiner'): s.add_parser(name).add_argument('prefix')
    a=p.parse_args(); return export(a.prefix) if a.mode=='export' else (audit(a.prefix) if a.mode=='audit' else (repair(a.prefix) if a.mode=='repair' else steiner(a.prefix)))
if __name__=='__main__':
    try: sys.exit(main())
    except Exception as e: print(f'offline oracle refused: {e}',file=sys.stderr); sys.exit(2)
