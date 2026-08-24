# Maya-quality LOD camera manipulator

## Objective

Replace the current world-space debug-line camera widget with a dependable,
camera-specific transform manipulator comparable to Maya's move and rotate
tools. The result must remain comfortable on a trackpad, use the same geometry
for drawing and picking, and preserve the viewer's non-blocking LOD update
model.

Maya's move manipulator provides individual axes, planar handles, and a free
centre handle. Its rotate manipulator provides axis rings, a view-aligned outer
ring, and an arcball. Maya also supports world, object/local, gimbal, and custom
orientations; snapping; active-handle feedback; pivot control; and cancellable,
undoable drags. The relevant Autodesk references are:

- [Move manipulator context](https://help.autodesk.com/cloudhelp/ENU/MayaCRE-Tech-Docs/CommandsPython/manipMoveContext.html)
- [Rotate manipulator context](https://help.autodesk.com/cloudhelp/ENU/MayaCRE-Tech-Docs/CommandsPython/manipRotateContext.html)

The LOD camera does not need Maya's scale tool or general-purpose hierarchy
pivot behavior. It does need Maya-quality selection, movement, rotation,
feedback, snapping, cancellation, and coordinate-space handling, plus a camera
frustum that accurately represents the LOD calculation.

The visual language follows Maya as well: thin red, green, and blue axes;
compact circular cone arrowheads; solid planar handles and a small centre;
pure yellow active
feedback; constrained rotation rings inside a neutral outer view ring; and no
axis labels or large secondary arcball ring. The camera frustum uses a subdued
selection green so it cannot be confused with the active handle.
Planar and centre handles are modeled as quads so their outlines contain only
the four perimeter edges; the centre is wireframe-only and rasterization never
leaks an implementation diagonal into the displayed manipulator.

## Current deficiencies

- Handles use fixed world-space dimensions, so their apparent size varies with
  distance and field of view.
- Move mode contains only three bare axis segments. It lacks arrowheads,
  two-axis plane handles, and free screen-plane movement.
- Rotate mode contains only three full world-axis rings. It lacks a view ring,
  arcball, local camera rotation, and front/back visual separation.
- Translation converts incremental cursor movement through the squared length
  of a projected axis. Movement becomes ill-conditioned when that axis points
  toward the view.
- Rotation uses the cursor's two-dimensional polar angle around the projected
  pivot for every ring. That does not represent motion in a tilted ring plane.
- Picking and drawing separately recreate the same nominal handles, allowing
  their shapes and tolerances to diverge.
- There is no hover highlight, active-handle highlight, axis label, cursor cue,
  drag delta, or explicit indication of the current coordinate space.
- There is no snapping, Escape-to-cancel, drag-level undo/redo, or keyboard
  alternative for selecting obscured handles.
- Only world axes are available. Camera-local right, up, and forward axes are
  more useful for positioning an LOD camera.
- The camera icon is a fixed decorative pyramid rather than a frustum derived
  from its field of view and aspect ratio.
- Camera orientation is retained as independently normalized forward and up
  vectors rather than a rotation representation with an enforced orthonormal
  frame.
- Manipulator rendering is always in front of the mesh, but complete rings do
  not distinguish their front and rear arcs, making their orientation unclear.
- Manipulator interaction remains embedded in the viewer's main loop and is
  difficult to test independently.

## Interaction contract

- `Q`, `W`, and `E` select, move, and rotate just as they do now.
- Clicking the camera selects it. Clicking empty space deselects it unless a
  view-navigation modifier is active. Dragging empty space to orbit or pan is
  navigation rather than a click, so it preserves the selected camera and its
  manipulator.
- The widget remains approximately constant in screen size at every usable
  view distance and window size.
- X, Y, and Z keep the conventional red, green, and blue colors. Hovered and
  active handles receive a clear highlight without losing their axis identity.
- Move axes constrain to one coordinate; plane handles constrain to two; the
  centre handle moves in the editor view plane.
- Rotate axis rings constrain rotation about one axis; the outer ring rotates
  about the editor view direction; the centre acts as an arcball.
- World and local spaces are available. Local space follows the LOD camera's
  right, up, and forward directions. World remains the initial default to
  preserve existing behavior.
- Shift retains the current precision modifier. A separate modifier or toggle
  enables configurable translation and angular snapping.
- Escape restores the exact pose from drag start. A completed drag produces
  one undo record regardless of its number of pointer frames.
- LOD reconciliation begins after a completed drag and remains incremental and
  responsive. A cancelled or zero-delta drag performs no reconciliation.
- Manipulator picking must use a forgiving invisible hit region while visible
  strokes remain visually thin.
- The displayed frustum must use the same position, orientation, field of view,
  and aspect ratio used by projected-size and visibility calculations.

## Implementation TODO chain

### 1. Manipulator state and shared handle model

- [x] Extract camera-manipulator interaction from `main.cpp` into a focused,
      renderer-independent component.
- [x] Introduce explicit handle identifiers for axes, planes, centre move,
      axis rings, view ring, and arcball.
- [x] Introduce world/local coordinate-space state and derive a stable
      orthonormal basis for each space.
- [x] Store camera orientation in a robust representation or rigorously
      re-orthonormalize its basis after every edit.
- [x] Generate one canonical handle geometry/metadata description used by both
      rendering and hit testing.
- [x] Add deterministic tests for handle identity, basis orientation, and
      camera-pose round trips.

### 2. Screen-space sizing and selection

- [x] Convert the desired pixel radius into a world-space scale at the camera
      pivot using viewport height, field of view, and pivot depth.
- [x] Clamp behavior near or behind the editor camera without NaNs, inverted
      axes, or unbounded scale.
- [x] Give strokes, arrowheads, planes, rings, and the centre distinct but
      overlapping-safe hit regions.
- [x] Resolve overlaps using handle priority, screen distance, and depth rather
      than axis iteration order.
- [x] Add hover state and freeze the selected handle for the entire drag.
- [x] Test constant apparent size and stable selection across zoom, window
      aspect ratios, high-DPI scaling, and nearly coincident projected axes.

### 3. Robust move manipulation

- [x] Implement ray/axis closest-point translation based on the drag-start
      state rather than accumulated frame deltas.
- [x] Add a stable fallback drag plane when an axis is nearly parallel to the
      viewing ray.
- [x] Implement XY, XZ, and YZ movement using ray/plane intersections.
- [x] Implement centre-handle movement in the editor view plane.
- [x] Add arrowheads, plane squares, a centre handle, and axis labels.
- [x] Validate move results at near-parallel, edge-on, close, distant, and
      off-centre viewpoints in both world and local spaces.

### 4. Robust rotate manipulation

- [x] Implement axis-ring rotation using drag-start and current ray/ring-plane
      intersections with signed three-dimensional angles.
- [x] Add a stable screen-tangent fallback for edge-on rotation rings.
- [x] Add a view-aligned outer rotation ring.
- [x] Add a virtual arcball centre handle with continuous motion across its
      boundary.
- [x] Preserve a valid right-handed orthonormal camera frame after arbitrarily
      long rotations.
- [x] Visually distinguish front and rear ring arcs while keeping handles easy
      to acquire.
- [x] Validate rotations through angle wraparound, poles, edge-on rings,
      repeated full turns, and both coordinate spaces.

### 5. Maya-grade feedback and control

- [x] Highlight hovered and active handles and dim inactive competing handles
      during a drag.
- [x] Select appropriate pointer cursors and show concise handle/space feedback.
- [x] Display the current translation or angular delta during manipulation.
- [x] Add configurable translation and rotation snapping with distinct
      absolute/relative semantics where applicable.
- [x] Preserve Shift precision independently of snapping.
- [x] Add Escape cancellation that restores the bitwise-equivalent starting
      pose and does not trigger LOD work.
- [x] Add drag-level undo and redo with one history entry per completed drag.
- [x] Add keyboard-accessible handle and coordinate-space selection.

### 6. Camera-specific visualization

- [x] Replace the fixed camera pyramid with a frustum derived from the actual
      LOD camera field of view and aspect ratio.
- [x] Keep the camera body/frustum selectable without making its visible lines
      excessively thick.
- [x] Add a camera-local forward/aim cue and make local translation along the
      viewing direction unambiguous.
- [x] Evaluate an optional aim-target handle; retain the arcball and explicit
      frustum aim cue when that is clearer for placing this free LOD camera.
- [x] Verify that rendered frustum boundaries agree with LOD visibility tests
      for representative points.

### 7. Viewer and LOD integration

- [x] Replace the old inline widget path with the extracted manipulator without
      regressing ordinary orbit, pan, dolly, selection, or Dear ImGui capture.
- [x] Keep manipulation visually interactive while deferring expensive mesh
      reconciliation until commit.
- [x] Ensure commit, cancel, undo, redo, and zero-delta release produce the
      correct reconciliation behavior.
- [x] Ensure a new drag or control change supersedes unfinished reconciliation
      and converges only to the newest camera pose.
- [x] Preserve trackpad-only operation and precision scrolling.
- [x] Update the controls panel and implementation documentation.

### 8. Automated and visual verification

- [x] Add unit tests for ray construction, hit ranking, screen-space scale,
      axis translation, plane translation, ring rotation, view rotation,
      arcball rotation, snapping, cancellation, and history.
- [x] Add integration tests for selection/mode transitions and ImGui input
      capture boundaries.
- [x] Add scripted pose sequences proving that every committed manipulation
      drives LOD split and merge convergence correctly.
- [x] Add deterministic visual baselines for move and rotate modes in world and
      local spaces, including hovered and active states.
- [x] Visually inspect close, distant, oblique, edge-on, and high-DPI cases and
      correct them until handle visibility and selection match expectations.
- [x] Run the complete release test suite without skips.
- [x] Run focused AddressSanitizer and UndefinedBehaviorSanitizer coverage for
      manipulator mathematics, cancellation/history, and LOD integration.
- [x] Rebuild and manually inspect the release viewer after the final change.

## Verification record

Verified on 2026-08-23 against the canonical release build:

- The complete release suite passed without skips: 162 test cases and 233,739
  assertions.
- Focused manipulator, frustum, malformed-command, and LOD tests passed: 13
  test cases and 151 assertions.
- AddressSanitizer and UndefinedBehaviorSanitizer passed the focused
  manipulator and LOD reconciliation set: 15 test cases and 178 assertions.
  Leak detection was disabled because Apple AddressSanitizer does not support
  it; address and undefined-behavior checks remained enabled and fail-fast.
- Deterministic Retina baselines are stored in
  `tests/visual_baselines/camera-manipulator-move.png` and
  `tests/visual_baselines/camera-manipulator-rotate.png`. They cover a hovered
  world-space plane handle and an active local-space rotation ring.
- `--manipulator-close-check`, `--manipulator-distant-check`, and
  `--manipulator-edge-on-check` were visually inspected in addition to the
  oblique move/rotate baselines. Handle size remained stable, the edge-on axis
  remained identifiable, and all cases were inspected at Retina scale.
- The optional aim-target handle was deliberately omitted. The accurate
  selectable frustum, explicit forward cue, local forward translation, and
  arcball already express the free camera pose without adding a second pivot
  with ambiguous LOD semantics.

## Completion criteria

The chain is complete only when every checkbox is verified. The camera must be
selectable and comfortably transformable with a laptop trackpad at any useful
zoom; world and local manipulation must be mathematically stable; all expected
Maya-style move and rotate handles must provide clear feedback; cancel,
snapping, undo, and redo must behave predictably; the visible frustum must
match the LOD camera; and automated, sanitizer, release, and deterministic
visual validation must all pass.
