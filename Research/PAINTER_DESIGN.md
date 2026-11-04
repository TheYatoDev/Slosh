# PAINTER_DESIGN.md — Slosh Paint (fluid painting on surfaces)

Status: milestone 1 implemented and validated, 2026-11-03.
References studied:
- **Fluid Painter 1.2.20** and **Fluid Painter NSFW 0.8.12**, Blender add-ons by Miki3d.
- **FluidNinja LIVE**, a UE marketplace pack. Used for its UE plumbing, not its approach.

User decisions (2026-11-03):
- Geometry first, simulated mode second.
- Used in: editor authoring, runtime gameplay, Sequencer/animation, skeletal meshes, and static meshes/world.
- Ported presets ship inside the Slosh plugin.
- Materials: one master material per shading family plus a material instance per preset.

## 1. What Fluid Painter does (measured from its library .blend)

A preset is a curve object with the `Fluid Painter` geometry-node group (323 nodes) plus a Smooth modifier (5–25
iterations). Its units are millimetres: `Unit = Fluid Scale × 1 mm`.

1. **Stroke.** The artist draws a curve on a surface (Blender's draw tool, surface depth). Pen pressure becomes the
   per-point curve radius.
2. **Resample.** Resample by length, spacing = `Thickness × Unit`. Radius = `pressure × U(1 − R/2, 1 + R/2)`, where R is
   Roughness, clamped to [0.1, 5].
3. **Offset+.** Move the stroke along the nearest-surface normal by `Offset × Unit`.
4. **Drops.**
   - Mode 0: none.
   - Mode 1, static bulb: over the last `Length` fraction of the stroke the radius grows as
     `1 + (Size − 1)·f^Exponent`.
   - Mode 2, animated: only a window of length `Length` is drawn, and it travels along the stroke.
     The phase is `fract(Move × Speed × rand + …)`, with optional per-stroke random speed and offset.
5. **Blobs.** On every sample sits an ellipsoid with semi-axes
   `Unit × clamp(Thickness × profile × radius, 0.1, 5·Thickness) × (1, 1, 1 − 0.99·Flatness)`, its Z aligned to the surface
   normal. Icosphere subdivision is picked by size.
6. **Merge.** Mesh → points → volume → mesh (a smooth union, threshold 0.1, voxel ≈ Unit·Thickness/quality), then the
   Smooth modifier.
7. **Attributes for shading.**
   - `Distance` / Fading+: distance to the drawing surface mapped by `Fading × Unit` to [0, 1].
   - `UV Scale` = MaterialUVScale / Scale.
   - Object coordinates.
8. **Layers A/B** (`Fluid Layer` group). Points are scattered on the fluid surface at density
   `Amount / Scale² × 1e5 per m²`, each carrying a small icosphere (radius `Size × Unit`) or a thin rod (Type).
   - Random scale `(1 − SizeRandom) + SizeRandom·U(0.1, 1)`.
   - Pushed inside by `Size × Unit` if "inside", then shifted along the normal by `Shift × Unit`.
   - Layer materials are bubbles (air or soap) and sugar (clear or colored).

43 presets: 34 regular (8 categories) and 9 NSFW. Their geometry values were extracted to
`fp_presets.json` (scratchpad) and are baked into the UE preset assets.

**Materials.** All procedural, no textures. Every graph is a combination of these patterns:
- Principled base values: color, roughness, IOR, subsurface, transmission, coat, metallic.
- A facing ramp (Layer Weight → Invert → ColorRamp) driving rim opacity or color.
- A Fresnel mix between a tinted Transparent BSDF and gloss, for water-like presets.
- A `Distance` ramp fading the base into the surface.
- Noise/Voronoi in object coordinates × UV Scale, driving color variation, roughness and bump.
- Is-Shadow-Ray → transparent, i.e. no shadow casting.

## 2. What FluidNinja LIVE contributes

A Niagara Grid2D smoke/water simulation. Its "painters" (`Ninja_Painter`, `*_ReadAndPaint` for SDF, RVT, splines and
geometry collections, plus a bone reader) inject density and velocity into the grid from tracked objects. Output reaches
surfaces through render targets and wet-mapped composite materials.

It is a different product, a 2D grid simulation rather than surface strokes. Its useful lessons for Slosh Paint:
- Bone tracking via tracker particles.
- Painting from arbitrary sources: mouse, sockets, splines, collisions.
- Data-asset presets.
- Wet-mapping composites (the basis for the later simulated/wetness mode).

## 3. Architecture (milestone 1: procedural strokes)

```
USloshPaintComponent  (on the painted actor, attached to the painted primitive)
  ├─ Strokes: TArray<FSloshPaintStroke>     ← saved source of truth (points in attachment space + preset + seed)
  ├─ Reveal (Interp) / DropTime (Interp) / bAnimateDrops
  ├─ async build → FSloshPaintMesher (worker thread)
  │    resample → offset → drop profile → ellipsoid field → smooth-union SDF → marching cubes
  │    → Taubin smooth → outward winding → fading / object-coord UVs → layer scatter
  └─ transient children, rebuilt from Strokes:
       UDynamicMeshComponent per attachment group (component, or one per bone on skeletal meshes)
       UInstancedStaticMeshComponent per (group, layer material)
USloshPaintPreset (UDataAsset)   — mirrors the Fluid Painter inputs 1:1, plus materials and smoothing
USloshPaintBrushComponent        — runtime brush: trace from owner, camera or screen point; spacing; pressure
USloshPaintLibrary               — BP helpers: surface hit (skinned-mesh refinement), find/add paint component
SloshEditor: "Slosh Paint" editor mode + click-drag paint tool (any visible surface, undo/redo)
```

**Spaces and skeletal meshes.**
- On static targets, stroke points live in the paint component's space; it is attached to the painted primitive, so
  paint moves with it.
- On skeletal targets, each point records its dominant skin bone and is stored in that bone's space. Strokes are built
  per bone group, and each group's mesh is attached to its bone, so paint follows the animation at no per-frame
  meshing cost.
- A stroke that crosses bones is split into runs that overlap by one sample.
- Hits on skeletal meshes are refined from the physics-asset body onto the CPU-skinned render surface
  (`GetSkinnedVertexPosition`) so paint lies on the skin, not on the capsules.

**Sequencer/animation.**
- `Reveal` (0–1 of the drawn length) and `DropTime` (Fluid Painter's "Drop Move") are `Interp` properties, so they can
  be keyed.
- `bAnimateDrops` advances DropTime with game time.
- Rebuilds are asynchronous and coalesced to at most one in flight.

**Materials.**
- `M_SloshPaint_Opaque`: Subsurface shading, dithered opacity mask for the facing and fading ramps. UE's Subsurface
  model adds SubsurfaceColor to diffuse even at full density, so the master feeds `SubsurfaceColor × SubsurfaceAmount`
  and `Opacity = 1 − SubsurfaceAmount` (Opacity is scattering density there).
- `M_SloshPaint_Translucent`: forward-shaded translucency with refraction, Fresnel and tint.
- Shared material functions: facing ramp, fading ramp, object-coordinate noise (color, roughness, bump).
- Per-preset instances under `Slosh/Content/Paint/Presets/{Regular,NSFW}`.
- Inputs from the mesh:
  - UV0 = (fading, stroke arc length / Unit).
  - UV1 = rest object coordinates × UVScale / Scale in metres, xy.
  - UV2 = the same coordinates, z.

## 4. Milestones

| M | Scope | Status |
|---|---|---|
| 1 | Stroke model, mesher, drops (static/animated), flatness/offset/roughness, layers, skeletal bone binding, Sequencer properties, runtime brush, painter Blueprints, editor paint mode, master materials and all 43 preset instances and data assets | done (§6) |
| 2 | Simulated mode: painted liquid flows and drips over surfaces (surface-particle or texture-space film solver driven by gravity, motion and curvature), plus a wetness render target feeding the materials | planned |
| 3 | Smooth skinning (blend weights instead of per-bone rigid groups), bake to static mesh, stroke recording/playback | planned |

## 5. Risks

- **Preset licensing.** Fluid Painter is a paid add-on and the zips carry no license text. The user chose to ship the
  ported presets in the plugin; the GitHub repository is public. Confirm redistribution rights before pushing preset
  content.
- **CPU meshing cost.** Animated drops and Reveal rebuild strokes. They stay asynchronous with one rebuild in flight;
  very long animated strokes may lag by a frame or two.
- **Skeletal refinement needs CPU vertex access.** It is available in the editor; at runtime the skeletal mesh needs
  "Allow CPU Access", otherwise painting falls back to physics-body hits.

## 6. Milestone 1 validation (2026-11-03)

**Content** (plugin, `/Slosh/Paint/`):
- `Materials/M_SloshPaint_{Opaque,Translucent}`.
- `Presets/{Regular,NSFW}/DA_SloshPaint_*`: 43 data assets.
- `Presets/.../Materials/MI_SloshPaint*`: 45 instances.
- `Blueprints/`:
  - `BP_SloshPainter`: mouse cursor painting, 9 regular presets on keys 1–9.
  - `BP_SloshPainter_NSFW`: the same with the NSFW presets.
  - `BP_SloshNozzle`: auto-paints along its forward axis (orange aim arrow in the editor).

**Static targets.** Eight presets were painted on spheres in the editor through MCP (spiral and drip strokes,
UnitScale 8): Honey, Milk, Chocolate Brown, Water+, Slime, Glaze+, Ketchup and Cum. All render correctly.
- Bead and drip-bulb shapes match the add-on.
- Glaze+ carries its sprinkle layer.
- Translucent presets refract.

**Skeletal target.** The demo game-mode test (`-SloshPaintTest`) paints an animated mannequin with traced strokes and
captures two animation phases. It also paints a sphere, with traced strokes and with the nozzle Blueprint sweeping
over it.
- Chest, arm and belly strokes resolve to 3–4 bones each (e.g., clavicle_r / spine_04 / clavicle_l /
  upperarm_twist_01_l).
- Their bone-attached pieces stay on the skin through the walk cycle.

**Bugs found and fixed during validation:**
- **Inside-out meshes.** Unreal is left-handed and GeometryCore's triangle normal is (C − A) × (B − A), so an
  outward-wound closed mesh has a *negative* signed-volume sum. The winding test assumed the opposite.
- **Glaze+ buried.** Fluid Painter's points-to-volume radius `Unit·clamp(T, 0.5, 5)/(1 + 0.5F)` inflates every
  ellipsoid. Without it, thin-flat presets sank into the surface.
- **Worker crash (access violation in `AppendNormals`).** `FDynamicMesh3`'s attribute set points back at its mesh,
  and UE containers relocate elements bitwise. Meshes held by value in a growing `TArray` or `TMap` (more than ~10 bone
  groups) were left with dangling overlay parents. Stroke pieces and bone groups are now heap-allocated.
- **Dark opaque presets rendered white.** This was the subsurface wiring above.

