# PHASE1_IMPLEMENTATION_PLAN.md

The user approved Phase 1 together with Phase 0 on 2026-11-03 ("Phase 0, then prototype"). This document
records what Stage 1 delivered and plans the stages after it. Each stage ends with a measurable gate
from EXPERIMENT_PLAN.md.

## Stage 1 — Dense GPU FLIP/APIC baseline ✅ (built and run)

| Item | Where | State |
|---|---|---|
| Plugin skeleton, 3 modules, shader mapping | `Plugins/Slosh/*.uplugin`, `*.Build.cs`, `SloshCoreModule.cpp` | compiled (UBT, MSVC, Development Editor) |
| Persistent RDG state, standalone per-frame graph, readback ring | `SloshCore/Private/SloshProxy.cpp` | runs |
| 13 solver kernels (emit, solids, P2G, mark, normalize, viscosity, divergence, RBGS, project, residual, extrapolate, submerged, G2P) | `Shaders/Private/Slosh.usf` | compiled by UE's shader compiler, run on GPU |
| APIC + FLIP transfers, RK2 advection, CFL substeps, density correction | same | runs |
| Analytic sphere/box/capsule colliders, moving obstacles, two-way feedback | `SloshColliderComponent.cpp`, `ProjectCS`, `SubmergedCS` | runs; see E3 |
| Screen-space fluid renderer (post-opaque) | `SloshRender/Private/SloshRenderer.cpp`, `SloshRender.usf` | runs (screenshot) |
| Blueprint API: volume, collider, emitter, material asset | `Slosh/Public/*.h` | compiled; called from the C++ demo only (no Blueprint test yet) |
| Automated test harness | `SloshDemo/Source/SloshDemo/SloshDemoGameMode.cpp` | runs |

**Gate (E2, E6 @64³):** sim + render ≤ 4 ms at ≥ 200k particles → **passed: 2.25 + 0.68 ms at 262k [Meas]**.

## Stage 2 — Solver accuracy (2–3 weeks est.)
1. ✅ MGPCG pressure (done 2026-11-03): 4–5 level V-cycle, GPU-side dot products, `Auto` solver selection.
   Outcome against the gate:
   - E3 PressureAndDrag draft at +3%: **passed**.
   - "Mean divergence 10× lower at equal cost": **not met at 64³**, where warm-started RBGS is better; at 128³ MGPCG is 2× cheaper.
   See PERFORMANCE_PLAN §7.
2. ✅ Ghost-fluid free surface (done 2026-11-03). The interface comes from the smooth P2G-weight density rather than a
   separate particle SDF; it is applied in RBGS, MGPCG level 0 and projection. Calm pool: box error 11.7 → 8.9 cm with the
   old estimator, +4.4 cm with surface-extrapolated coupling pressure.
3. Fractional solid face weights (Batty 2007) from the analytic SDFs. The floor-contact part is ✅ solved
   by seepage pressure on contact faces (2026-11-03); lateral-force noise from staircase faces remains.
4. Coupled variational viscosity, or at least a CG solve for high-ν slime (E5).
5. ✅ Two-sided position-based density projection (done 2026-11-03): calm-pool float within 0.1 cm of Archimedes,
   rest volume held. 5-minute drift ✅ measured: |drift| ≤ 0.007 %/min (E2b).

## Stage 3 — Interaction breadth (2 weeks est.)
1. Collider auto-fit from `UBodySetup::AggGeom` (sphere/box/sphyl elements, UE58_INTEGRATION_PHYSICS.md §3).
2. `USloshContainerComponent` (hollow SDFs), skeletal physics-asset colliders (E4).
3. Drains, column-height readback, async query library (GAMEPLAY_API §4).
4. Optional physics-thread force application through `ISimCallbackObject` (lower latency).

## Stage 4 — Rendering (2–3 weeks est.)
1. Foam/spray diffuse particles (sorted P2G gives neighbour data). 2. Raster sprite splat for close-ups.
3. Motion vectors. 4. World-space surface (SDF + GPU marching cubes + primitive proxy) for shadows and captures.

## Stage 2b — Particle passes ✅ (done 2026-11-03)
Per-frame spatial counting sort (4×4×4 bricks) and a trimmed APIC gather. 96³ gate **passed** (≈ 5–6.4 ms);
128³ was ≈ 14–30 ms after round 1. Round 2 (tiled P2G, conditional accumulator clear) gives **128³ ≈ 6.2 ms at
CFL 4** (≈ 16 ms at CFL 2). Sorting every 2 frames gave no measurable gain. Next: sparse active bricks, fused
extrapolation, and a CFL policy per domain size.

## Stage 5 — Scale and production
Sparse bricks: compute stage ✅ (2026-11-03; adaptive, PERFORMANCE_PLAN §10). Sparse MGPCG level 0 ✅ (§10b). Sparse storage ✅ (brick pool, §10c). 256³-class impact stability ✅ (R-21, §10d). Growable particle buffers and the >65535-group dispatch fix ✅ (§10e). Large-count cost pass ✅ (§10f; 9.8M particles 70 → 36 ms). Solver pass ✅ (§10g; density displacement −18–40% G2P). Accuracy-trading options ✅ (§10h: PCG 3 default, density interval opt-in, light V-cycle rejected). async compute, scalability CVars (`r.Slosh.*`), multi-volume budgets, Quadro RTX 4000
validation, Vulkan port test, legal review sign-off (LICENSING_AUDIT §open questions).

## What not to do yet
No Jolt adapter (keep `IFluidRigidBodyAdapter` as an interface sketch only), no networking of liquid
state, no Niagara dependency.
