# Showcase levels

Eight levels under `SloshDemo/Content/SloshShowcase/` together exercise every Slosh feature. They only use engine
content and Slosh's own assets, so they open on any machine with the repository. Each level has a title and a short
caption floating in the scene, and every important actor has a readable label in the Outliner. Select it and change its
settings while the level plays: most physical and visual settings apply live; grid resolution and cell size need a restart.

Open a level and press **Play**. Each level holds a `ASloshVolume` (or a `SloshShowcase`-tagged actor), so the default
`ASloshDemoGameMode` uses it as-is: no generated demo scene, one statistics line per second in `Saved/Logs/SloshDemo.log`.
To capture screenshots and exit automatically, run it from a terminal with `-SloshAutoTest` ([Testing.md](Testing.md)):

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" "<repo>\SloshDemo\SloshDemo.uproject" /Game/SloshShowcase/SH_04_Manipulators -game -SloshAutoTest
```

Frame times below are whole-frame GPU times measured on the development GPU (RTX 5060 Laptop) at 1280×720, about 7 s
into each level.

| Level | Shows | GPU frame |
|---|---|---|
| [SH_01_DamBreak](#sh_01_dambreak) | The solver on its own: a dam break against an obstacle | ~6.6 ms |
| [SH_02_Coupling](#sh_02_coupling) | Two-way rigid-body coupling: floating, sinking, a kinematic paddle | ~7.7 ms |
| [SH_03_Emitters](#sh_03_emitters) | Every emitter control | ~7.1 ms |
| [SH_04_Manipulators](#sh_04_manipulators) | Force fields of every type and a drain | ~7.5 ms |
| [SH_05_Materials](#sh_05_materials) | The same pour with three liquids | ~8.1 ms |
| [SH_06_SurfaceModes](#sh_06_surfacemodes) | The same pour drawn three ways | ~8.1 ms |
| [SH_07_SkeletalCollision](#sh_07_skeletalcollision) | Liquid on an animated skeletal mesh | ~6.3 ms |
| [SH_08_SloshPaint](#sh_08_sloshpaint) | Slosh Paint strokes (not simulated) | – |

The test level `Content/SloshTest/ManipulatorTest` is a compact version of SH_04 (pool, nozzle, whirlpool, drain).

---

## SH_01_DamBreak

**What happens.** A 1 m wide block of water at one end of a 3.2 × 3.2 × 2.4 m tank collapses, runs into a long static
box and sloshes back and forth until it settles.

**Actors.**

| Actor | Settings |
|---|---|
| `SloshVolume` | `GridResolution` 64×64×48, `CellSize` 5 cm, `ParticleCapacity` 600k, `InitialFillBox` (−155,−155,−115)→(−55,155,25) cm local. Inline water material. |
| `Obstacle` | `ASloshShowcaseBody`, box 30×160×60 cm, `OneWay` (the liquid sees it; it does not move). |

**Try.**
- `PressureSolver`: `RBGS` vs `MGPCG` (watch the free surface stay flatter with MGPCG at depth).
- `Physical.KinematicViscosity` 0.01 → a slow, syrupy collapse.
- `Physical.Transfer`: `FLIP` with `FlipRatio` 0.95 → livelier, noisier splashes.
- `CFL`, `MaxSubsteps` → cost against stability.
- `CellSize` 4 with a larger `GridResolution` → finer detail (restart needed).

See [Simulation.md](Simulation.md) for what each of these does inside the solver.

## SH_02_Coupling

**What happens.** A 80 cm deep pool. Two boxes and a light ball float and bob, a 140 kg ball sinks to the floor,
and a paddle driven back and forth by a `ASloshShowcaseMover` pushes waves through the pool that rock the floating bodies.

**Actors.**

| Actor | Settings |
|---|---|
| `FloatingBox_Pressure` | 40 cm box, 35 kg, `PressureAndDrag`: floats from the integrated solver pressure. |
| `FloatingBox_Buoyancy` | 50×30×30 cm box, 20 kg, `BuoyancyAndDrag`: Archimedes force from the submerged-volume estimate. |
| `LightBall` / `HeavyBall` | 40 cm / 8 kg (floats), 50 cm / 140 kg (sinks). |
| `Paddle_Kinematic` + `PaddleMover` | 20×140×90 cm box, `OneWay`; oscillates ±35 cm along X every 3 s. |

**Try.** Compare the two coupling modes side by side (PressureAndDrag settles more steadily; BuoyancyAndDrag never sticks
to the floor). Raise `MassKg` on a box until it sinks (the boxes have a 547 kg/m³ density at 35 kg/40 cm). Increase
the mover `Amplitude` or shorten its `Period` for bigger waves. Coupling details: [Simulation.md §8](Simulation.md).

## SH_03_Emitters

**What happens.** A shallow pool fed by five different emitters, with two floor drains keeping the level from rising.

| Actor | What it demonstrates |
|---|---|
| `Jet` | Straight jet: sphere emitter, 4000 particles/s, 320 cm/s. |
| `Fountain_Spread` | Points up: `VelocitySpread` 15°, `SpeedVariation` 0.3, a fountain that breaks into droplets. |
| `Sheet_BoxLitres` | Box emitter 8×100×6 cm oriented with the nozzle, `RateMode` `LitersPerSecond` 2: a wide sheet / waterfall. |
| `Pulse_Bursts` | `bContinuous` off, `BurstInterval` 1.5 s, `BurstParticles` 6000: discrete pulses through `Burst()`. |
| `Moving_Hose` + `HoseMover` | Sweeps ±120 cm; `bInheritVelocity` throws the hose's own motion into the stream. |
| `Drain_A`, `Drain_B` | 25 cm floor drains (instant removal). |

**Try.** Switch `bInheritVelocity` off on the hose and compare the stream shape. Set `MaxParticles` on the jet to make
it stop by itself. In Blueprint, call `Burst` or `BurstLiters` on any emitter. Reference: [Reference.md §7](Reference.md).

## SH_04_Manipulators

**What happens.** A 4.8 m long pool split into four zones, one force field each, plus a slow drain and a nozzle that
replaces what drains. Labels in front of the pool name the zones.

| Actor | Type | Settings |
|---|---|---|
| `Current_Directional` | Directional | Box 110×280×60 cm, 400 cm/s² along its +X (rotated to push across the pool). |
| `Fountain_Radial` | Radial | 45 cm sphere on the floor, 1500 cm/s² outward: a bulge that pushes liquid up and away. |
| `Whirlpool_Swirl` | Swirl | 70 cm sphere, 600 cm/s² around +Z, `SwirlInwardPull` 0.3: a vortex with a hollow centre. |
| `CalmZone_Drag` | Drag | Box 90×280×120 cm, 3/s: waves die out inside. |
| `SlowDrain` | Drain | 25 cm sphere, `RemovalRate` 2/s (a slow plughole rather than instant removal). |
| `Feed` | Nozzle | 6000 particles/s. |

The cyan (force field) and red (drain) shapes draw in the editor viewport and while playing (`bDrawShape`).

**Try.** Give the radial field a negative strength (it gathers liquid), set the drain `RemovalRate` to 0 (instant),
change `Shape` to `Everywhere` on the drag field (whole pool), raise `Falloff` for softer edges. Maths and limits
(32 manipulators per volume, compaction every ≤ 8 frames): [Simulation.md §7](Simulation.md).

## SH_05_Materials

**What happens.** Three separate volumes, each pouring 5 L/s onto a static ball. Same geometry, different liquid:

| Station | Volume `Material` | What to notice |
|---|---|---|
| Water | none (inline defaults) | Runs off instantly, classic water render. |
| Honey | `DA_SloshPaint_Honey` | Viscous (0.03 m²/s), adhesion 1, friction 0.95: coats the ball and piles on the floor; viscous render look. |
| Slime | `DA_SloshPaint_Slime` | Thick (0.03 m²/s) and very sticky (adhesion 1), with the preset's own colour. |

Every Fluid Painter preset is a Slosh liquid material, so try other presets from `/Slosh/Paint/Presets/Regular`
(Ketchup, Mayo, Milk, Chocolate...). Open a preset to edit its `Physical` and `Visual` sections live. Tuning guide:
[Materials.md](Materials.md).

Each station is its own volume (40×40×48 cells at 4 cm). Volumes are independent simulations; emitters feed the volume
that contains them.

## SH_06_SurfaceModes

**What happens.** The same honey pour three times, with the volume's `SurfaceMode` set to each option, left to right:

| Station | `SurfaceMode` | How it is drawn |
|---|---|---|
| Splats | `Splats` | Screen-space ellipsoid splats + adaptive smoothing (the preset's render settings). |
| Raymarched | `Raymarched` | A particle surface field ray-marched per pixel; Slosh liquid shading. |
| Mesh | `Mesh` | Surface nets on the GPU every frame, drawn by `USloshSurfaceMeshComponent` with the preset's own UE material (engine lighting and shadows). |

**Try.** On the Raymarched and Mesh volumes change `SurfaceIsoLevel` in small steps (0.45–0.55; it is sensitive),
`SurfaceSmoothing` (2–3) and `FieldResolution` (2 = finer, 8× the field cost). Set `SurfaceMaterial` on the Mesh volume
to any UE material. Pipeline details: [Rendering.md](Rendering.md).

## SH_07_SkeletalCollision

**What happens.** The engine's tutorial mannequin
(`/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP`) stands in the T pose inside a small volume. Two
nozzles pour honey on its shoulders; the left upper arm swings up and down and the whole body sways, and the liquid
flows over the moving arm.

| Actor | Settings |
|---|---|
| `Mannequin` | `ASloshSkelTestCharacter` (poseable mesh + `USloshSkeletalColliderComponent`). `SwingBone` `upperarm_l`, `SwingDegrees` −25. |
| `SloshVolume` | 60×36×56 cells at 4 cm, Honey preset, `SurfaceMode` Raymarched. |
| `Pour_L`, `Pour_R` | 3 L/s each. |

The skeletal collider fits about 40 capsules and boxes, one per significant bone, from the mesh's bind-pose vertices at
BeginPlay. The play log lists which bones are wet and how much (`wet n/40: bone:fraction`).

**Try.** Select `Mannequin` → `SloshCollider` and enable **Draw Capsules** to see the fitted shapes. Change `MaxCapsules`,
`RadiusScale` or `ExcludeBones` (the colliders are rebuilt at BeginPlay). Swap `SkeletalMesh` for your own character; its
LOD0 must be CPU-readable. Reference: [Reference.md §6](Reference.md).

## SH_08_SloshPaint

**What happens.** No simulation. Two `BP_SloshNozzle` actors auto-paint as `ASloshShowcaseMover`s sweep them: honey
draws a vertical line down the sphere and ketchup a horizontal line across the cube. The strokes are meshed on a worker
thread and shaded with each preset's material. `MousePainter` (`BP_SloshPainter`) lets you paint on any surface with
the mouse.

| Actor | Settings |
|---|---|
| `AutoNozzle_Honey`, `AutoNozzle_Ketchup` | `bAutoPaint`, one preset each, brush `Pressure` 4 (thicker beads). |
| `NozzleSweep`, `NozzleSweep2` | Vertical ±35 cm / horizontal ±30 cm sweeps. |
| `MousePainter` | `bMouseInput`: cursor painting, mouse wheel for pressure, keys to switch presets and undo ([SloshPaint.md §8](SloshPaint.md)). |

The level is tagged `SloshShowcase` (on its title text), so the demo game mode does not generate its test scene here.

**Try.** Change the nozzles' `Presets`, or raise the brush `Pressure` for fatter strokes. Edit a preset's `Thickness`,
`Flatness`, `DropMode` or layers and watch existing strokes rebuild. Everything else is in [SloshPaint.md](SloshPaint.md).

---

## Rebuilding the showcase

The levels were generated by scripts, through the editor MCP server and then headless with
`UnrealEditor-Cmd.exe -run=pythonscript`, from copies of `/Game/Startup` (sky, lights, floor). The helper actors live
in the demo module (`SloshShowcaseActors.h`: `ASloshShowcaseMover`, `ASloshShowcaseBody`; `SloshSkelTestActors.h`:
`ASloshPourNozzle`, `ASloshSkelTestCharacter`). The levels are ordinary assets, so edit them directly in the editor.
