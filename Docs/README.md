# Slosh developer documentation

Slosh is a GPU liquid simulation plugin for Unreal Engine 5.8: a sparse FLIP/APIC solver with multigrid pressure,
two-way rigid-body coupling, skeletal-mesh collision, emitters, drains and force fields, three ways of drawing the
liquid surface (screen-space splats, a ray-marched particle field, a GPU triangle mesh) and Slosh Paint, a
Fluid Painter-style tool for painted (non-simulated) liquid strokes.

This folder is the starting point for engineers joining the project. Read this page first, open the showcase
levels, then go deep in whichever area you will work on.

| Document | Read it when you... |
|---|---|
| [Showcase.md](Showcase.md) | want to see every feature running and know which actor to select to change it. |
| [Architecture.md](Architecture.md) | need the big picture: modules, threads, the frame timeline, GPU buffers, readback, adding a pass. |
| [Simulation.md](Simulation.md) | change the solver: every compute pass in execution order, coupling maths, settings, invariants. |
| [Rendering.md](Rendering.md) | change how liquid is drawn: splats, anisotropy, smoothing, composite, surface field, ray-march, mesh. |
| [Reference.md](Reference.md) | use the actors and components: every property, default, range and Blueprint function, plus recipes. |
| [Materials.md](Materials.md) | tune a liquid (water, honey, slime...) or use a Fluid Painter preset as a simulated liquid. |
| [SloshPaint.md](SloshPaint.md) | work on painted strokes: data model, meshing, skinned meshes, presets, editor tool. |
| [Testing.md](Testing.md) | build, run the command-line test scenes, read the logs and benchmarks. |
| [Troubleshooting.md](Troubleshooting.md) | something looks or behaves wrong (symptom, cause, fix). |

Older design notes (Phase 0 research, measurements, decisions) live in [`../Research`](../Research). They explain
*why* things are the way they are, but they predate parts of the code; when they disagree, the code and these docs win.

---

## 1. Repository layout

```
UE5 Fluid Sim/
├─ Docs/                          this documentation
├─ Research/                      design history and measurements (Phase 0 onwards)
└─ SloshDemo/                     host project (SloshDemo.uproject)
   ├─ Source/SloshDemo/           demo game module: test game mode, showcase/test helper actors
   ├─ Content/SloshShowcase/      the showcase levels (SH_01 ... SH_08)
   ├─ Content/SloshTest/          test levels (ManipulatorTest) and the dark test surface material
   └─ Plugins/Slosh/
      ├─ Shaders/Private/         all compute shaders (virtual path /Plugin/Slosh/Private)
      ├─ Content/Paint/           Slosh Paint masters, presets, Blueprints
      └─ Source/
         ├─ SloshCore/            render-thread solver (FSloshProxy) and shader bindings
         ├─ SloshRender/          screen-space renderer, surface field, surface mesh buffers
         ├─ Slosh/                gameplay UObjects: volume, colliders, emitters, manipulators, paint
         └─ SloshEditor/          Slosh Paint editor mode and tool
```

Dependency direction: `Slosh` → `SloshRender` → `SloshCore`. `SloshCore` knows nothing about UObjects; it receives a
plain-data `FSloshFrameInput` every frame and returns `FSloshResults` asynchronously. See
[Architecture.md §1](Architecture.md#1-modules).

## 2. Getting started

### 2.1 Requirements

- Unreal Engine **5.8** (launcher install at `C:\Program Files\Epic Games\UE_5.8`), Windows, D3D12 (SM5/SM6).
- Visual Studio 2022 with the UE C++ workload.
- Git LFS (`.uasset` / `.umap` are LFS-tracked).
- A GPU with a few GB of VRAM. The project was developed on an RTX 5060 Laptop GPU; numbers quoted in these docs come from it.

### 2.2 Build

Close the editor first: Live Coding blocks UnrealBuildTool for this project. Build from **PowerShell** (Git Bash
mangles the quoted `-Project` path):

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" SloshDemoEditor Win64 Development "-Project=C:\path\to\UE5 Fluid Sim\SloshDemo\SloshDemo.uproject" -WaitMutex
```

Shader-only changes (`.usf` / `.ush`) need no C++ build: the editor or game recompiles changed global shaders at startup.
More in [Testing.md §1](Testing.md#1-build).

### 2.3 First run

1. Open `SloshDemo/SloshDemo.uproject`.
2. In the Content Browser open `Content/SloshShowcase/SH_01_DamBreak` and press **Play**.
3. Walk through `SH_02` ... `SH_08`; [Showcase.md](Showcase.md) says what each level demonstrates.

The project's default game mode (`ASloshDemoGameMode`) uses a level as-is when it already contains a `ASloshVolume`
(or a `SloshShowcase`-tagged actor) and only logs statistics once per second to `Saved/Logs/SloshDemo.log`. An empty
level instead gets a generated dam-break test scene. Command-line test scenes and flags are in [Testing.md](Testing.md).

### 2.4 Your first liquid in a new level

1. Place a **SloshVolume** (Place Actors, search "Slosh"). The actor location is the domain centre; the box shows its size
   (`GridResolution × CellSize`). Keep the bottom of the box on your floor.
2. Either leave **Fill On Begin Play** on (fills `InitialFillBox`), or add liquid: place a **Slosh Pour Nozzle**
   (demo module) or add a **Slosh Emitter** component to any actor inside the box. Emitters point along their +X axis.
3. Make things solid to the liquid: add a **Slosh Collider** component (sphere, box or capsule) to any primitive, or a
   **Slosh Skeletal Collider** to a skeletal mesh. Physics-simulating parents float or sink (two-way coupling).
4. Optional: drop a **Slosh Drain** or **Slosh Force Field** actor, pick a liquid in the volume's **Material** slot
   (any Fluid Painter preset under `/Slosh/Paint/Presets/Regular` works) and choose a **Surface Mode**.

Recipes with exact values are in [Reference.md §14](Reference.md#14-recipes).

## 3. Feature map

| Feature | Where (gameplay) | Where (GPU) | Docs |
|---|---|---|---|
| FLIP/APIC solver, sparse bricks, MGPCG / RBGS pressure, viscosity, density projection | `ASloshVolume` | `FSloshProxy`, `Slosh.usf`, `SloshPressureMG.usf` | [Simulation](Simulation.md) |
| Spatial sort + compaction of removed particles | – | `SloshSort.usf` | [Simulation §4](Simulation.md), [Architecture §7](Architecture.md) |
| Analytic colliders and two-way coupling (OneWay, BuoyancyAndDrag, PressureAndDrag) | `USloshColliderComponent` | `BuildSolidCS`, `CouplingForceCS`, `SubmergedCS` | [Simulation §8](Simulation.md), [Reference §5](Reference.md) |
| Skeletal-mesh collision (capsules fitted per bone) | `USloshSkeletalColliderComponent` | same as colliders | [Reference §6](Reference.md) |
| Adhesion and boundary friction (sticky liquids) | `FSloshPhysicalProperties` | G2P collider and wall response | [Materials](Materials.md), [Simulation](Simulation.md) |
| Emitters: shapes, rates, spread, bursts, limits, inherited motion | `USloshEmitterComponent` | `EmitReserveCS`, `EmitParticlesCS` | [Reference §7](Reference.md) |
| Drains and force fields (directional, radial, swirl, drag) | `USloshDrainComponent`, `USloshForceFieldComponent` (+ placeable actors) | G2P manipulator loop | [Simulation §7](Simulation.md), [Reference §8](Reference.md) |
| Liquid materials; Fluid Painter presets as liquids | `USloshMaterialAsset`, `USloshPaintPreset` | – | [Materials](Materials.md) |
| Splats: spheres or anisotropic ellipsoids, bilateral smoothing, composite | `FSloshVisualProperties` | `SloshRender.usf` | [Rendering §5](Rendering.md) |
| Raymarched surface (particle field) | `ASloshVolume::SurfaceMode` | `SloshSurface.usf` | [Rendering §6–7](Rendering.md) |
| GPU surface mesh with UE materials | `USloshSurfaceMeshComponent` | surface nets, `FSloshSurfaceMeshBuffers` | [Rendering §8](Rendering.md) |
| Slosh Paint strokes | `USloshPaintComponent`, painter actors, editor mode | CPU meshing on a worker task | [SloshPaint](SloshPaint.md) |

## 4. Conventions every contributor should know

- **Units.** Gameplay code is in Unreal units (cm, kg, cm/s). The solver works in *sim space*: metres, origin at the
  domain's min corner, axes aligned with the volume actor. `ASloshVolume::GetSimToWorld()` converts; manipulator and
  collider structures are converted to sim space and SI units before they reach the GPU.
- **Latency.** Everything the GPU reports back (collider forces and wetness, particle and drained counts, max speed)
  arrives 2–4 frames late through asynchronous readback. Never treat it as this frame's state.
- **Particle slots.** The CPU only knows an *upper bound* of the particle count. Removed particles carry a negative
  `Age` until the next sort compacts them; every particle pass must skip them ([Architecture §7](Architecture.md)).
- **Shared shader parameters.** All solver passes share `FSloshPassParameters`. Adding a buffer means binding something
  valid for every pass; mind the SM5 limit of 8 UAVs per pass ([Architecture §10](Architecture.md)).
- **Defaults are the water look.** Inline `FSloshVisualProperties` defaults render classic water. The heavier
  "viscous" render settings (adaptive smoothing, anisotropy, edge fade, lit scatter) are stored explicitly in the paint
  presets. Do not change the struct defaults without re-saving every preset ([Materials §5](Materials.md)).
- **Content tooling.** Editor content (presets, test levels, showcase levels) was authored by scripts, either through
  the editor's MCP server or headless with `UnrealEditor-Cmd.exe -run=pythonscript` ([Testing §7](Testing.md)).

## 5. Status and open items

- **Preset licensing.** Redistribution rights for the Fluid Painter-derived presets must be confirmed before presets
  are pushed to a public remote ([SloshPaint §14](SloshPaint.md)).
- **Known limits.** These are collected in [Troubleshooting](Troubleshooting.md) and in the *Known limits* sections of
  [Simulation](Simulation.md#10-known-limits), [Rendering](Rendering.md#11-known-artifacts-and-limits) and
  [Architecture](Architecture.md#12-architectural-limits-and-caveats):
  - the particle speed clamp is counted but not applied;
  - colliders scale by their largest axis;
  - the surface-field kernel is capped at 4 voxels;
  - volumes are composited in registration order.
- **Tried and rejected.** A field blur with temporal blending of the surface field made the surface worse and was
  reverted ([Rendering §11](Rendering.md#11-known-artifacts-and-limits)).
