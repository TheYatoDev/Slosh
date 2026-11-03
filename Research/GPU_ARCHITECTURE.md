# GPU_ARCHITECTURE.md — Slosh Compute Architecture

**Scope:** the GPU solver as built in the Phase 1 prototype (`SloshDemo/Plugins/Slosh`), plus the
planned evolution. **[Impl]** = compiled and executed on the dev machine (RTX 5060 Laptop, UE 5.8.4, DX12);
**[Meas]** = measured with `ProfileGPU` in the auto-test (see `PERFORMANCE_PLAN.md` §4); **[Design]** = not built.

## 1. Module layout [Impl]

| Module | Loading phase | Contents | Depends on |
|---|---|---|---|
| `SloshCore` | PostConfigInit | Shader dir mapping (`/Plugin/Slosh`), sim global shaders, `FSloshProxy` (persistent GPU state + RDG step), POD types | Core, RenderCore, RHI |
| `SloshRender` | PostConfigInit | Screen-space fluid shaders, post-opaque delegate, render registry | SloshCore, Engine, RenderCore, Renderer, RHI |
| `Slosh` | Default | UObjects: `ASloshVolume`, `USloshColliderComponent`, `USloshEmitterComponent`, `USloshMaterialAsset`, subsystems | SloshCore, SloshRender, Engine, PhysicsCore |

Simulation never depends on rendering. Rendering reads only the particle buffer and the sim→world transform.

## 2. Data layout [Impl]

Sim space: meters, origin at the domain min corner, axes = volume actor axes, dense MAC grid `NX×NY×NZ`, cell size `h`.

| Buffer | Type | Size (64×64×48, 262k particles) | Notes |
|---|---|---|---|
| Particles | `StructuredBuffer<FSloshParticle>` 64 B | 16.8 MB | Pos, Vel, APIC rows Cx/Cy/Cz, Age. Ring buffer: emission overwrites oldest when full |
| Vel ×3, VelOld | `float` per face (U,V,W concatenated, offsets in `FaceOffsets`) | 4 × 2.4 MB | ping-pong for viscosity/extrapolation |
| Accum | `int` ×2 per face | 4.8 MB | fixed-point P2G momentum + weight (scale 2²⁰) |
| FaceFlags | `uint` per face | 2.4 MB | VALID / SOLID / HAS_WEIGHT / extrapolation layer bits |
| CellType, CellCount, Pressure, Divergence | per cell | 4 × 0.79 MB | CellType packs type + collider id |
| Colliders | `StructuredBuffer<FSloshCollider>` 80 B | ≤ 64 entries | uploaded per frame |
| Coupling | `int` ×16 per collider | 4 KB | force, torque, wetted faces, submerged cells, fluid velocity sum, centroid sum |
| Stats | `uint` ×4 | 16 B | max speed (float bits), fluid cells |

Total ≈ 42 MB for the default domain **[Impl, computed from allocation sizes]**.
All persistent buffers are `TRefCountPtr<FRDGPooledBuffer>` created via `GraphBuilder.ConvertToExternalBuffer`
and re-registered every frame with `RegisterExternalBuffer` (`RenderGraphBuilder.h:90,360`).

## 3. Execution pipeline (per game frame) [Impl]

Game thread (`ASloshVolume::Tick`, TG_PostPhysics) gathers colliders/emitters into `FSloshFrameInput`,
then `FSloshProxy::SubmitFrame_GameThread` enqueues one render command that builds a **standalone
`FRDGBuilder`**, runs the step and executes it. Because the step is not tied to a view, simulation cost is
independent of the number of views and runs even with no camera.

```
Clear(Coupling, Stats)
Emit*                      one dispatch per emit request (ring-buffer cursor)
for substep in 1..N:       N = clamp(ceil(v_max·dt / (CFL·h)), Min, Max), v_max from lagged readback
  BuildSolid               per cell: walls + analytic collider SDFs -> CellType (+collider id)
  P2G                      per particle: APIC/FLIP scatter to 8 faces × 3 comps, fixed-point InterlockedAdd; CellCount++
  MarkCells                per cell: fluid if count>0; non-fluid pressure = 0; warm start rescale dt/dt_prev
  NormalizeFaces+Forces    per face: u = Σmu/Σm, VelOld = u, +g·dt, solid faces = solid normal velocity, clear Accum
  [Viscosity Jacobi ×K]    per face: (I - ν dt ∇²)u = u*  (only if ν > 0)
  Divergence               per cell: ∇·u − density-correction target; clears CellCount
  Pressure RBGS ×M (×2)    per cell, red/black SOR, Neumann solids, Dirichlet p=0 air
  Project                  per face: u -= ∇p̂; pressure force/torque on two-way colliders (int atomics)
  [Residual]               last substep only: post-projection divergence stats
  Extrapolate ×L           per face: fill air band from valid neighbours (layer-tagged, deterministic)
  G2P+Advect               per particle: PIC/APIC gather or FLIP delta, RK2 advection, SDF collision, wall clamp, speed clamp
Submerged                  per cell: submerged-volume and centroid estimate per collider
EnqueueCopy -> FRHIGPUBufferReadback ring (4 slots)  — no CPU stall
```
Dispatches per substep: 9 + 2M + K + L (+1 residual on the last substep). Default (M=60, K=0, L=2): **131 dispatches** (matched the 131 passes
reported by `ProfileGPU` **[Meas]**).

## 4. Key GPU design decisions

| Decision | Rationale | FLIP Fluids reference behaviour |
|---|---|---|
| Dense grid, 1D dispatches of 128 threads | Simplest correct baseline; sparse bricks deferred | Dense `Array3d` + block-sorted particles |
| Fixed-point int atomics for P2G (scale 2²⁰) | Portable (no float atomics in UE 5.8 HLSL, per UE58_INTEGRATION.md §Float atomics); deterministic summation order-independent | Particle sort into 10³ blocks, no atomics (`velocityadvector.cpp:467-536`) |
| Trilinear APIC transfer (gradient form for C) | Matches FLIP Fluids' APIC mode; good angular momentum | `_getIndicesAndGradientWeights` (`fluidsimulation.cpp:7154`) |
| Red-black SOR with warm start | Fixed cost, no reductions, no CPU sync | PCG + MIC(0), tol 1e-9, ≤900 iters, cold start (`fluidsimulation.h:2436`) |
| Ghost-fluid free surface (Phase 2; was p = 0 at air-cell centres): θ from the 0.5 iso-level of the smooth P2G density, 1/θ diagonal stored in `CellDiag` | No separate level set needed; continuous as particles move; same operator in RBGS, MGPCG level 0 and projection | Ghost fluid θ = φ_air/φ_liq (`pressuresolver.cpp:767-867`) |
| Cell-centred binary solids from analytic SDF; only fluid–solid faces carry the solid normal velocity, solid interiors are extrapolated from fluid (free slip) | Cheap, handles moving bodies every substep; avoids spurious no-slip drag found in E3 | Variational face fractions from mesh SDF |
| Two-sided position-based density projection (Phase 2; replaced the velocity divergence target, which first used integer counts and then smooth weights) | Holds rest volume (calm drift −0.1%, agitated ±2% per 12 s); no momentum injected; floating box at Archimedes height | None (removes >250 particles/cell) |
| Append-only emission, stops at capacity (Phase 2; was ring recycling) | No compaction pass, no indirect dispatch; compatible with spatial sort | Particle removal + compaction |
| One shared parameter struct, all UAV | One struct for 12 kernels; RDG inserts UAV barriers | n/a |

## 5. Synchronisation and latency [Impl]

- GT→RT: render command per frame carrying settings/colliders/emits by value (no shared mutable state).
- RT→GT: `FRHIGPUBufferReadback` ring of 4, polled after each `Execute()`; results published under a lock
  with the sim frame number. Observed latency **3 frames** in the auto-test log **[Meas]**.
- Collider feedback is mapped back to components through a per-frame snapshot (`ColliderHistory`), so
  reordering/adding colliders cannot misattribute delayed forces.
- Lifetime: proxies are released on the render thread (`FSloshProxy::ReleaseOnRenderThread`); the
  render registry holds weak pointers.

## 6. Known limitations of the prototype

1. RBGS convergence degrades with depth: 60 iterations do not converge a 48-cell-deep column from scratch;
   warm start hides this for slowly varying flow. Replace with MGPCG (Phase 2).
2. First-order free surface and cell-binary solids → staircase boundaries, ~½-cell volume error near walls.
3. P2G atomics contend in dense regions (0.46 ms at 262k particles **[Meas]**); a counting sort + gather
   P2G removes atomics and improves cache behaviour (Phase 2 option).
4. No async compute: the standalone graph runs on the graphics queue before the frame's scene rendering.
   `ERDGPassFlags::AsyncCompute` is a straightforward follow-up once the sim is moved into the view
   family's graph (`PreRenderViewFamily_RenderThread`), at the cost of view-count coupling.
5. Dense allocation: memory scales with domain volume, not liquid volume (see PERFORMANCE_PLAN §5).

## 7. Evolution path [Design]

1. **MGPCG** pressure (geometric multigrid V-cycle as preconditioner, Jacobi smoothing, 3–4 levels); keeps
   the same matrix, removes the depth-dependent convergence problem.
2. **Ghost-fluid free surface** using a particle-splatted liquid SDF (one extra pass), matching FLIP Fluids.
3. **Fractional solid weights** (Batty 2007) from the analytic SDF evaluated at face corners.
4. **Sorted P2G** (counting sort by cell, prefix sum, gather) — also enables neighbour queries for foam.
5. **Sparse bricks** (8³ tiles, indirect dispatch over active tiles) for large open worlds.
6. **Async compute** and a sim-in-view-family mode for low-latency coupling.

## 8. Phase 2 addition — MGPCG pressure solver [Impl, Meas]

`SloshPressureMG.usf` adds 10 kernels: BuildType, PCGInit, Smooth, Restrict, CoarseSolve, Prolong, ApplyA,
FinalizeDot, UpdatePR and UpdateD. Persistent buffers: the r, z, d and q vectors, partial sums (one per 128-thread group),
4 scalars, and Type/X/B per coarse level. Passes are fused to cut dispatch count:
- The first red pre-smoothing sweep is merged into PCGInit/UpdatePR (x starts at 0, so red cells only need their own residual).
- The coarse red initialisation is merged into Restrict.
- The r·z reduction is merged into the final level-0 red post-smoothing sweep.

One PCG iteration at 4 levels is 21 dispatches, ≈ 0.11 ms at 64³. The default is `Auto` (PERFORMANCE_PLAN §7).
Two engine constraints were found during implementation:
- The shader compiler rejects group barriers after loops whose trip count varies per thread (X3663), so strided loops use uniform trip counts.
- RDG validation requires every statically referenced UAV to be bound, even inside a uniform branch that skips it.

## 9. Spatial particle sort [Impl, Meas]

`SloshSort.usf` plus `FSloshProxy::AddSpatialSort` run each frame after emission and before the substeps.
1. Keys are cells in 4×4×4-brick order: key = brick·64 + local.
2. Count pass: `InterlockedAdd` on the key count returns each particle's rank.
3. Exclusive scan of the counts in 1024-key blocks, then a single-group scan of the block sums.
4. Scatter to `ParticleSortBuffer`; the two particle buffers then swap.

Extra memory per particle: 64 B for the ping-pong copy plus 4 B of rank. Per key: 8 B.
Emission appends at `NumParticles` and stops at capacity; ring recycling was removed (PERFORMANCE_PLAN §8).
Measured effect: G2P up to 7.7× faster and P2G 1.4–1.5× faster at 96³.

## 10. Tiled P2G [Impl, Meas]

`P2GCS` now accumulates into a groupshared tile:
- The tile origin is the group's minimum particle cell − 1. The tile spans 8 nodes per axis, so it covers
  groups whose particles span ≤ 6 cells.
- Accumulation uses shared-memory `InterlockedAdd`; the tile is then flushed with one global atomic per non-zero entry.
- Out-of-tile contributions fall back to global atomics.

Measured gain: P2G 4.1× faster at 128³ and 6.4× at 64³ (PERFORMANCE_PLAN §9). It depends on the spatial
sort (§9); without sorting it stays correct but falls back to mostly global atomics.

## 11. Ghost-fluid surface and surface-pressure coupling [Impl, Meas]

- `DivergenceCS` stores the operator diagonal per fluid cell. Non-solid neighbours count 1; air neighbours count 1/θ,
  with θ = (ρ_f − 0.5)/(ρ_f − ρ_a) from the smooth P2G-weight density (`CellDensity`), clamped to [0.05, 1].
  `PressureRBGSCS` reads `CellDiag`, MGPCG's level-0 stencil reads it through `DiagF` (`bGhostDiag`), and `ProjectCS`
  applies the matching ghost pressure on fluid–air faces. Coarse MG levels keep the counted stencil: it is a
  preconditioner, so only level 0 must be exact.
- `CouplingForceCS` evaluates pressure on the body surface: a gradient shift for fluid cells, face extrapolation for the
  body's own surface cells.
- The calm-pool ground truth is in PHYSICS_COUPLING Addendum 5. Cost impact is not measurable (< 0.02 ms at 64³).

## 12. Two-sided density projection [Impl, Meas]

`DensityProjectionRhsCS` writes the right-hand side and zeroes q. The projection then reuses `PressureRBGSCS` or
`AddPressureSolveMGPCG` with different buffer bindings (`Pressure := DensityQ`, `Divergence := DensityRhs`, same
`CellDiag`), so there is no second solver to maintain. `G2PAdvectCS` adds `SampleDensityDisplacement` (−∇q, trilinear,
≤ 0.5 cells) to particle positions on the last substep. The velocity-based divergence target is disabled while the
projection is active. Cost is 0.37–0.41 ms per frame at 64³ and 0.85 ms at 128³ (PHYSICS_COUPLING Addendum 6).

## 13. Sparse bricks (compute and storage) [Impl, Meas]

- **Storage:** a brick pool. Per-cell buffers hold `NumPoolBricks` × 512 cells, and per-face buffers 3 planes of that
  (face `(a, F)` = `a * PoolCells + CellIndex(F)`).
  - `CellIndex(C)` = `BrickTable[C >> 3] * 512 + local(C & 7)`.
  - Slot 0 is the zeroed null brick (unallocated and out-of-grid cells); nothing writes it. P2G drops writes that
    resolve there.
  - Max-side boundary faces are not stored.
- **Allocator** (per substep, before BuildSolid/P2G):
  - The touch flags come from `G2PAdvectCS` (end positions, deduplicated per group) and `EmitParticlesCS`.
  - `BrickFreeCS` dilates the touched set by one brick and pushes unwanted slots onto a free stack.
  - `BrickAllocCS` pops slots for new bricks, counts overflow in `Stats[8]` and builds the active list.
  - `PoolArgsCS` writes 3 indirect entries (cells, MG level-1 cells, new slots).
  - `ClearNewSlotsCS` zeroes the warm-start state of recycled slots.
- **Iteration:** `SparseCell()` maps a thread either through the active list (indirect) or by a scan of every pool slot
  (direct, free slots exit). Both visit exactly the allocated bricks. The choice is made per substep from the lagged
  pool usage: the list is used only below `SparseMaxActiveFraction` (0.5).
- Face passes are per cell (`SLOSH_FACE_KERNEL`): each cell owns its three min faces.
- Allocator kernels bind the pool tables as UAVs (`RW*`), and solver kernels bind them as SRVs; a pass never binds both.
- MGPCG level 0 is pooled and sparse (`bPooledFine`, `FineIndex`, `SparseFine` / `SparseCoarse`, `AddMGPassL0`;
  indirect args entry 0 = fine cells, entry 1 = level-1 cells). Dot-product finalize ignores partials the sparse
  dispatch did not write. Coarse levels stay dense.
- Engine constraint: SM5 global shaders allow 8 UAVs, so read-only inputs (`ActiveBricks`, `DiagF`) are bound as SRVs.
- Results and limits: PERFORMANCE_PLAN §10–10c.

## 14. Advection robustness [Impl, Meas]

`G2PAdvectCS` uses RK2. The midpoint velocity blends to forward Euler with the particle's own grid velocity as the
midpoint distance (0.5·dt·|v|/h) goes from 1 to 2 cells, and stencil faces outside the known band take the particle's
velocity instead of 0. Without this, fast droplets on fine grids sampled still liquid or empty faces far ahead, hovered,
and accelerated to MaxSpeed (R-21, PERFORMANCE_PLAN §10d). Diagnostic: `Stats[9..13]` (clamped count, centroid,
in-solid count).

## 15. Particle buffers and dispatch shape [Impl, Meas]

- `ParticleCapacity` is an upper bound. `FSloshProxy::EnsureParticleCapacity` grows the particle, sort and rank buffers
  before emission (first allocation = initial fill, then at least 1.5×, aligned to 16k slots) and copies the live particles.
- All 1D dispatches use `Slosh::GroupCount1D`: at most 32768 groups per row, the rest in Y. Kernels index threads with
  `LinearThreadId(DTid, GroupSize)` and groups with `LinearGroupId`. MG dot-product partials ignore the extra groups of
  a wrapped last row (`GroupId < NumPartials`). PERFORMANCE_PLAN §10e.
- Particles are stored as `FSloshParticlePacked` (48 B: position, age, velocity, APIC matrix as 9 halves).
  `PackParticle` / `UnpackParticle` convert to the logical `FSloshParticle`, and the sort moves packed particles as-is.
- Simulated time per frame is capped by `MaxFrameDt` (1/30 s), and the sort runs every `SortInterval` (4) frames.
  PERFORMANCE_PLAN §10f.
- The density-projection displacement is precomputed per face (`DensityDisplacementCS`, into the spare velocity buffer
  that G2P binds as `VelDst`) and sampled trilinearly in G2P (PERFORMANCE_PLAN §10g).
