# Slosh simulation

This document walks through the Slosh solver step by step, in execution order, with the shader entry point
that implements each step. It covers the discretisation, every kernel's numerics, rigid-body coupling,
manipulators, every solver setting and its default, known limits, and measured performance.

Read [Architecture.md](Architecture.md) first for the threading model, buffer layouts (brick pool, MAC faces,
particle packing) and the per-frame pass list. Shader files are under `SloshDemo/Plugins/Slosh/Shaders/Private/`,
C++ under `SloshDemo/Plugins/Slosh/Source/`. The test scenes and flags used for the measurements quoted here are in
[Testing.md](Testing.md); symptoms and fixes are in [Troubleshooting.md](Troubleshooting.md).

Background design notes in `Research/` predate parts of the code. Where they disagree with this document, the code
(and this document) is current.

---

## 1. Method and notation

Slosh is a GPU FLIP/APIC solver (Zhu and Bridson 2005; Jiang et al. 2015) on a staggered MAC grid, with a
multigrid-preconditioned CG or red-black SOR pressure projection, a ghost-fluid free surface, a two-sided
position-based density projection, analytic SDF colliders, and delayed force feedback to Chaos rigid bodies. The step
order follows Blender FLIP Fluids (MIT, used as an algorithmic reference only): P2G, mark cells, body forces,
viscosity, pressure, extrapolate, G2P/advect.

| Symbol | Meaning | Code |
|---|---|---|
| `h` | cell size, m | `CellSize`, `InvCellSize` |
| `dt` | substep, s | `Dt` |
| `rho` | rest density, kg/m^3 | `FSloshSolverSettings::Density` |
| `N` | particles per cell at rest = 8 | `TargetParticlesPerCell` (hard-coded in `Step_RenderThread`) |
| `u_f` | velocity component on face f, m/s | `Vel[FaceIndex(Axis, F)]` |
| `p_hat` | scaled pressure `p dt / (rho h)`, m/s | `Pressure` |
| `Div_c` | `sum over axes (u_{c+e} - u_c)`, m/s (not divided by h) | `Divergence` (stores the right-hand side) |
| `rho_c` | smooth relative density: P2G weight on the cell's faces / (faces * N) | `CellDensity`, `CellDensityInterior` |

With these scalings the pressure equation and projection are

```
diag_c * p_hat_c - sum_{fluid n} p_hat_n = -Div_c          (Neumann at solids, Dirichlet p = 0 at the free surface)
u_f  <-  u_f - (p_hat_b - p_hat_a)                          (a = cell on the minus side of face f, b = plus side)
p [Pa] = p_hat * rho * h / dt                               (PressureToPascal, used only for coupling forces)
```

`diag_c` counts fluid neighbours as 1, air neighbours as `1/theta` (ghost fluid, section 5.7) and solid neighbours as 0.

Cell types live in `CellType` bits 0-1: `CELL_AIR` 0, `CELL_FLUID` 1, `CELL_SOLID` 2. Bit 2 (`CELL_DENSE_FLAG`) marks fluid
cells with at least `0.5 N` particles; bits 8-15 hold the collider index + 1 (0 = domain wall). Face flags:
`FACE_VALID` (carries a meaningful velocity), `FACE_SOLID` (touches a solid; holds the solid's normal velocity),
`FACE_HAS_WEIGHT` (P2G deposited weight), and bit `4 + n` (filled by extrapolation layer n).

---

## 2. One frame

```
FSloshProxy::Step_RenderThread
  EnsureParticleCapacity
  for each emit request:   EmitReserveCS, EmitParticlesCS                     (section 3)
  every SortInterval frames (and every 8 with drains):  spatial sort + compaction (section 4)
  if not paused:
    Substeps = clamp(ceil(v_est * FrameDt / (CFL * h)), MinSubsteps, MaxSubsteps)   (section 5.0)
    for each substep:
      BrickFreeCS, BrickAllocCS, PoolArgsCS, ClearNewSlotsCS                   (5.1)
      BuildSolidCS                                                            (5.2)
      P2GCS                                                                   (5.3)
      MarkCellsCS                                                             (5.4)
      NormalizeFacesCS            gravity, solid face velocities              (5.5)
      ViscosityJacobiCS x K       if nu > 0                                   (5.6)
      DivergenceCS                rhs + ghost-fluid diagonal                  (5.7)
      MGPCG or PressureRBGSCS                                                 (5.8)
      ProjectCS                   + binary coupling, seepage contacts         (5.9)
      CouplingForceCS             smooth coupling force                       (5.10)
      ResidualCS                  last substep, diagnostics                   (5.11)
      DensityProjectionRhsCS + solve   last substep, every Interval frames    (5.12)
      ExtrapolateCS x L                                                       (5.13)
      DensityDisplacementCS       when the projection ran                     (5.12)
      G2PAdvectCS                 transfer, RK2, forces, collisions, drains   (5.14)
    SubmergedCS                                                               (6)
  AccumulateStatsCS                                                           (6)
  readback copies
```

---

## 3. Emission

### 3.1 Sources (game thread)

All emission is expressed as `FSloshEmitRequest` (`SloshTypes.h`) in sim space and queued in `ASloshVolume::PendingEmits`:

| Source | Shape | Count |
|---|---|---|
| `bFillOnBeginPlay` + `InitialFillBox` (default `(-155,-155,-115)` to `(-75,155,15)` cm, domain-local) | `FillCells` | `cells * 8` |
| `ASloshVolume::FillBox(WorldCenter, Extent)` | `FillCells` (domain-local axis-aligned box) | `cells * 8` |
| `ASloshVolume::EmitSphere(WorldCenter, Radius, WorldVelocity, Count)` | `Sphere` | `Count` |
| `USloshEmitterComponent` inside the domain | `Sphere` or `Box` (oriented with the component) | rate and bursts, below |

`QueueFillLocalBox` converts the box to cell indices (floor of the min corner, ceil of the max corner), clamped to
`[1, N-1]` so the wall layer stays empty.

For emitter components, `ASloshVolume::Tick` computes, per frame:

```
ParticlesPerLiter = 8 / (h^3 * 1000)                 (h in m)
Carry += Rate * DeltaSeconds                          Rate = ParticlesPerSecond, or LitersPerSecond * ParticlesPerLiter
Count  = floor(Carry); Carry -= Count
Count += PendingBurst + round(PendingBurstLiters * ParticlesPerLiter)
Count  = min(Count, MaxParticles - EmittedParticles)  if MaxParticles > 0
Velocity = Forward * Speed (+ (Location - PreviousLocation) / DeltaSeconds if bInheritVelocity)
```

The sphere radius and box extent are scaled by the component's largest absolute scale. Emission is skipped while
`bSimulate` is false (bursts stay pending).

### 3.2 GPU emission (`EmitReserveCS`, `EmitParticlesCS` in `Slosh.usf`)

`EmitReserveCS` (one thread) reserves `EmitCount` slots at the GPU count; `EmitParticlesCS` writes them and pads the
range between the GPU count and the CPU bound with dead particles (the bookkeeping is in
[Architecture.md section 7](Architecture.md#7-particle-count-model)). Each thread draws three hashes
`H0 = PcgHash(I * 3 + EmitSeed * 7919)`, `H1 = PcgHash(H0)`, `H2 = PcgHash(H1)` (`EmitSeed` increments per request).

| `EmitMode` | Position |
|---|---|
| 0 `FillCells` | Particle `I` goes to cell `I / 8` of the box and sub-cell `I % 8` of a 2x2x2 split, at the sub-cell centre plus a jitter of +-0.2 cell per axis: `InCell = (Sub + 0.5 + 0.8 (Rnd - 0.5)) * 0.5` |
| 1 `Sphere` | Uniform in volume: `cos(theta)` uniform in [-1,1], `phi` uniform, `r = R * u^(1/3)` |
| 2 `Box` | Uniform in the oriented box: `Center + QuatRotate(EmitRotation, (2 Rnd - 1) * Extent)` |

Positions are clamped to `[1.01 h, (N - 1.01) h]`.

**Velocity spread and jitter.** If `|Velocity| > 1e-6` and `VelocitySpread > 0` or `SpeedJitter > 0`, the direction is
drawn uniformly over the spherical cap of half-angle `VelocitySpread` around the emit direction
(`cos(a)` uniform between `1` and `cos(VelocitySpread)`, azimuth uniform), and the speed is multiplied by
`max(1 + SpeedJitter * (2u - 1), 0)`. Emitter components pass `VelocitySpread` in degrees (converted to radians on the
game thread) and `SpeedVariation` as `SpeedJitter`.

New particles have `Age = 0` and a zero affine matrix. The thread marks the brick containing the particle in `BrickTouch`
so it gets storage in the next brick allocation (section 5.1).

Emission stops at capacity: the CPU clamps each request to the free slots below `min(ParticleCapacity, allocated)`.

---

## 4. Spatial sort and compaction (`SloshSort.usf`)

Runs after emission and before the substeps when `SimFrame % SortInterval == 0` (default every 4th frame), and every
8th frame whenever a drain is present. `FSloshProxy::AddSpatialSort` dispatches five passes over `NumParticles`:

1. `SortCountCS`: `rank_i = InterlockedAdd(SortCounts[key_i], 1)`. The key is the linear index of the particle's 4x4x4
   cell block (`SortKey`), or `NumSortKeys - 1` for dead particles.
2. `SortScanBlocksCS`: exclusive scan of the counts in 1024-key blocks (256 threads x 4 keys, Hillis-Steele in shared
   memory), writing block totals to `ScanBlockSums`.
3. `SortScanBlockSumsCS`: single-group exclusive scan of the block totals.
4. `SortScatterCS`: `ParticlesOut[SortOffsets[key] + ScanBlockSums[key / 1024] + rank] = Particles[i]` (packed, unmodified).
5. `SortWriteCountCS`: `ParticleCount[0] = start of the dead key's range` (the live count after compaction).

The sorted buffer then becomes the live buffer (`Swap(ParticleBuffer, ParticleSortBuffer)`).

Purpose: P2G and G2P threads in one 128-thread group touch neighbouring faces, which makes the tiled P2G (section 5.3)
effective and the G2P gathers cache-friendly. One key per 4^3 block keeps a group within one or two blocks and makes the
count arrays 64x smaller than per-cell keys. Sorting every 4 frames instead of every frame cost P2G/G2P at most 0.4 ms
per substep and saved more than that in every scene tested (`Research/PERFORMANCE_PLAN.md` section 10f). For very large
particle counts the sort is expensive (6.0 ms per sorted frame at 9.8M particles with the 48-byte layout), so raise
`SortInterval` there.

Order within a key comes from atomic ranks, so particle order is not reproducible run to run. Grid sums are integer and
order-independent, but anything keyed on the slot index (the drain probability hash) is not.

---

## 5. Substep

### 5.0 Substep count, CFL and `MaxFrameDt`

```
FrameDt   = clamp(DeltaTime, 1e-4, clamp(MaxFrameDt, 1/240, 1/10))
v_est     = max(LastMaxSpeed, 1 m/s) + |g| * FrameDt                 LastMaxSpeed: Stats[0] read back ~3 frames ago
Substeps  = clamp(ceil(v_est * FrameDt / (max(CFL, 0.1) * h)), max(MinSubsteps,1), max(MaxSubsteps,1))
dt        = FrameDt / Substeps
```

`CFL` is in cells per substep (default 2). With the defaults (5 cm cells, CFL 2, 60 FPS) `Substeps = ceil(v_est / 6 m/s)`:
one substep covers speeds up to about 5.8 m/s, and the 4-substep cap is reached at about 18 m/s.

`MaxFrameDt` (default 1/30 s) caps simulated time per frame. Below 30 FPS the liquid runs in slow motion instead of
simulating the whole frame time: simulating more time needs more substeps, which makes the frame slower still (a
feedback spiral observed at 9.8M particles; `Research/PERFORMANCE_PLAN.md` section 10f, `Research/RISK_REGISTER.md` R-22).

The substep count uses lagged speed, so a sudden impact can run under-substepped for about three frames. The RK2/Euler
blend (section 5.14) keeps that case stable.

Per substep, `Step_RenderThread` also sets:

- `ViscosityAlpha = nu * dt / h^2`
- `PressureToPascal = rho * h / dt`
- `WarmStartScale = dt / dt_prev` (`PrevDt` persists across frames), which keeps the warm-started pressure physically
  constant when `dt` changes, because `p_hat` scales with `dt`.

### 5.1 Brick allocation

Grid storage exists only for 8^3 bricks within one brick of a particle (layout in
[Architecture.md section 6.2](Architecture.md#62-mac-grid-and-brick-pool-layout)). At the start of every substep:

| Kernel | Threads | Work |
|---|---|---|
| (touch) | - | `BrickTouch[b] = 1` for bricks containing a particle: written by `G2PAdvectCS` at end positions of the previous substep (deduplicated per group through `TouchBrick` shared memory) and by `EmitParticlesCS` |
| `BrickFreeCS` | `NumBricks` | `BrickWant = any touched brick in the 3x3x3 brick neighbourhood`. Allocated bricks no longer wanted clear `BrickTable`, set `SlotBrick = 0xFFFFFFFF` and push their slot onto `FreeSlots`. Thread 0 resets `ActiveBricks[0]` and `NewSlots[0]` |
| `BrickAllocCS` | `NumBricks` | Clears `BrickTouch`. Wanted bricks without storage pop a slot (`InterlockedAdd(FreeSlots[0], -1)`); if the stack was empty, `Stats[8]++` and the brick stays on the null brick. New slots are listed in `NewSlots`. Every allocated, wanted brick appends itself to `ActiveBricks` |
| `PoolArgsCS` | 1 | Repairs a negative free count, writes the three indirect argument triples (active cells, level-1 MG cells, new slots), `Stats[7] = max(Stats[7], active)` |
| `ClearNewSlotsCS` | indirect, new slots | Zeroes `Pressure`, `CellCount` and `DensityQ` of recycled slots: the only per-cell values read before they are rewritten in a substep |

Dilation by one brick (8 cells) covers the P2G and G2P stencils, the extrapolation band and the RK2 midpoint reach.
Allocation costs about 0.01 ms (`Research/PERFORMANCE_PLAN.md` section 10c).

**Overflow.** The pool holds `ceil(SparseStorageBudget * NumBricks)` slots. Bricks beyond it get no storage: P2G drops
their contributions, they read as air, and `G2PAdvectCS` moves their particles ballistically (section 5.14).
`ASloshVolume::GetPoolOverflowBricks()` reports the count. A splash can need about 1.8x the calm brick count, so size the
budget for the peak (`Research/PERFORMANCE_PLAN.md` section 10c).

Every grid kernel below iterates exactly the allocated bricks, either through the active list (indirect) or by scanning
pool slots; see [Architecture.md section 6.3](Architecture.md#63-active-bricks-and-dispatch-shape).

### 5.2 `BuildSolidCS`: solids

Per allocated cell:

- Cells with any index equal to 0 or `N-1` become `CELL_SOLID` with collider id 0 (domain walls).
- Otherwise the cell centre `x_c = (C + 0.5) h` is tested against every collider's SDF; the collider with the most
  negative SDF wins and the cell becomes `CELL_SOLID | (k + 1) << 8`.
- Everything else is reset to `CELL_AIR` (fluid is marked after P2G).

Collider SDFs (`ColliderSDF` in `SloshCommon.ush`), evaluated in the collider's local frame
`L = QuatRotateInv(Rotation, P - Center)`:

| Shape | SDF | `Extent` |
|---|---|---|
| Sphere (0) | `length(L) - r` | `x = r` |
| Box (1) | `length(max(|L| - e, 0)) + min(max_component(|L| - e), 0)` | half extents |
| Capsule (2) | `L.z -= clamp(L.z, -hh, hh); length(L) - r` | `x = r`, `y = half height of the straight part` (local Z) |

`ColliderNormal` takes central differences of the SDF with `eps = 0.25 h`. `ColliderVelocityAt(P) = v + omega x (P - Center)`.
Colliders arrive as `FSloshColliderGPU` (80 bytes, sim space) built by `USloshColliderComponent::BuildGPUCollider`: physics
velocity for simulating bodies (corrected to the component origin), a finite difference of the transform for kinematic
or animated ones.

Solids are binary per cell (no fractional face weights). A collider thinner than about one cell may not cover any
cell centre and is then invisible to the grid; particles still collide with it analytically in G2P.

### 5.3 `P2GCS`: particle to grid

Each live particle scatters, for each MAC component, momentum and weight to the 8 nodes of its trilinear stencil
(nodes at that component's face positions):

```
w_n   = trilinear weight of node n
mom_n = w_n * (u_p + C_p . (x_n - x_p))      APIC (C_p = the particle's affine row for this component)
mom_n = w_n * u_p                             FLIP (C_p ignored)
Accum[F_n]              += round(mom_n * 2^20)
Accum[TotalFaces + F_n] += round(w_n   * 2^20)
CellCount[cell(x_p)]    += 1
```

Sums are 32-bit fixed point (`P2G_FIXED_SCALE` = 2^20), so they are exact and independent of particle order. The
comment in `SloshCommon.ush` bounds momentum per face well below `2^31 / 2^20 = 2048`.

**Tiling.** `P2GCS` accumulates into a groupshared tile of 8^3 nodes x 3 components x (momentum, weight) = 3072 ints
(12 KB). The tile origin is the group's minimum particle cell minus 1 on each axis (`InterlockedMin` on `TileOrigin`);
contributions inside the tile use shared-memory atomics, others fall back to global atomics. After a barrier each thread
flushes its share of non-zero tile entries with one global atomic per face. Because particles are spatially sorted,
nearly all contributions hit the tile. Writes that resolve to the null brick are dropped. Measured gain: P2G 4.1x faster
at 128^3 and 6.4x at 64^3 (`Research/PERFORMANCE_PLAN.md` section 9). A bounding-box tile was tried and measured slower
(section 10g).

### 5.4 `MarkCellsCS`

Per allocated non-solid cell: `CELL_FLUID` if `CellCount > 0`, else `CELL_AIR`; add `CELL_DENSE_FLAG` if
`CellCount >= 0.5 N`. Fluid cells keep their pressure as a warm start, multiplied by `WarmStartScale`; other cells get
`p_hat = 0`. Fluid cells increment `Stats[1]`.

### 5.5 `NormalizeFacesCS`: normalise, gravity, solid boundary

Face pass (each cell visits its three minimum faces):

```
u = (W > 0) ? Mom / W : 0                  (Mom, W from Accum; both cleared if non-zero, ready for the next substep)
FaceWeight = W / 2^20
FACE_HAS_WEIGHT if W > 0
VelOld = u                                  FLIP mode only (the FLIP reference)
```

Then, from the two adjacent cell types A (minus side) and B (plus side):

| Case | Velocity | Flags |
|---|---|---|
| both solid | solid velocity (discarded later: left invalid, so extrapolation fills it from the fluid side) | none |
| one solid | normal component of the solid's velocity at the face, 0 for walls | `VALID | SOLID` |
| at least one fluid | `u + g_axis * dt` (gravity) | `VALID` |
| air/air | 0 | none |

Solid interiors are deliberately not given the body's velocity: doing so caused spurious no-slip drag on liquid beside a
moving body (`Research/PHYSICS_COUPLING.md`, Prototype findings). Only the normal component is imposed (free slip);
tangential friction is applied to particles in G2P.

### 5.6 `ViscosityJacobiCS`: implicit viscosity

Runs only when `ViscosityAlpha = nu dt / h^2 > 0` and `ViscosityIterations > 0`. Solves `(I - nu dt Laplacian) u = u*`
with `ViscosityIterations` (default 12) Jacobi iterations per substep:

```
u_new = (u*_f + alpha * sum_{6 neighbours} u_n) / (1 + 6 alpha)
```

`u*` stays in `Vel[0]`; iterates ping-pong between `Vel[1]` and `Vel[2]`. Neighbours that are not `FACE_VALID` (or lie
outside the grid) contribute the centre value (zero gradient). Solid faces and non-valid faces are copied unchanged.

Jacobi with a fixed iteration count under-solves large `alpha`: the error contraction per iteration is about
`6 alpha / (1 + 6 alpha)`. Very thick liquids therefore end up less viscous than `nu` implies; pair them with
`BoundaryFriction` and `Adhesion` (see [Materials.md](Materials.md)).

### 5.7 `DivergenceCS`: right-hand side and operator diagonal

Per allocated cell (also clears `CellCount` for the next substep's P2G):

```
non-fluid:   Divergence = 0, CellDiag = 0
fluid:       Div   = sum_axes (u_{c+e} - u_c)
             rho   = sum of FaceWeight on the 6 faces / (6 N)
             Target = min(DensityCorrection * max(rho - (1 + DensityDeadband), 0), 0.5) * h / dt
             Divergence = Div - Target
             CellDiag   = sum over 6 neighbours: fluid 1, air 1/theta, solid 0
```

**Velocity density correction (legacy).** `Target` is non-zero only when `bTwoSidedDensityProjection` is off
(`Step_RenderThread` sets `DensityCorrection = 0` otherwise). It asks the projection to leave a positive divergence in
over-dense cells so they expand. The density comes from the smooth P2G weights, not integer counts: integer counts flicker
as particles cross cells and injected frame-to-frame pressure noise (+12-17% buoyancy bias, about 3x force jitter;
`Research/PHYSICS_COUPLING.md` Addendum 4). The dead-band avoids slow inflation of a one-sided correction (Addendum 5).

**Ghost-fluid free surface** (Gibou et al. 2002; Bridson 2015 section 5.4). The interface is the 0.5 iso-level of the
smooth density, linearly interpolated between a fluid cell and its air neighbour:

```
theta = clamp((rho_f - 0.5) / (rho_f - rho_a), 0.05, 1)        GhostTheta(); theta = 1 if rho_f - rho_a <= 1e-4
```

The air neighbour contributes `1/theta` to the diagonal, which imposes `p = 0` at the interface instead of at the air cell
centre. `bGhostFluid = false` makes every air face weigh 1 (classic first-order boundary). The same `CellDiag` is used by
RBGS, by MGPCG's level 0 (`DiagF`) and, through the matching ghost pressure, by `ProjectCS`. Measured effect in the calm
pool: floating-box height error 11.7 to 8.9 cm and bobbing SD 2.6 to 1.5 cm before the density projection removed the
remaining bias (`Research/PHYSICS_COUPLING.md` Addendum 5).

### 5.8 Pressure

`ASloshVolume::PressureSolver` selects the solver in `BuildSolverSettings`:

| Choice | Behaviour |
|---|---|
| `Auto` (default) | RBGS when `GridResolution.X * Y * Z <= AutoSolverCellThreshold` (400,000 dense cells), MGPCG above. The default 64x64x48 (196,608 cells) uses RBGS; 96x96x72 and larger use MGPCG |
| `MGPCG` | always MGPCG with `PCGIterations` (default 3) |
| `RBGS` | always RBGS with `PressureIterations` (default 60) |

Both solvers run a fixed iteration count; there is no convergence test and no CPU readback.

#### RBGS (`PressureRBGSCS`)

Red-black SOR, two passes per iteration (`Parity = (x + y + z) & 1`), over fluid cells with `CellDiag > 0`:

```
gs   = (sum_{fluid n} p_hat_n - Divergence_c) / CellDiag_c
p_hat_c += omega * (gs - p_hat_c)                      omega = SORWeight, default 1.7, clamped to [0.5, 1.95]
```

Warm-started from the previous substep. Cheap per iteration and with no reductions, but convergence degrades with depth:
60 iterations do not converge a 48-cell column from scratch, and the warm start hides it for slowly varying flow
(`Research/GPU_ARCHITECTURE.md` section 6).

#### MGPCG (`FSloshProxy::AddPressureSolveMGPCG`, `SloshPressureMG.usf`)

Conjugate gradient preconditioned by one geometric multigrid V-cycle (McAdams, Sifakis and Teran 2010):

- **Hierarchy.** Level 0 is the simulation grid stored in the brick pool (`bPooledFine`); each coarser level halves the
  dimensions (rounding up) until it has at most 512 cells, up to 8 levels. The default grid has 4 levels
  (64x64x48, 32x32x24, 16x16x12, 8x8x6). Coarse levels are dense buffers.
- **Coarse cell types** (`MGBuildTypeCS`, once per solve): air if any child is air, else fluid if any child is fluid,
  else solid.
- **Operator.** Level 0 uses the exact ghost-fluid diagonal (`bGhostDiag`: `DiagF + DiagEpsilon`, `DiagEpsilon = 1e-4`
  keeps sealed pockets non-singular). Coarse levels count non-solid neighbours (`+ DiagEpsilon`): they only form the
  preconditioner.
- **Transfers.** Piecewise-constant prolongation (`MGProlongCS`: `x_f += x_c(parent)`), restriction `R = P^T`
  (`MGRestrictCS`), coarse operator `4 L_coarse` (the Galerkin product for the 7-point stencil), so the coarse right-hand
  side is `(R r) / 4`.
- **Smoother.** Red-black Gauss-Seidel (`MGSmoothCS`, no over-relaxation): red then black on the way down, black then red
  on the way up (symmetric V-cycle). The first red sweep on each level is fused into the kernel that produced its
  right-hand side (`PCGInitCS` / `PCGUpdatePRCS` on level 0, `MGRestrictCS` on coarse levels), using the fact that x
  starts at 0. The coarsest level runs `MGCoarseIterations` (16) red-black sweeps inside one 256-thread group
  (`MGCoarseSolveCS`).
- **CG.** Warm start from the current pressure: `r = b - A p` (`PCGInitCS`), `z = M^-1 r`, `rho = r.z` (reduced in the
  last level-0 smoothing sweep), `d = z` (`PCGUpdateDCS`). Each iteration: `q = A d` with the `d.q` partials
  (`PCGApplyACS`), `PCGFinalizeDotCS`, `p += alpha d; r -= alpha q` (`PCGUpdatePRCS`), then V-cycle, `rho`, and
  `d = z + beta d`. The last iteration stops after the pressure update. All scalars stay in `PCGScalars` on the GPU.
- **Sparse level 0.** With the active list chosen, every level-0 kernel runs indirect over the active bricks;
  restriction runs over the 4^3 level-1 cells of each active brick (args offset 12); `PCGFinalizeDotCS` sums only the
  `4 * active` partials written. `FineNeighbourIndex` addresses same-brick neighbours at fixed slot offsets
  (+-1, +-8, +-64) and only looks up `BrickTable` across brick boundaries (V-cycle -6%, restriction -15%;
  `Research/PERFORMANCE_PLAN.md` section 10f).

One PCG iteration at 4 levels is 21 dispatches.

**Choosing.** Residual accuracy at 64x64x48 with density correction off (mean post-projection divergence, 1/s):
RBGS 60 = 0.0077 at 0.80 ms, MGPCG 8 = 0.011 at 0.96 ms, MGPCG 16 = 0.0008 at 1.85 ms. At 128x128x96, MGPCG reaches
RBGS-60 accuracy at about half the cost (`Research/PERFORMANCE_PLAN.md` section 7). Three PCG iterations matched four on
every accuracy test and saved 1.5 ms per substep at 9.8M particles (section 10h), hence the default of 3. A lighter
V-cycle was tried and was unstable (section 10h).

### 5.9 `ProjectCS`: projection and binary coupling

Face pass. Recomputes `FACE_VALID`/`FACE_SOLID` (keeping `FACE_HAS_WEIGHT`) and, by adjacent cell types:

| Case | Action |
|---|---|
| fluid/fluid | `u -= p_hat_b - p_hat_a`; `VALID` |
| fluid/air | ghost pressure in the air cell: `p_hat_air = p_hat_f - p_hat_f / theta` (theta from the two cells' densities), so the gradient is `-p_hat_f / theta` toward the interface; `VALID` |
| fluid/solid | velocity stays the solid's normal velocity; `VALID | SOLID`. For a two-way collider: wetted-face count `[6]`, fluid cell-centre velocity sum `[8..10]`, and, when `bSmoothCouplingForce` is off, the binary face force `p * h^2 * n_into_solid` and its torque about the collider centre `[0..5]` |
| solid/solid, different solids | **seepage contact** (below) |
| air/air, solid interior | nothing; left for extrapolation |

**Seepage pressure** (`EstimateSeepagePressure`, `AccumulateContact`). Binary cells leave no liquid layer between a body
and the floor or another body, so those faces would feel no pressure and a buoyant body resting on the bottom would never
lift off. Each face between a two-way collider's cell and a different solid receives the pressure of the liquid around the
body at that height: walk up to 16 cells in +-X and +-Y out of the body, average the pressure of the fluid cells reached,
and apply it as a force into the body. If no liquid is reached (dry contact), no force. Contact faces add force and torque
only, not drag statistics. This models liquid seeping under the body; real sealed contacts (suction) are not represented.
A rho = 550 kg/m^3 box placed on the floor under 60 cm of water lifted within 0.27 s with this rule
(`Research/PHYSICS_COUPLING.md` Addendum 3).

### 5.10 `CouplingForceCS`: smooth pressure force

Runs when there are colliders and `bSmoothCouplingForce` is on (default). For every non-air allocated cell within one cell
of a two-way collider's surface (`|phi| <= h`):

```
surface pressure p_s:
   fluid cell:          p_s = p_hat_c + grad(p_hat) . (-phi * n)      gradient from fluid neighbours (central or one-sided)
   body's own surface:  p_s = mean over fluid neighbours of (p_n - 0.5 (p_{n+d} - p_n))   (extrapolated to the shared face)
F += -p_s * PressureToPascal * h^2 * sum_{6 faces} w_f * n_f,     w_f = saturate(0.5 + phi(x_f) / h)
T += (x_f - Center) x dF
```

This is the closed-surface identity (fluid faces plus the solid interface sum to zero) with the fluid face fractions taken
from the body's SDF instead of binary flags, so the force varies continuously as a body moves through the grid. A body
cell's faces shared with another solid are skipped (the seepage rule handles them); fluid cells keep all six faces. Cells
inside a different body are skipped. Evaluating pressure on the body surface instead of the cell centre removed a
`(H + h)/H` inflation of the hydrostatic force (+12.5% for a 40 cm box at 5 cm cells; `Research/PHYSICS_COUPLING.md`
Addendum 5). The smoothing itself gave no measurable noise reduction against the binary faces (Addendum 4); it is kept
because it is continuous for slow sub-cell motion and costs about 0.01 ms.

Forces are summed over substeps in fixed point and divided by the substep count on readback, so the reported force is the
substep average.

### 5.11 `ResidualCS`: diagnostics (last substep)

Per fluid cell: post-projection divergence rate `|Div| / h` into `Stats[2]` (max) and `Stats[3..4]` (sum, count). For
interior fluid cells (no air neighbour): signed deviation of `CellDensityInterior` from 1 into `Stats[5..6]`. These become
`GetMaxDivergence`, `GetMeanDivergence` and `GetMeanInteriorDensity` (1 = rest; above 1 means compression).

### 5.12 Two-sided density projection

FLIP particle distributions drift: liquid slowly compresses or inflates. Slosh corrects positions directly, after
Kugelstadt et al. 2019 ("Implicit density projection for volume conserving liquids"). It runs on the last substep of
frames where `SimFrame % DensityProjectionInterval == 0`, when `bTwoSidedDensityProjection` is on (default) and the
effective relaxation is positive.

1. **`DensityProjectionRhsCS`** (bound with `Divergence := DensityRhs`, `Pressure := DensityQ`): zeroes q and writes, per
   fluid cell,

   ```
   dev = CellDensityInterior(c) - 1         density from faces not shared with solids (their weight comes from one side only)
   dev = max(dev, 0)                        if the cell touches air (a partly filled surface cell reads low without being under-dense)
   rhs = -r_eff * clamp(dev, -0.5, 0.5)
   r_eff = 1 - (1 - DensityRelaxation)^Interval      Interval = clamp(DensityProjectionInterval, 1, 8)
   ```

2. **Solve** `A q = r_eff * dev` with the pressure operator, ghost-fluid diagonal included, by reusing the pressure solvers
   with swapped bindings: MGPCG with 2 PCG iterations, or RBGS with `DensityProjectionIterations` (30) sweeps at
   `omega = 1.7`. There is no second solver to maintain.
3. **`DensityDisplacementCS`** (after extrapolation): per face, displacement in cells `d_f = -(q_b - q_a)`, zero on faces
   touching a solid, written into the spare velocity buffer. Precomputing it per face instead of per particle saved 1.6 ms
   of G2P at 9.8M particles (`Research/PERFORMANCE_PLAN.md` section 10g).
4. **`G2PAdvectCS`** samples `d` trilinearly at the particle's start position, clamps its length to 0.5 cells, scales by
   `h` and adds it to the advected position. Velocities are not changed, so no momentum is injected.

Over-dense regions get a positive potential peak and particles move away from it; under-dense interior regions contract.
Results in the calm pool: floating box at its Archimedes height to within measurement resolution (-0.0 cm, SD 0.9 cm);
5-minute drift +0.000 %/min (calm, 615k particles) and -0.007 %/min (agitated, 262k); cost +0.37-0.41 ms per frame at
64^3 (RBGS 30) and 0.85 ms at 128^3 (MGPCG 2) (`Research/PHYSICS_COUPLING.md` Addenda 6 and 7). Running it every second
frame (`DensityProjectionInterval = 2`) kept volume and buoyancy within run-to-run spread and is opt-in
(`Research/PERFORMANCE_PLAN.md` section 10h).

When the projection is off, the velocity correction of section 5.7 applies instead (`DensityCorrection`, default 0.15,
dead-band 0.05). It adds about +10% to the measured buoyancy of submerged bodies (`Research/PHYSICS_COUPLING.md`
Addendum 4).

### 5.13 `ExtrapolateCS`: velocity extrapolation

`ExtrapolationLayers` (2) passes. Layer n fills every face that is neither `FACE_VALID` nor filled by an earlier layer
with the mean of its known, non-solid neighbours on the same axis (6-neighbourhood), and sets flag bit `4 + n`. Each layer
reads only flags set before it, so the result is deterministic. Faces with no known neighbour get 0. Solid faces are known
(they are not overwritten) but never used as seeds: spreading a body's normal velocity into its interior would leak body
motion into nearby particles through the G2P stencil.

Two layers cover the G2P stencil of particles near the surface. Fast particles whose RK2 midpoint lands beyond the band are
handled in G2P (section 5.14).

### 5.14 `G2PAdvectCS`: grid to particle and advection

One thread per slot below `NumParticles`; dead slots return immediately. `AdvectParticle(I)` does, in order:

**1. Transfer** (`GatherComponent`, trilinear over the 8 nodes of each component, nodes clamped to the grid):

```
APIC:  v_new = sum w_n u_n                    C_axis = sum grad(w_n) u_n      (stored as halves)
FLIP:  v_pic = sum w_n u_n
       delta = sum_{HAS_WEIGHT} w_n (u_n - u_old_n) / sum_{HAS_WEIGHT} w_n      (falls back to v_pic - v_p per component
                                                                               when the weighted sum is < 1e-4)
       v_new = lerp(v_pic, v_p + delta, FlipRatio)          C = 0
```

APIC skips the `VelOld` and `FaceFlags` reads, two thirds of the gather traffic (`Research/PERFORMANCE_PLAN.md` section 8).

**2. Advection** (RK2 midpoint through the post-projection field, with an Euler blend):

```
V1 = v_pic (the grid velocity at x_p, in both transfer modes)
mid = clamp(x_p + 0.5 dt V1)          MidCells = 0.5 dt |V1| / h
if MidCells < 2:  V2 = lerp(Sample(mid, fallback = V1, checkKnown = MidCells > 1), V1, saturate(MidCells - 1))
else:             V2 = V1
X = x_p + dt V2
```

Below one cell this is plain RK2. Between one and two cells it blends to forward Euler, and stencil faces outside the known
band (not valid, not extrapolated) take the particle's own velocity instead of the zero they store. Beyond two cells it is
pure Euler. This is the fix for R-21: on fine grids a fast droplet's midpoint sampled still liquid or empty faces several
cells ahead, the droplet stopped moving while keeping its velocity, and gravity accelerated it indefinitely, which pinned
the substep count at its maximum (`Research/PERFORMANCE_PLAN.md` section 10d: at 256x256x128 the stuck particles went from
4-66 per frame to 0, and post-impact frames from 43-57 ms to 20-31 ms).

**3. Density shift**: `X += SampleDensityDisplacement(x_p)` on projection substeps (section 5.12).

**4. Pool overflow fallback**: if the particle's start cell has no storage (null brick), it moves ballistically:
`v_new = v_p + g dt`, `X = x_p + dt v_new`, `C = 0`.

**5. Force fields** at `X` (section 7): add to `v_new`.

**6. Collider response**, for each collider k:

```
D = SDF_k(X); r = ParticleRadius = ParticleRadiusCells * h = 0.25 h
adhesion (if AdhesionAccel > 0 and D < r + AdhesionDistance):
    W  = saturate(1 - (D - r) / AdhesionDistance)
    vn = dot(v_new - v_solid(X), n)
    v_new -= n * min(a W dt, max(vn, 0) + 0.25 a W dt)        a = Adhesion * 1.5 * |g|
penetration (if D < r):
    X += n (r - D)
    rel = v_new - v_solid(X); vn = dot(rel, n)
    if vn < 0:  rel -= vn n;  rel_t *= (1 - BoundaryFriction)
    v_new = v_solid + rel
```

Adhesion pulls particles within reach toward the surface relative to its motion. It only slows separation plus a small
approach term, so it never accelerates a particle into the body beyond one substep of contact. `Adhesion = 1` (1.5 g) holds
a contact layer on an overhang against gravity. Friction removes tangential relative velocity only while the particle is
approaching or resting on the surface.

**7. Domain walls**: `X` is clamped per axis to `[1.01 h, (N - 1.01) h]`, and the velocity component into the wall is
removed. If any axis is in contact and `BoundaryFriction > 0`, the velocity components of the axes **not** in contact are
multiplied by `1 - BoundaryFriction`, so viscous liquids pool on the floor instead of sliding out into a single layer.

**8. Speed diagnostics**: if `|v_new| > MaxSpeed`, `Stats[9..13]` record the count, centroid and how many started inside a
solid cell (`GetClampedParticleCount()`). **The current code does not clamp the velocity**; `MaxSpeed` /
`MaxParticleSpeed` is a diagnostic threshold only (see section 10).

**9. Drains** (section 7): a particle inside a drain becomes dead (`Age = -1`) with probability `1 - exp(-RemovalRate dt)`
(always when `RemovalRate = 0`), using `PcgHash(I * 2654435761 + FrameSeed * 97 + d)` with
`FrameSeed = SimFrame * 16 + Step`. `Stats[15]++`.

**10. Write-back**: position, velocity, `Age += dt`, repacked. `Stats[0] = max(Stats[0], |v_new|)` feeds the next substep
count. The thread returns the brick of `X`, and the first thread of each run of equal bricks in the group sets `BrickTouch`.

Every substep ends by copying the final field to `Vel[0]`
([Architecture.md section 6.5](Architecture.md#65-velocity-buffer-roles)).

---

## 6. End of frame

**`SubmergedCS`** (once per frame, after the last substep, only with colliders). For every solid cell of collider k
(collider id != 0): `Coupling[k][11]++` (solid cells). The cell counts as submerged if any of four horizontal walks (+-X,
+-Y, up to 12 cells) first leaves the body into a fluid cell that has `CELL_DENSE_FLAG` and no air in the three cells below
it. Submerged cells add to `[7]` and their offset from the collider centre to `[12..14]`. Only horizontal walks are used: an
upward walk would count a puddle resting on top of the body as submersion, a positive feedback with buoyancy. The dense and
supported conditions keep thin sheets and airborne lumps from buoying a body. This sensor under-reads near the surface
(0.43-0.46 against 0.547 expected for the reference box; `Research/PHYSICS_COUPLING.md` Addendum 3). It runs for every
collider, including one-way ones.

**`AccumulateStatsCS`** (one thread, every frame): folds the frame's drained count into the running total and exposes the
GPU particle count; see [Architecture.md section 7](Architecture.md#7-particle-count-model).

---

## 7. Manipulators: force fields and drains

`USloshDrainComponent` and `USloshForceFieldComponent` (both `USloshManipulatorComponent`, after ZibraLiquids' void and
force-field manipulators; placeable as `ASloshDrainActor` / `ASloshForceFieldActor`) become `FSloshManipulatorGPU`
entries (80 bytes, at most 32 per volume) via `BuildGPU`. Units are converted to SI (cm/s^2 to m/s^2, cm to m).

Shapes reuse the collider SDFs (sphere, box, capsule along local Z) plus `Everywhere` (SDF = -1 for every point).

```
ManipulatorWeight(F, x) = 1                                  inside the shape (SDF <= 0)
                        = saturate(1 - SDF / Falloff)        outside, if Falloff > 0
                        = 0                                  otherwise
```

| Kind | Component setting | Effect on `v_new` per substep (W = weight at X) |
|---|---|---|
| `Drain` (0) | `USloshDrainComponent::RemovalRate` (1/s, 0 = instant) | removes particles strictly inside the shape (no falloff) with probability `1 - exp(-rate dt)` |
| `Directional` (1) | `ForceType = Directional`, `Strength` cm/s^2 | `+= Axis * Strength * W * dt`, Axis = component forward (X) |
| `Radial` (2) | `Strength` (negative attracts) | `+= normalize(X - Center) * Strength * W * dt` |
| `Swirl` (3) | `Strength`, `SwirlInwardPull` (`Param0`) | `+= (tangent - radial_unit * Param0) * Strength * W * dt`, tangent = `Axis x radial_unit`, Axis = component up (Z) |
| `Drag` (4) | `Strength` in 1/s | `*= exp(-Strength * W * dt)` |

Force fields act after advection, so they change the velocity the particle carries into the next substep's P2G (explicit,
first order). They run before collider response, so a field pushing liquid into a collider is cancelled at contact.
Force fields with `Strength` near zero and disabled manipulators are not uploaded. A drain with `Everywhere` empties the
volume. Drained slots are compacted by the next sort (at least every 8 frames while a drain exists).

---

## 8. Rigid-body coupling

Coupling is explicit and delayed: the GPU accumulates per-collider quantities (sections 5.9, 5.10, 6), the readback
delivers them about 3 frames later ([Architecture.md section 9](Architecture.md#9-asynchronous-readback-and-feedback)),
`USloshColliderComponent::ReceiveFeedback` turns them into a force and torque, and `TickComponent` (`TG_PrePhysics`)
applies them with `AddForce` / `AddTorqueInRadians` while the body is in the liquid.

### 8.1 Which colliders couple

- **Obstacle (always).** Every collider is a solid boundary for the grid (5.2, 5.5) and for particles (5.14), using its
  velocity.
- **Two-way flag.** `Flags = COLLIDER_FLAG_TWO_WAY` when `WantsTwoWayCoupling()` (mode is not `OneWay` and the attach
  parent, or else the owner's root, is a primitive simulating physics) or `bReportFeedbackWhenKinematic` is set. Only
  flagged colliders accumulate pressure force, torque, wetted faces and local fluid velocity.
- **Submersion** (`SubmergedCells`, `SolidCells`, centroid) is accumulated for every collider, so `GetSubmergedFraction()`,
  `IsInLiquid()` and the `OnEnterLiquid` / `OnExitLiquid` events work for one-way and kinematic colliders.

### 8.2 Feedback modes (`ESloshCouplingMode`)

| Mode | Fluid force used | Notes |
|---|---|---|
| `OneWay` | none | the body moves the liquid, the liquid never pushes back; default for skeletal colliders |
| `BuoyancyAndDrag` | Archimedes buoyancy from the submerged-cell estimate, applied at the submerged centroid, plus drag | never sticks to the floor, but bobs because the sensor is delayed |
| `PressureAndDrag` (default) | integrated solver pressure force and torque (including seepage contacts) plus drag | settles floating bodies stably; a rho = 550 box settled at submerged fraction 0.564 against 0.547 (`Research/PHYSICS_COUPLING.md` Addendum 2) |

### 8.3 Force model (`ReceiveFeedback`)

With `V_sub = SubmergedCells * h^3`, `f = clamp(V_sub / V_shape, 0, 1)` (analytic shape volume), body mass `m`, local fluid
velocity `v_f = MeanFluidVelocity`:

```
drag (N)        F_d = 0.5 * rho * DragCoefficient * (0.25 * WettedFaces * h^2) * |v_f - v_b| * (v_f - v_b)
PressureAndDrag F   = PressureForce + F_d                       T = PressureTorque (about the collider centre)
BuoyancyAndDrag F_b = -rho * V_sub * g * BuoyancyScale          F = F_b + F_d
                T   = (P_app - COM) x F_b,  P_app = collider location + SubmergedCentroidOffset
linear damping  F  += LinearDrag * f * m * (v_f - v_b)
angular damping T  -= AngularDrag * f * max(inertia) * omega_b
added mass      m_a = AddedMassCoefficient * rho * V_sub
                F_applied = (m F - m_a m g) / (m + m_a),  |F_applied| <= m * MaxFluidAcceleration
                T_applied = T * m / (m + m_a)
smoothing       Pending = lerp(Pending, new, 0.5)
```

(Forces in N are converted to Unreal units by x100, torques in N m by x10^4.) The implicit added-mass form keeps light
bodies stable under lagged explicit forcing (`Research/PHYSICS_COUPLING.md` section 5). `IsInLiquid` uses hysteresis: enter
above `EnterThreshold`, leave below half of it.

Collider coupling properties and defaults (`USloshColliderComponent`):

| Property | Default | Meaning |
|---|---|---|
| `CouplingMode` | `PressureAndDrag` | section 8.2 |
| `BuoyancyScale` | 1.0 | multiplier on Archimedes force (`BuoyancyAndDrag`) |
| `DragCoefficient` | 0.8 | quadratic drag on a quarter of the wetted area |
| `LinearDrag` | 3.0 1/s | heave/sway damping per unit submerged fraction |
| `AngularDrag` | 1.5 1/s | angular damping per unit submerged fraction |
| `AddedMassCoefficient` | 0.5 | added-mass coefficient (0.5 = sphere) |
| `MaxFluidAcceleration` | 2500 cm/s^2 | clamp on fluid-induced acceleration |
| `bReportFeedbackWhenKinematic` | false | accumulate pressure forces for kinematic bodies (probes, sensors) |
| `EnterThreshold` | 0.05 | submerged fraction for `OnEnterLiquid` |

### 8.4 Skeletal meshes

`USloshSkeletalColliderComponent` fits one collider per significant bone at `BeginPlay`
(`RebuildColliders`): each LOD0 vertex goes to its most-weighted bone; bones with fewer than
`max(12, MinVertexFraction * vertices)` vertices hand theirs to the parent; the `MaxCapsules` (40) bones with most vertices
keep a shape. Each point set gets a PCA fit: a capsule along the principal axis (length from the `LengthTrim`-trimmed
extent, radius the `RadiusPercentile` (0.8) percentile of distance from the axis), or an oriented box when the
cross-section is strongly flattened (`sqrt(l3/l2) < 0.55` and `sqrt(l2/l1) > 0.45`, typical for torso and hips). Shapes are
attached to their bones and follow animation; their velocity comes from the finite-differenced transform. The mesh needs
CPU-readable LOD0 data. Defaults to `OneWay`; `bReportWetness` sets `bReportFeedbackWhenKinematic` on the generated
colliders.

---

## 9. Settings

### 9.1 Solver settings

Every field of `FSloshSolverSettings` (`SloshTypes.h`), its default there, the `ASloshVolume` property that drives it and
that property's default. "Physical" means `FSloshPhysicalProperties`, taken from the volume's `Material`
(`USloshMaterialAsset`) when set, else from the inline `Physical` struct; see [Materials.md](Materials.md). Fields marked
"not exposed" keep the core default.

| Field | Core default | Driven by (volume default) | Effect |
|---|---|---|---|
| `GridDim` | 64, 64, 48 | `GridResolution` (64, 64, 48) | Cells per axis, clamped to [4, 256]. Changing it reallocates (wipes the liquid) |
| `CellSize` | 0.05 m | `CellSize` (5 cm, min 0.5) | `h`. Domain size = `GridResolution * CellSize` |
| `ParticleCapacity` | 262144 | `ParticleCapacity` (262144) | Maximum particles, clamped to [1024, 16M]; buffers grow on demand |
| `TransferMode` | APIC | Physical `Transfer` (APIC) | APIC or PIC/FLIP blend |
| `FlipRatio` | 0.95 | Physical `FlipRatio` (0.95) | FLIP mode only: 0 = PIC, 1 = FLIP |
| `Density` | 1000 kg/m^3 | Physical `Density` (1000) | Converts pressure to Pascal for coupling forces, and scales CPU buoyancy, drag and added mass; does not change the flow |
| `KinematicViscosity` | 0 m^2/s | Physical `KinematicViscosity` (0) | Enables the implicit viscosity passes (5.6) |
| `BoundaryFriction` | 0 | Physical `BoundaryFriction` (0) | Tangential velocity removed at collider and wall contact (5.14) |
| `Adhesion` | 0 | Physical `Adhesion` (0, max 2) | Attraction to collider surfaces in units of 1.5 g (5.14) |
| `AdhesionDistance` | 0.025 m | Physical `AdhesionDistance` (2.5 cm) | Reach of adhesion beyond the particle radius |
| `DensityCorrection` | 0.15 | Physical `DensityCorrection` (0.15) | Velocity density correction; ignored while the two-sided projection is on |
| `DensityDeadband` | 0.05 | `DensityDeadband` (0.05) | Velocity correction acts only above `(1 + deadband)` x rest density |
| `bTwoSidedDensityProjection` | true | same (true) | Position-based density projection (5.12) instead of the velocity correction |
| `DensityRelaxation` | 0.5 | same (0.5) | Fraction of the density error removed per projection |
| `DensityProjectionIterations` | 30 | not exposed | RBGS sweeps of the density projection (MGPCG always uses 2 PCG iterations) |
| `DensityProjectionInterval` | 1 | same (1, max 8) | Project every N frames; relaxation compounded to `1 - (1 - r)^N` |
| `bSparseBricks` | true | same (true) | Allow indirect dispatch over the active-brick list |
| `SparseMaxActiveFraction` | 0.5 | same (0.5) | Use the list only while pool usage (lagged) is below this |
| `SparseStorageBudget` | 1.0 | same (1.0, min 0.01) | Pool capacity as a fraction of all bricks. Changing it reallocates |
| `ParticleRadiusCells` | 0.25 | not exposed | Particle collision radius in cells (5.14) |
| `Gravity` | (0, 0, -9.81) | world gravity, rotated into the volume's axes | Body force (5.5) |
| `PressureSolver` | MGPCG | `PressureSolver` (`Auto`) with `AutoSolverCellThreshold` (400,000) | 5.8 |
| `PressureIterations` | 60 | same (60, max 400) | RBGS iterations |
| `SORWeight` | 1.7 | same (1.7, range 1.0-1.95) | RBGS over-relaxation (clamped to [0.5, 1.95] in the proxy) |
| `PCGIterations` | 8 | `PCGIterations` (3) | MGPCG outer iterations (clamped to [1, 100]) |
| `MGCoarseIterations` | 16 | not exposed | Red-black sweeps on the coarsest MG level (clamped to [1, 200]) |
| `SortInterval` | 4 | same (4, max 60) | Spatial sort every N frames, 0 = never (drains still force every 8) |
| `bSmoothCouplingForce` | true | same (true) | SDF-weighted coupling force (5.10) instead of binary faces |
| `bGhostFluid` | true | same (true) | Ghost-fluid free surface (5.7) |
| `ViscosityIterations` | 12 | same (12, max 100) | Jacobi iterations per substep |
| `ExtrapolationLayers` | 2 | not exposed | Velocity extrapolation layers (5.13) |
| `MinSubsteps` | 1 | not exposed | Lower bound on substeps |
| `MaxSubsteps` | 4 | same (4, max 16) | Upper bound on substeps |
| `MaxFrameDt` | 1/30 s | same (1/30 s, range 0.004-0.1) | Simulated time per frame at most (proxy clamps to [1/240, 1/10]) |
| `CFL` | 2.0 | same (2.0, range 0.25-10) | Cells a particle may travel per substep |
| `MaxSpeed` | 20 m/s | `MaxParticleSpeed` (2000 cm/s) | Threshold for the clamped-particle diagnostic (no clamp is applied; section 10) |

### 9.2 Hard-coded constants

| Constant | Value | Where |
|---|---|---|
| Rest particles per cell | 8 | `TargetParticlesPerCell` in `Step_RenderThread`; fill emission; `ParticlesPerLiter` |
| P2G fixed-point scale | 2^20 | `P2G_FIXED_SCALE` (`SloshCommon.ush`) |
| Coupling fixed-point scale | 1024 | `Slosh::CouplingFixedScale` |
| Ghost theta minimum | 0.05 | `GHOST_THETA_MIN` |
| MG diagonal epsilon | 1e-4 | `DiagEpsilon` in `AddPressureSolveMGPCG` |
| MG coarsest level | <= 512 cells, <= 8 levels | `MGCoarsestMaxCells`, `MGMaxLevels` |
| Density projection | MGPCG 2 iterations, RBGS omega 1.7, displacement <= 0.5 cell, rhs clamp +-0.5 | `Step_RenderThread`, `SampleDensityDisplacement` |
| Velocity density correction cap | 0.5 h / dt | `DivergenceCS` |
| Adhesion acceleration | `Adhesion * 1.5 * |g|` | `Step_RenderThread` |
| Seepage walk / submersion walk | 16 / 12 cells | `EstimateSeepagePressure`, `SubmergedCS` |
| Compaction with drains | every 8 frames | `Step_RenderThread` |
| Brick size / sort block | 8^3 / 4^3 | `SLOSH_BRICK`, `SortKey` |
| Thread group | 128 (MG finalize and coarse 256, scan 256) | `SLOSH_THREADS`, `MG_*_THREADS`, `SCAN_THREADS` |

### 9.3 Manipulator and emitter defaults

| Component | Property | Default |
|---|---|---|
| `USloshManipulatorComponent` | `Shape` / `Radius` / `BoxExtent` / `CapsuleHalfHeight` | Sphere / 25 cm / 25 cm / 25 cm |
| `USloshDrainComponent` | `RemovalRate` | 0 (instant) |
| `USloshForceFieldComponent` | `ForceType` / `Strength` / `SwirlInwardPull` / `Falloff` | Directional / 500 cm/s^2 / 0.3 / 10 cm |
| `USloshEmitterComponent` | `Shape` / `Radius` / `BoxExtent` | Sphere / 10 cm / 10 cm |
| | `RateMode` / `ParticlesPerSecond` / `LitersPerSecond` / `MaxParticles` | ParticlesPerSecond / 20000 / 1.0 / 0 (unlimited) |
| | `Speed` / `VelocitySpread` / `SpeedVariation` / `bInheritVelocity` | 300 cm/s / 0 deg / 0 / false |

---

## 10. Known limits

Numerics:

- **No speed clamp.** `MaxSpeed` is counted (`Stats[9..13]`) but never applied to particle velocities in
  `AdvectParticle` (`Slosh.usf`, the `Speed > MaxSpeed` block). Comments, property tooltips and `Research/` describe it as a
  clamp. CFL substepping and the RK2/Euler blend are what keep fast particles stable today.
- **Binary solids.** Cells are solid when their centre is inside a collider; no fractional face weights (Batty et al.
  2007). Boundaries are staircased, sub-cell bodies may be invisible to the grid, and lateral forces in strong currents
  carry staircase noise (50-180 N measured before the smooth estimator; `Research/PHYSICS_COUPLING.md` Addendum 2). Fast
  thin colliders can tunnel; particle collision only acts within `0.25 h` of the surface.
- **Fixed iteration counts.** Neither pressure solver checks convergence. RBGS degrades with depth; MGPCG with 3
  iterations leaves a residual (mean divergence about 0.06 1/s at 64^3 with 4 iterations and density correction off).
- **Viscosity.** Fixed-count Jacobi under-solves large `nu dt / h^2`; viscosity is uniform per volume; tangential no-slip
  at solids comes only from particle friction, not from the viscosity solve.
- **No surface tension or cohesion.** Adhesion acts only toward colliders, not between liquid particles or toward walls.
- **One liquid per volume.** No per-particle material, colour or phase. The domain is an axis-aligned box in actor space
  with solid walls; there are no open boundaries (use drains).
- **Pool overflow** makes particles ballistic until storage frees up.
- **Not reproducible run to run.** Atomic sort ranks reorder particles, and drain decisions hash the slot index. Coupling
  metrics vary about +-0.15 between identical runs because the agitated flow is chaotic (`Research/PERFORMANCE_PLAN.md`
  section 9).

Coupling:

- **Delayed and explicit.** Forces are 2-4 frames old. `BuoyancyAndDrag` shows a bobbing limit cycle; `PressureAndDrag`
  avoids it but still uses lagged forces, stabilised by added mass, damping, a clamp and 50% temporal smoothing.
- **Torque reference.** `PressureTorque` is accumulated about the collider centre, while `AddForce` applies the force at the
  body's centre of mass. When a collider is offset from the body's COM, the moment `(Center - COM) x F` is not applied.
- **Submersion sensor** (`SubmergedCS`) under-reads near the surface (section 6); body height is the more reliable measure.
- **Seepage** assumes liquid under every wet contact; suction is not modelled.
- **Density projection bias.** With the velocity correction instead of the projection, submerged bodies see about +10%
  buoyancy.

Scale (limits from [Architecture.md section 12](Architecture.md#12-architectural-limits-and-caveats)): 256 cells per axis,
16M particles, 64 colliders and 32 manipulators per volume.

---

## 11. Measured performance

All numbers are from `Research/PERFORMANCE_PLAN.md` and `Research/PHYSICS_COUPLING.md`, measured on an NVIDIA GeForce
RTX 5060 Laptop GPU (8 GB), UE 5.8.4, DX12, `-game` at 1280x720, Development build, with one `ProfileGPU` frame per
configuration (about +-20% run-to-run variance). Each row was measured at the stage of development noted; later
features (density projection, brick pool) changed some totals.

| Scene | Particles | Result | Source |
|---|---|---|---|
| 64x64x48 demo pool | 262k | sim 1.38 ms after tiled P2G; 2.2-2.5 ms with brick pool and density projection | PERFORMANCE_PLAN 9, 10c |
| 96x96x72 | 0.88M | sim + render about 6.4 ms at CFL 2 (1.71 substeps), before tiled P2G | PERFORMANCE_PLAN 8 |
| 128x128x96 agitated | 1.72-1.78M | substep 4.25-4.70 ms; sim + render about 6.2 ms at CFL 4, 16.3 ms at CFL 2 | PERFORMANCE_PLAN 9 |
| 128x128x96 shallow calm pool | 1.14M | frame GPU 7.9 ms (includes about 4 ms of base scene) | PERFORMANCE_PLAN 10g |
| 256x256x128, 20 cm deep | 9.8M | calm frame 32.0 ms (1 substep) | PERFORMANCE_PLAN 10h |

Per-pass costs at 128x128x96 with 1.7M particles, per substep (PERFORMANCE_PLAN 9): P2G 0.61-0.80 ms, NormalizeFaces
0.29 ms, MGPCG x4 1.5 ms, G2P + advect 0.74 ms, extrapolate x2 0.42-0.45 ms; sort 1.1 ms per sorted frame.

Other measured costs:

| Item | Cost | Source |
|---|---|---|
| Brick allocation (4 passes + indirect clear) | about 0.01 ms | PERFORMANCE_PLAN 10c |
| MGPCG V-cycle | 0.084 ms at 64^3, 0.153 ms at 96^3 | PERFORMANCE_PLAN 7 |
| MGPCG 3 vs 4 iterations | 6.4 vs 7.9 ms per substep at 9.8M; 0.95 vs 1.1 ms at 128^3 | PERFORMANCE_PLAN 10h |
| Two-sided density projection | +0.37-0.41 ms per frame at 64^3 (RBGS 30), 0.85 ms at 128^3 (MGPCG 2) | PHYSICS_COUPLING Addendum 6 |
| Sparse dispatch, 128^3 shallow pool (25% bricks active) | frame 10.70 to 9.14 ms (grid passes plus MGPCG level 0) | PERFORMANCE_PLAN 10b |
| Grid memory, 128x128x96 shallow pool | 213 MiB at budget 1.0, 76 MiB at budget 0.35 | PERFORMANCE_PLAN 10c |
| Grid memory, 256x256x128 shallow pool | 351 MiB at budget 0.3 | PERFORMANCE_PLAN 10c |

Accuracy (calm-pool ground truth, `-SloshCalm`, defaults): floating box at its Archimedes height (-0.0 cm, SD 0.9 cm),
submerged probe `Fz / (rho g V)` = 0.985, volume drift +0.000 %/min over 5 minutes; agitated pool -0.007 %/min
(`Research/PHYSICS_COUPLING.md` Addenda 6 and 7).

Where the time goes at scale: particle passes (P2G, G2P) and the per-frame sort scale with particle count; MGPCG level 0 is
bandwidth-bound at about twice the ideal time, 8 level-0 passes per PCG iteration (`Research/PERFORMANCE_PLAN.md`
section 10g). Substep count multiplies everything except the sort, so CFL is the strongest single lever (CFL 4 runs 128^3 in
budget at the cost of noisier coupling: mean residual divergence 1.0 against 0.54 1/s).

---

## 12. Invariants to keep when changing the solver

- **Accumulators clear themselves.** `NormalizeFacesCS` zeroes every `Accum` entry P2G touched; `DivergenceCS` zeroes
  `CellCount`. A new scatter pass must leave its accumulators zero for the next substep, or clear them itself.
- **Recycled slots.** A pool slot freed and reallocated keeps stale data. Anything read before it is written in a substep
  (today `Pressure`, `CellCount`, `DensityQ`) must be zeroed in `ClearNewSlotsCS`.
- **Null brick is read-only.** Indices below 512 are the shared null brick; check `IsNullCell` before writing.
- **Pressure is scaled.** `p_hat` scales with `dt`; keep `WarmStartScale` when touching the warm start, and convert with
  `PressureToPascal` for physical units.
- **Face flags are rebuilt each substep.** `NormalizeFacesCS` sets them, `ProjectCS` recomputes `VALID`/`SOLID` keeping
  `HAS_WEIGHT`, extrapolation adds layer bits. G2P's known-face test depends on them.
- **Dead particles.** Every particle pass must skip `IsDeadParticleAge(Age)` slots; only the sort moves them.
- **Brick touch.** A particle whose end position is not marked in `BrickTouch` gets no storage next substep. Any pass that
  moves particles must mark their bricks (deduplicated per group as `G2PAdvectCS` does).
- **UAV budget.** `G2PAdvectCS` and `BrickAllocCS` already use 8 UAVs
  ([Architecture.md section 10.1](Architecture.md#101-the-shared-parameter-struct)).
- **Validate with the ground-truth scenes.** Calm-pool float height and probe, multi-minute drift, floor-start lift, and
  both dispatch paths. See [Testing.md](Testing.md).
