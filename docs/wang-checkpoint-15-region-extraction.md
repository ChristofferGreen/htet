# Wang checkpoint 15: region extraction and output composition

## Pinned reference behavior

The authority for this stage is `DT::ColorVirtualTet`, `DT::ColorTets`, and
`DT::colorTetComponent` in `third_party/FHCCT-WYFDT_TEST/src/dt.cpp` at
`46e41e2439979a3e7db65fb135c0fcae3d53952e`.

`ColorTets` first finds a tetrahedron incident to the reference ghost vertex
and floods its connected component with `virtualID`.  Component traversal
crosses a tetrahedron face exactly when that face is not present in
`BndTri`.  It then assigns distinct positive identifiers to every remaining
component.  `ColorVirtualTet` may subsequently map explicitly requested hole
or layer components to `virtualID`; `RemoveTet` deletes the virtual
tetrahedra.  Without those optional requests, all finite non-virtual
components are retained.

## Prototype adaptation

The prototype's Delaunay seed removes its finite construction cage and has no
ghost tetrahedra in the recovered mesh.  `classify_canonical_plc_regions`
therefore seeds the reference virtual component from every unconstrained
finite hull face.  Both recovered outer subfaces and recovered core-interface
subfaces are flood barriers, matching the reference `BndTri` test.  This is a
representation change only: a cell is outside exactly when the corresponding
reference cell is connected to the ghost without crossing a recovered
constraint.

The application has a retained regular core that is generated independently
of Wang recovery.  Strictly interior background-cell witnesses nominate the
component on the core side of the recovered interface.  The classifier calls
that component `core` and the other finite non-virtual component `shell`.
`construct_canonical_plc_volume` keeps only the recovered shell cells, then
appends the separately conformed regular-core tetrahedra.  Core conformance,
quality repair, and final composition remain outside the Wang recovery
kernel.  The final surface/core validator, rather than region numbering,
decides whether the composed mesh is publishable.

The prototype does not expose the reference command-line hole, layer, or body
identifier remapping in this application path.  Those are output-selection
features, not alternate recovery operations, and the hexahedron/DC/core
pipeline supplies neither holes nor layer deletions.

## Focused gates

The checkpoint fixtures exercise the three source-derived component cases:

1. With no recovered barrier, all six tetrahedra of one cube are reached from
   the finite hull and map to the reference ghost/virtual component.
2. A closed recovered surface around the central cube of a 3-by-3-by-3 grid
   retains its six cells and classifies the other 156 as outside.
3. A closed outer surface plus a closed central core interface classifies six
   core cells and 156 shell cells, with no outside cells.

The two closed fixtures independently check that every constraint edge has
two incident constraint triangles.  Their tetrahedra are positively oriented,
and the extracted shell boundary is exactly the expected outer boundary plus,
where applicable, the core interface.  The existing nonmatching-PLC volume
fixture additionally runs the composed shell and regular core through the
closed-boundary, orientation, overlap, and provenance validator.

Checkpoint 15 is complete under the documented ghost/cage and retained-core
adaptations.
