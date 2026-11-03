# Slosh — GPU FLIP/APIC liquid plugin for Unreal Engine 5.8 (Phase 1 prototype)

Dense-MAC-grid FLIP/APIC liquid solver written as RDG compute shaders, plus analytic rigid-body coupling and
screen-space fluid rendering. It does not depend on Niagara. Design and measurements are in `../../../Research/`.

## Quick start
1. Copy `Plugins/Slosh` into a C++ project (UE 5.8, Windows, DX12/SM5+). Build the editor target.
2. Place an **SloshVolume** actor. Its location is the domain centre; grid size is `GridResolution × CellSize`.
   It fills `InitialFillBox` on BeginPlay.
3. On any physics-simulating static mesh actor, add a **SloshCollider** component (sphere/box/capsule sized
   in cm, inherits scale). The liquid flows around it and it receives buoyancy/drag.
4. Optionally add a **SloshEmitter** component inside the volume.
5. Optionally assign a **SloshMaterialAsset** (Data Asset) for density/viscosity/transfer mode and its
   separate visual properties.

## Demo / benchmark
`SloshDemo` (the host project in this repo) builds a dam-break test scene from its game mode:
```
UnrealEditor.exe SloshDemo.uproject -game -windowed -ResX=1280 -ResY=720 -SloshAutoTest [-SloshGrid=96,96,72] [-SloshIters=60]
```

## Modules
- `SloshCore` (PostConfigInit): solver shaders, `FSloshProxy` (render-thread GPU state).
- `SloshRender` (PostConfigInit): screen-space renderer via the public post-opaque delegate.
- `Slosh` (Default): actors, components, data assets, subsystems.

## Known limitations
Single liquid per volume. Ghost-fluid free surface (interface from the smooth particle density). Binary cell solids in the solve (rigid-body forces use SDF-weighted faces). Pressure solver: Auto (RBGS on small grids, MGPCG above 400k cells).
Feedback forces arrive 2–4 frames late. Liquid draws before translucency and has no motion vectors or
shadows. Emission stops once `ParticleCapacity` is reached; particle buffers grow with the live count up to it (no reset), so a generous cap costs no memory. Volume is held by a two-sided position-based density projection (`bTwoSidedDensityProjection`). Grid storage is a pool of 8³ bricks allocated near particles (`SparseStorageBudget` caps it; watch `GetPoolOverflowBricks()`), and grid passes use an active-brick list when under half the pool is in use (`bSparseBricks`). Particles are spatially sorted every 4 frames (`SortInterval`) and stored in 48 bytes; simulated time per frame is capped at `MaxFrameDt` (1/30 s), so slow frames run in slow motion instead of spiralling. See `Research/RISK_REGISTER.md`.

## Licence notes
See `NOTICE.md`. The solver is original code that uses Blender FLIP Fluids (MIT engine) only as an
algorithmic reference.
