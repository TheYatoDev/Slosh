# PERFORMANCE_PLAN.md

## 1. Hardware and method

- **Measured on:** NVIDIA GeForce RTX 5060 Laptop GPU, 8 GB (`nvidia-smi`, driver 617.42), Windows 11,
  UE 5.8.4, DX12, `-game -windowed 1280×720`, Development build (editor binaries running in game mode).
  The brief's reference Quadro RTX 4000 / i9-10900 is **not** available here. All numbers below are for the laptop GPU.
- **Instrument:** `ProfileGPU` (UE 5.8 RHI GPU profiler, per-pass timings written to the log) captured mid-run;
  per-second averages of `FApp` delta time and `RHIGetGPUFrameCycles` logged by the demo harness.
  Raw logs: `SloshDemo/Saved/Bench/*.log`.
- **Caveats:** a single `ProfileGPU` frame per configuration (variance ±20% between runs of the same
  config was observed: 2.25 vs 2.72 ms sim at 64³); laptop power/thermal state uncontrolled; the
  frame total includes the template map's own rendering (~4 ms GPU).

## 2. Budget

Provisional 4 ms total fluid budget at 60 FPS (16.7 ms frame), per the brief. This is aspirational.

| Component | Budget | 64×64×48 measured |
|---|---|---|
| Simulation | 2.5 ms | 2.25–2.72 ms |
| Rendering (SSFR) | 1.0 ms | 0.68 ms |
| Coupling/readback | 0.2 ms | ≤ 0.05 ms (copies + residual) |
| CPU (game thread) | 0.3 ms | not profiled separately (frame time 7–8 ms total at ~130–150 FPS) |

## 3. Cost model (per substep)

Dispatches = 9 + 2·PressureIters + ViscosityIters + ExtrapLayers. Bandwidth-bound passes:
- P2G ≈ particles × (3 comps × 8 faces × 2 atomics) → **atomic-throughput bound**.
- G2P ≈ particles × (3 × 8 × 3 reads + RK2 resample 24 reads + colliders).
- RBGS ≈ 2 × cells × (6 neighbour reads) per iteration; ~13 µs/iteration at 196k cells.
Substeps grow as h shrinks (CFL): halving the cell size doubles substeps for the same flow speed.

## 4. Measured results (E2/E6) [Meas]

| Config | Cells | Particles | Substeps | Sim GPU | P2G | Pressure | G2P | Render | Frame | Mean ∇·u (1/s) |
|---|---|---|---|---|---|---|---|---|---|---|
| 64×64×48, 60 it | 196,608 | 262,144 | 1 | **2.25–2.72 ms** | 0.46–0.64 | 0.79–1.12 | 0.66–0.68 | **0.68 ms** | 6.6–7.9 ms | 0.12–0.25 |
| 64×64×48, 20 it | 196,608 | 262,144 | 1 | 2.39 ms | 0.65 | 0.43 | 1.03* | 0.85 | 6.6–7.6 ms | 0.22–0.33 |
| 64×64×48, 150 it | 196,608 | 262,144 | 1 | 4.02 ms | 0.62 | 2.42 | 0.68 | 0.69 | 7.9–8.9 ms | 0.11–0.13 |
| 96×96×72, 60 it | 663,552 | 0.77–0.86 M | 2–3 | 27.3 ms (3 substeps) ≈ 9.1 ms/substep | 1.50 | 1.95–2.41 | 2.33 | — | 16–27 ms | 0.20–0.23 |
| 128×128×96, 60 it | 1,572,864 | 1.67–1.76 M | 4 (cap) | 59.1 ms ≈ 14.8 ms/substep | 3.47–4.18 | 3.40–3.44 | 4.89 | — | 50–60 ms | 0.47–0.52 |

\* single-frame outlier; G2P does not depend on pressure iterations.

Pass/fail against the criteria fixed in EXPERIMENT_PLAN:
- **64³-class, ≥ 200k particles, sim + render ≤ 4 ms: PASS** (2.9–3.4 ms).
- **128³-class ≤ 8 ms: FAIL** (≈ 59 ms). The dominant terms are particle passes (P2G + G2P ≈ 8–9 ms/substep at
  1.7M particles) multiplied by 4 CFL substeps. Pressure is the third term.
- Divergence decreases monotonically with iterations (0.28 → 0.18 → 0.12 s⁻¹), plateauing because the
  density-correction target deliberately leaves a residual in over-dense cells.

Memory (computed from allocation sizes): 64×64×48 ≈ 42 MB; 96×96×72 ≈ 153 MB; 128×128×96 ≈ 360 MB.

## 5. Scalability strategy

1. **Particle count is the primary lever** (P2G+G2P ≈ 55% of sim time). Keep 8 particles/cell only near
   the surface; 2–4 particles/cell in the bulk (narrow-band FLIP, Ferstl et al. 2016) [Design].
2. **Sorted, atomic-free P2G** — counting sort per cell then gather (expected 2× on P2G, Est).
3. **MGPCG** replaces 60 RBGS iterations (~1 ms at 64³, scaling with cells × iterations) with ~10 CG
   iterations of a V-cycle preconditioner (Est 0.4–0.8 ms at 64³, better convergence at depth).
4. **CFL-aware resolution**: 128³ at 2.5 cm needs 4 substeps for the same flow; prefer 64³–96³ grids
   with larger cells in gameplay volumes, and a separate visual-detail layer (foam/spray particles).
5. **Scalability CVars** (Design): `r.Slosh.PressureIterations`, `r.Slosh.MaxSubsteps`,
   `r.Slosh.RenderScale` (half-res SSFR), `r.Slosh.ParticleBudget`.
6. **Async compute**: move the step into the view-family graph and flag particle passes `AsyncCompute`.
7. **Frame pacing**: the substep count is clamped (MaxSubsteps) and DeltaTime is clamped to 1/20 s, so a
   hitch slows the liquid instead of exploding the cost.

## 6. Readback / sync

No GPU→CPU stalls by construction: readbacks are polled with `IsReady()` only, 4-slot ring; measured
latency 3 frames (4 at 128³ where frames are slower). CPU never waits on the GPU.

## 7. Phase 2 — MGPCG vs RBGS [Meas, 2026-11-03]

MGPCG = CG preconditioned by one geometric-multigrid V-cycle (McAdams et al. 2010): piecewise-constant
prolongation, R = Pᵀ, coarse operator 4·L, red-black GS smoothing (1 pre/1 post), the coarsest level
(≤ 512 cells) solved inside a single thread group, and all dot products and α/β kept on the GPU.
`Shaders/Private/SloshPressureMG.usf`, `FSloshProxy::AddPressureSolveMGPCG`.

**Residual accuracy** (density correction off, so the metric isolates the solver; mean |∇·u| after projection, t ≥ 6 s):

| Grid | Solver | Pressure GPU / substep | Mean ∇·u (s⁻¹) |
|---|---|---|---|
| 64×64×48 | RBGS 60 | 0.80 ms | 0.0077 |
| 64×64×48 | RBGS 150 | 2.16 ms | 0.0001 |
| 64×64×48 | MGPCG 4 (4 levels) | 0.48 ms | 0.063 |
| 64×64×48 | MGPCG 8 | 0.96 ms | 0.011 |
| 64×64×48 | MGPCG 16 | 1.85 ms | 0.0008 |

**Large grid** (density correction on, mean ∇·u over the run): 128×128×96 RBGS 20 = 1.04 ms / 0.93,
RBGS 60 = 3.1–3.4 ms / 0.54, MGPCG 4 (5 levels) = 1.45–1.61 ms / 0.68, MGPCG 8 = 2.7–3.0 ms / 0.56.
At 128³, MGPCG reaches RBGS-60 accuracy at about half the cost. At 64³, warm-started RBGS is cheaper for the same accuracy.

**Hydrostatic accuracy (pressure probe).** A fixed, fully submerged 40 cm box reports the integrated solver
pressure force. The mean of Fz / (ρ g V_voxel) over t > 8 s:

| Grid | RBGS 20 | RBGS 60 | MGPCG 4 | MGPCG 8 | MGPCG 16 |
|---|---|---|---|---|---|
| 64×64×48 | 0.99 | 1.06 | — | 1.00 | 1.18 |
| 128×128×96 | 0.95 | 0.92 | 0.94 | 0.95 | — |

The time-averaged hydrostatic pressure is correct to within ~8% with every solver. Instantaneous values swing ±50% at 64³
(±20% at 128³) from pool dynamics: the paddle, the emitter jet and sloshing.

**Decision:** the pressure solver is `Auto`. It uses RBGS 60 at ≤ 400k cells and MGPCG 4 above
(`ASloshVolume::AutoSolverCellThreshold`). The V-cycle costs 0.084 ms at 64³ and 0.153 ms at 96³.
One single-group coarse solve on 3072 cells cost 0.112 ms; deepening the hierarchy to ≤ 512 cells
brought it to 0.015 ms.

**Still the 128³ bottleneck:** particle passes. P2G + G2P cost ≈ 8–9 ms per substep at 1.7M particles, times 4 CFL substeps.
The pressure solve is not the limiting factor.

## 8. Particle-pass optimisation [Meas, 2026-11-03]

Two changes:
1. **APIC gather trimmed.** G2P skips the FLIP-delta reads (`VelOld`, `FaceFlags`) in APIC mode, which removes two thirds of the gather traffic.
2. **Spatial sort.** Each frame, a GPU counting sort orders particles by cell in 4×4×4-brick order
   (`SloshSort.usf`: count with atomic rank, 1024-key block scan, block-sum scan, scatter; 5 passes).
   The sorted copy becomes the live particle buffer. `ASloshVolume::SortInterval`, default 1.

Per-pass effect at 96×96×72 with ~820k particles (one ProfileGPU frame each):

| Pass | Before | After |
|---|---|---|
| G2P + advect | 2.67 ms | 0.35–0.74 ms |
| P2G | 1.46 ms | 0.97–1.05 ms |
| Sort (new) | — | 0.55–0.90 ms |
| Substep total | 7.06 ms | 3.50–3.92 ms |

Whole-frame results. Sim+render is estimated as frame GPU minus the ~4.1 ms base GPU cost of the template scene, so it is an Estimate built on measured numbers:

| Grid / particles | CFL | Avg substeps | Sim + render (Est) | Profiled sim frame | Verdict |
|---|---|---|---|---|---|
| 64³ / 262k | 2 | 1.0 | 2.35 ms (measured passes) | 1.94 ms | PASS (was 3.3–3.8 ms) |
| 96³ / 0.88M | 2 | 1.71 | ≈ 6.4 ms | 4.54 ms | **PASS ≤ 8 ms** (was 16–27 ms) |
| 96³ / 0.88M | 4 | 1.00 | ≈ 5.0 ms | 4.88 ms | PASS |
| 128³ / 1.78M | 2 | 4.0 | ≈ 30 ms | 34.3 ms | FAIL (was 50–59 ms) |
| 128³ / 1.78M | 4 | 1.65 | ≈ 13.7 ms | 15.5 ms | FAIL |

**CFL trade-off.** CFL 4 always runs one substep at 96³, but the floating body is noisier:
height SD 19 cm against 4 cm, submerged fraction 0.66 against 0.557 (Archimedes 0.547). The default stays CFL 2.

**Substep feedback loop (observed).** The substep count is ceil(v_max·dt_frame / (CFL·h)), so a slow frame
gives a larger dt, which demands more substeps and makes the next frame slower still. This explains the 2–4 substep
swings seen before the optimisation. Mitigation options: the existing frame-dt clamp (1/20 s), a higher CFL, or a
substep budget that slows the liquid instead of adding work [Design].

**Behaviour change.** Emission now **stops at capacity** instead of recycling the oldest slots. After sorting, slot
order is spatial, so recycling would erase a contiguous region of the pool.

**Remaining 128³ hotspots** (per substep): P2G 2.5 ms, NormalizeFaces 1.6 ms, MGPCG-4 1.4 ms, G2P 0.8 ms, plus the
sort at 1.1–1.7 ms per frame. Next levers:
- wave-aggregated P2G atomics (particles in a wave now share faces);
- sorting every 2–4 frames;
- a narrow-band particle count;
- fusing NormalizeFaces with BuildSolid.

## 9. 128³ push [Meas, 2026-11-03]

Three further changes:
1. **Tiled P2G.** Each 128-thread group accumulates its (spatially sorted) particles into a groupshared tile:
   8³ nodes × 3 MAC components × momentum and weight, 12 KB. It then flushes one global atomic per touched
   face. Stencil nodes outside the tile fall back to global atomics. Sums are integers, so the result is
   identical for any particle order.
2. **Conditional accumulator clear.** `NormalizeFacesCS` only zeroes accumulators that P2G actually touched. Most faces
   of a large domain are empty air.
3. **No `VelOld` write in APIC mode.** The FLIP reference velocity is never read there.

Per-pass results at 128×128×96 with 1.72–1.78M particles (one ProfileGPU frame each):

| Pass (per substep) | Round 1 (sort) | Round 2 |
|---|---|---|
| P2G | 2.5 ms | **0.61–0.80 ms** |
| NormalizeFaces | 1.6 ms | **0.29 ms** |
| MGPCG ×4 (5 levels) | 1.4 ms | 1.5 ms |
| G2P + advect | 0.8 ms | 0.74 ms |
| Extrapolate ×2 | — | 0.42–0.45 ms |
| **Substep total** | 7.3–8.0 ms | **4.25–4.70 ms** |
| Sort (per frame) | 1.1–1.7 ms | 1.1 ms |

At 64³, P2G went from 0.64 to 0.10 ms and the whole sim from 1.94 to 1.38 ms.

Whole frame at 128³ (Est: frame GPU minus the ~4.1 ms base scene):

| CFL | Avg substeps | Sim + render (Est) | Profiled frame | Verdict |
|---|---|---|---|---|
| 2 | 3.18 | ≈ 16.3 ms | 14.1 ms sim | over budget |
| 4 | 1.12 | **≈ 6.2 ms** | 5.9 ms sim + 0.57 ms render | **PASS ≤ 8 ms** |

Sorting every 2 frames (`-SloshSortInterval=2`) changed nothing measurable at 128³ (24.9 vs 25.8 ms frames, within noise).

Run-to-run spread: probe Fz/ρgV reads 1.00–1.31 across identical configurations. This is not a solver change: the
integer P2G sums are order-independent. It is the chaotic pool flow responding to different frame times. Treat
any single-run coupling number as ±0.15.

**Recommendation.** CFL 2, the default, is best for coupling quality at 64³–96³. For 128³-class domains use CFL 4, which
fits the budget, accepting noisier rigid-body coupling (mean residual divergence 1.0 vs 0.54 s⁻¹).
Remaining per-substep costs at 128³: MGPCG 1.5, G2P 0.74, P2G 0.6–0.8, extrapolation 0.43, divergence 0.2–0.4 ms.
Next levers:
- sparse active bricks, which would skip the roughly 70% of the domain that is air;
- fewer MGPCG iterations, or RBGS at moderate depth;
- fusing extrapolation layers into one pass.

## 10. Sparse bricks — compute stage [Meas, 2026-11-03]

**What is sparse.** Each substep, after `MarkCells`, the GPU builds an active list of 8³ bricks:
1. `BrickMarkCS` records which bricks contain fluid.
2. `BrickListCS` dilates that set by one brick, adds last substep's set (so cells that just emptied are cleaned once),
   and compacts the result into a list.
3. `SparseArgsCS` writes the indirect dispatch arguments.

These grid passes then iterate only the listed bricks through indirect dispatch: normalize/forces, viscosity, divergence,
RBGS, projection, extrapolation, residual, coupling force, and the density-projection setup. Face passes were restructured
so that each cell owns its three min faces plus the boundary faces on the grid's max side.

**What stays dense.** BuildSolid and MarkCells always run densely, so cell types are valid everywhere and nothing stale
is ever read. MGPCG and the particle passes are also dense. **Storage stays dense** (no memory saving yet).

**Adaptive.** Indirect dispatch costs a few µs per pass. RBGS issues 120 tiny passes per substep, so once most of the domain
is active, sparse dispatch is slower than dense. Measured at 64³ calm (67% active): RBGS 0.75 → 1.18 ms and density
projection 0.41 → 0.78 ms under forced sparse. The solver therefore dispatches sparsely only while the (lagged) active-brick
fraction is below `SparseMaxActiveFraction` (default 0.5).

| Scene | Active bricks | Path chosen | Effect |
|---|---|---|---|
| 64³ calm pool (100 cm deep) | 67% | dense | unchanged |
| 128³ agitated demo | 57% | dense | unchanged |
| **128³ shallow calm (20 cm), 1.14M particles** | **25%** | **sparse** | see table |

| 128³ shallow pool, per substep | Dense | Sparse |
|---|---|---|
| NormalizeFaces | 0.298 ms | 0.115 ms |
| Divergence | 0.077 ms | 0.030 ms |
| Project | 0.188 ms | 0.086 ms |
| Extrapolate ×3 | 1.235 ms | 0.237 ms |
| Density projection (incl. MGPCG ×2) | 1.03 ms | 0.66 ms |
| Substep | 4.92 ms | 4.29 ms |
| **Frame GPU (average)** | **10.45 ms** | **9.71 ms** |

Correctness: in the 64³ calm pool, forced sparse gives a float error of +0.2 cm (dense −0.0 cm) and probe 0.984 (dense 0.985).
Agitated and floor-start runs match dense.

**Next stages.**
1. Sparse MGPCG level 0, which is the remaining 1.4 ms in sparse scenes. This needs sparse dot-product partials.
2. Sparse **storage**: a brick pool with an indirection table, needed before domains much beyond 128³ fit in memory.

### 10b. Sparse MGPCG level 0 [Meas, 2026-11-03]

The level-0 MGPCG kernels now iterate the active-brick list when sparse dispatch is selected: PCGInit, the level-0
smoothing sweeps, restriction to level 1, prolongation to level 0, ApplyA, UpdatePR and UpdateD.
- Restriction iterates the 4³ level-1 cells inside each active 8³ brick, via a second indirect-args entry.
- Dot-product partials are per group; `PCGFinalizeDotCS` sums only the 4 × (active brick count) partials the dispatch wrote,
  inside a uniform loop, so the barrier stays legal.
- Coarse levels (≤ 1/8 the size) stay dense.
- `ActiveBricks` and the ghost diagonal are bound as SRVs, because SM5 allows only 8 UAVs per shader
  (`PCGUpdatePRCS` had reached 9).

| 128³ shallow calm pool (25% active, 1.14M particles) | Dense | Sparse grid passes only (§10) | **Sparse incl. MGPCG L0** |
|---|---|---|---|
| Pressure MGPCG ×4 | 1.656 ms | 1.404 ms | **1.030 ms** |
| V-cycle | 0.208 ms | 0.196 ms | 0.152 ms |
| Substep | 5.41 ms | 4.29 ms | **3.66 ms** |
| **Frame GPU (average)** | **10.70 ms** | 9.71 ms | **9.14 ms** |
| Calm float error | −0.2 cm | −0.3 cm | −0.3 cm |

Correctness with MGPCG and sparse dispatch both forced on at 64³: the calm float error is +0.1 cm and the probe reads
Fz/ρgV = 1.002. The agitated run is stable (interior density 0.998). Dense-dominated scenes still choose dense dispatch
(active fraction 0.57–0.75).

Remaining dense work in sparse scenes: MG coarse levels, BuildSolid/MarkCells/brick marking (≈ 0.15 ms at 128³), and the
particle passes. **Storage is still dense**, which is the next stage for domains beyond 128³.

### 10c. Sparse brick storage [Meas, 2026-11-03]

Grid storage is now a **brick pool**. Only 8³ bricks within one brick of a particle own storage.
- Every per-cell buffer holds `NumPoolBricks` slots of 512 cells; every per-face buffer holds three such planes.
  `BrickTable` maps bricks to slots.
- Slot 0 is a zeroed **null brick**. Unallocated and out-of-grid cells resolve to it and read as air.
- Max-side boundary faces are no longer stored.
- Capacity is `SparseStorageBudget` × all bricks. Overflowing bricks get no storage; they are counted in `PoolOverflowBricks`,
  and their particles move ballistically.
- MGPCG level 0 lives in the pool; coarse levels stay dense (≤ 1/8 of the cells).
- The particle sort now uses one key per 4³ brick (64× smaller count/offset arrays).

Allocation runs on the GPU every substep, in 4 passes plus 1 indirect clear (`BrickFreeCS`, `BrickAllocCS`, `PoolArgsCS`,
`ClearNewSlotsCS`), costing ≈ 0.01 ms. `G2PAdvectCS` and `EmitParticlesCS` mark touched bricks, deduplicated per thread
group. A first version used a separate touch pass that re-read every 64-byte particle; it cost 0.19 ms at 1.1M particles
and 1.0 ms at 3.6M. A recycled slot only needs its pressure warm start, particle count and density potential zeroed:
every other value is rewritten each substep before it is read, and the accumulators clear themselves.

**Grid memory** (MiB). The dense column is computed from the pre-pool buffer sizes; the others are allocation totals.

| Grid | Dense (before) | Pool, budget 1.0 | Pool, reduced budget | Frame GPU |
|---|---|---|---|---|
| 64×64×48 demo | ≈ 28 | 27 | — | unchanged (sim ≈ 2.2–2.5 ms) |
| 128×128×96 shallow pool (1.14M particles, 25% of bricks) | ≈ 226 | 213 | **76** (budget 0.35, no overflow) | 9.1 ms before; 9.1–10.0 ms now |
| 256×256×128 shallow pool (3.6M particles, 12% of bricks) | ≈ 1202 | 1135 | **351** (budget 0.3) | calm: 18.2–19.3 ms dense, 17.8–19.3 ms pool |

Correctness against the pre-pool numbers:
- 64³ calm pool: float error −0.4 cm with RBGS and +1.1 cm with MGPCG and the active list forced (single samples of a
  bobbing box). The pressure probe reads Fz/ρgV = 0.985 on both paths, matching the pre-pool 0.985.
- Agitated, floor-start and 128³ gentle-drop runs float at 27.5–28.5 cm against 28.1 cm expected, on both dispatch paths
  and at budget 0.35.
- Forced overflow (budget 0.25 in the demo) stays stable and reports about 150 bricks short.

Remaining notes:
- The active list beats the pool scan once usage is low: 10.3 vs 11.4 ms at 25% usage (the `SparseMaxActiveFraction` rule).
- At 256³ a splash needs about 1.8× the calm brick count. Budget 0.3 overflowed by 3.4k bricks during the splash
  (the same splash is already unstable, below), so size the budget for the peak, not the calm state.
- Particle buffers now dominate memory: 132 B per capacity slot (two particle copies plus the sort rank). The demo
  allocates 6.3M slots (830 MiB) for 1.1M particles at 128³; capacity should follow the scene.
- Pre-existing, not pool-related: at 256×256×128 (1.25 cm cells, CFL 4) the box impact drives particles to the
  20 m/s clamp, which forces 4 substeps (≈ 50 ms frames). The pre-pool dense build reproduces it (R-21).

### 10d. Fast-droplet advection fix (R-21) [Meas, 2026-11-03]

**Symptom.** At 256×256×128 (1.25 cm cells, CFL 4) a body impact left particles pinned at the 20 m/s clamp for good.
That kept the substep count at its maximum of 4 (≈ 50 ms frames). The pre-pool dense build reproduced it.

**Diagnosis.** New clamp diagnostics: `Stats[9..13]` → `ClampedParticles`, `ClampedCentroid`, `ClampedInSolid`, plus
`-SloshTraceSpeed`. They showed 4–60 droplets that stayed at a fixed point about 12 cm above a 4 cm film while moving
downward at the clamp.
- G2P advects with RK2 and samples the grid velocity at the midpoint, 0.5·dt·|v| ahead. At 20 m/s with
  dt = 1/240 s that is 3.3 cells ahead, inside the still film (or past the 2-layer extrapolation band, where faces read 0).
- The droplet therefore did not move, kept its own velocity, and gravity drove it to the clamp.
- At 128³ (2.5 cm cells) the midpoint stays within about 1.7 cells, which is why only fine grids showed it.
- More PCG iterations (12) and disabling ghost fluid did not remove the stuck droplets, and lowering the clamp to 11 m/s
  did not either. 8 substeps did, at roughly double the cost.

**Fix** (`G2PAdvectCS`):
1. The midpoint velocity blends to forward Euler with the particle's own grid velocity as the midpoint distance goes from
   1 to 2 cells. Normal flow, with the midpoint under 1 cell, is pure RK2 as before.
2. Faces outside the known band (not fluid-valid, solid or extrapolated) take the particle's velocity instead of 0.

| 256×256×128 shallow pool, box impact | Before | After |
|---|---|---|
| Particles at the 20 m/s clamp | 4–66 per frame, never cleared | 0 (impact peak 15.6 m/s) |
| Substeps after impact | 4 indefinitely | 1–2 within ≈ 2.5 s |
| Frame after impact | 43–57 ms | 20–31 ms |

No regressions:
- 64³ calm probe 0.987 (was 0.985), and the agitated, floor-start and default scenes are unchanged.
- 128³ gentle and plunge drops float at 27.3–28.8 cm (expected 28.1). `clamp=0` in every scene.
- The impact second still overflows a 0.3 brick budget at 256³; the pool sizing advice in §10c stands.

### 10e. Particle capacity sizing and dispatch limits [Meas, 2026-11-03]

**Growable particle buffers.** `ParticleCapacity` is now a *maximum* (up to 16M), not an allocation.
- The particle, sort and rank buffers start empty.
- They are sized exactly to the initial fill on the first frame, then grow by at least 1.5× when emission needs more slots.
  `EnsureParticleCapacity` copies the live particles on the GPU, so growth does not reset the simulation.
- Changing `ParticleCapacity` no longer reallocates the grid.

| Scene | Particles | Before (capacity-sized) | Now |
|---|---|---|---|
| 64×64×48 demo (emitter fills the cap) | 262k | 33 MiB | 33 MiB |
| 64³ calm pool | 615k | 99 MiB (786k slots) | 78 MiB |
| 128×128×96 shallow pool | 1.14M | 830 MiB (6.3M slots) | **144 MiB** |
| 256×256×128 shallow pool | 3.6M | 528 MiB (4M clamp) | 456 MiB |
| 256×256×128, 20 cm deep | 9.8M | impossible (4M clamp) | 1235 MiB |

**65535-group limit.** D3D allows at most 65535 groups per dispatch dimension, i.e. 8.39M threads at 128 per group.
A full-budget pool scan at 256×256×128 needs 65536 groups, and more than 8.4M particles also exceed it.
- 1D dispatches now wrap into rows of 32768 groups: `Slosh::GroupCount1D`, `LinearThreadId` / `LinearGroupId` in
  SloshCommon.ush, and `WriteDispatchArgs` for the indirect args.
- This covers the sim, MG, sort and splat kernels.
- Validated with a 256×256×128 forced pool scan (2 rows, correct volume and density) and with 9.8M particles (3 rows).

**Stat fix.** The interior-density diagnostic summed density × 10⁴ in 32 bits and wrapped beyond about 0.4M interior cells:
it read 0.26 at 9.8M particles, and the particle-volume figure derived from it read 9125 L for 2394 L of liquid. It now
sums the signed deviation from rest density.

Cost at 9.8M particles (256×256×128, budget 0.5): ≈ 70 ms per frame at 2 substeps. The per-frame spatial sort alone is
8.2 ms, so raise `SortInterval` for very large particle counts. That scene is far beyond the 60 FPS budget; frames slower
than 50 ms also hit the frame-dt clamp and lose CFL headroom.

### 10f. Large-particle-count cost [Meas, 2026-11-03]

Profile at 9.8M particles (256×256×128, 20 cm deep, CFL 4), before these changes:
- Per substep (≈ 22 ms): P2G 4.2 ms, MGPCG 8.0 ms, G2P 6.5–7 ms, extrapolation 1.5 ms.
- Per frame: spatial sort 8.2 ms, render 2.5 ms.
- The flow needed 1 substep, but slow frames were clamped to 1/20 s of simulated time, which needed 2–4 substeps,
  which made frames slower still (a feedback spiral).

Changes:
1. **`MaxFrameDt`** (default 1/30 s): the most simulated time per frame. Below 30 FPS the liquid runs in slow motion
   instead of spiralling. Scenes above 30 FPS are unaffected.
2. **48-byte particles:** the APIC affine matrix is stored as 9 halves (`FSloshParticlePacked`; kernels unpack it).
   The sort went from 7.6 to 6.0 ms at 9.8M.
3. **`SortInterval` default 4:** sorting every 4th frame cost P2G/G2P ≤ 0.4 ms per substep and saved more in every scene
   tested (dam break −0.3 ms, 128³ −0.5 ms, 9.8M −3 ms).
4. **G2P midpoint:** the known-face test (R-21) runs only when the midpoint is 1–2 cells away; under 1 cell the stencil
   is inside the extrapolated band, and over 2 cells the step is pure Euler.
5. **MG same-brick neighbours:** `FineNeighbourIndex` addresses neighbours inside the same 8³ brick at fixed slot
   offsets (±1/±8/±64), so only brick-boundary neighbours use the BrickTable lookup. V-cycle −6%, restriction −15%.

| Frame GPU | Before | After |
|---|---|---|
| 9.8M particles, calm | 70 ms (2 substeps) | **36 ms** (1 substep) |
| 9.8M particles, settling after impact | 85–115 ms (4 substeps) | 49–51 ms (1–2 substeps) |
| 128×128×96 shallow pool | 9.1–10.0 ms | 8.1–8.7 ms |
| 64³ calm pool | 8.6–9.0 ms | 7.8–8.0 ms |
| 64×64×48 dam break | 6.7–7.2 ms | unchanged |

Accuracy is unchanged:
- Calm probe 0.985 (RBGS) and 0.990 (MGPCG); calm float 107.6–107.8 cm against 108.1 cm expected.
- 128³ floats 27.7–28.5 cm against 28.1 cm expected; no clamped particles.

The remaining large-scale cost is per substep: MGPCG level 0 (bandwidth-bound) and P2G atomics. At 9.8M particles,
60 FPS is out of reach on this GPU (RTX 5060 Laptop).

### 10g. Solver pass: P2G, MGPCG level 0, density displacement [Meas, 2026-11-03]

**Kept: density displacement face pass.** G2P used to evaluate the density-projection displacement per particle: 24
stencil nodes, each reading 2 cell types and 2 potentials. `DensityDisplacementCS` now writes the face displacements
once into the spare velocity buffer, and G2P samples them like a velocity. The result is identical (same formula, same
inputs).

| G2P on displacement substeps | Before | After (incl. the face pass) |
|---|---|---|
| 9.8M particles | 8.45 ms | 6.85 ms |
| 128×128×96 shallow pool | 1.01 ms | 0.60 ms |
| 64×64×48 dam break | 0.170 ms | 0.139 ms |

Frame GPU: 9.8M calm 36.5 → 34.5 ms, 128³ shallow pool 8.1 → 7.9 ms. Accuracy:
- Calm probe 0.981–1.004 (5 runs) and calm float mean 107.6–108.3 cm (expected 108.1).
- Box bob SD 1.4–3.3 cm; the old path measured 0.8–2.7 cm over 7 runs. These overlap: it is chaotic run-to-run variation.

**Rejected, measured slower: P2G bounding-box tile.** Clearing and flushing only the group's node bounding box, instead
of the fixed 8³ tile, made P2G slower in every scene (9.8M 4.41 → 4.90 ms, 128³ 0.43 → 0.49 ms, dam 0.120 → 0.133 ms).
The runtime-bounded loops and the divisions in the flush cost more than the skipped shared-memory entries. The fixed,
unrolled tile stays.

**Rejected by analysis: fusing the PCG direction update into ApplyA.** Each thread would update d while its
neighbours read the old d (a race), unless d were double-buffered. That costs a buffer and about as many reads as the
saved pass (0.12 ms per iteration at 9.8M).

**No measurable change: MG smoother reads.** Skipping the x/b reads that a sweep doesn't need left the V-cycle at
1.22 ms (9.8M).

**Where level 0 stands.** At 9.8M (2.1M active cells) every level-0 pass costs 0.14–0.25 ms, about twice the
bandwidth-ideal time, and there are 8 per PCG iteration. Further gains need fewer passes rather than faster ones:
- a cheaper V-cycle (fewer smoothing sweeps),
- running the density projection every other frame (its MGPCG ×2 costs 4.2 ms at 9.8M),
- fewer PCG iterations with a stronger preconditioner.

Each of these trades accuracy and needs the drift and buoyancy tests.

### 10h. Accuracy-trading solver options [Meas, 2026-11-03]

All three options were evaluated with MGPCG forced, against 2–3 baseline runs each.

| Option | Cost | Accuracy | Decision |
|---|---|---|---|
| **3 PCG iterations** (was 4) | 9.8M: MGPCG 7.9 → 6.4 ms per substep; 128³: 1.1 → 0.95 ms | 64³ calm: probe 0.984 (base 0.986–0.990), float 108.1 cm (exp. 108.1). 128³ plunge drop: float 28.1 cm, mean div 0.0069 1/s (base 0.0085–0.018). 128³ gentle drop: float 27.8 cm. No clamped particles | **Default** |
| **Density projection every 2nd frame** (relaxation compounded to 1 − (1 − r)²) | 9.8M calm frame −1 ms | Probe 0.984 / 0.985 / 0.959 (3 runs; base 0.981–1.004), float 107.8–108.1 cm. 2-min drift: dam −0.002 %/min, calm 0.000 %/min (base −0.007 / 0.000) | **Opt-in** `DensityProjectionInterval` |
| **Light V-cycle** (level 0: fused red half-sweep down, one red sweep up) | V-cycle 1.24 → 0.51 ms | **Unstable:** mean div 0.025 → 33 1/s at 9.8M and 15 1/s at 64³; boxes sank, particles clamped | **Removed** |

Notes:
- The light V-cycle is symmetric on paper, but black cells get only the piecewise-constant coarse correction. That is
  far too weak a preconditioner for PCG with 3–4 iterations.
- Density every 2nd frame stays opt-in. At typical sizes the saving is ≈ 0.25 ms per frame, and alternating-frame
  position shifts have not been checked visually.

9.8M particles with both options: calm frame 33.4 → 32.0 ms.
