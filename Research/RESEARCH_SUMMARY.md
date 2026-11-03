# RESEARCH_SUMMARY.md — UE 5.8 Interactive Liquid Simulation

**Date:** 2026-11-03 · **Reference source:** Blender-FLIP-Fluids @ `70a0e954018fe39e1f9c3631264989569752bb7a`
(engine 1.8.8) · **Engine:** UE 5.8.4 (CL 58902052, launcher build) · **Measured hardware:** RTX 5060 Laptop 8 GB.

## Executive summary

The Phase 0 research covers the 16 deliverables in this folder. A Phase 1 prototype, built after
explicit approval, sits in `SloshDemo/Plugins/Slosh`. It compiles and runs, and was measured.

- **FLIP Fluids** is an excellent **algorithmic reference** and a poor runtime dependency. The engine is MIT and
  readable, with FLIP and APIC both implemented. It is built only with MinGW, designed offline, and solves pressure
  with serial double-precision MIC(0)-PCG. It has **no fluid→solid force at all**: its coupling is one-way.
- A **GPU-native FLIP/APIC** on a dense MAC grid runs a 64×64×48 domain with **262k particles in
  2.25–2.72 ms of simulation plus 0.68 ms of rendering** (measured, ProfileGPU). That is inside the 4 ms budget at about 130–150 FPS overall.
- It **does not scale to 128³ yet**: about 59 ms at 1.7M particles with 4 CFL substeps. Particle passes dominate.
- **Coupling (updated in Phase 2):**
  - With the pressure-integral mode, a light body floats and settles within +3% of its Archimedes draft.
  - A fixed probe confirms the solver's hydrostatic pressure to within ±8%.
  - Bodies resting on the floor now lift off, through seepage pressure on contact faces (PHYSICS_COUPLING Addendum 3).
  - Force noise was traced to the density correction's integer particle counts. A smooth P2G-weight density
    halves the probe force jitter, holds volume drift to −1.9% per 12 s, and puts the floating box at −3% of its draft.
  - The remaining ~100 N lateral jitter is the flow-noise floor of the agitated pool.
  - A **calm-pool ground truth** (`-SloshCalm`) plus a **ghost-fluid free surface** and surface-extrapolated coupling pressure
    put the submerged probe force within −1.6% of hydrostatic, and the floating box at +4.4 cm of its Archimedes height
    (−1.2 cm with density correction off; the difference is the correction's ~5% residual inflation).
  - Earlier "+10% bias" figures were an estimator artefact plus an emerged probe, both now fixed.
  - A **two-sided, position-based density projection** replaces the one-sided velocity correction. The calm-pool float
    error drops from +4.4 cm to **−0.0 cm**, the liquid holds rest volume (agitated ±2% per 12 s), and agitated force
    jitter falls to ~105 N. Cost is +0.4 ms per frame at 64³.
  - **Sparse bricks (compute stage):** grid passes iterate only active 8³ bricks when under half the domain is active.
    In a 128³ shallow pool they cut those passes 2.5–5×. With MGPCG level 0 also sparse, the frame drops from 10.70 to
    9.14 ms (pressure solve −38%). Dense scenes are unchanged.
  - **Sparse storage (brick pool):** only bricks near particles own grid memory, up to `SparseStorageBudget`. A 128³-class
    shallow pool goes from ≈ 226 to 76 MiB at the same frame time, and 256×256×128 runs in 351 MiB (dense ≈ 1.2 GiB).
    Particle buffers now grow with the live count (`ParticleCapacity` is only a maximum): the 128³ shallow pool's
    particle memory went from 830 to 144 MiB, and scenes of up to 16M particles are possible.
  - **5-minute drift test:** volume is conserved to within 0.03% (calm +0.000 %/min, agitated −0.007 %/min). The calm
    floating box stays at −0.2 cm with no trend.
- **Particle-pass optimisation (Phase 2):**
  - A per-frame GPU spatial sort plus a trimmed APIC gather bring **96³ / 0.88M particles to ≈ 5–6.4 ms** (was 16–27 ms).
  - 64³ drops to 2.35 ms.
  - A second round (tiled shared-memory P2G, conditional accumulator clears) brings **128³ / 1.78M particles to ≈ 6.2 ms
    at CFL 4** (≈ 16 ms at the default CFL 2).
  - 64³ simulation is 1.38 ms.
- **MGPCG (Phase 2)** is implemented. It is about 2× cheaper than RBGS at 128³ for equal accuracy and is auto-selected
  above 400k cells. At 64³, warm-started RBGS is cheaper.

## Answers to the final decision questions (brief §17)

1. **Is Blender FLIP Fluids a viable technical foundation?** Yes as the *algorithmic* foundation (MIT, complete,
   source-verified); no as a linked runtime component.
2. **Integrate directly, partially reuse, or use as reference?** **Use it as a reference** (Strategy C). Keep the
   CPU engine as an offline validation baseline (E1). Do not translate code from the Batty-derived files, which have no
   published licence, or from any GPL addon code.
3. **Is GPU-native FLIP/APIC practical on the target hardware?** **Yes at 64³-class resolution** (measured
   ≤ 3.4 ms total). It is not yet practical at 128³ (≈59 ms). That needs narrow-band particles, sorted P2G and MGPCG.
4. **FLIP/APIC vs DFSPH vs PBF?** APIC is preferred, with PBF as the fallback. The weighted matrix is effectively a
   tie (PBF 3.80, APIC 3.71). APIC wins on incompressibility, viscosity and gameplay queries (SOLVER_COMPARISON.md).
5. **Coupling strategy for Chaos?** **Asynchronous delayed feedback**: GPU per-body reductions, a 4-slot non-blocking readback,
   implicit added-mass force application, and analytic buoyancy plus drag as the default. Next steps are a surface-height
   sensor and physics-thread application.
6. **Rendering architecture?** **Screen-space fluid rendering** through the public post-opaque delegate now
   (0.68 ms at 720p). A world-space SDF with marching-cubes mesh comes later, for shadows and captures.
7. **Can water, blood and slime share one foundation?** Yes for the solver core. Water and blood are parameter
   changes. Slime needs an added rheology layer (yield stress or shear thinning, plus elastic stress on APIC gradients),
   and higher viscosity alone does not give it (LIQUID_MATERIALS.md).
8. **Highest-risk technical assumption?** It *was* pressure-solve accuracy for buoyancy; Phase 2 measurements
   (probe Fz/ρgV 0.92–1.06) retire it. The highest risk is now **scaling the particle passes**: 128³ costs about 59 ms,
   dominated by P2G and G2P across 4 CFL substeps. After two optimisation rounds, 128³ fits the budget at CFL 4
   (≈ 6.2 ms). The highest open risk is now **coupling quality vs substep budget**: CFL 4 is affordable but gives
   noisier rigid-body coupling. Next comes **domain size beyond 128³**, which needs sparse bricks.
9. **Smallest invalidating experiment?** E3 (a floating body) in PressureAndDrag mode at 128³ with MGPCG inside
   2 ms. If the draft is still off by more than 25%, physical two-way coupling through the grid is not viable in
   real time, and analytic buoyancy (or PBF) becomes permanent.
10. **What to implement first in Phase 2?** MGPCG is done (2026-11-03). Next: narrow-band particles and sorted, atomic-free
    P2G (the 96³/128³ gate), then fractional solid weights (floor lift, lateral noise).

## Recommendation: **CONDITIONAL GO**

Proceed with the GPU-native FLIP/APIC architecture, on two conditions:
- **(a)** ~~a converged pressure solve plus a settling E3~~: **met in Phase 2.** E3 settles at +3% of the draft, the probe is within ±8%, and MGPCG is in;
- **(b)** ~~a 96³-class scene at ≤ 8 ms~~: **met in Phase 2.** ≈ 6.4 ms at CFL 2 and ≈ 5.0 ms at CFL 4 (PERFORMANCE_PLAN §8).
- **Status:** both conditions are met for 64³–96³ volumes, and for 128³ at CFL 4 (PERFORMANCE_PLAN §9).

The baseline is not theoretical: it was built, run and measured. The conditions cover exactly the parts the
measurements show are unproven.

## Deliverables index
| # | File | Author / evidence |
|---|---|---|
| 1 | FLIP_FLUIDS_SOURCE_AUDIT.md | source audit, file:line cited |
| 2 | FLIP_FLUIDS_FEASIBILITY.md | build system and C ABI audit |
| 3 | LICENSING_AUDIT.md | header scan of every file |
| 4 | SOLVER_COMPARISON.md | weighted matrix, verified licences |
| 5 | UE58_INTEGRATION.md (+ UE58_INTEGRATION_PHYSICS.md) | installed 5.8.4 headers, path:line |
| 6 | GPU_ARCHITECTURE.md | as built |
| 7 | PHYSICS_COUPLING.md | design + measured addendum |
| 8 | LIQUID_MATERIALS.md | design; params as built |
| 9 | RENDERING_ARCHITECTURE.md | as built + measured |
| 10 | GAMEPLAY_API.md | as built |
| 11 | EXPERIMENT_PLAN.md | E2, E3, E6 run |
| 12 | PERFORMANCE_PLAN.md | measured tables |
| 13 | RISK_REGISTER.md | statuses from runs |
| 14 | ARCHITECTURE_DECISION.md | decision record |
| 15 | PHASE1_IMPLEMENTATION_PLAN.md | Stage 1 done, Stages 2–5 planned |
| 16 | RESEARCH_SUMMARY.md | this file |

Screenshot of the running prototype: `media/demo_64x64x48.png`.
