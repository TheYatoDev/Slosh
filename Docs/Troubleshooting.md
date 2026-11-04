# Troubleshooting

These are known gotchas, collected from code comments, the Research notes and development history. Each entry gives **symptom → cause → fix**. File references point at the code that explains the behaviour.

Related pages: [Testing](./Testing.md) (log fields such as `ovf=`, `clamp=`, `latency=`), [Reference](./Reference.md), [Materials](./Materials.md), [Rendering](./Rendering.md), [Simulation](./Simulation.md), [Slosh Paint](./SloshPaint.md).

---

## Contents

1. [Build, repository and tooling](#1-build-repository-and-tooling)
2. [Rendering](#2-rendering)
3. [Simulation and capacity](#3-simulation-and-capacity)
4. [Emitters, drains and force fields](#4-emitters-drains-and-force-fields)
5. [Rigid-body coupling and colliders](#5-rigid-body-coupling-and-colliders)
6. [Skeletal mesh colliders](#6-skeletal-mesh-colliders)
7. [Slosh Paint and geometry code](#7-slosh-paint-and-geometry-code)
8. [Engine and GPU programming gotchas (for contributors)](#8-engine-and-gpu-programming-gotchas-for-contributors)

---

## 1. Build, repository and tooling

| Symptom | Cause | Fix |
|---|---|---|
| `Build.bat` fails with a Live Coding / "unable to build while Live Coding is active" error, or waits forever on the mutex. | The open editor's **Live Coding** session holds the build lock, which blocks UnrealBuildTool. | Close the editor (kill `UnrealEditor.exe` if needed), build, then relaunch. Don't mix Live Coding patches with full UBT builds. |
| UBT reports that it can't find the project, or a mangled path, when building from Git Bash. | Git Bash (MSYS) rewrites the quoted `-Project="C:\…\SloshDemo.uproject"` argument. | Build from **PowerShell** with the full `Build.bat` path ([Testing §1](./Testing.md#1-build)). |
| After cloning, maps and assets fail to load ("package is corrupt", tiny `.uasset`/`.umap` files). | `.uasset`, `.umap`, `.fbx`, `.exr`, `.wav`, `.mp4` and `.blend` are stored with **Git LFS** (`.gitattributes`). Without LFS you get pointer files. | `git lfs install`, then `git lfs pull`. |
| The first launch after a shader edit takes minutes. | Slosh shaders (`/Plugin/Slosh`) recompile on startup. | Expected. Later launches use the DDC. |
| `-SloshSkelTest` exits immediately with `skel test: mesh … not found`. | The built-in default mesh is optional local test content that isn't in the repository. | Pass `-SloshSkelMesh=/Game/…/YourMesh.YourMesh` (any UE mannequin works). |
| `-SloshPaintTest` logs `character MISSING`. | The mannequin it loads is optional local test content. | The sphere strokes still run. Provide a mannequin locally if you need the character half. |
| A test level opens with missing actors or meshes. | Some levels in `Content/SloshTest` reference optional local test content. | Use the committed `/Game/SloshTest/ManipulatorTest`, or build your own level. |
| Solver flags such as `-SloshGrid=` or `-SloshCFL=` do nothing. | The map already contains an `ASloshVolume`, so the game mode runs in **placed-scene** mode and applies only the surface flags. | Edit the placed volume, or run on a map without a volume (`/Game/Startup`). |
| `-SloshNoAniso` doesn't change the image. | Inline `Visual.Anisotropy` already defaults to 0 (`SloshMaterialAsset.h`), so the flag sets 0 to 0 (`SloshDemoGameMode.cpp:161`). | A/B against a volume using a preset (anisotropy 0.85), or set `Anisotropy` by hand. |
| `-SloshDensityCorrection=` / `-SloshNoDensityCorrection` change nothing. | `DensityCorrection` only drives the velocity correction, which is disabled while `bTwoSidedDensityProjection` is on (`SloshProxy.cpp:421`). | Add `-SloshVelocityDensityCorrection`, or tune `DensityRelaxation` instead. |
| An MCP script's `find_actors(name="SloshVolume_0")` finds nothing. | `find_actors` matches actor **labels**, not object names. | Address actors by object path: `/Game/Map.Map:PersistentLevel.SloshVolume_0`. |
| MCP `BlueprintTools.create` makes a Blueprint with the wrong parent. | The parent class goes in `asset_type`. | Pass the parent class as `asset_type`. |

---

## 2. Rendering

| Symptom | Cause | Fix |
|---|---|---|
| No liquid at all, but `particles=` grows in the log. | **MSAA is unsupported.** With forward shading plus MSAA the scene colour has more than one sample and the screen-space renderer returns early (`SloshRenderer.cpp:440-442`). | Use the deferred renderer with TSR/TAA/FXAA. Don't use forward-shading MSAA. |
| No liquid in the editor viewport. | Volumes create their proxy in `BeginPlay`, so nothing is simulated or drawn outside PIE or `-game`. | Press Play. The `Bounds` box shows the domain in the editor. |
| No liquid while playing. | `bRenderLiquid` is off; or `bSimulate` was off from the start and nothing was emitted; or the fill box lies outside the domain or inside the wall layer. | Check those three. `InitialFillBox` is relative to the domain centre, and the outermost cell layer is always empty. |
| Water looks melted or over-smoothed: no ripples, streaks on walls, ragged edges. | The viscous render settings (`SmoothingRadius 24`, `SmoothingScale 4`, `SmoothingIterations 3`, `EdgeFadeThickness 1.5`, `Anisotropy 0.85`, `ScatterLighting 1`) are in use on water, for example from a preset assigned as `Material`. | For water use the classic look: `7 / 0 / 1`, anisotropy 0, edge fade 0, scatter lighting 0, radius 0.6. These are the inline defaults; compare against a `-SloshCalm` baseline ([Materials §2](./Materials.md#2-how-a-volume-picks-its-liquid)). |
| Raymarched or Mesh surface balloons into fat blobs and merges separate streams. | `SurfaceIsoLevel` is **very sensitive**. 0.4 already balloons the surface (lower = fatter). | Keep 0.5 (the particle-radius surface). Change it in 0.02 to 0.05 steps. Use `SurfaceSmoothing` for smoothness instead. |
| Thin films and coats look beaded or blobby in any mode. | Resolution. Films are only 1 to 2 particles thick; at 2.5 cm cells a coat is a row of beads. | Smaller `CellSize` first (1.6 cm gives a continuous coat). Then `Anisotropy` 0.85, `EdgeFadeThickness` 1 to 2, or Raymarched mode. Heavy smoothing on coarse liquid only makes it wobbly. |
| Mesh mode: pieces of the surface are missing in big scenes. | The `MaxSurfaceVertices` budget is exhausted (surface past it is dropped; hard cap 8,388,608). | Raise `MaxSurfaceVertices`, or lower `FieldResolution`. |
| Mesh mode: the liquid is the grey default material. | `SurfaceMaterial` is empty and `Material` isn't a Fluid Painter preset (`GetEffectiveSurfaceMaterial`). | Set `SurfaceMaterial`, or assign a `USloshPaintPreset`. |
| Mesh mode: fast liquid smears or ghosts under TSR/TAA or motion blur. | The surface mesh writes no motion vectors (`bVelocityRelevance = false`, `SloshSurfaceMeshComponent.cpp:45`). The mesh is rebuilt every frame with no vertex correspondence. | Expected. Raymarched/Splats avoid it, or lower the TSR history weight for close-ups. |
| The liquid has no sun highlight, or the highlight comes from the wrong light. | `SunLight` auto-selects the **first** `ADirectionalLight` found (`SloshVolume.cpp:285-291`). | Set `SunLight` explicitly in levels with several directional lights. |
| Translucent objects sort wrongly against the liquid; refraction doesn't show objects off screen. | Splats and Raymarched composite after opaque geometry in screen space (Research/RISK_REGISTER R-12). | Inherent to screen-space liquid. Use Mesh mode when correct translucency sorting matters. |
| Close-up splats look clipped or square. | Splat footprints are capped at 32 px (`MaxSplatPixels`, `SloshRenderer.cpp:52`). | Move the camera back, or use Raymarched/Mesh for close-ups. |
| Spray looks like round dots while sheets look smooth. | Anisotropy only stretches particles with enough neighbours (35 weighted). Isolated spray stays round on purpose; with fewer neighbours, falling columns turned into "pins". | Expected. |

---

## 3. Simulation and capacity

| Symptom | Cause | Fix |
|---|---|---|
| Emission silently stops; `particles=` plateaus. | `ParticleCapacity` is reached. Emission appends at the GPU count and stops at capacity (`SloshProxy.cpp:485-511`). | Raise `ParticleCapacity` (up to 16,777,216). Buffers grow on demand, so a high cap costs nothing until used. A full cell is 8 particles. |
| Parts of the liquid stop interacting with the grid and fall or fly ballistically; `ovf=` is above 0 in the log. | **Sparse storage overflow.** `SparseStorageBudget` is too small for the bricks the liquid touches (often at a splash peak). Bricks beyond the pool get no storage. | Raise `SparseStorageBudget` until `GetPoolOverflowBricks()` / `ovf=` stays 0 through the biggest splash. 1.0 can never overflow. |
| All the liquid disappears when you edit a property during PIE. | Changing `GridResolution` or `SparseStorageBudget` changes the GPU layout and reallocates everything (`SloshTypes.h:156`, `SloshProxy.cpp:349`). `InitialFillBox` isn't re-applied. | Expected. Set layout properties before play, or call `FillBox` after the change. |
| The liquid is clipped or offset compared with the `Bounds` box on very large grids. | `GridResolution` has no editor clamp, but the solver clamps each axis to 4..256 (`SloshProxy.cpp:345`). The actor's bounds still use the unclamped value. | Keep each axis at 256 or below. Use a larger `CellSize` for bigger domains. |
| The liquid runs in slow motion when the frame rate drops. | `MaxFrameDt` (1/30 s) caps simulated time per frame, to prevent the substep feedback spiral. | Expected below 30 FPS. Raise `MaxFrameDt` (up to 0.1) to trade stability for real-time accuracy. |
| `substeps=` is pinned at `MaxSubsteps` and `clamp=` stays above 0. | Particles hit `MaxParticleSpeed` and the step is unresolved (very fast emitters, violent impacts, or CFL too high). | Lower emitter `Speed`; lower `CFL`; raise `MaxSubsteps`. Diagnose with `-SloshTraceSpeed` (the `inSolid=` count points at collider penetration). |
| A viscous liquid still splashes like water. | ν is below the visibility threshold for the cell size (ν·dt/h² ≳ 0.05: about 3e-2 at 5 cm, 8e-3 at 2.5 cm), or 12 `ViscosityIterations` under-damp a stiff solve. | Smaller cells, higher ν, `ViscosityIterations` 30 to 50 ([Materials §3](./Materials.md#3-physical-properties-in-depth)). |
| A viscous liquid spreads into a single sparse particle layer on the floor. | No tangential friction at the domain walls. | Raise `BoundaryFriction` (it also applies at domain walls). |
| A sticky liquid slides off a character but sticks to the floor, or the reverse. | `Adhesion` only acts near **colliders**. Domain walls get friction but no adhesion. | Combine `BoundaryFriction` (walls and colliders) with `Adhesion` (colliders). |
| Slime has no strings and never springs back. | The model is Newtonian; there is no viscoelasticity or yield stress yet (Research/LIQUID_MATERIALS §4). | Approximate with high ν plus adhesion, or wait for a rheology model. |
| Volume slowly grows or shrinks over minutes. | The density projection is off or too weak. | Keep `bTwoSidedDensityProjection` on (default). Measured drift is about 0.000 %/min calm and −0.007 %/min agitated. Watch `pvol=`, not `liquid=`. |
| `liquid=` / `GetLiquidVolumeLiters()` jumps during splashes. | It counts every cell holding any particle. | Use `GetParticleVolumeLiters()` (`pvol=`) for volume. |
| Results differ between machines or runs. | GPU float math is not deterministic across vendors and drivers; flow is chaotic (coupling metrics vary by ±0.15 run to run). | Don't rely on determinism. For multiplayer, run a local visual simulation from replicated events, with server-side gameplay proxies (Research/RISK_REGISTER, Networking). |

---

## 4. Emitters, drains and force fields

| Symptom | Cause | Fix |
|---|---|---|
| The emitter emits nothing. | Its location is outside every volume's domain; or `MaxParticles` is used up; or the volume is paused (`bSimulate = false` also stops emitters). | Move it inside. Call `ResetEmittedCount()`: `ResetSimulation()` doesn't reset emitters. Unpause the volume. |
| A horizontal jet loses its sideways speed and drops almost straight down. | The first G2P steps lose much of a thin jet's horizontal momentum at coarse resolution. | Aim pours mostly downward. Use a wider `Radius` (one cell or more) or a smaller `CellSize`. |
| Liquid appears in only one of two overlapping volumes around an emitter. | By design: an emitter feeds only the first volume that contains it each frame (`EmitFrame`, `SloshVolume.cpp`). | Place the emitter inside the volume you want, or do not overlap domains around it. |
| `ASloshPourNozzle` ignores `StartEmitting`/`StopEmitting`. | Its `Tick` writes `bEmitting` every frame from `StartDelay`/`StopAfter`. | Use those two properties, or `Burst`. |
| Draining doesn't reduce `GetParticleCount()` right away. | Drained particles are only marked dead. Slots are reclaimed at the next compaction (the spatial sort, and at least every 8 frames while a drain is active, `SloshProxy.cpp:544`). The count then arrives through a 2 to 4 frame readback. | Expected, up to about 12 frames. `GetDrainedParticleCount()` is the accurate running total. |
| The whole volume empties. | A drain with `Shape = Everywhere`. | Use a bounded shape. `Everywhere` is meant for force fields. |
| `RemovalRate = 10` removes far more than 10 particles per second. | `RemovalRate` is a per-particle rate (1/s), not a count: each particle inside is removed with probability `1 − exp(−rate·dt)`. 0 means instant. | Pick the rate from the half-life you want: `t½ = ln 2 / rate`. |
| A force field or drain has no effect. | Disabled; or `Strength ≈ 0` (the field is skipped); or the location is beyond the volume's bounds plus its reach; or the domain already has 32 manipulators (the rest are silently dropped). | Check `bEnabled`, `Strength`, placement, and the count. `bDrawShape` shows the region (grey when disabled). |
| A whirlpool flings liquid outward. | `SwirlInwardPull` is too low for the swirl strength. | 0.3 to 0.6 keeps a vortex together (demo: strength 600, pull 0.3). |

---

## 5. Rigid-body coupling and colliders

| Symptom | Cause | Fix |
|---|---|---|
| Enter/exit events, submerged fraction and forces arrive late. | **Readback latency.** All GPU feedback is 2 to 4 frames old (4-slot non-blocking readback ring; usually 3 frames, see `latency=`). | By design. Use `GetFeedbackAgeFrames()`. For gameplay-critical checks such as drowning, use a CPU proxy (a water plane or trigger). |
| A floating body bobs continuously. | Lagged explicit coupling; `BuoyancyAndDrag` uses the delayed submerged-volume sensor. | Use `PressureAndDrag` (default; measured to settle stably). Raise `LinearDrag`/`AngularDrag`. Keep `AddedMassCoefficient` around 0.5. |
| Coupling becomes noisy or inaccurate after raising `CFL` for performance. | CFL 4 halves the cost, but larger steps degrade the pressure the body feels. | Use CFL 2 in scenes where floating bodies matter. |
| `BuoyancyScale` has no effect. | It is only used in `BuoyancyAndDrag` mode. `PressureAndDrag` integrates real solver pressure. | Switch modes, or change the body's mass. |
| A body resting on the floor gets no lift. | Liquid can't reach the faces in contact. | `PressureAndDrag` applies "seepage" (contact faces receive the surrounding pressure). For bodies that rest on the bottom for long periods, `BuoyancyAndDrag` never sticks. |
| A body doesn't react to the liquid at all. | The collider's attach parent (or the owner's root) isn't a primitive that **simulates physics**; or `CouplingMode = OneWay`; or the collider is outside the domain. | Attach the collider to the simulating mesh component and enable Simulate Physics. |
| `GetSubmergedFraction()` is always 0, and `OnEnterLiquid` never fires, on a kinematic or animated object. | The GPU only accumulates feedback for colliders with the two-way flag. | Set `bReportFeedbackWhenKinematic = true` (`bReportWetness` on skeletal colliders). |
| The collider is much larger than the mesh, or the wrong shape. | Shapes scale by the **largest** world-scale axis (`SloshColliderComponent.cpp:78`); non-uniform scale isn't supported. Sizing in world units while the component inherits the mesh scale double-scales. | Size the collider in the mesh's local units (50 for a 100 cm basic shape) and keep scale uniform. For a slab, use `BoxExtent` with unit scale rather than a scaled cube. The demo paddle (`SloshDemoGameMode.cpp:212`, scale 0.2 × 1.2 × 0.8) shows the problem: its box collider becomes a 60 cm cube. |
| Rigid bodies fall out of the pool or through the liquid floor. | The grid walls bound only the liquid. Physics doesn't know about them. | Add physics walls and a floor matching the domain (the demo spawns invisible 1 m thick walls). The liquid floor is one cell above the domain's min Z. |
| Some colliders are ignored. | More than 64 colliders overlap the domain. The extras are skipped silently, in registration order (`SloshVolume.cpp:416`). Skeletal capsules count too. | Lower `MaxCapsules` on skeletal colliders, or remove distant colliders. |
| Forces look jittery in the log. | Remaining flow noise (around 100 N lateral on a 40 cm box); there is 50% temporal smoothing already. | Expected. Compare multi-run averages. `GetLastPressureForceNewtons()` is the unsmoothed signal. |

---

## 6. Skeletal mesh colliders

| Symptom | Cause | Fix |
|---|---|---|
| Warning: `LOD0 vertex data is not CPU-readable; enable Allow CPU Access on the mesh`. | Fitting reads LOD0 positions and skin weights on the CPU. Cooked builds strip that data unless requested. | Enable **Allow CPU Access** on the skeletal mesh. It is always available in the editor and in `-game` from the editor binary. |
| Warning: `no skinned mesh / render data to fit`. | The component isn't attached to (or on the same actor as) a skinned mesh, or the mesh asset wasn't set at BeginPlay. | Attach it to the skeletal/poseable mesh. If you set the mesh later, call `RebuildColliders()`. |
| Few colliders, or none, on a character whose physics asset has many bodies. | Fitting ignores the physics asset by design. It uses vertices by dominant bone, because some physics assets have only a handful of bodies. Bones below `max(12, MinVertexFraction × verts)` merge into their parent. | Lower `MinVertexFraction`, raise `MaxCapsules`, and check with `bDrawCapsules`. |
| Liquid leaks through thin limbs. | Capsules thinner than about a cell don't block a coarse grid. | `RadiusScale` 1.1 to 1.3, `MinRadius` of about one cell, or a smaller `CellSize`. |
| Hair, cloth or accessories block the liquid unrealistically. | They own vertices and got shapes. | Add their root bones to `ExcludeBones` (the whole subtree is excluded). |
| Other colliders stop working once a character enters. | The default 40 capsules share the 64-collider domain limit. | Lower `MaxCapsules` to 20 to 30. |
| The character is pushed around by the liquid. | `CouplingMode` isn't `OneWay` and the mesh simulates physics. | Keep `OneWay` for animated characters (the default). |

---

## 7. Slosh Paint and geometry code

| Symptom | Cause | Fix |
|---|---|---|
| A random crash after paint builds, inside `FDynamicMesh3` attribute code. | `FDynamicMesh3` was stored **by value** in a growing `TArray`/`TMap`. UE containers relocate elements bitwise, which leaves the attribute set's back-pointer to its mesh dangling (`SloshPaintComponent.cpp:35`). | Heap-allocate meshes (`TUniquePtr`/`TSharedPtr`) inside containers. Never keep `FDynamicMesh3` by value in a container that can grow. |
| Generated meshes are inside out (lighting inverted, back faces visible). | **Unreal is left-handed.** An outward-facing closed mesh has a **negative** signed volume in GeometryCore, the opposite of right-handed tools (`SloshPaintMesher.cpp:333`). | Flip the winding when the signed volume is positive, not when it is negative. |
| Subsurface paint materials glow or tint even at opacity 1. | Subsurface shading adds `SubsurfaceColor` regardless of opacity. | Scale `SubsurfaceColor` by the amount or opacity parameter in the material. |
| Runtime painting on skeletal meshes hits nothing in a cooked build. | Skin traces (`TraceSkinnedSurface`) need CPU-readable render data. | Enable **Allow CPU Access** on the mesh. |
| Animated drops or `Reveal` keyframes lag 1 to 2 frames. | Meshing runs on a worker task with one build in flight. Animated strokes rebuild every frame (Research/RISK_REGISTER R-24). | Expected. Static paint costs nothing per frame. |
| Distributing the project with the bundled presets. | The presets are ports of the Fluid Painter add-on, a paid Blender product whose archives carry no licence text (Research/RISK_REGISTER R-23). | Confirm redistribution rights before shipping or publishing preset content. The `bAdultContent` folder can be excluded separately. |

More paint-specific issues: [Slosh Paint](./SloshPaint.md).

---

## 8. Engine and GPU programming gotchas (for contributors)

| Symptom | Cause | Fix |
|---|---|---|
| A shader fails to bind, or a compile error about too many UAVs. | SM5 allows at most **8 UAVs** per shader. | Bind read-only buffers as SRVs. Allocator passes swap pool SRVs for UAVs (a pass must not bind both views of one buffer, `SloshProxy.cpp:452`). |
| Large scans or particle passes silently process only part of the data. | D3D's 65,535 thread-group limit per dispatch dimension. | Use `Slosh::GroupCount1D` and `LinearThreadId` (1D dispatches wrap into 32,768-group rows, `SloshTypes.h:101-103`) for every new 1D pass. |
| A compile error when calling `GetMaterialRelevance` on a custom primitive. | It exists only on `UMeshComponent`. | Use `Material->GetRelevance_Concurrent(ShaderPlatform)` (see `SloshSurfaceMeshComponent.cpp`). |
| `SetStaticMesh` is rejected on a runtime-spawned `AStaticMeshActor`. | Spawned static-mobility components refuse mesh changes. | `SetMobility(EComponentMobility::Movable)` first (the demo game mode does this). |
| A separate particle-read pass costs about 1 ms at 3.6M particles. | Particle buffers are bandwidth-bound (48 B per particle). | Fold new per-particle work into G2P rather than adding a pass. |
| A "light" V-cycle (red-only level-0 smoothing) diverges (div 0.025 → 33). | Unstable smoother. | Don't reintroduce it (Research/PERFORMANCE_PLAN). |
| The `RDG_GPU_STAT_SCOPE` macro is missing. | It was removed in UE 5.8 (GPU profiler rework). | Use `RDG_EVENT_SCOPE_STAT`, as `SloshProxy.cpp` does. |
| The post-opaque callback breaks after an engine upgrade. | The renderer relies on the post-opaque render delegate and the `FViewInfo` → `FSceneView` layout (RISK_REGISTER R-11, R-13). | Rebuild and check `SloshRenderer.cpp` on each engine upgrade. The project is pinned to 5.8. |
