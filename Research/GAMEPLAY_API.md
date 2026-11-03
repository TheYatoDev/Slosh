# GAMEPLAY_API.md — C++ and Blueprint Interface

**[Impl]** exists in the prototype plugin · **[Design]** proposed.

## 1. Classes

### `ASloshVolume` (actor) [Impl] — `Source/Slosh/Public/SloshVolume.h`
A box domain; actor location is the domain centre, rotation orients the grid, scale is ignored.

| Member | Kind | Latency / authority |
|---|---|---|
| `GridResolution`, `CellSize`, `ParticleCapacity` | config | changing layout reallocates on next reset |
| `Material` (`USloshMaterialAsset`) or inline `Physical` / `Visual` | config | live |
| `bFillOnBeginPlay`, `InitialFillBox` | config | — |
| `PressureSolver` (Auto/MGPCG/RBGS), `AutoSolverCellThreshold`, `PCGIterations`, `PressureIterations`, `SORWeight`, `ViscosityIterations`, `MaxSubsteps`, `CFL`, `MaxParticleSpeed` | quality | live |
| `bSimulate`, `bRenderLiquid`, `SunLight`, sky colours | runtime | live |
| `FillBox(WorldCenter, Extent)` | BlueprintCallable | applied next sim frame |
| `EmitSphere(WorldCenter, Radius, WorldVelocity, Count)` | BlueprintCallable | next sim frame |
| `ResetSimulation()`, `SetPaused(bool)` | BlueprintCallable | next sim frame |
| `ContainsPoint(World)`, `GetDomainSize()` | BlueprintPure | **synchronous, exact** (geometry only) |
| `GetParticleCount()` | BlueprintPure | **synchronous, exact** (CPU-tracked; emission stops at `ParticleCapacity`) |
| `GetLiquidVolumeLiters()` | BlueprintPure | **delayed** (GPU readback, 2–4 frames), over-estimates during splashes (counts cells containing any particle) |
| `GetMaxParticleSpeed()`, `GetLastSubstepCount()`, `GetFeedbackLatencyFrames()` | BlueprintPure | delayed / diagnostic |
| `GetMeanInteriorDensity()`, `GetParticleVolumeLiters()` | BlueprintPure | delayed; compression and volume diagnostics (particle-implied volume excludes splash cells) |
| `GetActiveBrickFraction()`; properties `bSparseBricks`, `SparseMaxActiveFraction` | BlueprintPure / config | delayed; sparse-dispatch diagnostics and control |
| `ParticleCapacity`; `GetParticleMemoryMB()` | config / BlueprintPure | maximum live particles (≤ 16M; buffers grow with the live count, no reset); memory actually allocated |
| `PCGIterations` (default 3), `DensityProjectionInterval` (default 1) | config | MGPCG outer iterations; density projection every N frames (2 validated for large scenes, PERFORMANCE_PLAN §10h) |
| `MaxFrameDt`, `SortInterval` | config | most simulated time per frame (1/30 s; slower frames run in slow motion instead of spiralling) and the sort cadence (default every 4 frames) |
| `GetClampedParticleCount()` | BlueprintPure | delayed; particle updates that hit MaxParticleSpeed last frame (persistent non-zero = unresolved step) |
| `SparseStorageBudget`; `GetPoolUsage()`, `GetPoolOverflowBricks()`, `GetGridMemoryMB()` | config / BlueprintPure | brick-pool capacity (fraction of all 8³ bricks; changing it reallocates) and its diagnostics; overflow > 0 means raise the budget |

### `USloshColliderComponent` (scene component) [Impl]
Analytic sphere/box/capsule seen by the liquid; attach to a primitive. When the parent simulates physics
and `CouplingMode != OneWay`, delayed GPU feedback becomes forces.

| Member | Notes |
|---|---|
| `Shape`, `Radius`, `BoxExtent`, `CapsuleHalfHeight` | cm, scaled by component scale |
| `CouplingMode` | `OneWay`, `BuoyancyAndDrag`, `PressureAndDrag` (default since Phase 2: settles floating bodies; contact faces get seepage pressure, so bodies resting on the bottom still lift) |
| `BuoyancyScale`, `DragCoefficient`, `AngularDrag`, `AddedMassCoefficient`, `MaxFluidAcceleration` | stability knobs (see PHYSICS_COUPLING.md) |
| `OnEnterLiquid`, `OnExitLiquid` (BlueprintAssignable) | fired on the game thread from delayed data, with hysteresis |
| `GetSubmergedFraction()`, `IsInLiquid()`, `GetLastFluidForce()`, `GetLocalFluidVelocity()`, `GetFeedbackAgeFrames()` | all **delayed**; `GetFeedbackAgeFrames` makes the latency explicit |

### `USloshEmitterComponent` [Impl]
`bEmitting`, `Radius`, `ParticlesPerSecond`, `Speed` (along forward X). Picked up by the volume that contains it.

### `USloshMaterialAsset` [Impl]
`FSloshPhysicalProperties Physical` + `FSloshVisualProperties Visual` (see LIQUID_MATERIALS.md).

### `USloshSubsystem` (world subsystem) [Impl]
Registry; `FindVolumeAt(WorldLocation)` (synchronous).

## 2. Query classification

| Question | Answer path | Sync? | Accuracy |
|---|---|---|---|
| Is this point inside a liquid domain? | geometry | sync | exact |
| Is an object submerged / how much? | `GetSubmergedFraction` | 2–4 frames | ±1 cell along the waterline; convex shapes only (horizontal walk to dense, supported liquid) |
| What liquid touches this actor? | collider → volume → `Material` | sync (per volume, single material) | exact for the prototype |
| Local fluid velocity near a body | `GetLocalFluidVelocity` | delayed | mean over wetted faces |
| Fluid force on an object | `GetLastFluidForce` | delayed | model-dependent (buoyancy/drag or pressure integral) |
| How much fluid in a region? | `GetLiquidVolumeLiters` (whole domain) | delayed | cell-count approximation |
| Fluid level at XY [Design] | per-column height readback (NX×NY floats, ~16 KB) | delayed | ±½ cell |
| Point query "is (x,y,z) wet?" [Design] | batched query buffer → readback | delayed, batched | cell resolution |

API rule: **no function returns GPU-derived data under a name that implies it is current.** Every
delayed getter is documented as such and paired with `GetFeedbackAgeFrames` / `GetFeedbackLatencyFrames`.
For latency-critical gameplay (e.g. "player is drowning"), use a CPU proxy, e.g. a gameplay water plane
driven by the delayed column-height readback.

## 3. Lifecycle, streaming and ownership

- GPU state is created in `BeginPlay` and released on the render thread in `EndPlay` [Impl]. A streamed-out
  level therefore drops its liquid. Persisting liquid across streaming would need a GPU→CPU snapshot
  (≈17 MB per 262k particles) [Design].
- One simulation owner per volume. World Partition: place volumes in always-loaded cells or a dedicated
  data layer. Volumes do not interact with each other [Design: hand-off emitters at boundaries].
- PIE/Editor: simulation runs only in game worlds (BeginPlay). An editor-preview tick is a Phase 2 item.

## 4. Planned additions [Design]
`USloshQueryLibrary` (async Blueprint nodes returning via latent action: `QueryWetness`,
`QueryColumnHeight`), `USloshDrainComponent` (particle sink), `USloshContainerComponent` (analytic
hollow box/cylinder SDF for moving cups/buckets, needed for Experiment 4), and per-volume
`OnLiquidVolumeChanged` events.
