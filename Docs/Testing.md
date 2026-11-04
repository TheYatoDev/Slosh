# Testing, Benchmarks and Tooling

Slosh has no unit-test suite. It is validated by running the demo game module headlessly-but-rendered from the command line. `ASloshDemoGameMode` (`SloshDemo/Source/SloshDemo/SloshDemoGameMode.h/.cpp`) builds a test scene, logs metrics once per second, takes screenshots and exits. This page documents every flag it parses, what each log field means, where the output goes, and how to build.

Related pages: [Troubleshooting](./Troubleshooting.md), [Reference](./Reference.md), [Simulation](./Simulation.md), [Showcase](./Showcase.md).

---

## 1. Build

Close the editor first. **Live Coding holds a lock that blocks UnrealBuildTool**, so a running editor with Live Coding makes the build fail or hang. Kill `UnrealEditor.exe`, build, then relaunch.

Run the build from **PowerShell**, not Git Bash. Git Bash rewrites the quoted `-Project=` path, and UBT then can't find the project.

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" `
    SloshDemoEditor Win64 Development `
    -Project="C:\Users\Dae\Desktop\UE5 Fluid Sim\SloshDemo\SloshDemo.uproject" -WaitMutex
```

Notes:
- Target `SloshDemoEditor` builds the plugin modules (SloshCore, SloshRender, Slosh, SloshEditor) and the SloshDemo game module.
- Shader changes (`Plugins/Slosh/Shaders/**`) need no C++ build. They recompile when the editor or game starts, and the first launch after a change is slow.
- The engine is the launcher install `UE_5.8` (5.8.4).

---

## 2. Running a test

Always use the full engine path and the full project path:

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" `
    "C:\Users\Dae\Desktop\UE5 Fluid Sim\SloshDemo\SloshDemo.uproject" `
    <map> -game -windowed -ResX=1600 -ResY=900 -log <Slosh flags>
```

- `<map>` is optional. Without it the project's `GameDefaultMap` (`/Game/Startup`) loads. Use a package path such as `/Game/SloshTest/ManipulatorTest`.
- `-game` runs the game from the editor binary. Editor-only data is available, so skeletal meshes are CPU-readable and Slosh Paint skin traces work.
- The game mode is `GlobalDefaultGameMode=/Script/SloshDemo.SloshDemoGameMode` (`Config/DefaultEngine.ini`), so every map runs it unless it overrides the game mode.
- Without `-SloshAutoTest`, the demo scene runs until you close the window. The paint test always exits by itself. The skeletal test and placed scenes exit only with `-SloshAutoTest`.
- Don't pass `-nullrhi`: the solver and the screenshots need a GPU.

Example: a 96³-class benchmark with MGPCG, quit after 20 s:

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" `
    "C:\Users\Dae\Desktop\UE5 Fluid Sim\SloshDemo\SloshDemo.uproject" `
    -game -windowed -ResX=1600 -ResY=900 -SloshAutoTest -SloshAutoTestSeconds=20 -SloshGrid=96,96,72 -SloshSolver=MGPCG
```

---

## 3. Scene selection

`StartPlay` picks exactly one mode, in this order:

| Priority | Condition | Mode | What it builds |
|---|---|---|---|
| 1 | `-SloshPaintTest` | Paint test | Animated mannequin and a sphere, painted by scripted traces and a nozzle Blueprint. Two screenshots, then exit. |
| 2 | `-SloshSkelTest` | Skeletal test | Poseable skeletal mesh with fitted bone capsules, and a cylinder pouring viscous liquid onto its shoulder. |
| 3 | The loaded map already contains an `ASloshVolume` | **Placed scene** | Nothing is spawned. The first volume found is used as-is, only the surface flags are applied, and stats are logged. |
| 4 | Otherwise | Demo scene | A dam break with a floating box, a heavy sphere, a kinematic paddle and an emitter, or the calm pool with `-SloshCalm`. |

**Which flags apply where:**

| Flag group | Demo scene | Placed scene | Skeletal test | Paint test |
|---|---|---|---|---|
| `-SloshAutoTest`, `-SloshAutoTestSeconds=` | yes | yes | yes | seconds only (always exits) |
| Surface (`-SloshSurface=`, `-SloshIso=`, `-SloshFieldRes=`, `-SloshSurfaceSmoothing=`) | yes | yes | yes | — |
| Grid/solver/density/sparse/tracing/scene flags | yes | **ignored** | — | — |
| `-SloshSkel*` | — | — | yes | — |

The placed scene doesn't apply solver flags. To benchmark solver settings in your own level, edit the volume itself.

### Placed-scene mode

A level that already contains an `ASloshVolume` is treated as finished content. The game mode only:
- logs `placed scene: using <Volume> (grid X=.. Y=.. Z=.., cell N cm); demo scene not spawned`;
- applies the four surface flags to that volume;
- logs a `placed t=` stats line every second, including per-character wetness for every `ASloshSkelTestCharacter` in the level;
- with `-SloshAutoTest`, writes `Saved/Bench/Placed_A.png` … `Placed_D.png` at 3, 6, 9 and 12.5 s and exits after `max(AutoTestSeconds, 13)` s.

Committed test level: `/Game/SloshTest/ManipulatorTest` (a pool, a pouring nozzle, a whirlpool and a corner drain). Other test levels in `Content/SloshTest` may use optional local test content that isn't in the repository.

---

## 4. Command-line flags

### 4.1 Harness

| Flag | Type / default | Effect |
|---|---|---|
| `-SloshAutoTest` | switch | Demo scene: runs `ProfileGPU` at 50% of the run time, `HighResShot 1` at 75%, then exits at `AutoTestSeconds`. Placed scene: four screenshots, then exits. Skeletal test: exits at `AutoTestSeconds`, because screenshots are taken in any case. |
| `-SloshAutoTestSeconds=S` | float, 14 | Run length. Raised to at least 13 for placed and skeletal runs and at least 6 for the paint test. |

### 4.2 Demo-scene domain and solver (demo scene only)

| Flag | Sets | Default in the demo | Effect / notes |
|---|---|---|---|
| `-SloshGrid=X,Y,Z` | `GridResolution` | 64,64,48 | Also sets `CellSize = 320 / X` (the domain stays 3.2 m wide) and `ParticleCapacity = X·Y·Z·2`. It needs exactly three comma-separated integers. |
| `-SloshSolver=Auto\|RBGS\|MGPCG` | `PressureSolver` | Auto | Case-insensitive; anything else means Auto. |
| `-SloshIters=N` | `PressureIterations` | 60 | RBGS iterations. |
| `-SloshPCGIters=N` | `PCGIterations` | 3 | MGPCG outer iterations. |
| `-SloshCFL=F` | `CFL` | 2.0 | Higher is cheaper with worse coupling. CFL 4 roughly halves 128³ frame time. |
| `-SloshMaxSubsteps=N` | `MaxSubsteps` | 4 | — |
| `-SloshMaxFrameDt=S` | `MaxFrameDt` | 1/30 | Simulated seconds per frame at most (slow motion below that frame rate). |
| `-SloshMaxSpeed=cm/s` | `MaxParticleSpeed` | 2000 | — |
| `-SloshSortInterval=N` | `SortInterval` | 4 | 0 disables sorting. |
| `-SloshNoGhostFluid` | `bGhostFluid = false` | — | Classic first-order free surface (A/B testing). |
| `-SloshFaceCoupling` | `bSmoothCouplingForce = false` | — | Legacy binary-face force estimator (A/B testing). |
| `-SloshDensityCorrection=F` | `Physical.DensityCorrection` | 0.15 | Only affects the velocity correction, so it has no effect unless `-SloshVelocityDensityCorrection` is also passed. |
| `-SloshNoDensityCorrection` | `Physical.DensityCorrection = 0` | — | Same caveat. Intended to isolate pressure-solver accuracy in the divergence metric. |
| `-SloshVelocityDensityCorrection` | `bTwoSidedDensityProjection = false` | — | Use the older one-sided velocity correction. |
| `-SloshDensityDeadband=F` | `DensityDeadband` | 0.05 | Velocity correction only. |
| `-SloshDensityRelaxation=F` | `DensityRelaxation` | 0.5 | Two-sided projection. |
| `-SloshDensityInterval=N` | `DensityProjectionInterval` | 1 | 2 is validated for large scenes. |
| `-SloshDenseGrid` | `bSparseBricks = false` | — | Dense grid passes (A/B testing). |
| `-SloshSparseThreshold=F` | `SparseMaxActiveFraction` | 0.5 | — |
| `-SloshPoolBudget=F` | `SparseStorageBudget` | 1.0 | Watch `ovf=` in the log. |
| `-SloshNoAniso` | `Visual.Anisotropy = 0` | — | Round splats for A/B comparisons. The inline `Visual.Anisotropy` already defaults to 0, so this flag currently changes nothing in the demo scene. |
| `-SloshSurface=Splats\|Raymarched\|Mesh` | `SurfaceMode` | Splats | Any value starting with `Ray` means Raymarched; `Mesh` means Mesh; anything else means Splats. (Also applies to the placed scene and the skeletal test.) |
| `-SloshIso=F` | `SurfaceIsoLevel` | 0.5 | Very sensitive: try 0.45 to 0.55. (Same three modes.) |
| `-SloshFieldRes=F` | `FieldResolution` | 1.0 | (Same three modes.) |
| `-SloshSurfaceSmoothing=F` | `SurfaceSmoothing` | 2.0 | (Same three modes.) |

### 4.3 Demo-scene content and diagnostics

| Flag | Effect |
|---|---|
| `-SloshCalm` | Ground-truth scene: the whole tank filled `CalmDepth` cm deep, with no paddle, sphere or emitter, and capacity `X·Y·Z·4`. The box drops at 4 s from z = 140. Adds a `calm` log line every second. Use this scene for hydrostatic accuracy; the agitated pool is not a valid reference. |
| `-SloshCalmDepth=cm` | Calm fill depth (default 100). Only with `-SloshCalm`. |
| `-SloshPressureProbe` | Spawns a fixed, fully submerged 40 cm cube collider (kinematic, `bReportFeedbackWhenKinematic`) and logs a `probe` line comparing integrated pressure with Archimedes. |
| `-SloshBuoyancyCoupling` | Spawned bodies use `BuoyancyAndDrag` instead of `PressureAndDrag`. |
| `-SloshGentleDrop` | The floating box spawns at z = 100, just above the surface, which separates floating equilibrium from plunge dynamics (default z = 230). |
| `-SloshFloorStart` | The box spawns resting on the floor under the liquid at z = 20.5 (floor-contact lift test). This takes precedence over the calm and gentle drop heights. |
| `-SloshManipTest` | The emitter gets `VelocitySpread 12°`, `SpeedVariation 0.2`, and adds a floor drain (radius 30 at (110, 110, 15)) and a whirlpool (Swirl, radius 90, strength 600, inward pull 0.3, falloff 30 at (−40, −40, 40)). No effect with `-SloshCalm`. |
| `-SloshTraceBox` | Per-frame `trace` and `rawforce` lines for the floating box (and the probe). |
| `-SloshTraceSpeed` | Per-frame `speed` line (CFL and clamp diagnostics). |

### 4.4 Skeletal test (`-SloshSkelTest`)

| Flag | Default | Effect |
|---|---|---|
| `-SloshSkelTest` | — | Builds the skeletal scene. Screenshots `Saved/Bench/SkelTest_A.png` … `_D.png` at 3, 6, 9 and 12.5 s. Pouring stops at 9 s. Exits only with `-SloshAutoTest` (after at least 13 s). |
| `-SloshSkelMesh=/Game/Path/Asset.Asset` | A local test mesh | Skeletal mesh to use. The built-in default points at optional local test content, so on a fresh clone **pass a mesh**, or the test logs `skel test: mesh … not found` and exits. A neutral choice is any UE mannequin, such as `SKM_Manny_Simple`. |
| `-SloshSkelDebug` | off | `bDrawCapsules` on the fitted colliders. |
| `-SloshSkelViscosity=m²/s` | 0.01 | Inline `KinematicViscosity` (the liquid is otherwise honey-like: 1400 kg/m³, friction 0.9). Ignored when a preset is used. |
| `-SloshSkelPreset=Name` | none | Uses `/Slosh/Paint/Presets/Regular/DA_SloshPaint_<Name>` as the volume `Material`, for example `Honey`. The preset replaces the inline physics and look. |

The scene:
- Domain: the mesh bounds plus the nozzle, padded 30/30/15 cm, at 2.5 cm cells with each axis rounded up to a multiple of 8, floor at z = 0, capacity 2²⁰.
- Nozzle: radius 3, 2500 particles/s, speed 60 cm/s, aimed mostly downward.
- Motion: the left upper arm swings 22° with a 5 s period, and the body sways ±6°.

### 4.5 Paint test (`-SloshPaintTest`)

It spawns an animated mannequin (optional local test content) and a sphere, then paints five scripted strokes with three presets through `USloshPaintLibrary::TracePaintSurface` (`UnitScale 6`). It sweeps `BP_SloshNozzle` across the sphere for 2.5 s, writes `Saved/Bench/PaintTest_A.png` at 3.5 s and `PaintTest_B.png` at 4.7 s, logs every visible primitive (to check bone attachment), and exits after `AutoTestSeconds` (default 14 s, at least 6 s), with or without `-SloshAutoTest`. A missing mesh is logged as `character MISSING`; the sphere strokes still run.

---

## 5. Reading the logs

**Where:** `SloshDemo/Saved/Logs/SloshDemo.log`. The previous run is renamed `SloshDemo-backup-<date>.log`, and a second concurrent instance writes `SloshDemo_2.log`. All demo lines use the category `LogSloshDemo`. Capsule fitting logs under `LogSloshSkeletalCollider`.

### 5.1 Startup line (demo scene)

```
Slosh demo: grid X=64 Y=64 Z=48, cell 5.00 cm, capacity 262144, CFL 2.0, sort every 4, solver Auto (RBGS iters 60, PCG iters 3)
```

### 5.2 Per-second stats line (demo scene)

```
t=5.0s frame=7.98ms (125.3 fps) gpu=7.59ms particles=236304 drained=0 substeps=1 maxSpeed=597cm/s liquid=3894.4L rho=0.9978 pvol=3700.4L active=0.77 pool=0.77 ovf=0 mem=27+25MB clamp=0 latency=3 div(mean/max)=0.000/0.00 1/s | box z=35.8 sub=0.95 F=V(X=13741.08,Y=1606.67,Z=34042.50) | sphere z=24.5 sub=0.88
```

| Field | Source | Meaning | Healthy |
|---|---|---|---|
| `t` | elapsed | Seconds since play started. | — |
| `frame` / `fps` | game-thread delta, averaged over the last second | Whole-frame time, including rendering. | Below 16.7 ms for 60 FPS. |
| `gpu` | `RHIGetGPUFrameCycles` | GPU frame time, averaged. | — |
| `particles` | `GetParticleCount()` | Live GPU particles *(delayed)*. Drained particles leave at the next compaction. | Stops growing at `ParticleCapacity`. |
| `drained` | `GetDrainedParticleCount()` | Cumulative drained particles. | live + drained = emitted. |
| `substeps` | `GetLastSubstepCount()` | CFL substeps used last frame. | 1 to 2. Pinned at `MaxSubsteps` means it is unresolved. |
| `maxSpeed` | `GetMaxParticleSpeed()` | Fastest particle, cm/s. | Below `MaxParticleSpeed`. |
| `liquid` | `GetLiquidVolumeLiters()` | Fluid cells × cell volume. Over-estimates during splashes. | — |
| `rho` | `GetMeanInteriorDensity()` | Mean relative interior density (1 = rest). | 0.98 to 1.02. |
| `pvol` | `GetParticleVolumeLiters()` | Volume implied by particle count and `rho`. The steadier volume metric. | Constant without emitters or drains (drift about 0%/min). |
| `active` | `GetActiveBrickFraction()` | Fraction of all 8³ bricks holding storage. | — |
| `pool` | `GetPoolUsage()` | Allocated bricks / pool capacity. Equals `active` at budget 1.0. | Below 1.0 with headroom. |
| `ovf` | `GetPoolOverflowBricks()` | Bricks that found the pool full. | **Must be 0.** Otherwise raise `SparseStorageBudget` (`-SloshPoolBudget=`). |
| `mem` | `GetGridMemoryMB()` + `GetParticleMemoryMB()` | GPU MiB: grid, pool and solver buffers + allocated particle slots. | — |
| `clamp` | `GetClampedParticleCount()` | Particle updates that hit `MaxParticleSpeed` last frame. | 0. Persistent non-zero means the step is unresolved. |
| `latency` | `GetFeedbackLatencyFrames()` | Frames between submission and readback. | 2 to 4 (usually 3). |
| `div(mean/max)` | `GetMeanDivergence()` / `GetMaxDivergence()` | Post-projection divergence, 1/s. | Mean about 0.00x. A spike to large values signals instability. |
| `box z / sub / F` | floating box (spawns at 4 s) | Height (cm), submerged fraction, last applied fluid force (kg·cm/s²). `z=0.0 sub=0.00 F=-` before it spawns. | Settles near its Archimedes height. |
| `sphere z / sub` | 140 kg heavy sphere | Height and submerged fraction. | Rests on the floor. |

### 5.3 Other demo lines

| Prefix | When | Fields |
|---|---|---|
| `probe t= F=(x, y, z) N archimedes(voxel)= ratio= mean(t>8s) Fz/archimedes= lateral=` | `-SloshPressureProbe` | Integrated pressure force on the fixed probe; Archimedes force from its voxelised volume; this second's ratio; mean ratio after 8 s; mean lateral force. Calm target: ratio about 0.98 to 1.0. |
| `calm t= surface(est)= expectedBoxZ= boxZ=` | `-SloshCalm` | Surface height from the particle count at rest density, the box's expected equilibrium (density ratio 0.547, 40 cm cube), and its actual height. |
| `trace t= x= y= z= vz= sub= in= age= F= vf=` | `-SloshTraceBox` | Per-frame box state: position, vertical velocity, submerged fraction, `IsInLiquid`, feedback age, applied force, local fluid velocity. |
| `rawforce t= box=fx,fy,fz probe=fx,fy,fz` | `-SloshTraceBox` | Unsmoothed pressure force in N, for jitter statistics. |
| `speed t= max= substeps= clamped= inSolid= at=(x,y,z)m div= box z=` | `-SloshTraceSpeed` | Max speed, substeps, clamped updates, how many started inside a solid cell, the clamped particles' centroid (sim metres), and max divergence. |

### 5.4 Skeletal and placed lines

```
skel t=4.0s frame=..ms gpu=..ms particles=.. substeps=.. maxSpeed=..cm/s liquid=..L clamp=.. | wet colliders 7/31: upperarm_l:0.42 ...
placed t=2.0s frame=9.09ms gpu=7.97ms particles=191816 drained=66456 substeps=1 maxSpeed=659cm/s liquid=3314.75L clamp=0 | TestSubject wet 3/31: ...
```

`wet colliders N/M` lists every fitted bone collider with a non-zero submerged fraction as `bone:fraction` *(delayed)*. The startup line `skel test: mesh …, N colliders, …, grid … domain …` and the `LogSloshSkeletalCollider` line, which gives every fitted shape as `bone(cap|box verts, r=, h=)`, show what was fitted.

---

## 6. Output files and benchmarks

| Output | Path (under `SloshDemo/Saved`) | Written by |
|---|---|---|
| Log | `Logs/SloshDemo.log` | Every run. |
| Demo screenshot | `Screenshots/WindowsEditor/HighresScreenshotNNNNN.png` | `-SloshAutoTest` demo scene (`HighResShot 1` at 75%). |
| GPU profile | in the log (`ProfileGPU` dump) | `-SloshAutoTest` demo scene at 50%. |
| Skeletal test | `Bench/SkelTest_A.png` … `_D.png` | `-SloshSkelTest`. |
| Placed scene | `Bench/Placed_A.png` … `_D.png` | Placed level with `-SloshAutoTest`. |
| Paint test | `Bench/PaintTest_A.png`, `_B.png` | `-SloshPaintTest`. |
| Benchmark archive | `Bench/*.log`, `Bench/*.png` | Copied there by hand after a run, for example `128x128x96_it60.log` or `ab_new_calm_c.log`. Name copies `<grid>_<variant>.log` so A/B pairs sort together. |

Each test overwrites its `Bench` images, so copy anything you want to keep before rerunning.

### Benchmark recipes

| Question | Command flags | Read |
|---|---|---|
| Cost vs resolution | `-SloshAutoTest -SloshGrid=64,64,48`, then `96,96,72`, then `128,128,96` | `frame`, `gpu`, `mem`, and the `ProfileGPU` dump |
| Solver crossover | `-SloshSolver=RBGS` vs `-SloshSolver=MGPCG` at each grid | `gpu`, `div` |
| CFL trade-off | `-SloshCFL=2` vs `-SloshCFL=4` | `gpu` vs the box `z` and `sub` stability |
| Hydrostatic accuracy | `-SloshCalm -SloshPressureProbe -SloshAutoTestSeconds=20` | `probe … mean Fz/archimedes`, `calm … boxZ` vs `expectedBoxZ` |
| Volume drift | `-SloshCalm -SloshAutoTestSeconds=300` (or the agitated default) | `pvol` slope, `rho` |
| Sparse storage sizing | `-SloshPoolBudget=0.3` (lower until `ovf` > 0 at the splash peak) | `pool`, `ovf`, `mem` |
| Coupling jitter | `-SloshTraceBox` | Standard deviation of the `rawforce` box components after settling |
| Fast-flow robustness | `-SloshGrid=256,256,128 -SloshCFL=4 -SloshTraceSpeed` | `clamp`/`clamped`, `substeps` |

Reference measurements (RTX 5060 Laptop, UE 5.8.4). Use them only as rough guides; single runs vary.

| Scene | Result |
|---|---|
| 64³-class sim | about 1.4 ms simulation |
| 96³ | about 5 to 6.4 ms frame |
| 128³ | about 6.2 ms at CFL 4, about 16 ms at CFL 2 |
| 128×128×96 shallow pool | grid memory 226 → 76 MiB with sparse storage |
| 9.8M particles, 256×256×128 calm | about 35 ms |
| Calm pool | box within about 0.1 cm of Archimedes; probe Fz/ρgV ≈ 0.98; drift about 0.000 %/min |

Coupling metrics vary by about ±0.15 run to run (chaotic flow), and the calm box bob standard deviation ranges from 0.8 to 3.3 cm. Compare averages of several runs, not single runs.

---

## 7. Optional: authoring editor content through the Unreal MCP server

Most editor content was built by scripts driving the running editor, rather than by hand: the Slosh Paint presets, master materials and Blueprints, the test levels, and the preset simulation values. You don't need this workflow; everything can be edited normally in the editor.

- The project enables the `ModelContextProtocol` and `AllToolsets` plugins (`SloshDemo.uproject`). With the editor open, an MCP server listens at `http://127.0.0.1:8000/mcp`.
- Scripts are sent as a single call to `ProgrammaticToolset.execute_tool_script` (toolset `editor_toolset.toolsets.programmatic.ProgrammaticToolset`). The script runs inside the editor and calls other tools through `execute_tool(<tool name>, <json args>)`. Tool names include:
  - `editor_toolset.toolsets.scene.SceneTools.find_actors` / `load_level` / `add_to_scene_from_class`
  - `editor_toolset.toolsets.object.ObjectTools.get_properties` / `set_properties`
  - `editor_toolset.toolsets.actor.ActorTools.get_components` / `set_actor_transform`
  - `editor_toolset.toolsets.asset.AssetTools.find_assets` / `save_assets`
  - `editor_toolset.toolsets.material_instance.MaterialInstanceTools.get_scalar_parameter` / `get_vector_parameter`

Example script, which sets a placed volume to a preset and saves the level:

```python
import json
O  = "editor_toolset.toolsets.object.ObjectTools."
AS = "editor_toolset.toolsets.asset.AssetTools."
def T(tool, **kw): return execute_tool(tool, json.dumps(kw))
def run():
    # Object path (not the label) of the volume; check the real name in the World Outliner or with get_components.
    vol = {"refPath": "/Game/SloshTest/ManipulatorTest.ManipulatorTest:PersistentLevel.SloshVolume_0"}
    T(O + "set_properties", instance=vol, values=json.dumps({
        "Material": {"refPath": "/Slosh/Paint/Presets/Regular/DA_SloshPaint_Honey.DA_SloshPaint_Honey"},
        "SurfaceMode": "Raymarched"}))
    return T(AS + "save_assets", asset_paths=["/Game/SloshTest/ManipulatorTest"])
```

MCP gotchas:
- `find_actors(name=…)` matches actor **labels**, not object names. Address actors by their `PersistentLevel` object path instead.
- `BlueprintTools.create` takes the parent class as `asset_type`.
- The editor must not be in a Live Coding compile while scripts run. Close it before a C++ build (§1).
