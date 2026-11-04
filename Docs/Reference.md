# Slosh API Reference

This page lists every user-facing type in the Slosh plugin (`SloshDemo/Plugins/Slosh/Source/Slosh/Public`) and the two demo-module helper actors (`SloshDemo/Source/SloshDemo/SloshSkelTestActors.h`). Each section gives the class's purpose, how to use it in a level, every property and function with its exact name, type, default, range and units, and any behaviour that only shows up in the `.cpp`.

Related pages: [Architecture](./Architecture.md) (modules and data flow), [Simulation](./Simulation.md) (solver internals), [Rendering](./Rendering.md) (surface modes), [Materials](./Materials.md) (liquid tuning), [Slosh Paint](./SloshPaint.md) (painted strokes in depth), [Testing](./Testing.md), [Troubleshooting](./Troubleshooting.md).

---

## Contents

1. [Conventions and hard limits](#1-conventions-and-hard-limits)
2. [Enums](#2-enums)
3. [ASloshVolume](#3-asloshvolume)
4. [USloshMaterialAsset, FSloshPhysicalProperties, FSloshVisualProperties](#4-usloshmaterialasset-fsloshphysicalproperties-fsloshvisualproperties)
5. [USloshColliderComponent](#5-usloshcollidercomponent)
6. [USloshSkeletalColliderComponent](#6-usloshskeletalcollidercomponent)
7. [USloshEmitterComponent](#7-usloshemittercomponent)
8. [Manipulators: USloshManipulatorComponent, USloshDrainComponent, USloshForceFieldComponent](#8-manipulators)
9. [ASloshDrainActor, ASloshForceFieldActor](#9-asloshdrainactor-asloshforcefieldactor)
10. [USloshSurfaceMeshComponent](#10-usloshsurfacemeshcomponent)
11. [USloshSubsystem, USloshRenderEngineSubsystem](#11-usloshsubsystem-usloshrenderenginesubsystem)
12. [Slosh Paint types](#12-slosh-paint-types)
13. [Demo-module helpers: ASloshSkelTestCharacter, ASloshPourNozzle](#13-demo-module-helpers)
14. [Recipes](#14-recipes)

---

## 1. Conventions and hard limits

### Units and spaces

| Quantity | In the Unreal API (properties, Blueprint) | Inside the solver (`SloshCore`) |
|---|---|---|
| Length | cm | m (`CellSize * 0.01`) |
| Velocity | cm/s | m/s |
| Acceleration (force fields) | cm/s² (gravity is 980) | m/s² |
| Force | `GetLastFluidForce`: kg·cm/s² (Unreal units); `GetLastPressureForceNewtons`: N | N |
| Volume | liters (getters) | m³ |
| Density | kg/m³ | kg/m³ |
| Kinematic viscosity | m²/s | m²/s |

- **Domain space.** The volume's actor location is the domain centre and its rotation orients the grid. **Actor scale is ignored.** Sim space puts the domain's min corner at the origin, in metres (`ASloshVolume::GetSimToWorld`).
- **Gravity.** The world's `GravityZ` is rotated into sim space every frame, so a rotated volume still pours "down" in world space (`SloshVolume.cpp:115`).
- **Component scale.** Colliders, emitters and manipulators scale their shapes by the **largest absolute axis** of their world scale (`GetScale3D().GetAbsMax()`). A non-uniformly scaled parent does not produce a non-uniform shape (see [Troubleshooting](./Troubleshooting.md)).
- **Particle density.** A full cell holds 8 particles at rest. Every litre conversion in the code uses this rule: particles per litre = `8 / (CellSize_m³ · 1000)`. At 5 cm cells that is 64 particles/L, at 2.5 cm 512/L, and at 1.6 cm ≈ 1953/L.
- **Delayed values.** Anything read back from the GPU is 2 to 4 frames old (4-slot readback ring, typically 3 frames). These getters are marked *(delayed)* below. Never use them as authoritative for the current frame.

### Hard limits

| Limit | Value | Where | What happens past it |
|---|---|---|---|
| Colliders per domain | 64 (`Slosh::MaxColliders`) | `SloshTypes.h:95` | Extra colliders are silently skipped, in subsystem registration order (`SloshVolume.cpp:416`). Skeletal-mesh capsules count too. |
| Manipulators per domain | 32 (`Slosh::MaxManipulators`) | `SloshTypes.h:96` | Extra ones are silently skipped (`SloshVolume.cpp:393`). Disabled ones and zero-strength force fields don't count. |
| Particle capacity | 1024 to 16,777,216 (`Slosh::MaxParticleCapacity`) | `SloshProxy.cpp:346` | Clamped. Emission stops at capacity. |
| Grid resolution per axis | 4 to 256 | `SloshProxy.cpp:345` | Clamped in the solver only. `GridResolution` has no editor clamp, so the actor's bounds can disagree with the simulated grid. |
| Surface mesh vertices | 1024 to 8,388,608 | `SloshVolume.cpp:265` | Clamped. Surface past the budget is dropped. |
| Splat footprint | 32 px (`MaxSplatPixels`) | `SloshRenderer.cpp:52` | Close-up splats are capped. |

---

## 2. Enums

| Enum | Header | Values (default first where a property uses it) | Meaning |
|---|---|---|---|
| `ESloshSurfaceMode` | `SloshVolume.h` | `Splats`, `Raymarched`, `Mesh` | How the liquid surface is drawn. **Splats** are screen-space particle splats with depth smoothing: the cheapest option, but thin films can look beady. **Raymarched** ray-marches a smoothed density field per pixel with Slosh shading, so it is smooth by construction. **Mesh** turns the same field into a GPU triangle mesh every frame and draws it with a UE material (engine lighting and shadows). |
| `ESloshPressureSolverType` | `SloshVolume.h` | `Auto`, `MGPCG`, `RBGS` | **Auto** uses RBGS at or below `AutoSolverCellThreshold` cells and MGPCG above it. **MGPCG** is the multigrid-preconditioned CG solver. **RBGS** is red-black SOR with a fixed iteration count. |
| `ESloshTransfer` | `SloshMaterialAsset.h` | `APIC`, `FLIP` | Particle/grid transfer. APIC is low-noise and conserves angular momentum. FLIP is a PIC/FLIP blend set by `FlipRatio`. |
| `ESloshColliderShapeType` | `SloshColliderComponent.h` | `Sphere`, `Box`, `Capsule` | Analytic collider shape. |
| `ESloshCouplingMode` | `SloshColliderComponent.h` | `OneWay`, `BuoyancyAndDrag`, `PressureAndDrag` | See [§5](#5-usloshcollidercomponent). |
| `ESloshEmitterShape` | `SloshEmitterComponent.h` | `Sphere`, `Box` | `Box` is oriented with the component. |
| `ESloshEmitRateMode` | `SloshEmitterComponent.h` | `ParticlesPerSecond`, `LitersPerSecond` | `LitersPerSecond` doesn't depend on cell size. |
| `ESloshManipulatorShapeType` | `SloshManipulatorComponent.h` | `Sphere`, `Box`, `Capsule`, `Everywhere` | `Capsule` runs along local Z. `Everywhere` covers the whole domain; a drain with this shape empties the volume. |
| `ESloshForceType` | `SloshManipulatorComponent.h` | `Directional`, `Radial`, `Swirl`, `Drag` | See [§8](#8-manipulators). |
| `ESloshPaintDropMode` | `SloshPaintPreset.h` | `None`, `Static`, `Animated` | Fluid Painter drop modes. |
| `ESloshPaintAim` | `SloshPaintBrushComponent.h` | `ComponentForward`, `MouseCursor`, `ScreenCenter` | Brush ray source. |

---

## 3. ASloshVolume

`UCLASS(BlueprintType, Blueprintable)`, header `SloshVolume.h`.

A box-shaped liquid simulation domain: a GPU FLIP/APIC solver on a MAC grid (sparse 8³ brick storage). It owns the simulation proxy, gathers emitters, colliders and manipulators each frame, renders the liquid, and passes delayed GPU feedback back to colliders.

**Use in a level.** Drag a *Slosh Volume* into the level. The root `Bounds` box component (visible in the editor, hidden in game, no collision) shows the domain, which is `GridResolution × CellSize` cm, recomputed in `OnConstruction`. Set `CellSize` first, because it sets the detail level. Then size the domain with `GridResolution`, choose a liquid (`Material`, or the inline `Physical`/`Visual`), and set `InitialFillBox`, or turn off `bFillOnBeginPlay` and use emitters. The liquid exists only during play: nothing is simulated or rendered in an editor viewport outside PIE/-game.

**Tick order.** The volume ticks in `TG_PostPhysics`, so collider transforms are this frame's physics results. Colliders apply the resulting forces in the next frame's `TG_PrePhysics`.

**Live edits.** The solver settings (`BuildSolverSettings`) and render settings (`UpdateRenderRegistration`) are rebuilt every tick, so nearly every property takes effect on the next frame while playing. There are two exceptions. Changing `GridResolution` or `SparseStorageBudget` reallocates the GPU state, which **empties the volume**, and `InitialFillBox` is not re-applied. Changing `CellSize` doesn't reallocate (the layout key is only `GridResolution` plus `SparseStorageBudget`), but it resizes the domain under the existing particles, so call `ResetSimulation` afterwards.

### 3.1 Properties: Domain (`Slosh|Domain`)

| Property | Type | Default | Range / units | What it does |
|---|---|---|---|---|
| `GridResolution` | `FIntVector` | `(64, 64, 48)` | cells; the solver clamps each axis to 4..256 | Cells per axis. Cost and memory scale with the cell count. The first and last cell on each axis are solid walls. |
| `CellSize` | `float` | `5.0` | cm, ClampMin 0.5 | Edge length of one cell. Sets the size of the smallest feature: films and streams thinner than about 1 to 2 cells break into beads. |
| `ParticleCapacity` | `int32` | `262144` | ClampMin 1024; the solver clamps to 16M | **Maximum** live particles. GPU buffers start empty and grow by 1.5× on demand, copied on the GPU with no reset, so a high cap costs nothing until it is used. |

The defaults give a domain of 320 × 320 × 240 cm.

### 3.2 Properties: Liquid (`Slosh`)

| Property | Type | Default | EditCondition | What it does |
|---|---|---|---|---|
| `Material` | `TObjectPtr<USloshMaterialAsset>` | null | — | The liquid type. When it is set, its `Physical` and `Visual` are used and the inline structs below are greyed out and **ignored**. Any `USloshPaintPreset` (Fluid Painter preset) can go here. |
| `Physical` | `FSloshPhysicalProperties` | see [§4](#4-usloshmaterialasset-fsloshphysicalproperties-fsloshvisualproperties) | `Material == nullptr` | Inline solver properties. |
| `Visual` | `FSloshVisualProperties` | see [§4](#4-usloshmaterialasset-fsloshphysicalproperties-fsloshvisualproperties) | `Material == nullptr` | Inline render properties (the classic water look). |

Resolution rule (`SloshVolume.h:310-311`): `GetPhysical()` returns `Material ? Material->Physical : Physical`, and `GetVisual()` does the same for `Visual`. The volume never mixes the two: an asset replaces both structs.

### 3.3 Properties: Initial state (`Slosh|Initial`)

| Property | Type | Default | Units | What it does |
|---|---|---|---|---|
| `bFillOnBeginPlay` | `bool` | `true` | — | At BeginPlay, queue a fill of `InitialFillBox`. |
| `InitialFillBox` | `FBox` | Min `(-155, -155, -115)`, Max `(-75, 155, 15)` | cm, relative to the domain centre, domain-local axes | Region filled at rest density (8 particles per cell). It is snapped outward to whole cells and clamped to cells 1..N-1, so the wall layer stays empty. The default is the dam-break column against the −X wall. |

### 3.4 Properties: Solver (`Slosh|Solver`)

| Property | Type | Default | Range | EditCondition | What it does |
|---|---|---|---|---|---|
| `PressureSolver` | `ESloshPressureSolverType` | `Auto` | — | — | Pressure solver choice. |
| `AutoSolverCellThreshold` | `int32` | `400000` | ≥ 0 | `PressureSolver == Auto` | In Auto mode, MGPCG is used when the cell count exceeds this value. The crossover was measured; see Research/PERFORMANCE_PLAN.md. |
| `PCGIterations` | `int32` | `3` | 1..100 | `PressureSolver != RBGS` | MGPCG outer iterations, each one multigrid V-cycle. 3 matched 4 on every accuracy test. |
| `PressureIterations` | `int32` | `60` | 1..400 | `PressureSolver != MGPCG` | RBGS (SOR) iterations. |
| `SORWeight` | `float` | `1.7` | 1.0..1.95 | — | Over-relaxation factor for RBGS. |
| `ViscosityIterations` | `int32` | `12` | 0..100 | — | Jacobi iterations of the implicit viscosity solve. At high viscosity (large ν·dt/h²) 12 iterations under-damp, so thick liquids benefit from more. |
| `MaxSubsteps` | `int32` | `4` | 1..16 | — | Upper bound on CFL substeps per frame. |
| `MaxFrameDt` | `float` | `1/30` | 0.004..0.1 s | — | Most simulated time per frame. Below 1/MaxFrameDt FPS the liquid runs in slow motion instead of spiralling into more substeps. |
| `CFL` | `float` | `2.0` | 0.25..10 | — | Cells a particle may travel per substep (substeps = speed·dt/(CFL·h), using the lagged max speed). Higher values are cheaper but lower coupling quality; CFL 4 roughly halved 128³ frame time. |
| `SortInterval` | `int32` | `4` | 0..60 (0 = never) | — | Spatially sort particles every N frames. The sort also compacts removed particles. |
| `bSmoothCouplingForce` | `bool` | `true` | — | — | Rigid-body pressure force from SDF-weighted faces (smooth while bodies move) instead of binary faces. |
| `bGhostFluid` | `bool` | `true` | — | — | Ghost-fluid free surface: p = 0 at the reconstructed interface. |
| `DensityDeadband` | `float` | `0.05` | 0..0.5 | — | Velocity density correction only: corrects only above (1 + deadband) × rest density. |
| `bTwoSidedDensityProjection` | `bool` | `true` | — | — | Position-based two-sided density projection, which holds volume without injecting momentum. When on, it replaces the velocity correction. |
| `bSparseBricks` | `bool` | `true` | — | — | Iterate grid passes over the active-brick list (indirect dispatch). |
| `SparseMaxActiveFraction` | `float` | `0.5` | 0..1.01 | `bSparseBricks` | The list is used only while less than this fraction of the pool is active. |
| `SparseStorageBudget` | `float` | `1.0` | 0.01..1.0 | — | Brick-pool capacity as a fraction of all 8³ bricks. Bricks past the budget get no storage, and their particles fall back to ballistic motion. Watch `GetPoolOverflowBricks()`. **Changing it at runtime resets the liquid.** |
| `DensityRelaxation` | `float` | `0.5` | 0..1 | `bTwoSidedDensityProjection` | Fraction of the density error removed per projection. |
| `DensityProjectionInterval` | `int32` | `1` | 1..8 | `bTwoSidedDensityProjection` | Project every N frames, with relaxation compounded to 1 − (1 − r)^N. 2 is validated for large scenes. |
| `MaxParticleSpeed` | `float` | `2000` | ≥ 0.1, cm/s | — | Particle speed clamp. Persistent hits show up in `GetClampedParticleCount()`. |

### 3.5 Properties: Runtime, Surface, Render

| Property | Category | Type | Default | Range | EditCondition | What it does |
|---|---|---|---|---|---|---|
| `bSimulate` | `Slosh\|Runtime` | `bool` | `true` | — | — | `false` pauses the solver. Emitters also stop emitting while paused, but rendering continues. |
| `bRenderLiquid` | `Slosh\|Runtime` | `bool` | `true` | — | — | Turns the liquid's rendering on or off (in Mesh mode it also creates or destroys the mesh component). |
| `SurfaceMode` | `Slosh\|Surface` | `ESloshSurfaceMode` | `Splats` | — | — | Surface technique. It can be switched at runtime. |
| `FieldResolution` | `Slosh\|Surface` | `float` | `1.0` | 0.5..3.0 voxels per cell | `SurfaceMode != Splats` | Density-field voxels per solver cell. 2 doubles the detail at 8× the memory and splat cost. |
| `SurfaceSmoothing` | `Slosh\|Surface` | `float` | `2.0` | 0.5..6.0 splat radii | `SurfaceMode != Splats` | Kernel radius of each particle in the field. Larger values are smoother and rounder, with less detail. |
| `SurfaceIsoLevel` | `Slosh\|Surface` | `float` | `0.5` | 0.05..0.95 (renderer clamps 0.02..0.98) | `SurfaceMode != Splats` | Where the surface sits. 0.5 puts it at the particle radius; lower values are fatter and more connected, higher values thinner. **Very sensitive:** 0.4 already visibly balloons the surface. |
| `SurfaceMaterial` | `Slosh\|Surface` | `TObjectPtr<UMaterialInterface>` | null | — | `SurfaceMode == Mesh` | Mesh-mode material. When empty, the preset's paint material is used if `Material` is a `USloshPaintPreset`, otherwise the engine default surface material. |
| `MaxSurfaceVertices` | `Slosh\|Surface` | `int32` | `1000000` | ≥ 1024 (clamped ≤ 8,388,608) | `SurfaceMode == Mesh` | GPU vertex budget (index budget is 6×). |
| `SunLight` | `Slosh\|Render` | `TObjectPtr<ADirectionalLight>` | null | — | — | Light used for the liquid's specular. When unset, the first `ADirectionalLight` found in the world is used. Light colour is `colour × min(Intensity, 10) × 0.5`. |
| `SkyZenithColor` | `Slosh\|Render` | `FLinearColor` | `(0.25, 0.45, 0.9)` | — | — | Sky colour used in Splats/Raymarched reflections. |
| `SkyHorizonColor` | `Slosh\|Render` | `FLinearColor` | `(0.75, 0.85, 1.0)` | — | — | Horizon colour for reflections. |
| `Bounds` | `Slosh` (VisibleAnywhere, private) | `UBoxComponent` | — | — | — | Root component sized to the domain. |

Which `Visual` settings each surface mode uses: Splats uses all of them. Raymarched and Mesh build the field from `ParticleRadiusScale` and the `Anisotropy*` settings. The `Smoothing*` settings apply only to Splats. Absorption, scatter, Fresnel, specular, refraction, `EdgeFadeThickness` and `ScatterLighting` apply to Splats and Raymarched. Mesh mode shades with its UE material instead.

### 3.6 Blueprint functions

| Function | Specifiers | Signature | Behaviour |
|---|---|---|---|
| `FillBox` | BlueprintCallable | `void FillBox(FVector WorldCenter, FVector Extent)` | Queues a fill at rest density. `WorldCenter` is in world space. `Extent` gives the half sizes in cm **along the domain's local axes**, and the box is snapped to whole cells excluding the wall layer. Applied on the next sim frame. |
| `EmitSphere` | BlueprintCallable | `void EmitSphere(FVector WorldCenter, float Radius, FVector WorldVelocity, int32 Count)` | Emits `Count` particles in a sphere of `Radius` cm with velocity `WorldVelocity` (cm/s, world). It does nothing if `Count ≤ 0` or `Radius ≤ 0`. |
| `ResetSimulation` | BlueprintCallable, **CallInEditor** | `void ResetSimulation()` | Removes all liquid and reallocates GPU state on the next frame. It clears pending emits and results. It does **not** re-apply `InitialFillBox` and doesn't reset emitters' `EmittedParticles`. It also appears as a details-panel button while playing. |
| `SetPaused` | BlueprintCallable | `void SetPaused(bool bPaused)` | Sets `bSimulate = !bPaused`. |
| `ContainsPoint` | BlueprintPure | `bool ContainsPoint(FVector WorldLocation) const` | Oriented-box containment test (actor scale ignored). |
| `GetDomainSize` | BlueprintPure | `FVector GetDomainSize() const` | `GridResolution × CellSize`, cm. |
| `GetParticleCount` | BlueprintPure | `int32 GetParticleCount() const` | Live GPU count from the latest readback, with drains included *(delayed)*. Before the first readback it returns everything emitted, capped at capacity. |
| `GetLiquidVolumeLiters` | BlueprintPure | `float` | Fluid-cell count × cell volume *(delayed)*. Over-estimates during splashes, because any cell holding a particle counts as fluid. |
| `GetMaxParticleSpeed` | BlueprintPure | `float` | Max particle speed, cm/s *(delayed)*. |
| `GetLastSubstepCount` | BlueprintPure | `int32` | Substeps used *(delayed)*. |
| `GetFeedbackLatencyFrames` | BlueprintPure | `int32` | Frames between submission and the latest readback; −1 before the first readback. |
| `GetEffectiveSurfaceMaterial` | BlueprintPure (`Slosh\|Surface`) | `UMaterialInterface*` | `SurfaceMaterial`, otherwise the preset's paint `Material`, otherwise null. |
| `GetActiveBrickFraction` | BlueprintPure (`Slosh\|Diagnostics`) | `float` | Fraction of all 8³ bricks holding storage *(delayed)*. −1 if not measured. |
| `GetPoolUsage` | BlueprintPure (Diagnostics) | `float` | Allocated bricks / pool capacity *(delayed)*. |
| `GetPoolOverflowBricks` | BlueprintPure (Diagnostics) | `int32` | Bricks that needed storage but found the pool full last frame (summed over substeps). **Non-zero means raise `SparseStorageBudget`.** |
| `GetClampedParticleCount` | BlueprintPure (Diagnostics) | `int32` | Particle updates that hit `MaxParticleSpeed` last frame. Persistent non-zero means the step is unresolved. |
| `GetParticleMemoryMB` | BlueprintPure (Diagnostics) | `float` | MiB of allocated particle slots (not `ParticleCapacity`). |
| `GetGridMemoryMB` | BlueprintPure (Diagnostics) | `float` | MiB of grid, brick pool, solver and sort buffers. |
| `GetDrainedParticleCount` | BlueprintPure (Diagnostics) | `int64` | Particles removed by drains since play started or the last reset *(delayed)*. |
| `GetMeanDivergence` / `GetMaxDivergence` | BlueprintPure (Diagnostics) | `float` | Post-projection velocity divergence, 1/s (solver convergence). |
| `GetMeanInteriorDensity` | BlueprintPure (Diagnostics) | `float` | Mean relative density of interior liquid (1 = rest). Values above 1 mean compression. |
| `GetParticleVolumeLiters` | BlueprintPure (Diagnostics) | `float` | `count / 8 / meanInteriorDensity × cell volume`. A steadier volume estimate than `GetLiquidVolumeLiters`. |

### 3.7 C++-only entry points

| Member | Use |
|---|---|
| `const FSloshResults& GetLatestResults() const` | Full readback (`SloshTypes.h:208`): per-collider feedback, `FluidCells`, `InteriorCells`, divergences, brick stats, `ClampedInSolid`, `ClampedCentroid`, memory, `AllocatedParticleCapacity`, `LiveParticles`, `DrainedParticles`, `Substeps`, `SimFrame`. The demo's `-SloshTraceSpeed` reads it. |
| `FTransform GetSimToWorld() const` | Sim metres to world cm, including the ×100 scale. |
| `const FSloshPhysicalProperties& GetPhysical() const` / `GetVisual()` | The resolved structs (asset or inline). |

---

## 4. USloshMaterialAsset, FSloshPhysicalProperties, FSloshVisualProperties

`USloshMaterialAsset` (`UCLASS(BlueprintType)`, a `UDataAsset`) is one liquid type with two separate structs: `Physical` is read only by the solver and `Visual` only by the renderer. Create one under *Miscellaneous → Data Asset → SloshMaterialAsset*, or use any Fluid Painter preset (`USloshPaintPreset` derives from it). Tuning guidance and the cookbook are in [Materials](./Materials.md).

| Property | Type | Category |
|---|---|---|
| `Physical` | `FSloshPhysicalProperties` | `Slosh` |
| `Visual` | `FSloshVisualProperties` | `Slosh` |

### 4.1 FSloshPhysicalProperties (`USTRUCT(BlueprintType)`)

| Property | Type | Default | Range / units | EditCondition | What it does |
|---|---|---|---|---|---|
| `Density` | `float` | `1000` | ≥ 1, kg/m³ | — | Rest density. It drives buoyancy and pressure forces on colliders. |
| `KinematicViscosity` | `float` | `0` | ≥ 0, m²/s | — | Implicit viscosity. Water is 1e-6, which is invisible at grid scale. It becomes visible from about 1e-2 at 2.5 to 5 cm cells. |
| `Transfer` | `ESloshTransfer` | `APIC` | — | — | Transfer scheme. |
| `FlipRatio` | `float` | `0.95` | 0..1 | `Transfer == FLIP` | 0 = pure PIC, 1 = pure FLIP. |
| `BoundaryFriction` | `float` | `0` | 0..1 | — | Tangential velocity removed at solid contact and at domain walls (0 = free slip, 1 = sticky). |
| `Adhesion` | `float` | `0` | 0..2; 1 = a 1.5 g pull | — | Pull toward nearby **collider** surfaces, applied in the G2P collider response. At 1 a film holds against gravity on overhangs. |
| `AdhesionDistance` | `float` | `2.5` | 0.1..20 cm | — | Reach of adhesion from the surface. About one solver cell works well. |
| `DensityCorrection` | `float` | `0.15` | 0..1 | — | Strength of the particle-density correction that counters volume drift. |

### 4.2 FSloshVisualProperties (`USTRUCT(BlueprintType)`, all in `Slosh|Render`)

| Property | Type | Default | Range / units | EditCondition | What it does |
|---|---|---|---|---|---|
| `AbsorptionPerMeter` | `FVector` | `(4.0, 1.2, 0.8)` | 1/m per RGB channel | — | Beer–Lambert absorption. |
| `ScatterColor` | `FLinearColor` | `(0.02, 0.08, 0.10)` | — | — | Colour the liquid tends toward when thick (in-scattering). |
| `RefractionStrength` | `float` | `0.02` | ≥ 0 | — | Screen-space refraction offset. |
| `FresnelF0` | `float` | `0.02` | 0..1 | — | Normal-incidence reflectance (water 0.02). |
| `SpecularPower` | `float` | `400` | ≥ 1 | — | Sun highlight exponent. |
| `ParticleRadiusScale` | `float` | `0.6` | ≥ 0.1, × CellSize | — | Splat radius. It also sets the field kernel radius in Raymarched/Mesh. |
| `SmoothingRadius` | `int32` | `7` | 0..32 px | — | Upper bound on the bilateral depth-smoothing radius. |
| `SmoothingScale` | `float` | `0` | 0..8 splat radii | — | 0 = the classic fixed `SmoothingRadius`-pixel filter, which keeps water ripples. Above 0, the radius scales with depth (`SmoothingScale × splat radius`, capped at `SmoothingRadius`) and slope-aware depth smoothing and a thickness blur are enabled. |
| `SmoothingIterations` | `int32` | `1` | 1..4 | — | Horizontal + vertical smoothing passes. |
| `Anisotropy` | `float` | `0` | 0..1 | — | Yu & Turk ellipsoid splats: films become discs and streams become rods. 0 = round. |
| `AnisotropyMaxStretch` | `float` | `4.0` | 1..8 | `Anisotropy > 0` | Largest ratio between an ellipsoid's longest and shortest axis. |
| `AnisotropySmoothing` | `float` | `0.5` | 0..1 | `Anisotropy > 0` | Pull of each splat toward its neighbourhood centre. |
| `EdgeFadeThickness` | `float` | `0` | 0..10 cm | — | Liquid thinner than this along the view ray fades out. 0 = hard edges. |
| `ScatterLighting` | `float` | `0` | 0..1 | — | Wrapped-diffuse lighting of the scatter colour. 0 = flat (the classic look). |

---

## 5. USloshColliderComponent

`UCLASS(ClassGroup=(Slosh), meta=(BlueprintSpawnableComponent))`, a `USceneComponent`. Header `SloshColliderComponent.h`.

An analytic shape (sphere, box or capsule) that the liquid treats as solid. If its **attach parent** primitive simulates physics (or, failing that, the owner's root primitive), the delayed GPU feedback becomes forces and torques on that body.

**Use in a level.** Add the component to an actor and attach it to the visible mesh. Size it to the mesh **in the mesh's local units**: an engine basic shape is 100 cm, so use `Radius = 50` or `BoxExtent = 50`. The component then inherits the mesh's world scale. Only uniform scale is supported, because shapes use the largest scale axis. It registers with `USloshSubsystem` in `OnRegister` and is picked up by any volume whose bounds, expanded by 2 cells plus the shape's reach, contain the component's location.

### 5.1 Properties

| Property | Category | Type | Default | Range / units | What it does |
|---|---|---|---|---|---|
| `Shape` | `Slosh\|Shape` | `ESloshColliderShapeType` | `Sphere` | — | Shape type. |
| `Radius` | `Slosh\|Shape` | `float` | `50` | ≥ 0.1 cm | Sphere/capsule radius. |
| `BoxExtent` | `Slosh\|Shape` | `FVector` | `(50, 50, 50)` | cm | Box half extents. |
| `CapsuleHalfHeight` | `Slosh\|Shape` | `float` | `50` | ≥ 0 cm | Half height of the **cylindrical part** along local Z (the hemispheres are extra). |
| `CouplingMode` | `Slosh\|Coupling` | `ESloshCouplingMode` | `PressureAndDrag` | — | How the liquid acts back on the body. |
| `BuoyancyScale` | `Slosh\|Coupling` | `float` | `1.0` | ≥ 0 | Multiplier on Archimedes buoyancy. **Used only in `BuoyancyAndDrag`.** |
| `DragCoefficient` | `Slosh\|Coupling` | `float` | `0.8` | ≥ 0 | Quadratic drag Cd on a frontal area of ¼ of the wetted surface. |
| `LinearDrag` | `Slosh\|Coupling` | `float` | `3.0` | ≥ 0, 1/s | Linear damping relative to the local liquid, per unit submerged fraction. It counters bobbing caused by the delayed feedback. |
| `AngularDrag` | `Slosh\|Coupling` | `float` | `1.5` | ≥ 0, 1/s | Angular damping per unit submerged fraction. |
| `AddedMassCoefficient` | `Slosh\|Coupling` | `float` | `0.5` | ≥ 0 | Implicit added mass (0.5 = sphere). Keeps light bodies stable under lagged forcing. |
| `MaxFluidAcceleration` | `Slosh\|Coupling` | `float` | `2500` | ≥ 0, cm/s² | Clamp on fluid-induced acceleration (the force is clamped to `mass × this`). |
| `bReportFeedbackWhenKinematic` | `Slosh\|Coupling` | `bool` | `false` | — | Accumulate and report pressure, submersion and wetness even when the body is kinematic. Use it for sensors, probes and wetness. Without it, kinematic bodies report no feedback. |
| `EnterThreshold` | `Slosh\|Events` | `float` | `0.05` | 0..1 | Submerged fraction above which the body counts as in the liquid. It exits below `0.5 × EnterThreshold` (hysteresis). |

### 5.2 Coupling modes in detail (`SloshColliderComponent.cpp:129-202`)

| Mode | Force on the body | Torque | Recommended for |
|---|---|---|---|
| `OneWay` | None. The body moves the liquid, but the liquid never pushes back. | None | Animated characters, kinematic paddles, sensors (with `bReportFeedbackWhenKinematic`). |
| `BuoyancyAndDrag` | `−ρ · V_submerged · g · BuoyancyScale`, applied at the submerged centroid, plus drag | Righting moment from the off-centre buoyancy | Bodies that may rest on the bottom. They never stick, but they bob because the sensor is delayed. |
| `PressureAndDrag` | Integrated solver pressure on the surface, plus drag | Integrated pressure torque | Floating bodies (measured to settle stably). Faces touching the floor, walls or other bodies receive the surrounding liquid's pressure ("seepage"), so resting bodies still feel buoyancy. |

The two-way modes also add the following terms, in Unreal units:
- Quadratic drag: `0.5 · ρ · Cd · (0.25 · wettedArea) · |v_rel| · v_rel`.
- Linear damping: `LinearDrag · submerged · mass · (v_fluid − v_body)`.
- Angular damping: `−AngularDrag · submerged · I_max · ω`.
- Added mass: `F = (m·F_dyn − m_a·m·g)/(m + m_a)`, where `m_a = AddedMassCoefficient · ρ · V_sub`.
- Clamp: `|F| ≤ m · MaxFluidAcceleration`.
- Smoothing: a 50% temporal lerp of force and torque.

Forces are applied in `TickComponent` (pre-physics) only while `IsInLiquid()` and the body simulates physics.

Collider velocity sent to the solver comes from the physics body's linear and angular velocity when it simulates, corrected for the component's offset from the centre of mass. Otherwise it is the finite difference of the component transform, so kinematic and animated colliders push liquid correctly.

### 5.3 Events

| Delegate | Signature | When |
|---|---|---|
| `OnEnterLiquid` | `FSloshContactSignature(USloshColliderComponent* Collider)` | Submerged fraction rises above `EnterThreshold` *(delayed)* |
| `OnExitLiquid` | same | Submerged fraction drops to `0.5 × EnterThreshold` or below |

Both delegates fire only for colliders the GPU accumulates feedback for (`Slosh.usf` `IsTwoWayCollider`): a simulating body in a two-way mode, or any collider with `bReportFeedbackWhenKinematic`. For every other collider, `GetSubmergedFraction()` stays 0.

### 5.4 Blueprint functions (all BlueprintPure, all *delayed*)

| Function | Returns |
|---|---|
| `GetSubmergedFraction()` | Submerged cell volume / analytic shape volume, clamped to 0..1. |
| `IsInLiquid()` | Enter/exit state with hysteresis. |
| `GetLastFluidForce()` | Last force applied to the body, world space, kg·cm/s². |
| `GetLastPressureForceNewtons()` | Raw integrated pressure force, world space, N (unsmoothed). |
| `GetVoxelVolumeLiters()` | Collider volume as voxelised by the solver (solid cells × cell volume). |
| `GetLocalFluidVelocity()` | Mean liquid velocity on the wetted faces, world space, cm/s. |
| `GetFeedbackAgeFrames()` | Age of the feedback in frames; −1 before the first feedback. |

### 5.5 C++ entry points (called by `ASloshVolume`)

| Member | Purpose |
|---|---|
| `FSloshColliderGPU BuildGPUCollider(const FTransform& WorldToSim, float DeltaTime)` | Packs shape, rotation, velocities and the two-way flag into sim space. |
| `void ReceiveFeedback(const FSloshColliderFeedback&, const FTransform& SimToWorldNoScale, float CellSizeMeters, float FluidDensity, int32 AgeFrames)` | Turns readback into submersion, events and pending forces. |
| `UPrimitiveComponent* GetSimulatedBody() const` | The attach parent primitive (or owner root) **if it simulates physics**, else null. |
| `bool WantsTwoWayCoupling() const` | `CouplingMode != OneWay && GetSimulatedBody()`. |

---

## 6. USloshSkeletalColliderComponent

`UCLASS(ClassGroup=(Slosh), meta=(BlueprintSpawnableComponent))`, a `USceneComponent`. Header `SloshSkeletalColliderComponent.h`.

Makes a skinned mesh solid to the liquid. At BeginPlay it reads LOD0 bind-pose vertices and assigns each vertex to its **dominant skin bone**. Bones with fewer than `max(12, MinVertexFraction × vertexCount)` vertices hand their vertices to their parent. For each remaining bone (up to `MaxCapsules`, largest first) it fits:
- a **capsule** along the principal axis (PCA), with length from the `LengthTrim`-trimmed extent and radius from the `RadiusPercentile` distance from the axis; or
- a **box**, when the cross-section is strongly flattened (`sqrt(λ3/λ2) < 0.55` and `sqrt(λ2/λ1) > 0.45`, typical for torsos and hips), measured between the 6th and 94th percentiles.

Each shape becomes a child `USloshColliderComponent` named `SloshBone_<Bone>`, attached to that bone's socket, so it follows animation with no per-frame work. Generated colliders get `EnterThreshold = 0.02`.

**Use in a level.** Attach it to a `SkeletalMeshComponent` or `PoseableMeshComponent`, or put it anywhere on an actor that has one; it targets the attach parent first, then the owner's first skinned mesh. Requirements:
- LOD0 render data must be CPU-readable. That is always true in the editor and in `-game` launched from the editor binary. Cooked builds need **Allow CPU Access** on the mesh; otherwise you get the warning "LOD0 vertex data is not CPU-readable".
- Capsules share the 64-collider budget with every other collider in the domain. The default of 40 leaves 24.

### 6.1 Properties

| Property | Category | Type | Default | Range | What it does |
|---|---|---|---|---|---|
| `MaxCapsules` | `Slosh\|Fit` | `int32` | `40` | 1..64 | Upper bound on generated shapes; the bones with the most vertices win. |
| `MinVertexFraction` | `Slosh\|Fit` | `float` | `0.006` | 0..0.2 | Minimum fraction of all vertices a bone needs for its own shape (the floor is 12 vertices). |
| `RadiusPercentile` | `Slosh\|Fit` | `float` | `0.8` | 0.3..1.0 | Percentile of vertex distance from the axis used as the radius. |
| `LengthTrim` | `Slosh\|Fit` | `float` | `0.03` | 0..0.25 | Fraction of vertices trimmed at each end before measuring length. |
| `RadiusScale` | `Slosh\|Fit` | `float` | `1.0` | ≥ 0.1 | Multiplier on fitted radii and box extents. Inflate slightly so thin limbs still block a coarse grid. |
| `MinRadius` | `Slosh\|Fit` | `float` | `2.0` | ≥ 0.1 cm | Floor on radii and box extents. |
| `ExcludeBones` | `Slosh\|Fit` | `TArray<FName>` | empty | — | Bones (and their whole subtree) that never get a shape; their vertices are ignored. |
| `CouplingMode` | `Slosh\|Coupling` | `ESloshCouplingMode` | `OneWay` | — | Copied to every generated collider. Animated characters should stay OneWay. |
| `bReportWetness` | `Slosh\|Coupling` | `bool` | `false` | — | Sets `bReportFeedbackWhenKinematic` on each collider so per-bone `GetSubmergedFraction()` works as wetness. |
| `bDrawCapsules` | `Slosh\|Debug` | `bool` | `false` | — | Draws the fitted shapes every frame (cyan). |

### 6.2 Functions

| Function | Specifiers | Behaviour |
|---|---|---|
| `int32 RebuildColliders()` | BlueprintCallable | Clears the existing shapes and refits from the current mesh. Logs a per-bone summary (`LogSloshSkeletalCollider`) and returns the count. Call it after changing the mesh or any Fit setting at runtime. |
| `void ClearColliders()` | BlueprintCallable | Destroys the generated colliders (also called at EndPlay). |
| `const TArray<USloshColliderComponent*>& GetColliders() const` | BlueprintPure | The generated colliders. Each one's `GetAttachSocketName()` is its bone. |
| `USkinnedMeshComponent* GetTargetMesh() const` | BlueprintPure | The attach parent if it is skinned, otherwise the owner's first `USkinnedMeshComponent`. |

---

## 7. USloshEmitterComponent

`UCLASS(ClassGroup=(Slosh), meta=(BlueprintSpawnableComponent))`, a `USceneComponent` that doesn't tick. Header `SloshEmitterComponent.h`.

Emits liquid along its **forward (+X) axis** into every volume that contains its location. The volume does the emission work in its own tick (`SloshVolume.cpp:316-381`).

**Use in a level.** Add it to any actor and point +X where the liquid should go. It must sit inside a volume's domain: outside every domain it emits nothing and doesn't count toward `MaxParticles`. Aim pours mostly downward, because a horizontal jet loses its sideways speed quickly at coarse resolution.

### 7.1 Properties

| Property | Category | Type | Default | Range / units | EditCondition | What it does |
|---|---|---|---|---|---|---|
| `bEmitting` | `Slosh` | `bool` | `true` | — | — | Continuous emission on/off. Bursts ignore it. |
| `Shape` | `Slosh\|Shape` | `ESloshEmitterShape` | `Sphere` | — | — | Emission region. |
| `Radius` | `Slosh\|Shape` | `float` | `10` | ≥ 1 cm | `Shape == Sphere` | Sphere radius (× max component scale). |
| `BoxExtent` | `Slosh\|Shape` | `FVector` | `(10, 10, 10)` | cm | `Shape == Box` | Box half extents, oriented with the component (× max component scale). |
| `RateMode` | `Slosh\|Rate` | `ESloshEmitRateMode` | `ParticlesPerSecond` | — | — | Rate unit. |
| `ParticlesPerSecond` | `Slosh\|Rate` | `float` | `20000` | ≥ 0 | `RateMode == ParticlesPerSecond` | Particles per second (8 fill a cell). |
| `LitersPerSecond` | `Slosh\|Rate` | `float` | `1.0` | ≥ 0 L/s | `RateMode == LitersPerSecond` | Volume rate, converted with the volume's cell size. |
| `MaxParticles` | `Slosh\|Rate` | `int32` | `0` | ≥ 0 (0 = unlimited) | — | Lifetime cap, bursts included. |
| `Speed` | `Slosh\|Velocity` | `float` | `300` | cm/s | — | Initial speed along +X. |
| `VelocitySpread` | `Slosh\|Velocity` | `float` | `0` | 0..180° | — | Cone half angle of the randomised direction. |
| `SpeedVariation` | `Slosh\|Velocity` | `float` | `0` | 0..1 | — | ± random speed fraction (0.2 gives 80% to 120%). |
| `bInheritVelocity` | `Slosh\|Velocity` | `bool` | `false` | — | — | Adds the emitter's own finite-difference velocity. |

### 7.2 Functions

| Function | Specifiers | Behaviour |
|---|---|---|
| `StartEmitting()` / `StopEmitting()` | BlueprintCallable | Sets `bEmitting`. |
| `Burst(int32 Count)` | BlueprintCallable | Adds `Count` particles to emit on the next frame (independent of `bEmitting`; respects `MaxParticles`). |
| `BurstLiters(float Liters)` | BlueprintCallable | Same as `Burst`, but as a volume, converted when emitted. |
| `GetEmittedParticles()` | BlueprintPure | `int64` total particles this emitter has emitted since play started or since `ResetEmittedCount`. It counts game-side requests, so particles dropped at capacity are still counted. |
| `ResetEmittedCount()` | BlueprintCallable | Restarts the `MaxParticles` budget. |

Public fields that `ASloshVolume` reads and writes: `Carry` (fractional particles carried between frames), `PendingBurst`, `PendingBurstLiters`, `EmittedParticles`, `PreviousLocation`, `bHasPreviousLocation`, `MeasuredVelocity` and `VelocityFrame` (motion measured once per frame), `EmitFrame` (an emitter feeds one volume per frame even where volumes overlap).

---

## 8. Manipulators

Header `SloshManipulatorComponent.h`, modelled on ZibraLiquids' manipulators. Manipulators register with the subsystem in `OnRegister`, including in editor worlds, so `bDrawShape` works in the viewport. A volume includes up to 32 of them, those whose location lies within its bounds expanded by the manipulator's reach. They act inside G2P on the GPU.

### 8.1 USloshManipulatorComponent (abstract base)

`UCLASS(Abstract, ClassGroup=(Slosh))`.

| Property | Category | Type | Default | Range / units | What it does |
|---|---|---|---|---|---|
| `bEnabled` | `Slosh` | `bool` | `true` | — | When false, the manipulator isn't sent to the GPU and doesn't count toward the 32. |
| `Shape` | `Slosh\|Shape` | `ESloshManipulatorShapeType` | `Sphere` | — | Region of effect. |
| `Radius` | `Slosh\|Shape` | `float` | `25` | ≥ 0.1 cm | Sphere/capsule radius. |
| `BoxExtent` | `Slosh\|Shape` | `FVector` | `(25, 25, 25)` | cm | Box half extents (oriented). |
| `CapsuleHalfHeight` | `Slosh\|Shape` | `float` | `25` | ≥ 0 cm | Straight section along local Z. |
| `bDrawShape` | `Slosh\|Debug` | `bool` | `false` | — | Draws the shape in the editor and in play: red for a drain, cyan for a force field, grey when disabled. `Everywhere` isn't drawn. |

C++ entry points: `virtual bool BuildGPU(const FTransform& WorldToSim, FSloshManipulatorGPU& Out) const` returns false when the manipulator doesn't apply. `float GetReach() const` is the rough reach in cm (shape plus falloff, × scale; `1e9` for Everywhere). Subclasses override the protected `GetFalloffCm()` and `GetDebugColor()`.

### 8.2 USloshDrainComponent

`UCLASS(ClassGroup=(Slosh), meta=(BlueprintSpawnableComponent))`. Removes liquid that enters its shape: a sink, an overflow, a floor drain.

| Property | Type | Default | Range | What it does |
|---|---|---|---|---|
| `RemovalRate` | `float` | `0` | ≥ 0, 1/s | Per-particle removal rate. Each particle inside is removed with probability `1 − exp(−RemovalRate · dt)` per step, so this is a rate per particle, not "particles per second" as the header comment suggests. **0 removes everything inside at once.** |

Removed particles are marked dead (`Age < 0`) and skipped by every pass. Their slots are reclaimed only at the next compaction, which happens during the spatial sort and, while any drain is active, at least every 8 frames. `GetDrainedParticleCount()` accumulates removals.

### 8.3 USloshForceFieldComponent

`UCLASS(ClassGroup=(Slosh), meta=(BlueprintSpawnableComponent))`.

| Property | Type | Default | Range / units | EditCondition | What it does |
|---|---|---|---|---|---|
| `ForceType` | `ESloshForceType` | `Directional` | — | — | See the table below. |
| `Strength` | `float` | `500` | cm/s² (Drag: 1/s) | — | Acceleration; gravity is 980. Negative Radial attracts. If it is about 0 the field is skipped. |
| `SwirlInwardPull` | `float` | `0.3` | 0..4 | `ForceType == Swirl` | Pull toward the axis as a fraction of the swirl strength (keeps a whirlpool together). |
| `Falloff` | `float` | `10` | ≥ 0 cm | — | Distance beyond the shape over which the force fades to zero. |

| `ForceType` | Direction | Typical use |
|---|---|---|
| `Directional` | Component forward (+X) | Wind, a push, a conveyor. With `Everywhere` it adds extra gravity. |
| `Radial` | Away from the centre (positive) or toward it (negative) | Explosions, magnets, gathering. |
| `Swirl` | Around component up (+Z), plus inward pull | Whirlpools, stirring. |
| `Drag` | Opposes velocity at `Strength` 1/s (negative values are treated as 0) | Calm zones, thickening areas. |

---

## 9. ASloshDrainActor, ASloshForceFieldActor

Header `SloshManipulatorActors.h`. Placeable wrappers, found in *Place Actors* as **Slosh Drain** and **Slosh Force Field**.

| Actor | Component (root, `VisibleAnywhere`, `BlueprintReadOnly`) | Note |
|---|---|---|
| `ASloshDrainActor` | `Drain` : `USloshDrainComponent` | `bDrawShape = true` by default. |
| `ASloshForceFieldActor` | `ForceField` : `USloshForceFieldComponent` | `bDrawShape = true` by default. |

All settings live on the component. Select the component in the details panel to edit `Shape`, `Radius`, `RemovalRate`, `ForceType`, and so on.

---

## 10. USloshSurfaceMeshComponent

`UCLASS(ClassGroup=(Slosh), NotBlueprintable)`, a `UPrimitiveComponent`. Header `SloshSurfaceMeshComponent.h`.

Draws a volume's liquid as a real triangle mesh. Each frame the renderer rebuilds the mesh on the GPU (surface nets over the density field) into pooled buffers, and this component draws it with one indirect indexed draw using an ordinary UE material, so it gets engine lighting, shadows and any material. **You never add it yourself.** `ASloshVolume` creates it (transient, named `SloshSurfaceMesh`, attached to `Bounds`) when `SurfaceMode == Mesh` and `bRenderLiquid`, and destroys it otherwise. Vertices are in the volume's local space.

| Setting | Value |
|---|---|
| Collision | None, no overlap events |
| `CastShadow` | `true` |
| Occluder / occlusion | Not an occluder; `CanBeOccluded()` returns false |
| Velocity relevance | **false** (no motion vectors: TSR/TAA can smear fast liquid) |
| Material | The volume's `GetEffectiveSurfaceMaterial()`, or the engine default surface material when that is null |

C++: `Configure(Buffers, HalfExtent, Material)` is called by the volume every tick and only dirties render state when something changed. `GetBuffers()`, `GetMaterial(0)` and `SetMaterial(0, M)` are the standard primitive overrides (one material slot).

---

## 11. USloshSubsystem, USloshRenderEngineSubsystem

Header `SloshSubsystem.h`.

### USloshSubsystem (`UWorldSubsystem`)

A per-world registry of volumes, colliders, emitters and manipulators (weak pointers). Components register themselves in `OnRegister` and unregister in `OnUnregister`; volumes do so in `BeginPlay`/`EndPlay`.

| Member | Kind | Behaviour |
|---|---|---|
| `static USloshSubsystem* Get(const UWorld*)` | C++ | Null-safe accessor. |
| `ASloshVolume* FindVolumeAt(FVector WorldLocation) const` | BlueprintCallable | First registered volume whose domain contains the point, or null. |
| `Register/UnregisterVolume`, `Register/UnregisterCollider`, `Register/UnregisterEmitter`, `Register/UnregisterManipulator` | C++ | Called by the components themselves. |
| `GetColliders()`, `GetEmitters()`, `GetManipulators()` | C++ | Arrays of weak pointers, iterated by each volume every tick. |

### USloshRenderEngineSubsystem (`UEngineSubsystem`)

Calls `SloshRender::Startup()` in `Initialize` and `SloshRender::Shutdown()` in `Deinitialize`. Those calls register and remove the renderer's post-opaque delegate for the engine's lifetime. It has no user-facing API.

---

## 12. Slosh Paint types

Slosh Paint ports Blender's Fluid Painter: strokes painted on surfaces become meshed liquid geometry. This section is the API reference. Behaviour, meshing and the editor mode (*Slosh Paint*, `SloshEditor` module) are covered in [Slosh Paint](./SloshPaint.md).

### 12.1 USloshPaintPreset (`UCLASS(BlueprintType)`, derives from `USloshMaterialAsset`)

A paintable liquid. Lengths are in **preset units = `Scale` × 1 mm**. Because it derives from `USloshMaterialAsset`, it also has `Physical` and `Visual` and can be assigned to `ASloshVolume::Material` (see [Materials](./Materials.md)).

| Property | Category | Type | Default | Range | What it does |
|---|---|---|---|---|---|
| `Category` | `Preset` | `FName` | None | — | Grouping label (BlueprintReadOnly). |
| `bAdultContent` | `Preset` | `bool` | `false` | — | Marks presets kept in a separate content folder so they can be excluded. |
| `Material` | `Fluid` | `TObjectPtr<UMaterialInterface>` | null | — | Paint material. In Mesh surface mode, the volume also uses it for the liquid surface. |
| `MaterialUVScale` | `Fluid` | `float` | `1.0` | ≥ 0.001 | Scales procedural material features relative to the liquid. |
| `Scale` | `Fluid` | `float` | `1.0` | 0.1..100000 | Preset unit = Scale × 1 mm. |
| `Thickness` | `Fluid` | `float` | `1.0` | 0.1..5 | Bead radius and resampling spacing (× pressure). |
| `Quality` | `Fluid` | `int32` | `0` | 0..2 | Meshing resolution (0 = Fluid Painter default, 2 = finest). |
| `Roughness` | `Fluid` | `float` | `0` | 0..1 | Random bead-radius variation. |
| `Flatness` | `Fluid` | `float` | `0` | 0..1 | Squash against the surface (1 = flat film). |
| `Offset` | `Fluid` | `float` | `0` | −100..100 | Move along the surface normal. |
| `Fading` | `Fluid` | `float` | `1.0` | 0.001..10 | Height range of the material's fading ramp. |
| `SmoothIterations` | `Fluid` | `int32` | `10` | 0..64 | Laplacian smoothing passes. |
| `bCastShadows` | `Fluid` | `bool` | `false` | — | Fluid Painter liquids cast no shadows by default. |
| `DropMode` | `Drops` | `ESloshPaintDropMode` | `None` | — | Drop behaviour. |
| `DropLength` | `Drops` | `float` | `0.5` | 0.1..1 | Fraction of the stroke the drop covers. |
| `DropSize` | `Drops` | `float` | `2.0` | 1..5 | Drop radius relative to the bead radius. |
| `DropExponent` | `Drops` | `float` | `1.0` | 1..10 | Profile exponent (1 = cone). |
| `DropMove` | `Drops` | `float` | `0` | — | Phase offset added to `DropTime`. |
| `bDropMoveRandom` / `DropMoveSeed` | `Drops` | `bool` / `int32` | `false` / `0` | — | Randomised phase. |
| `DropSpeed` | `Drops` | `float` | `1.0` | −100..100 | Animated drop speed (stroke lengths per unit of `DropTime`). |
| `bDropSpeedRandom` / `DropSpeedSeed` | `Drops` | `bool` / `int32` | `false` / `0` | — | Randomised speed. |
| `LayerA`, `LayerB` | `Layers` | `FSloshPaintLayer` | disabled | — | Scattered instances (bubbles, sugar, sprinkles). |

C++: `float GetUnitCm() const` returns `max(Scale, 0.1) × 0.1`. `static FOnPresetChanged OnPresetChanged` is broadcast from `PostEditChangeProperty`, and paint components using the preset rebuild.

### 12.2 FSloshPaintLayer (`USTRUCT(BlueprintType)`)

| Property | Type | Default | Range | What it does |
|---|---|---|---|---|
| `bEnabled` | `bool` | `false` | — | Layer on/off. All other fields have `EditCondition = bEnabled`. |
| `bRods` | `bool` | `false` | — | Thin rods instead of beads. |
| `Material` | `UMaterialInterface*` | null | — | Instance material. |
| `bInside` | `bool` | `false` | — | Sink the instances below the liquid surface. |
| `Size` | `float` | `1.0` | 0.1..50 | Instance radius (preset units). |
| `SizeRandom` | `float` | `0` | 0..1 | Size variation. |
| `Amount` | `float` | `1.0` | 0.1..10 | Density: `Amount / Scale² × 10⁵` per m². |
| `Shift` | `float` | `0` | −10..10 | Extra offset along the normal. |
| `Seed` | `int32` | `0` | — | Scatter seed. |

### 12.3 FSloshPaintPoint and FSloshPaintStroke (`SloshPaintTypes.h`)

| Struct | Field | Type | Default | Notes |
|---|---|---|---|---|
| `FSloshPaintPoint` | `Position` | `FVector` | zero | In `Bone` space on skeletal meshes, otherwise paint-component space. |
| | `Normal` | `FVector` | `UpVector` | Same space as `Position`. |
| | `Pressure` | `float` | `1.0` | 0.1..5, bead-radius multiplier. |
| | `Bone` | `FName` | None | Dominant skin bone, or None. |
| `FSloshPaintStroke` | `Preset` | `USloshPaintPreset*` | null | One preset per stroke. |
| | `Points` | `TArray<FSloshPaintPoint>` | — | The saved source of truth. |
| | `Seed` | `int32` | `0` | Set randomly by `BeginStroke`. |
| | `bVisible` | `bool` | `true` | — |
| | `Id` | `int32` | `0` | VisibleAnywhere; unique within the component (fixed up on register). |
| | `Revision` | `int32` | `0` | Transient; bumped on every edit. |

### 12.4 USloshPaintComponent

`UCLASS(ClassGroup=(Slosh), meta=(BlueprintSpawnableComponent))`. It attaches to the painted primitive, keeps the strokes, and regenerates geometry asynchronously into transient child components (per bone on skeletal meshes).

| Property | Category | Type | Default | Range | What it does |
|---|---|---|---|---|---|
| `Strokes` | `Slosh\|Paint` | `TArray<FSloshPaintStroke>` | empty | — | Saved strokes (BlueprintReadOnly). |
| `Reveal` | `Slosh\|Paint\|Animation` | `float` | `1.0` | 0..1 | Fraction of each stroke drawn. Interp (Sequencer-keyable); BlueprintSetter `SetReveal`. |
| `DropTime` | `Slosh\|Paint\|Animation` | `float` | `0` | — | Animated-drop time. Interp; BlueprintSetter `SetDropTime`. |
| `bAnimateDrops` | `Slosh\|Paint\|Animation` | `bool` | `true` | — | In game worlds, advances `DropTime` by `DeltaTime × DropTimeScale` when any stroke uses `Animated` drops. |
| `DropTimeScale` | `Slosh\|Paint\|Animation` | `float` | `0.25` | — | `DropTime` units per second. |
| `UnitScale` | `Slosh\|Paint\|Quality` | `float` | `1.0` | 0.01..100 | Scales every preset unit on this component. |
| `QualityScale` | `Slosh\|Paint\|Quality` | `float` | `1.0` | 0.25..4 | Meshing resolution multiplier. |

| Function | Specifiers | Behaviour |
|---|---|---|
| `static USloshPaintComponent* FindOrAddPaintComponent(USceneComponent* Target)` | BlueprintCallable, `DefaultToSelf=Target` | Returns the paint component attached to `Target`, or creates one (transient at runtime; a saved, transactional instance component in the editor). |
| `int32 BeginStroke(USloshPaintPreset*, FVector WorldLocation, FVector WorldNormal, float Pressure = 1, FName Bone = None)` | BlueprintCallable | Starts a stroke and returns its id. |
| `bool AddStrokePoint(int32 StrokeId, FVector, FVector, float Pressure = 1, FName Bone = None)` | BlueprintCallable | Appends a world-space point. |
| `void EndStroke(int32 StrokeId)` | BlueprintCallable | Finishes the stroke and rebuilds it at full quality. |
| `int32 AddStroke(Preset, const TArray<FVector>& Locations, const TArray<FVector>& Normals, float Pressure = 1, FName Bone = None)` | BlueprintCallable | Adds a whole stroke. Normals can be one per point or a single shared normal. Returns 0 if `Locations` is empty. |
| `bool RemoveStroke(int32)`, `void UndoLastStroke()`, `void ClearStrokes()` | BlueprintCallable | Edits the stroke list. |
| `void SetReveal(float)`, `void SetDropTime(float)` | BlueprintCallable, BlueprintSetter | `Reveal` is clamped to 0..1. Setting `DropTime` rebuilds only if any stroke has animated drops. |
| `void Rebuild()` | BlueprintCallable, CallInEditor | Clears the cache and regenerates everything. |
| `int32 GetNumStrokes()`, `bool IsBuilding()` | BlueprintPure | — |
| `OnPaintBuilt` | BlueprintAssignable, `FSloshPaintBuiltSignature(USloshPaintComponent*)` | Fires when a build is applied. |

C++: `GetStrokeSpaceTransform(FName Bone)`, `GetSkinnedTarget()`, `FindStroke(int32)`, `NotifyStrokesChanged(bool bClearCache)` (call it after editing `Strokes` directly).

### 12.5 USloshPaintBrushComponent

`UCLASS(ClassGroup=(Slosh), meta=(BlueprintSpawnableComponent))`. While painting it traces every tick and lays strokes on whatever the ray hits (skeletal meshes on their rendered skin). It starts a new stroke when the ray moves to another component or leaves the surface.

| Property | Category | Type | Default | Range | What it does |
|---|---|---|---|---|---|
| `Preset` | `Slosh\|Brush` | `USloshPaintPreset*` | null | — | Preset painted. Without one, nothing is painted. |
| `Aim` | `Slosh\|Brush` | `ESloshPaintAim` | `ComponentForward` | — | Ray source. |
| `AimController` | `Slosh\|Brush` | `APlayerController*` | null | — | For MouseCursor/ScreenCenter aiming; defaults to player 0. |
| `Pressure` | `Slosh\|Brush` | `float` | `1.0` | 0.1..5 | Bead-radius multiplier. |
| `Spacing` | `Slosh\|Brush` | `float` | `0` | ≥ 0 cm | Minimum point spacing. 0 = automatic: `max(0.5 × Thickness × unitCm, 0.02)`. |
| `MaxDistance` | `Slosh\|Brush` | `float` | `2000` | ≥ 1 cm | Ray length. |
| `TraceChannel` | `Slosh\|Brush` | `ECollisionChannel` | `ECC_Visibility` | — | Trace channel. |
| `bPaintSkinnedWithoutCollision` | `Slosh\|Brush` | `bool` | `true` | — | Also hit skeletal meshes without collision (CPU-skinned test). |

| Function / event | Behaviour |
|---|---|
| `StartPainting()` / `StopPainting()` | `StopPainting` also ends the current stroke. |
| `bool IsPainting() const` | BlueprintPure. |
| `FSloshPaintHit PaintAlongRay(FVector Start, FVector End)` | One paint step along an explicit ray. In `ComponentForward` mode the owner actor is ignored by the trace. |
| `bool GetAimRay(FVector& OutStart, FVector& OutEnd) const` | The ray this frame. Returns false without a controller in the mouse and screen modes. |
| `OnStrokeStarted`, `OnStrokeFinished` | `FSloshPaintStrokeSignature(USloshPaintComponent*, int32 StrokeId)` |

### 12.6 FSloshPaintHit and USloshPaintLibrary (`SloshPaintLibrary.h`)

`FSloshPaintHit` (all BlueprintReadOnly): `bHit` (`false`), `Location`, `Normal` (`UpVector`), `Component` (the primitive painted), `Bone`, `Distance`.

| Function | Behaviour |
|---|---|
| `static FSloshPaintHit TracePaintSurface(WorldContext, FVector Start, FVector End, ECollisionChannel TraceChannel, const TArray<AActor*>& ActorsToIgnore, bool bIncludeSkinnedWithoutCollision = true)` | A line trace, then a CPU-skinned surface test against every visible skinned mesh whose bounds the ray crosses. It keeps the nearest hit, and skeletal meshes are hit on their skin rather than their physics bodies. |
| `static bool TraceSkinnedSurface(USkinnedMeshComponent*, FVector Start, FVector End, FVector& OutLocation, FVector& OutNormal, FName& OutBone, float& OutDistance)` | Ray against the CPU-skinned render surface. Needs CPU-accessible render data (always available in the editor; at runtime enable **Allow CPU Access**). |
| `static int32 PaintDab(const FSloshPaintHit& Hit, USloshPaintPreset* Preset, float Pressure = 1)` | One-point stroke at a hit (impacts, splats). Returns the stroke id or 0. |

### 12.7 ASloshPainterActor

`UCLASS(Blueprintable)`. The drop-in runtime painter and the base of `BP_SloshPainter` and `BP_SloshNozzle` (`/Slosh/Paint/Blueprints`). Its constructor sets `Brush->Aim = MouseCursor`, unlike the component default.

| Property | Type | Default | What it does |
|---|---|---|---|
| `Brush` | `USloshPaintBrushComponent*` | created | VisibleAnywhere, BlueprintReadOnly. An editor-only orange `AimArrow` shows its forward axis. |
| `Presets` | `TArray<USloshPaintPreset*>` | empty | Presets to cycle through. At BeginPlay the brush takes `Presets[0]` if it has no preset. |
| `bMouseInput` | `bool` | `true` | Binds input on player 0: LMB paints, mouse wheel changes pressure by ±`PressureStep` (clamped 0.1..5), keys 1 to 9 select a preset, Tab selects the next one, Z undoes. In MouseCursor mode it also shows the cursor. |
| `bAutoPaint` | `bool` | `false` | Calls `StartPainting()` at BeginPlay (nozzles). |
| `PressureStep` | `float` | `0.1` | Pressure per wheel notch. |

Functions: `SelectPreset(int32)` ends the current stroke, because a stroke has one preset, and resumes painting if it was painting. `NextPreset()`. `UndoLastStroke()` removes the last stroke *this painter* made, walking back through its history. `GetPresetIndex()`.

---

## 13. Demo-module helpers

Headers `SloshDemo/Source/SloshDemo/SloshSkelTestActors.h` and `SloshShowcaseActors.h`. These are in the **demo game module**, not the plugin (no `SLOSH_API`). Copy them into your project if you need them.

### ASloshSkelTestCharacter

A placeable skeletal-collision test subject: a `UPoseableMeshComponent` made solid to the liquid by a `USloshSkeletalColliderComponent`, with a procedural bone swing and body sway so the colliders move. Ticks in `TG_PrePhysics`.

| Property | Category | Type | Default | Units | What it does |
|---|---|---|---|---|---|
| `SkeletalMesh` | `Slosh Test` | `USkeletalMesh*` | null | — | Mesh to show. `OnConstruction` applies it to `Mesh`. |
| `Mesh` | `Slosh Test` | `UPoseableMeshComponent*` | created (root) | — | No collision. |
| `SloshCollider` | `Slosh Test` | `USloshSkeletalColliderComponent*` | created | — | Capsule-fit settings live here. `bReportWetness = true`. |
| `bAnimate` | `Slosh Test\|Motion` | `bool` | `true` | — | Enables swing and sway. |
| `SwingBone` | `Slosh Test\|Motion` | `FName` | `Upperarm_L` | — | Bone rotated back and forth. |
| `SwingAxis` | `Slosh Test\|Motion` | `FVector` | `(0, 1, 0)` | component space | Swing axis. |
| `SwingDegrees` | `Slosh Test\|Motion` | `float` | `-22` | degrees | Peak angle; the bone goes from bind pose to this and back on a cosine ramp. |
| `SwingPeriod` | `Slosh Test\|Motion` | `float` | `5` | s, ≥ 0.1 | Swing period. |
| `SwayDegrees` | `Slosh Test\|Motion` | `float` | `6` | degrees | Whole-body yaw amplitude around the placed rotation. |
| `SwayPeriod` | `Slosh Test\|Motion` | `float` | `9` | s, ≥ 0.1 | Sway period. |

`FString DescribeWetBones(int32& OutWetCount) const` (BlueprintPure) returns `" Bone:fraction ..."` for every generated collider with a non-zero submerged fraction *(delayed)*. The demo game mode's placed-scene log uses it.

### ASloshPourNozzle

A placeable pouring nozzle: a `USloshEmitterComponent` root that emits along +X, with a visual cylinder (`/Engine/BasicShapes/Cylinder`, 9 cm × 40 cm, no collision) lying behind it.

| Property | Type | Default | What it does |
|---|---|---|---|
| `Body` | `UStaticMeshComponent*` | created | Visual only. |
| `Emitter` | `USloshEmitterComponent*` | `Radius 3`, `ParticlesPerSecond 2500`, `Speed 60` | Rate, radius and speed live here. 2500 particles/s is about 1.3 L/s at 1.6 cm cells and about 4.9 L/s at 2.5 cm cells. |
| `StartDelay` | `float` | `0` s | Wait this long after play starts. |
| `StopAfter` | `float` | `9` s | Stop pouring after this much play time (0 = never). |
| `bContinuous` | `bool` | `true` | `false` = no continuous pouring, bursts only. |
| `BurstInterval` | `float` | `0` s | Call `Emitter->Burst(BurstParticles)` every this many seconds while active (0 = no bursts). |
| `BurstParticles` | `int32` | `4000` | Particles per timed burst. |

`Tick` overwrites `Emitter->bEmitting` every frame (`active && bContinuous`, where active comes from `StartDelay`/`StopAfter`), so calling `StartEmitting`/`StopEmitting` on this actor has no lasting effect. Use the properties, or call `Burst` instead.

### ASloshShowcaseMover

Moves another actor kinematically so liquid has something moving to react to (used by the showcase levels: paddle, moving hose, paint nozzle sweeps). Colliders on the target pick the motion up as velocity (finite difference of the transform).

| Property | Type | Default | What it does |
|---|---|---|---|
| `Target` | `AActor*` | null | Actor to move. Its transform at BeginPlay is the rest pose. |
| `Mode` | `ESloshShowcaseMotion` | `Oscillate` | `Oscillate` (back and forth along `Axis`), `Rotate` (spin at `RotationRate`), `Orbit` (circle around the mover in its XY plane, radius `Amplitude`). |
| `Axis` | `FVector` | `(1, 0, 0)` | Oscillate direction (world). |
| `Amplitude` | `float` | `50` cm | Oscillate half travel / orbit radius. |
| `Period` | `float` | `4` s | Oscillation / orbit period. |
| `RotationRate` | `FRotator` | `(0, 45, 0)` deg/s | Rotate mode. |
| `StartDelay` | `float` | `0` s | Hold still this long first. |

### ASloshShowcaseBody

A basic-shape body the liquid sees: a `UStaticMeshComponent` root (`/Engine/BasicShapes/Cube` or `Sphere`) with a `USloshColliderComponent` sized to it (radius / half extent 50 inheriting the mesh scale).

| Property | Type | Default | What it does |
|---|---|---|---|
| `Shape` | `ESloshShowcaseShape` | `Box` | `Box` or `Sphere`. |
| `Size` | `FVector` | `(40, 40, 40)` cm | Full size (sphere: `X` is the diameter). Applied as mesh scale in `OnConstruction`. |
| `bSimulatePhysics` | `bool` | `false` | Physics body (floats/sinks under `PressureAndDrag` / `BuoyancyAndDrag`); otherwise a static or mover-driven kinematic obstacle. |
| `MassKg` | `float` | `20` kg | Mass override when simulating. |
| `CouplingMode` | `ESloshCouplingMode` | `PressureAndDrag` | Copied to the collider. Use `OneWay` for obstacles and paddles. |
| `SurfaceMaterial` | `UMaterialInterface*` | null | Optional mesh material. |

---

## 14. Recipes

### 14.1 A pool with floating objects

1. Place an `ASloshVolume`. For a 4 × 4 × 2 m pool at 5 cm cells, set `GridResolution = (80, 80, 40)` and `CellSize = 5`. The `ParticleCapacity` default of 262,144 covers only 32,768 full cells.
2. Set `InitialFillBox` to the bottom half, for example Min `(-200, -200, -100)`, Max `(200, 200, 0)`. The wall layer is excluded automatically. That fill is about 78 × 78 × 20 ≈ 122k cells, or about 0.97M particles, so set `ParticleCapacity` to about 1,200,000 (filled cells × 8 plus emitter headroom).
3. Add a physics floor and walls that match the domain. The grid walls bound only the liquid, not rigid bodies; the demo spawns invisible 1 m thick cube walls. The liquid floor sits one cell above the domain's min Z, because cell 0 is a wall.
4. For each floating object, place a Static Mesh Actor (Movable, **Simulate Physics**, a mass override chosen for the density ratio you want), add a `USloshColliderComponent` **attached to the mesh**, and set `Shape` and `Radius`/`BoxExtent` to the mesh's unscaled half size (50 for engine basic shapes). Keep `CouplingMode = PressureAndDrag`. Keep the actor's scale uniform.
5. Expect the body to settle within a few percent of its Archimedes draft. A 40 cm, 35 kg cube (density ratio 0.547) floats about 22 cm deep. If it bobs, raise `LinearDrag` and `AngularDrag`. If it jitters, lower the `CFL` back to 2.
6. Use `OnEnterLiquid`/`OnExitLiquid` for splash sounds. Remember they are 2 to 4 frames late.

### 14.2 A pouring nozzle

Quickest: place the demo's `ASloshPourNozzle` inside a volume and rotate it so +X points mostly down. Set `StartDelay` and `StopAfter`.

From scratch: add a `USloshEmitterComponent` to any actor inside the domain.
- Use `RateMode = LitersPerSecond` so the flow is independent of `CellSize`.
- Keep `Radius` at least one cell (smaller jets are under-resolved).
- `Speed` 50 to 150 cm/s suits a pour. Use `VelocitySpread` 5 to 15° and `SpeedVariation` 0.1 to 0.2 for a broken, splashy jet.
- Turn on `bInheritVelocity` for a hand-held hose or a swinging bucket.
- Set `MaxParticles` to pour an exact amount: litres × particles per litre (§1).
- For a single splash, call `BurstLiters(2.0)`.

### 14.3 A drain and a whirlpool

These values come from the demo's `-SloshManipTest` (also in `/Game/SloshTest/ManipulatorTest`):
1. Place a **Slosh Drain** on the floor in a corner. Set `Shape = Sphere`, `Radius = 30`, `RemovalRate = 0` (instant). A sphere half below the floor reads as a floor drain.
2. Place a **Slosh Force Field** in the pool. Set `ForceType = Swirl`, `Radius = 90`, `Strength = 600` cm/s², `SwirlInwardPull = 0.3`, `Falloff = 30`, and keep its +Z axis vertical.
3. Watch `GetDrainedParticleCount()`. `GetParticleCount()` drops within about 8 frames, after the next compaction. Live + drained should equal emitted.
4. For a slow overflow instead of a sink, set `RemovalRate` to around 2 to 5 (1/s).

### 14.4 Making a skeletal mesh solid to the liquid

1. Add a `USloshSkeletalColliderComponent` to the character and attach it to the skeletal mesh component.
2. Leave `CouplingMode = OneWay`, because animated characters shouldn't be pushed by the liquid. Turn on `bReportWetness` to read per-bone wetness from `GetColliders()[i]->GetSubmergedFraction()`.
3. Play with `bDrawCapsules` on and check the fit. Then adjust:
   - Add tiny bones (hair, cloth helpers, weapons) to `ExcludeBones`.
   - Raise `RadiusScale` (1.1 to 1.3) if thin limbs leak liquid on a coarse grid.
   - Lower `MaxCapsules` if the scene has other colliders (the domain budget is 64).
4. In cooked builds, enable **Allow CPU Access** on the skeletal mesh.
5. For liquid that should coat the body, use a sticky liquid (`BoundaryFriction` 0.8 to 1, `Adhesion` 0.6 to 1) and a small `CellSize`. Films are 1 to 2 particles thick, so 1.6 cm cells give a continuous coat where 2.5 cm cells look beaded.
6. After swapping the mesh at runtime, call `RebuildColliders()`.

### 14.5 Switching surface modes

| Goal | Settings |
|---|---|
| Water, pools, splashes (cheapest) | `SurfaceMode = Splats` with the default inline `Visual` (the classic water look). |
| Smooth viscous liquid with Slosh shading | `SurfaceMode = Raymarched`. Keep `SurfaceIsoLevel = 0.5` and `SurfaceSmoothing = 2`; raise `FieldResolution` to 1.5 to 2 for close-ups. |
| Liquid lit by the engine and casting shadows, with a real UE material | `SurfaceMode = Mesh`, and set `SurfaceMaterial`, or use a Fluid Painter preset as `Material`, whose paint material is then used. Raise `MaxSurfaceVertices` if parts of the surface disappear. |

`SurfaceMode` is BlueprintReadWrite and applied every tick, so `Set Surface Mode` works at runtime. From the command line, the demo accepts `-SloshSurface=Splats|Raymarched|Mesh`, `-SloshIso=`, `-SloshFieldRes=` and `-SloshSurfaceSmoothing=` (see [Testing](./Testing.md)). Change `SurfaceIsoLevel` in steps of 0.02 to 0.05: 0.4 already balloons the surface.

### 14.6 Using a Fluid Painter preset as a liquid

1. Set the volume's `Material` to a preset, for example `/Slosh/Paint/Presets/Regular/DA_SloshPaint_Honey`. The inline `Physical`/`Visual` grey out and are ignored.
2. The simulation now uses the preset's physical values (honey: 1420 kg/m³, ν = 3e-2 m²/s, friction 0.95, adhesion 1.0) and its visual values (the viscous look: smoothing 24/4/3, anisotropy 0.85, edge fade 1.5, lit scatter, radius 0.9).
3. In `Mesh` mode the surface is drawn with the preset's paint material, so the simulated liquid matches painted strokes of the same preset.
4. Viscous presets want small cells (1.6 to 2.5 cm) and enough `ViscosityIterations`. A character-pour test with 1.6 cm cells, 9500 particles/s and Honey ran at about 9 ms per frame.
5. To tweak one value without editing the shared preset, duplicate the asset. The volume cannot override a single field of an assigned asset.

From the command line: `-SloshSkelTest -SloshSkelPreset=Honey` loads `/Slosh/Paint/Presets/Regular/DA_SloshPaint_Honey`.
