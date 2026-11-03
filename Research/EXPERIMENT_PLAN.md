# EXPERIMENT_PLAN.md

Every experiment lists its setup, its measurements, pass/fail criteria (fixed **before** running), and its
current status. Hardware for all runs: NVIDIA GeForce RTX 5060 Laptop GPU (8 GB, driver 617.42, reported by
`nvidia-smi`), Windows 11, UE 5.8.4 (CL 58902052), DX12, 1280×720 windowed. The brief also names a
Quadro RTX 4000 + i9-10900; nothing has been run on it yet.

Harness (built): `SloshDemo` project → `ASloshDemoGameMode`. Command line:
```
UnrealEditor.exe SloshDemo.uproject -game -windowed -ResX=1280 -ResY=720 -SloshAutoTest
    [-SloshAutoTestSeconds=S] [-SloshGrid=X,Y,Z] [-SloshIters=N]
```
The harness logs once per second: frame ms, GPU ms (`RHIGetGPUFrameCycles`), particles, substeps, max speed,
liquid litres, feedback latency, post-projection divergence, and body height/submersion/force. At S/2 it runs
`ProfileGPU` (per-pass GPU timings to the log), at 3S/4 it takes `HighResShot 1`, and at S it exits.
Bench logs go to `SloshDemo/Saved/Bench/*.log`.

---

## E1 — Standalone FLIP Fluids baseline (reference, CPU)
- **Goal:** run the MIT engine outside Blender to get reference numbers and reference trajectories.
- **Steps:** use the prebuilt MinGW `build/bl_flip_fluids/.../libffengine.dll` through its C ABI from a
  small MSVC host (`FluidSimulation_new_from_dimensions`, `_initialize`, `_add_mesh_obstacle`,
  `_add_mesh_fluid`, `_update(dt)`, `_get_marker_particle_position_data`, `_get_frame_stats_data`). See
  FLIP_FLUIDS_FEASIBILITY.md §5 for the exact sequence.
- **Measure:** seconds per frame at 64×64×48 / 8 ppc, particle count, pressure iterations, and the stats blob's timings.
- **Pass:** the DLL loads, 60 frames simulate, and particle positions are retrievable. **Fail:** crash at the
  C boundary, or the build needs Blender/Python.
- **Status:** not run (planned).

## E2 — Dam break (incompressibility, stability, cost)
- **Setup:** default demo: 64×64×48 cells at 5 cm, a 16×62×26-cell column (206k particles), an emitter, 60 RBGS iterations.
- **Measure:** post-projection mean/max divergence (1/s), fluid-cell volume vs particle-implied volume,
  substeps, per-pass GPU time, and stability over 20 s.
- **Pass:** no NaN or blow-up for 20 s; mean |∇·u| < 0.5 s⁻¹ in steady state; total sim GPU < 3 ms.
  Then repeat at 20 and 150 iterations and check that divergence decreases monotonically with iterations.
- **Status:** run. **PASS:** 20 s stable at 20/60/150 iterations; mean divergence 0.28 → 0.18 → 0.12 s⁻¹
  (monotonic); sim 2.25–2.72 ms. Volume metric (fluid-cell count) over-estimates during splashes; a
  particle-count-based volume metric is needed for drift measurement (open).

## E2b — Long-run volume drift (new)
- **Setup:** the calm and agitated scenes, 300 s each, default settings.
- **Measure:** mean interior density and the particle-implied volume (GPU statistic), plus the calm-pool box error over time.
- **Pass:** |drift| < 0.1 %/min.
- **Status:** run. **PASS**: calm +0.000 %/min, agitated −0.007 %/min; calm box error −0.2 cm with no trend (PHYSICS_COUPLING Addendum 7).

## E3 — Floating rigid body (two-way coupling)
- **Setup:** a 40 cm cube, 35 kg (ρ_b ≈ 547 kg/m³, expected draft ≈ 0.55 × 40 = 22 cm), dropped into
  settled water at t = 4 s; a 25 cm sphere at 140 kg (ρ_b ≈ 2140, must sink); BuoyancyAndDrag mode.
- **Measure:** equilibrium submerged fraction vs Archimedes (0.547 expected), settling time, height
  oscillation amplitude, and feedback latency (frames).
- **Pass:** the box floats and settles to a submerged fraction of 0.55 ± 0.15 within 5 s, the sphere rests on the
  bottom, and nothing is ejected from the domain.
- **Status:** run (6 iterations; traces in `SloshDemo/Saved/Logs` and `Bench/coupling_pressure.log`). **Result: PARTIAL, fails on settling.**
  - The sphere sinks and rests on the bottom with submerged fraction 0.85–0.93 in every run. PASS.
  - The box floats and is buoyed out of the water. It no longer leaves the domain once tank walls exist.
    But it enters a sustained limit cycle (≈2.4 s period, z 25–160 cm, equilibrium ≈ 73 cm) and never
    settles. FAIL against the 5 s settling criterion.
  - Root causes found and fixed along the way, each confirmed by a trace:
    (1) solid-interior MAC faces carried the body velocity, giving spurious no-slip that dragged liquid along;
    (2) the submersion walk included +Z, so a puddle on top counted as submersion (positive feedback);
    (3) thin sheets and airborne lumps counted as liquid, now filtered by a dense and supported test;
    (4) extrapolation seeded the solid interior from solid boundary faces.
  - Remaining causes: 3-frame force latency combined with liquid genuinely lifted by the fast-rising body;
    the box plunges to the floor on each re-entry.
  - **Phase 2 rerun (2026-11-03): PASS.** Two demo bugs had invalidated earlier runs: the box plunged to the
    domain floor, and the "invisible tank walls" had no collision because runtime-spawned Static-mobility
    components reject `SetStaticMesh`. With the box placed gently and the walls fixed, **PressureAndDrag**
    settles it: for t ≥ 12 s, mean z = 88 cm, SD 7.6 cm, mean submerged fraction **0.564 vs 0.547 Archimedes
    (+3%)** (`Bench/final_default_64.log`). BuoyancyAndDrag still bobs (SD 47.9 cm). PressureAndDrag is now the default.
  - **E3e — calm-pool ground truth (new, `-SloshCalm`):** a still 100 cm pool whose surface follows from particle volume.
    With the one-sided velocity correction, the box floated at +4.4 cm (−1.2 cm with correction off). With the
    two-sided density projection (now the default) the box error is **−0.0 cm** (SD 0.9 cm), and the probe reads
    Fz/ρgV = 0.985. **PASS** for hydrostatic accuracy (PHYSICS_COUPLING Addendum 5). Earlier probe numbers from
    the agitated pool are superseded: the probe was not fully submerged there.
  - **E3d — force noise (new):** frame-to-frame jitter of the raw pressure force on the fixed probe and the moving box.
    The smooth density correction halves probe jitter (435 → 212 N lateral). The box stays at the ~100 N flow-noise floor.
    See PHYSICS_COUPLING Addendum 4.
  - **E3c — floor-contact lift (new):** with `-SloshFloorStart` the box starts resting on the floor under the liquid.
    Before the seepage fix it stayed pinned. After it, it lifts within 0.27 s and floats. **PASS** (PHYSICS_COUPLING Addendum 3).
  - **E3b — pressure probe (new):** a fixed, fully submerged kinematic box. Time-averaged Fz / ρgV = 0.92–1.06 for
    RBGS and MGPCG at 64³ and 128³ (PERFORMANCE_PLAN §7). **PASS.**
  - **Superseded Phase 1 finding:** "PressureAndDrag mode: the box sinks to the floor and stays there." The pressure integral is net *downward*
    (−230…−450 N) once no fluid cells lie beneath the body, and laterally noisy (±400 N). This confirms
    R-05: the binary-solid, RBGS pressure field cannot yet provide physical buoyancy.
- **Follow-up:** repeat in PressureAndDrag mode and compare equilibrium with BuoyancyAndDrag. This directly
  tests pressure-solve convergence (R-05).

## E4 — Moving container
- **Setup [needs `USloshContainerComponent`, Design]:** a hollow box SDF (difference of two boxes) driven
  kinematically: translate at 1 m/s, rotate 45° over 1 s, then hold.
- **Measure:** particles inside the solid after each step (penetration count), retained volume, the energy
  trace (kinetic energy readback), and cost.
- **Pass:** penetration below 0.1% of particles per frame, and no particle tunnelling at 2 m/s wall speed with ≤ 4 substeps.
- **Status:** the moving-obstacle path exists (kinematic paddle in the demo, finite-difference velocity); no container primitive yet.

## E5 — Variable liquid properties
- **Setup:** a column collapse with the three LIQUID_MATERIALS presets.
- **Measure:** front position x(t), final spread, time to rest, and viscosity pass cost (K = 12, 40).
- **Pass:** the ordering water > blood > slime holds for front speed; slime holds a mound ≥ 50% of the initial height
  after 3 s (expected to **fail** with Newtonian viscosity only, which shows the need for a rheology model); no instability.
- **Status:** not run.

## E6 — GPU scaling
- **Setup:** identical scene scaled by cell size (domain 3.2 m wide): grids 64×64×48, 96×96×72 and 128×128×96,
  with particle capacity 2 × cells (the brief's 10k/25k/50k/100k/200k particle steps come from emitter-only scenes).
- **Measure:** per-pass GPU ms (P2G, pressure, G2P, render splat), total frame ms, and VRAM (allocation sizes).
- **Pass criteria (set before running):** 64³-class grid at ≥ 200k particles: sim + render ≤ 4 ms. 128³-class
  grid: ≤ 8 ms (Est) and identify the dominant pass.
- **Status:** run (PERFORMANCE_PLAN §4, §8). After the particle optimisation (spatial sort, trimmed APIC gather):
  64³ **PASS** at 2.35 ms. 96³ **PASS** at ≈ 6.4 ms with CFL 2 and ≈ 5.0 ms with CFL 4 (Est from frame GPU).
  128³: ≈ 30 ms (CFL 2) and ≈ 13.7 ms (CFL 4) after round 1. After round 2 (tiled P2G, conditional clear, §9):
  **≈ 16.3 ms at CFL 2, and PASS at ≈ 6.2 ms with CFL 4.**
  VRAM (sparse storage, PERFORMANCE_PLAN §10c): grid buffers for the 128×128×96 shallow pool are 213 MiB at budget 1.0
  and 76 MiB at 0.35 (dense ≈ 226). 256×256×128 now fits in 351 MiB (dense ≈ 1.2 GiB): 18–19 ms calm; after the advection fix
  (R-21) 20–31 ms once the impact settles.

## E6b — Rendering scaling
Close-up camera (fluid filling the screen) at 1080p/1440p; measure splat cost against MaxSplatPixels. Not run.

## Smallest experiment that could invalidate the architecture
**E3 in PressureAndDrag mode at 128³ with 60 RBGS iterations.** If the pressure field cannot hold a light
body at the Archimedes draft within ±25% (i.e. hydrostatic pressure is not converged) and MGPCG cannot fix
it inside a 2 ms budget, then the grid solver cannot deliver physical two-way coupling in real time. The
fallback would then be analytic buoyancy (current default) or a PBF solver (SOLVER_COMPARISON fallback).
