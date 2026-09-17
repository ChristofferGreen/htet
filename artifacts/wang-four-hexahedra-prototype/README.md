# Four-hexahedra Wang prototype demo

This package is generated from the production N5 contained-noisy-sphere
transaction. The sphere is centered in the root tetrahedron and has a
0.23 base radius with 0.02 Perlin radial displacement, leaving a guaranteed
0.041 clearance to the closest root-tetrahedron face. It shows the target as
separate inspectable layers and as one validated output:

1. `01-four-hexahedra.vtk` — the root tetrahedron represented by four
   hexahedral sampling domains;
2. `02-dual-contour-surface.vtk` — the frozen, closed triangulated DC surface;
3. `03-implicit-tetrahedral-core.vtk` — the retained regular tetrahedral core;
4. `04-wang-transition.vtk` — transition tetrahedra produced by Wang recovery;
5. `05-complete-prototype.vtk` — complete published volume, with cell scalar
   `region=0` for transition and `region=1` for retained core;
6. `prototype-stages.svg` and `prototype-stages.png` — a four-stage visual
   overview;
7. `summary.json` — counts and the publication invariants.
8. `interactive-inspector.html` — a rotatable, layer-by-layer 3D inspector
   with clipping and cell picking. `prototype-data.js` is regenerated beside
   it by the exporter.

Open the VTK files in ParaView. For the complete output, color cells by
`region`, show edges, and use a clip plane to inspect the transition/core
interface.

Reproduce from the repository root:

```sh
cmake -S . -B build-owned
cmake --build build-owned --target wang_prototype_demo_export -j4
./build-owned/wang_prototype_demo_export artifacts/wang-four-hexahedra-prototype
python3 scripts/render_wang_prototype_demo.py artifacts/wang-four-hexahedra-prototype
# Optional raster copy when librsvg is installed:
rsvg-convert -w 1500 -h 420 artifacts/wang-four-hexahedra-prototype/prototype-stages.svg \
  -o artifacts/wang-four-hexahedra-prototype/prototype-stages.png
```

To inspect interactively, serve the package directory and open
`interactive-inspector.html` in a browser:

```sh
python3 -m http.server 8766 --directory artifacts/wang-four-hexahedra-prototype
```

For the inspector's mesh-resolution slider, keep the local rebuild service
running in a second terminal. It executes the same contained-sphere/DC/Wang
exporter and then reloads the inspector with the rebuilt data:

```sh
python3 scripts/wang_prototype_live_server.py --port 8767
```

The slider supports N4 through N12 and rebuilds the DC surface, Wang
transition, and implicit core from one sizing policy. The core uses red depth
four at N4, depth five at N5--N8, and depth six at N9--N12. Its retained
material-side clearance is half the realised maximum core-tet edge, so a finer core
grows toward the frozen DC sheet and leaves a thinner explicit transition.
The inspector reports the realised maximum core-tet edge, clearance, and core
and transition volumes; these values, rather than cell counts, show whether
the transition has physically narrowed.

The DC surface has zero boundary edges, zero non-manifold edges, and no
artificial closure triangles. Mesh-quality thresholds are diagnostics only.
Publication is based on the geometric/topological contract: frozen outer faces
and retained core are preserved, cells are positive and conforming, and strict
overlaps are absent.
