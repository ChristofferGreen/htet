"""Exact rational segment/triangle witnesses for exported binary64 PLC input."""
from fractions import Fraction
from pathlib import Path
import json
import math
import sys

def read_points(path):
    lines = Path(path).read_text().splitlines()
    count = int(lines[0].split()[0])
    return {int(v[0]): tuple(Fraction(float(s)) for s in v[1:4])
            for v in (line.split() for line in lines[1:count+1])}

def sub(a,b): return tuple(x-y for x,y in zip(a,b))
def dot(a,b): return sum(x*y for x,y in zip(a,b))
def cross(a,b): return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])

def witness(path, edge, face):
    pts=read_points(path)
    p,q=(pts[i] for i in edge)
    a,b,c=(pts[i] for i in face)
    direction=sub(q,p);e1=sub(b,a);e2=sub(c,a)
    h=cross(direction,e2);den=dot(e1,h)
    if not den: return {"parallel":True}
    s=sub(p,a);u=dot(s,h)/den
    r=cross(s,e1);v=dot(direction,r)/den;t=dot(e2,r)/den
    strict=0<t<1 and 0<u and 0<v and u+v<1
    return {"file":Path(path).name,"edge":edge,"face":face,"strict_intersection_exact":strict,
            "segment_t":float(t),"triangle_barycentric":[float(1-u-v),float(u),float(v)]}

root=Path(sys.argv[1])
for name,edge,face in [("step-n6.poly",[3,8],[0,1,2]),("step-n6.poly",[7,8],[4,5,6]),
                       ("qef-surface.poly",[10,1],[0,8,9])]:
    print(json.dumps(witness(root/name,edge,face)))
for name in ["shell-n6.poly","shell-n8.poly","shell-n8-nearzero.poly"]:
    path=root/name;pts=read_points(path);lines=path.read_text().splitlines();cursor=len(pts)+1
    count=int(lines[cursor].split()[0]);cursor+=1;minimum=180.0
    for i in range(count):
        kind=int(lines[cursor].split()[2]);ids=list(map(int,lines[cursor+1].split()[1:]));cursor+=2
        if kind!=1: continue
        for v in range(3):
            a=sub(pts[ids[(v+1)%3]],pts[ids[v]]);b=sub(pts[ids[(v+2)%3]],pts[ids[v]])
            cosine=float(dot(a,b))/math.sqrt(float(dot(a,a))*float(dot(b,b)))
            minimum=min(minimum,math.degrees(math.acos(max(-1,min(1,cosine)))))
    print(json.dumps({"file":name,"minimum_frozen_surface_angle_degrees":minimum}))
