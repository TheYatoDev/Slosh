# SOLVER_COMPARISON.md

**Project:** UE5.8 Interactive Fluid Simulation Plugin — Phase 0 (research only, no implementation)
**Document:** Liquid solver comparison: FLIP / APIC vs DFSPH, PBF / XPBD, NVIDIA FleX, WCSPH, MLS-MPM, hybrids
**Date:** 2026-11-03
**Use case:** Real-time gameplay liquids (water, blood, slime), two-way rigid-body coupling with Chaos, 60 FPS, aspirational total fluid budget ~4 ms/frame (simulation + coupling + rendering)
**Related deliverables:** `FLIP_FLUIDS_SOURCE_AUDIT.md` (authoritative for FLIP Fluids internals), `LICENSING_AUDIT.md`, `PHYSICS_COUPLING.md`, `LIQUID_MATERIALS.md`, `RENDERING_ARCHITECTURE.md`, `PERFORMANCE_PLAN.md`

---

## 0. Evidence legend

Every non-trivial claim in this document is tagged with one of:

| Tag | Meaning |
|---|---|
| **[Verified]** | Checked in this session against a primary source (license file, source file, header, GitHub API, `nvidia-smi`). Commit hash or date recorded in §3. |
| **[Paper]** | Stated in, or a direct consequence of, the cited publication. Not re-derived here. |
| **[Estimate]** | Engineering estimate by this author. Assumptions are stated. **Not a measurement.** |
| **[Unverified]** | Plausible or commonly reported but not checked in this session. Must be confirmed before it drives a decision. |

No benchmark in this document was executed. No published performance number is quoted unless attributed; where I could not re-verify a paper's number, I do not quote it.

---

## 1. Executive summary

* **Raw weighted-matrix result (§10):** PBF/XPBD scores **3.80 / 5**, APIC **3.71**, FLIP **3.58**, DFSPH **3.54**, FleX **3.42**, MLS-MPM **3.31**, WCSPH **2.99**. The gap between first and second (0.09) is **smaller than the effect of changing one score by one point on the 12-weight GPU-suitability criterion (0.12)**, and only slightly larger than a one-point change on any 6-weight criterion (0.06). The matrix therefore doesn't clearly separate PBF from APIC.
* **Sensitivity (§11):** The winner changes from PBF to APIC if any one of these happens: incompressibility weight +9, viscosity weight +5, numerical stability weight +9, or rendering weight +9. It also changes if GPU suitability or cohesion/adhesion is dropped as a criterion. No plausible shift lets FleX, WCSPH or MLS-MPM win. DFSPH wins only when rigid coupling or volume preservation is weighted at about 3x.
* **Recommendation (§12):**
  * **Primary: GPU-native APIC on a dense MAC grid, with an optional FLIP-delta blend.** FLIP Fluids' engine already supports both transfer modes (verified in source, §8.1). This is a judgment call that overrides a statistically insignificant matrix lead, for four stated reasons. (1) PBF's deficits are intrinsic to its solver: iteration-limited incompressibility and buoyancy fidelity, and no physically parameterized viscosity. Fixing them pushes PBF's cost toward DFSPH and erodes its main advantage. APIC's deficits are additive features at the free surface and boundaries: cohesion, adhesion and surface tension. (2) APIC's per-particle affine matrix is the velocity gradient an MPM-style viscoelastic stress model needs. That gives slime a growth path that reuses the same P2G/G2P kernels. (3) An MIT-licensed CPU reference engine with the same discretization (FLIP Fluids) is available for CPU-vs-GPU parity validation. (4) A grid answers gameplay queries (local velocity, fluid level, submerged fraction) and produces world-space SDFs naturally.
  * **Fallback: PBF/XPBD implemented in-house in HLSL/RDG** (not the FleX binary). Algorithmic references: PositionBasedDynamics (MIT, CPU) and the NVIDIA PhysX 5 particle-system CUDA source (BSD-3, verified present in the repo).
  * **Decision gate:** Phase 1 should run a small APIC vs PBF bake-off on the same dam-break + floating-box scene (Experiments 2 and 3). If the APIC pressure projection can't reach its sub-budget at gameplay scale (§6.4), switch to the fallback. If the project owner weights implementation speed or cohesion/adhesion above the weights stated here, **PBF is the better choice, and the matrix already favors it.**
* **Licensing (verified):** SPlisHSPlasH is MIT. PositionBasedDynamics is MIT. NVIDIA FleX is under the "NVIDIA Source Code License (1-Way Commercial)", which permits redistribution and commercial use but has **no patent grant**. **The FleX core solver ships as prebuilt binaries only**, the last push was 2021-04-15 (a license-file update), it is version 1.2.0, and it bundles the CUDA 9.2 runtime. NVIDIA PhysX 5 is BSD-3 and now includes CUDA source for its PBD particle system. CUDA is NVIDIA-only.
* **FleX is not recommended as a dependency.** The license is not the obstacle. The obstacles are its closed, unmaintained binary core, CUDA 9.2-era binaries of unknown compatibility with Blackwell (RTX 50-series), and the fact that it would own its own rigid bodies instead of Chaos.

---

## 2. Use-case constraints and assumptions

### 2.1 Hardware

| GPU | Facts | Tag |
|---|---|---|
| NVIDIA GeForce RTX 5060 Laptop GPU (this machine) | `nvidia-smi`: 8151 MiB, driver 617.42, max memory clock 12001 MHz, max SM clock 3090 MHz, max power limit 115 W, PCIe gen 5 max | [Verified] |
| | Memory bandwidth ≈ 384 GB/s, assuming a 128-bit bus and the reported 12001 MHz clock corresponds to ~24 Gbps GDDR7 | [Estimate] — bus width not reported by `nvidia-smi`; confirm with GPU-Z |
| | Laptop sustained clocks depend on thermal/power state; budgets must be measured under sustained load | [Estimate] |
| NVIDIA Quadro RTX 4000 (original brief) | Turing TU104, 8 GB GDDR6, 256-bit, ~416 GB/s, SM 7.5 | [Unverified] — public spec, not checked here |

Both GPUs have about 400 GB/s of memory bandwidth and 8 GB of VRAM. Bandwidth-bound kernels (grid sweeps, particle passes) should therefore be in the same order of magnitude on both. The Blackwell part has much higher clocks and (per public specs, [Unverified]) a much larger L2 cache. Dense-grid pressure iterations whose working set fits in L2 may therefore run disproportionately faster on the RTX 5060. **Do not extrapolate laptop timings to the Quadro without measuring.**

### 2.2 Budget split (provisional)

[Estimate] Assumptions: 1080p–1440p, one fluid domain, screen-space or SDF raymarch surface, asynchronous force readback.

| Sub-system | Provisional share of 4.0 ms |
|---|---|
| Simulation (transfers, pressure, viscosity, collisions) | 2.0–2.5 ms |
| Surface reconstruction + liquid shading | 1.0–1.5 ms |
| Coupling reductions, query writes, readback copies | 0.2–0.5 ms |

The solver therefore has roughly **2–2.5 ms**, not 4 ms. Every comparison below is read against that.

### 2.3 Hard technical constraints that affect solver choice

1. **DX12 / SM6 via UE5.8 RDG.** CUDA-only libraries (FleX CUDA backend, PhysX 5 GPU) need CUDA–D3D12 interop, add a vendor lock to NVIDIA, and sit outside RDG's resource tracking.
2. **No native float atomic add on UAVs in core HLSL** (to my knowledge; NVAPI and AGS expose vendor extensions). [Unverified for UE5.8 — `UE58_INTEGRATION.md` must check what UE exposes.] Scatter-heavy P2G (FLIP/APIC/MPM) must use fixed-point integer atomics, CAS loops, vendor extensions, or sort-then-gather. This matters less for SPH/PBF, which are gather-only once neighbors are sorted.
3. **Chaos owns rigid bodies.** Any solver that wants to own the rigid bodies itself (FleX's unified rigid particles) conflicts with the project mandate.
4. **GPU→CPU force feedback has ≥1–2 frames of latency** unless the frame stalls. This is shared by all GPU candidates and is covered in `PHYSICS_COUPLING.md`.

---

## 3. Candidates and reference implementations

### 3.1 Reference repositories inspected

| Repo | Commit / date inspected | License (file inspected) | Notes | Tag |
|---|---|---|---|---|
| rlguy/Blender-FLIP-Fluids | `70a0e954018fe39e1f9c3631264989569752bb7a` (2026-08-24) | Repo contains `LICENSE_GPLv3.md`, `LICENSE_MIT.md`, `LICENSE_Standard_Royalty_Free.md`. The engine is MIT per the brief. **Boundary mapping is deferred to `LICENSING_AUDIT.md`.** | `src/engine/` has `pressuresolver.*`, `pcgsolver/`, `viscositysolver.*`, `velocityadvector.*`. APIC is implemented (see §8.1). | [Verified] file list; license scope not audited here |
| InteractiveComputerGraphics/SPlisHSPlasH | `dba6b26b9387ab6bdd187d6df0a7c9d490d03500` (2026-10-28) | **MIT**, "Copyright (c) 2016 Jan Bender" | Pressure solvers: WCSPH, PCISPH, PBF, IISPH, DFSPH, PF. Explicit and implicit viscosity, surface tension, vorticity, drag, elastic solids, rigid-fluid coupling with static and dynamic bodies. Neighbor search on CPU or GPU (cuNSearch). Solvers are CPU (OpenMP) as far as the README states. External deps carry their own licenses. | [Verified] |
| InteractiveComputerGraphics/PositionBasedDynamics | `10a70bc146a97873dc3c8fef372f5217e010542e` (2026-09-01) | **MIT**, "Copyright (c) 2015-present, PositionBasedDynamics contributors" | Position-based fluids, PBD and XPBD constraints, rigid bodies and joints, graph-colored parallel solver. The README doesn't mention a GPU path. | [Verified] |
| NVIDIAGameWorks/FleX | `b1ea0f87b72582649c935d53fd8531b1e7335160` (2021-04-15, "Update LICENSE.txt"). Not archived. Created 2017-01-24. | **"Nvidia Source Code License (1-Way Commercial)"** | Version 1.2.0 (`NV_FLEX_VERSION 120`). **The core solver is prebuilt only**: `lib/win64/NvFlexRelease{CUDA,D3D}_x64.lib` and `bin/win64/*.dll`. No solver kernels exist in the repo; the only `.cu`/`.hlsl` are the extensions library and demo rendering shaders. `core/` is demo math and utility code. Bundles `cudart64_92.dll` (CUDA 9.2). Backends: CUDA, D3D11, D3D12. | [Verified] |
| NVIDIA-Omniverse/PhysX | `b97365c6ddb32fda8469bc788ec18024862b0368` (2026-10-15). Latest release tag `110.3-omni-and-physx-5.9.1` (2026-10-27). | **BSD 3-Clause**, "Copyright (c) 2008-2025, NVIDIA Corporation" | Contains CUDA source for the particle system: `physx/source/gpusimulationcontroller/src/CUDA/particlesystem.cu`, `anisotropy.cu`, `isosurfaceExtraction.cu`, `diffuseParticles.cu`, `SDFConstruction.cu`, and `gpunarrowphase/src/CUDA/cudaParticleSystem.cu`. PhysX 5 absorbed FleX's PBD liquid capability. GPU source was opened with 5.6 (April 2025). | [Verified] repo tree and license. [Paper/press] history per NVIDIA/CG Channel; API deprecation status not reviewed |

### 3.2 Candidate summaries

**FLIP (Fluid-Implicit-Particle)** — Brackbill & Ruppel 1986; brought to graphics by Zhu & Bridson 2005. Particles carry velocity. Each step does P2G splat to a staggered MAC grid (Harlow & Welch 1965), applies forces, does a pressure projection (Poisson solve, typically PCG/MGPCG) to make the grid velocity divergence-free, then interpolates the **change** in grid velocity back to particles (G2P). It is usually blended with PIC (`v = α·v_PIC + (1−α)·v_FLIP`, α ≈ 0.01–0.05) to suppress noise. Free surface comes from particle-derived level sets. [Paper]

**APIC (Affine Particle-In-Cell)** — Jiang, Schroeder, Selle, Teran, Stomakhin 2015. Each particle carries a velocity plus an affine velocity matrix **C** (a local velocity gradient). The transfers are PIC-like (no FLIP delta) but preserve affine velocity fields and (on collocated grids) angular momentum. The MAC-grid extension with smooth kernels is in Ding, Shinar, Schroeder 2020. **PolyPIC** (Fu et al. 2017) adds higher polynomial modes. RPIC (rigid-only part) is described in Jiang et al. 2015. See §8. [Paper]

**DFSPH (Divergence-Free SPH)** — Bender & Koschier 2015 (SCA), extended 2017 (IEEE TVCG). It runs two implicit pressure solvers per step: a divergence-free solver (velocity) and a constant-density solver (position), each a Jacobi-style iteration over SPH neighbors with a precomputed per-particle factor α. Its accuracy at a given time step is higher than WCSPH's and competitive with IISPH. Implicit viscosity (Weiler et al. 2018) and many surface tension and boundary models are available in SPlisHSPlasH. [Paper]

**PBF (Position Based Fluids)** — Macklin & Müller 2013. A density constraint per particle, solved with Jacobi-style position projection inside the PBD loop. Adds an artificial pressure term (tensile-instability fix), XSPH viscosity and vorticity confinement. It stays stable at large time steps, and incompressibility depends on iteration count. **XPBD** (Macklin, Müller, Chentanez 2016) adds compliance so stiffness is independent of iteration count and time step. "Small steps" (Macklin et al. 2019) shows that substeps beat iterations for accuracy per cost. [Paper]

**NVIDIA FleX** — Macklin, Müller, Chentanez, Kim 2014, "Unified Particle Physics for Real-Time Applications". PBD-based unified solver: fluids (PBF), rigid bodies (shape matching), cloth, ropes, soft bodies, all as particles. Its exposed liquid parameters include `cohesion`, `surfaceTension`, `viscosity` (XSPH), `adhesion`, `vorticityConfinement`, `buoyancy`, `smoothing`, `maxSpeed` and `numIterations`, plus `NvFlexGetAnisotropy` and `NvFlexGetContacts`. Collision shapes are **kinematic** ("contact finding against kinematic shapes"). `NvFlexInitDesc` accepts an external D3D device and, in D3D12, an `ID3D12CommandQueue` for render and compute, with `runOnRenderContext` choosing between the direct queue and a compute queue. [Verified in `include/NvFlex.h`]

**WCSPH (Weakly Compressible SPH)** — Monaghan 1994; Becker & Teschner 2007 for graphics. Explicit, with a stiff Tait equation of state. Its time step is limited by the artificial speed of sound (CFL against `c_s ≈ 10·v_max` or more), so it needs many small steps for low compression. [Paper]

**MLS-MPM** — Hu, Fang, Ge, Qu, Zhu, Pradhana, Jiang 2018. A material point method that uses the APIC affine matrix as the MLS velocity gradient. This removes the need for weight gradients and simplifies P2G/G2P. It includes Compatible PIC (CPIC) for two-way rigid coupling with thin boundaries. It accepts arbitrary constitutive models, so it suits viscoelastic, elastoplastic and non-Newtonian materials (Ram et al. 2015; Yue et al. 2015; Su et al. 2021). For liquids it is usually weakly compressible (EOS on `J = det F`), so the time step is limited by sound speed. GPU optimization is covered in Gao et al. 2018. [Paper]

**Hybrids worth naming:** narrow-band FLIP (Ferstl et al. 2016): particles only near the surface, grid in the interior. Heightfield + 3D grid + particles (Chentanez, Müller, Kim 2015) for large water bodies. APIC grid solver with per-particle elastic stress (an MPM-style constitutive model on top of the incompressible APIC projection) for slime (§9). A grid liquid with SPH/PBF secondary particles for droplets and blood spray. [Paper for the first two; the last two are design options, not specific papers]

---

## 4. Licensing and distribution findings (verified)

| Item | Finding | Consequence for a separately distributed UE plugin | Tag |
|---|---|---|---|
| SPlisHSPlasH | MIT (© 2016 Jan Bender). Dependencies (Eigen, pybind, imgui, etc.) under their own licenses. | Free to use as an **algorithmic and validation reference**; code reuse needs MIT notice retention plus a dependency audit. | [Verified] |
| PositionBasedDynamics | MIT (© 2015-present PBD contributors). | Same as above. | [Verified] |
| NVIDIA FleX 1.2.0 | NVIDIA Source Code License (1-Way Commercial): perpetual, worldwide, royalty-free copyright license to reproduce, modify, sublicense and distribute. Redistribution must carry the license and keep notices. **No patent grant.** A patent-suit termination clause applies. No trademark rights. No non-commercial or NVIDIA-hardware-only restriction in the text. | Legally redistributable, but **the core solver is binary-only**, so the project can't fix bugs, port it to Blackwell or new CUDA, change kernels, or integrate it into RDG. Last code activity was 2018; the 2020 and 2021 commits only update the license. | [Verified] text and repo contents |
| FleX GPU dependency | D3D11/D3D12 compute or CUDA 9.2 (bundled `cudart64_92.dll`). The README requires a "D3D11 capable graphics card" and driver ≥ 396.45 (NVIDIA). | **D3D path:** should run on any DX11/12 GPU, but it was compiled in the 2017–2018 era; behavior on current drivers and Blackwell is untested. **CUDA path:** CUDA 9.2 predates Blackwell (SM 12.x). It only runs if the binaries embed PTX that the current driver can JIT, which I have **not** checked. | [Verified] files. [Unverified] Blackwell compatibility |
| PhysX 5 (FleX successor for PBD liquids) | BSD-3 (© 2008–2025 NVIDIA). CUDA source for particles is present. | A good **BSD-3 algorithmic reference for a GPU PBF port to HLSL**. It can't be used directly in a DX12 RDG pipeline without CUDA interop and an NVIDIA-only restriction. | [Verified] |
| FLIP Fluids engine | MIT per the brief. The repo also contains GPLv3 and "Standard Royalty Free" license files. | Component boundaries are **deferred to `LICENSING_AUDIT.md`**. Reimplementing from the algorithm (Strategy C) minimizes exposure either way. | [Verified] file presence only |
| Published algorithms (APIC, MPM, PBF, DFSPH, etc.) | Patent status **not checked**. FleX's license grants no patent rights. | **Open legal question:** run a patent search on PBF/unified particle physics (NVIDIA) and on APIC/MPM transfers before commercial release. | [Unverified] |

---

## 5. Criterion-by-criterion comparison (qualitative)

Scores for these appear in §10. FLIP and APIC are separate columns because the question requires it; they share almost everything except transfers.

| Criterion | FLIP (GPU, MAC) | APIC (GPU, MAC) | DFSPH | PBF / XPBD | FleX (library) | WCSPH | MLS-MPM |
|---|---|---|---|---|---|---|---|
| **Real-time GPU suitability** | Good. Data-parallel transfers plus a structured-grid solve. Concerns: scatter atomics in P2G, pressure-solve dispatch count. | Same as FLIP; slightly heavier particle traffic. | Moderate. Neighbor search every step, 2 solvers × iterations × neighbor loops, CFL ~0.4 h/v. | Excellent; designed for it. Gather-only kernels, few dispatches, stable at frame-rate steps. | Proven real-time, but closed binary outside RDG. | Poor. Many small steps. | Moderate. Scatter P2G with 27-node quadratic kernel, stress per particle, sound-speed time step. |
| **Incompressibility** | Grid divergence → solver tolerance each step. With truncated GPU iterations, residual divergence remains. | Same. | Good: density and divergence both enforced to thresholds. | Iteration-limited; visibly compressible at low iteration counts in deep columns ([Paper]: depends on iterations). XPBD doesn't change the converged constraint, only the stiffness model. | Same as PBF. | Low unless very stiff. | Weakly compressible via EOS (unless combined with a projection). |
| **Volume preservation (long term)** | Known drift: particle clumping and gaps, since density isn't enforced. Mitigation: density-correction projection (Kugelstadt et al. 2021), reseeding. | Same. | Good: particle count fixed, density targeted directly. | Fixed particle count, density constraint, but compression under load. | Same as PBF. | Oscillatory density, fixed mass. | `J` tracked per particle; EOS restores, with oscillation. |
| **Dynamic rigid-body interaction (two-way)** | Solid fractions on faces (Batty et al. 2007). Rigid gets force/torque by integrating pressure over the boundary (good hydrostatics → good buoyancy). Monolithic coupling is hard on GPU. Weak coupling is practical. | Same. APIC's angular-momentum-preserving transfers help with spinning objects only marginally. | Boundary particles (Akinci et al. 2012) or volume/density maps (Koschier & Bender 2017; Bender et al. 2019). Per-boundary-particle pressure force sums naturally to force and torque. Strong coupling for light bodies needs interlinked solvers (Gissler et al. 2019). | Rigid surface sampled with particles or SDF contacts. Impulses from position corrections give the force, but they're iteration-dependent and noisy, so buoyancy fidelity follows incompressibility. | Two-way only with FleX's **own** shape-matched rigids. Kinematic shapes are one-way. Contacts can be read back (`NvFlexGetContacts`) to derive approximate forces for Chaos. | Like SPH (boundary particles). | Natural grid momentum exchange; CPIC handles thin rigid shells (Hu et al. 2018). |
| **Moving boundaries** | SDF / face-fraction voxelization per step with solid velocity BC. Robust for containers and rotating bodies. Thin or fast objects are limited by grid resolution. | Same. | Boundary must be resampled or remapped each step. Thin or fast walls can leak unless the step is small. | Position-projection collisions are robust. FleX demonstrates SDF/convex/mesh kinematic shapes. | Same; built in. | As SPH. | Grid BC, robust. |
| **Viscosity** | Low: free (FLIP is least dissipative). High: implicit variational viscosity (Batty & Bridson 2008; FLIP Fluids has `viscositysolver.cpp`). This is an extra 3-component linear solve, expensive on GPU. | Same; APIC adds a little numerical damping. | Implicit viscosity (Weiler et al. 2018) handles a wide range. | XSPH velocity smoothing: an artistic control, not a physically parameterized viscosity. High viscosity is approximate. | XSPH (`viscosity` param). | Explicit viscosity term; the time step falls for high viscosity. | Viscous stress; explicit, so `dt ∝ ρΔx²/μ` for high μ unless implicit. |
| **Surface tension** | Ghost-fluid / curvature from the level set. Weak and noisy at coarse gameplay resolution. | Same. | Several models (e.g. Akinci et al. 2013). | Cohesion plus surface-tension terms (FleX style), cheap. | `surfaceTension`, `cohesion` params. | Akinci-style. | Not native; needs added terms. |
| **Cohesion / adhesion** | Not native. Adhesion via sticky/partial-slip BCs; cohesion only via surface tension. | Same. | Akinci et al. 2013 (cohesion and adhesion). | Excellent; directly parameterizable. | `cohesion`, `adhesion` params. | Akinci-style. | Sticky BCs natural; cohesion via the constitutive model. |
| **Viscoelasticity** | Needs a transported stress/strain tensor (Goktekin et al. 2004), effectively becoming MPM-like. | Better positioned: C ≈ ∇v per particle is exactly what updates a per-particle deformation or conformation tensor. | Possible (SPH elastic solids exist in SPlisHSPlasH; viscoelastic SPH literature exists) but not turnkey for fluids. | Conformation constraints (Barreiro et al. 2017); Clavet-style springs. | None for fluids. Plastic shape matching for rigid clusters only (`NvFlexSetRigids` takes thresholds and creeps). | Clavet et al. 2005 (double-density relaxation + springs): the classic real-time slime. | Native: any constitutive model (Oldroyd-B-like, Herschel–Bulkley, elastoplastic). |
| **Numerical stability** | Stable at large CFL (FLIP Fluids default `_CFLConditionNumber = 5.0`, [Verified]). Pure FLIP is noisy; needs a PIC blend. | Very stable, low noise. | Iteration-dependent. Particle disorder at free surface. CFL ~0.4. | Unconditionally stable projection, but damping and energy loss. FleX clamps with `maxSpeed` ([Verified]). | Same. | Stiff; blows up on too large a step. | Explicit; stable only within the elastic-wave CFL. |
| **Computational complexity** | §6 | §6 | §6 | §6 | §6 | §6 | §6 |
| **Memory** | Dense grid scales with domain volume (≈80–100 B/cell, §7). Particles ≈28 B. | Particles ≈64 B; no saved-velocity grid copy needed. | ≈100–250 B/particle (more with stored neighbor lists). | ≈60–120 B/particle. | Opaque; preallocated max particles. | Similar to SPH. | ≈100+ B/particle (F, C), plus grid. |
| **Scalability** | Cost scales with **domain** volume unless sparse or narrow-band. Particle count scales well since pressure is solved on the grid. | Same. | Scales with fluid particle count only; unbounded domain. | Same as SPH. | Fixed max particles, single solver. | As SPH. | Needs a sparse grid for large domains. |
| **Rendering compatibility** | Particles plus a grid-native level set / SDF. World-space meshing (MC) or raymarch is natural. Screen-space also works. | Same. | Particles only. Screen-space (van der Laan et al. 2009) or splat-to-grid, or anisotropic kernels (Yu & Turk 2013). | Same as SPH. | Provides smoothed positions and anisotropy for screen-space ([Verified] API). | As SPH. | Particles plus grid. |
| **Implementation difficulty** | Medium-high: MAC grid, atomics, PCG/MGPCG, cell labeling, extrapolation, reseeding/compaction, level set. | FLIP + small delta. | Medium: GPU sort / neighbor search, two solvers, boundary maps. | Low-medium. | Low to integrate, high to make robust in UE (interop, closed binary). | Low. | Medium (constitutive models, CPIC is harder). |
| **Maintenance** | In-house code; MIT CPU reference available. | Same. | In-house; MIT reference. | In-house; MIT CPU and BSD-3 CUDA references. | Unmaintained closed binary. | In-house. | In-house; many references (Taichi examples etc., licenses not checked). |
| **Licensing** | Algorithm only; reference MIT (scope → `LICENSING_AUDIT.md`). | Same. | MIT reference. | MIT / BSD-3 references. | Permissive, but no patent grant and no source. | — | — |

---

## 6. Computational complexity and per-step cost

### 6.1 Per-substep cost terms

Notation: `Np` particles, `Nc` grid cells (dense domain), `Nf` fluid cells, `Nn` SPH neighbors per particle (≈30–50 for a support radius of 2× spacing in 3D), `K` grid nodes touched per particle per velocity component (8 trilinear or 27 quadratic B-spline), `I` solver iterations.

| Method | Neighbor search | Grid transfers | Pressure / constraint solve | Other | Time-step limit |
|---|---|---|---|---|---|
| FLIP | none | P2G `O(Np·3K)` **scatter (atomics)**; G2P `O(Np·3K)` gather; plus saved grid velocity copy and subtract `O(Nc)` | `O(Nf·I)` with 7-point stencil. CG iterations grow with grid dimension (≈ `O(n)` for n³), MGPCG ≈ near-constant iterations. Each iteration is 1 (Jacobi) to ~5 (CG: SpMV, 2 reductions, axpy) to ~15–25 (MGPCG V-cycle) dispatches [Estimate] | Cell labeling, solid SDF/face fractions `O(Nc)`; velocity extrapolation `O(Nc)`; particle advection `O(Np)`; reseeding/compaction (prefix sums) | Advective CFL; FLIP tolerates CFL ≫ 1 (FLIP Fluids default 5 [Verified]) |
| APIC | none | As FLIP, plus per-node affine term in P2G and C reconstruction in G2P; no saved-velocity grid copy | As FLIP | As FLIP | As FLIP |
| DFSPH | `O(Np)` counting sort / hash grid (~4–6 dispatches) **every step** | none | Density + α `O(Np·Nn)`; divergence solver `I_div·O(Np·Nn)`; density solver `I_dens·O(Np·Nn)`; each iteration ≈ 2 neighbor loops | Implicit viscosity: CG with `O(Np·Nn)` 3×3 blocks per iteration | CFL ≈ 0.4·h/v_max [Paper] |
| PBF / XPBD | `O(Np)` per substep | none | `I · 2 · O(Np·Nn)` (λ pass, Δp pass) | Collisions `O(Np·shapes)`; XSPH + vorticity `O(Np·Nn)` | Stable at frame-rate steps; accuracy from substeps (Macklin et al. 2019) |
| FleX | internal | — | internal (PBF) | — | `NvFlexUpdateSolver(dt, substeps)` [Verified signature] |
| WCSPH | `O(Np)` per step | none | `O(Np·Nn)` per step (explicit EOS) | — | Sound-speed CFL; ~10x more steps than incompressible solvers [Paper, order of magnitude] |
| MLS-MPM | none | P2G `O(Np·27)` scatter with stress; G2P `O(Np·27)` | EOS (none) or optional projection | SVD/polar per particle for some models | Elastic wave speed √(E/ρ); viscous explicit limit |

**Key structural difference.** Particle methods (SPH, PBF) pay `Nn ≈ 30–50` neighbor interactions per particle per solver iteration. Grid methods (FLIP, APIC) pay `3K = 24–81` scatter and gather operations per particle once per substep, then solve pressure on `Nf` cells with a 7-point stencil. At around 8 particles per cell, `Nf ≈ Np/8`. The pressure solve then runs on about an eighth as many unknowns as an SPH solver, at the price of more iterations and a dependence on domain size.

### 6.2 Dispatch-count estimate per substep

[Estimate] Assumptions: one compute dispatch per logical pass, no kernel fusion.

| Method | Approx. dispatches per substep |
|---|---|
| FLIP / APIC with Jacobi or red-black GS, 40 iterations | ~20 fixed + 40–80 pressure ≈ **60–100** |
| FLIP / APIC with MGPCG, 8 iterations | ~20 fixed + 8×(15–25) ≈ **140–220** |
| DFSPH (2 div + 4 density iterations) | 6 sort + 2 + 4 + 8 + 4 ≈ **25–35** |
| PBF (4 iterations) | 6 sort + 1 + 8 + 3 ≈ **18–25** |

On DX12 each dispatch with a UAV barrier costs a fixed overhead that has to be **measured** (Experiment 6). This is a real risk for the grid solvers. Mitigations: fused kernels, block-local red-black Gauss-Seidel in groupshared memory, a fixed iteration count with warm start from the previous frame's pressure, and restricting the solve to compacted fluid cells.

### 6.3 Bandwidth-floor estimates on the RTX 5060 Laptop

[Estimate] Assumptions: 384 GB/s peak, 60% achievable, FP32, no L2 reuse (pessimistic). These are **lower bounds** for DRAM-bound passes, not predictions.

| Pass | Data per iteration or pass | Floor time |
|---|---|---|
| Pressure CG iteration, dense 128×64×128 (1.05 M cells), ~36 B/cell | ≈38 MB | ≈0.16 ms. **30 iterations ≈ 5 ms → over budget** |
| Same, restricted to ~20% fluid cells (≈210 k) | ≈7.6 MB | ≈0.03 ms; 30 iterations ≈ 1 ms |
| Pressure iteration, dense 64³ (262 k cells) | ≈9.4 MB (may fit in L2 [Unverified L2 size]) | ≈0.04 ms DRAM floor; 30 iterations ≈ 1.2 ms (less if L2-resident) |
| APIC P2G + G2P, 200 k particles, 64 B/particle state | ≈26 MB of particle traffic + grid atomics | Particle traffic floor ≈0.1 ms. **Atomic contention dominates and can't be estimated; must measure.** |
| PBF iteration, 100 k particles, `Nn ≈ 40`, sorted (neighbors mostly cache hits) | Compute/latency-bound | Not estimable from bandwidth; must measure |

**Takeaway [Estimate]:** A dense 128×64×128 MAC grid with a conventional CG solve **will not fit** in ~2–2.5 ms on this laptop. A 64³–96³ grid, or a larger domain with a solve over compacted fluid cells, warm-started, at a fixed 20–40 iterations or a few MG V-cycles, plausibly fits. That points to **~50 k–150 k particles** for the APIC path inside the simulation sub-budget. PBF at 2 substeps × 3–4 iterations plausibly fits a similar **~50 k–100 k particles**. **Both ranges are unvalidated hypotheses for Experiment 6.**

### 6.4 Proposed Phase 1 sub-budget gates

[Estimate — to be confirmed in `PERFORMANCE_PLAN.md`] On the RTX 5060 Laptop at sustained clocks, 100 k particles, 64×64×64 or equivalent domain, all 1 substep:

* Pressure projection ≤ 1.0 ms at a divergence residual that keeps volume drift < 1% over 10 s of dam break (with density correction on).
* P2G + G2P ≤ 0.6 ms.
* Total APIC substep ≤ 2.0 ms.

If the gates fail after reasonable optimization, switch to the PBF fallback.

---

## 7. Memory

[Estimate] FP32, no compression.

| Method | Per-particle | Per-cell / grid | Example |
|---|---|---|---|
| FLIP | pos 12 + vel 12 + id/material 4 ≈ **28 B** (+ sort keys and ping-pong ≈ ×2) | u,v,w 12 + weights 12 + saved u 12 + p 4 + div 4 + CG r,z,d,Ad 16 + solid face fractions 12 + SDF 4 + label 4 ≈ **80 B** | 200 k particles ≈ 11 MB; 128×64×128 grid ≈ 84 MB; MG hierarchy +~15% |
| APIC | FLIP + C (9 floats = 36 B) ≈ **64 B** | FLIP minus saved u (−12 B) ≈ 68 B | 200 k particles ≈ 26 MB with ping-pong |
| DFSPH | pos, vel, ρ, α, κ_div, κ_dens, accel, id ≈ 60–80 B; stored neighbor lists 40 × 4 B = 160 B if cached | hash grid cell starts | 200 k ≈ 15–50 MB |
| PBF | pos, predicted pos, vel, λ, Δp, id ≈ 60–70 B | hash grid | 200 k ≈ 12–15 MB |
| MLS-MPM | pos, vel, C (36), F (36), J, material ≈ 100–110 B | nodes: mass + momentum 16 B (sparse) | 200 k ≈ 22 MB |

All candidates fit comfortably in 8 GB at gameplay scales. Memory isn't a differentiator (low weight, §10), except that **dense-grid memory grows with domain volume**. That cost turns into bandwidth cost, as §6.3 shows.

---

## 8. APIC vs FLIP

### 8.1 What FLIP Fluids implements ([Verified] at commit `70a0e95`, `src/engine/`)

* `fluidsimulation.cpp` has `setVelocityTransferMethodFLIP()` and `setVelocityTransferMethodAPIC()`. The engine default is `_velocityTransferMethod = VelocityTransferMethod::FLIP` (`fluidsimulation.h`).
* PIC/FLIP blend: `_ratioPICFLIP = 0.05`. A separate `_ratioPICAPIC = 0.00` exists. The exact semantics of the PIC/APIC blend are deferred to `FLIP_FLUIDS_SOURCE_AUDIT.md`.
* APIC affine data is stored **per MAC component** as three `vec3` particle attributes: `AFFINEX`, `AFFINEY`, `AFFINEZ` (`_markerParticles.addAttributeVector3("AFFINEX")` and so on). That's 9 floats per particle in total.
* Other defaults seen: `_CFLConditionNumber = 5.0`, `_pressureSolveTolerance = 1e-9`, `_maxPressureSolveIterations = 900`. These are offline-quality settings; a real-time GPU port will use far looser tolerance and fixed iteration counts.
* The addon UI default may differ from the engine default; not checked.

The GPU port can therefore offer FLIP, APIC, and a blend, and validate each against the same CPU engine.

### 8.2 When APIC helps

1. **Angular momentum.** APIC transfers preserve affine velocity fields and, on collocated grids, conserve angular momentum across P2G→G2P (Jiang et al. 2015). PIC destroys rotational detail every step. **Caveat [Paper]:** the original MAC-grid APIC variant did *not* exactly conserve angular momentum. Ding, Shinar & Schroeder (2020) fixed this with smooth-kernel MAC transfers. A MAC GPU implementation should follow Ding et al. if exact angular momentum matters.
2. **Less noise than FLIP.** FLIP's delta transfer lets particle velocities drift apart from the grid, which shows up as particle noise, ringing and "jittery" free surfaces; hence the 1–5% PIC blend. APIC has no FLIP delta, so its transfers don't accumulate that noise. Fewer particles per cell may be needed for a clean surface. [Paper; quantitative reduction not claimed]
3. **Less dissipation than PIC.** APIC keeps vortices and swirling motion that PIC smears. [Paper]
4. **More dissipative than FLIP.** Per Ding et al. 2020, APIC dissipates energy even in the Δt → 0 limit, while FLIP doesn't. For **splashy, energetic gameplay water**, a mostly-FLIP blend can look livelier. For **viscous blood and slime**, APIC's damping is a feature.
5. **Stable at large time steps and good for coupled stress models.** C is a per-particle velocity-gradient estimate, which MPM-style constitutive models (slime, §9) consume directly. MLS-MPM is built on this.

### 8.3 Extra cost of APIC

* **Storage:** +9 floats (36 B) per particle at FP32. In staggered MAC form this is one 3-vector per velocity component (c_x, c_y, c_z ∈ ℝ³), matching FLIP Fluids' `AFFINEX/Y/Z`. The total is still 9 floats; "3 per MAC component" means three components × three floats. A 2D MAC version needs 4 floats. FP16 storage of C is a candidate optimization (accuracy impact unknown; [Unverified]).
* **Bandwidth:** Particle state read/written in P2G/G2P grows from ~24–28 B to ~60–64 B, about +130–150% of *particle* bytes. APIC also **removes** FLIP's saved-grid-velocity copy and subtract pass (−12 B/cell plus one `O(Nc)` pass).
* **ALU:** P2G adds `c_a · (x_i − x_p)` per node and component (3 FMAs). G2P accumulates an outer product per node to rebuild c_a (e.g. `c_a = (4/Δx²) Σ w_i u_i (x_i − x_p)` for quadratic B-splines [Paper]). Both are cheap relative to atomics.
* **Net [Estimate]:** For pressure-dominated frames, APIC adds roughly 10–25% to total solver time over FLIP. For transfer-dominated frames (many particles, small grid) it can add more. Measure in Experiment 6.

### 8.4 Common blends

| Blend | Formula / idea | Use |
|---|---|---|
| PIC/FLIP | `v_p ← α·I(u_new) + (1−α)·(v_p + I(u_new − u_old))`, α ≈ 0.01–0.05 (Zhu & Bridson 2005; FLIP Fluids default α = 0.05 [Verified]) | Default production FLIP. Higher α means calmer and more viscous-looking. |
| APIC (pure) | `v_p ← I(u_new)`, `C_p ← gradient-like reconstruction` | Low noise, rotational detail, viscous materials |
| APIC + FLIP delta | APIC velocity plus a fraction β of the FLIP delta, keeping C from APIC | Restores FLIP energy while keeping affine detail. Used in several solvers, and in MPM by Fei et al. 2021 [Paper, not re-verified]. |
| PIC/APIC | Blend APIC with a fraction of PIC, or scale C by s < 1 ("affine damping") | Extra stabilization or damping for viscous materials; FLIP Fluids exposes `_ratioPICAPIC` |
| Per-material blend | α/β chosen per liquid material (water: FLIP-leaning; slime: APIC) | Fits the Data Asset material system in `LIQUID_MATERIALS.md` |

**PolyPIC and other affine variants.** PolyPIC (Fu et al. 2017) stores more polynomial modes per velocity component (beyond affine, up to the full multilinear basis), which further reduces transfer dissipation. Storage and ALU grow with the number of modes. For gameplay at coarse resolution the gain is unlikely to be visible relative to cost [Estimate], so it's **not recommended for Phase 1**. RPIC (rigid-only C) is a cheaper intermediate (Jiang et al. 2015) but brings little benefit once APIC is present.

**Recommendation for this project:** Implement APIC as the transfer, with an optional FLIP-delta blend coefficient (β) and an affine damping factor per material. Ship FLIP-leaning water and APIC-pure slime and blood. Validate each mode against the FLIP Fluids CPU engine.

---

## 9. Slime, non-Newtonian and viscoelastic behavior

### 9.1 Why "just increase viscosity" is not sufficient

A Newtonian viscosity μ resists only the **rate** of deformation (stress ∝ strain rate). Slime-like materials show behavior that a large μ can't produce:

| Observed slime behavior | Physical cause | Can high Newtonian μ reproduce it? |
|---|---|---|
| Stretches into **strings and threads** before snapping; filaments persist | Extensional elastic stress (polymer stretching); high extensional viscosity relative to shear (Trouton ratio ≫ 3) | **No.** A Newtonian thread thins and breaks under surface tension (capillary pinch-off), and no elastic stress resists thinning. |
| **Recoils or springs back** after being pulled | Stored elastic stress with a relaxation time λ (Maxwell/Oldroyd-B) | **No.** A viscous fluid has no memory; it stops, it doesn't come back. |
| **Holds its shape** on a wall or in a blob, then flows when pushed | Yield stress (Bingham / Herschel–Bulkley) | **No.** A Newtonian fluid of any μ eventually flows and spreads flat; high μ only slows it. |
| Runs when stirred, thick when still | Shear thinning, μ_eff(γ̇) | **No.** Needs a shear-rate-dependent viscosity. |
| **Sticks** to surfaces and other objects | Adhesion (boundary condition / interfacial energy) | **No.** Viscosity is a bulk property; sticking is a boundary phenomenon. |
| Holds together as a coherent blob | Cohesion / surface tension / tensile strength | **No.** |

**Numerical problems too:** explicit viscosity has a diffusion limit `Δt ≲ ρΔx²/(2dμ)` (d = dimension) [standard diffusion stability; Estimate of exact constant]. A large μ therefore forces tiny steps or an **implicit** solve (Batty & Bridson 2008 for grids; Weiler et al. 2018 for SPH), which is a full extra linear system per step. Even implicit, very high μ makes the solve ill-conditioned. Free-surface viscous boundary conditions are needed for correct buckling and coiling (Batty & Bridson 2008).

**Conclusion:** Slime needs (a) a generalized-Newtonian viscosity μ(γ̇) with optional yield, (b) an elastic stress state per particle with relaxation (viscoelastic), (c) cohesion/surface tension, and (d) adhesion at boundaries. "Increase μ" covers none of the four. It produces honey or tar, not slime.

### 9.2 How each candidate handles slime

| Candidate | Generalized Newtonian (shear thinning, yield) | Viscoelastic (strings, recoil) | Cohesion / adhesion | Verdict |
|---|---|---|---|---|
| FLIP | Variable μ per cell in an implicit viscosity solve (feasible, costly on GPU). Yield via regularized μ. | Needs a per-particle stress or conformation tensor plus a velocity gradient. FLIP has no per-particle gradient, so it must be computed from the grid at G2P. Eulerian precedent: Goktekin et al. 2004. | Weak (BC-based adhesion, level-set surface tension) | Possible but awkward |
| APIC | As FLIP | **Better:** C_p ≈ ∇v at the particle drives the update of a per-particle elastic strain (left Cauchy–Green or conformation tensor). Stress divergence is added to grid forces in P2G, so it becomes an MPM-style stress on an incompressible APIC solver. | Weak; needs additive models | **Recommended growth path** (hybrid, §3.2) |
| DFSPH | Weiler et al. 2018 implicit viscosity with variable μ | SPH elasticity exists (Peer et al. 2018; Kugelstadt et al. 2021, both in SPlisHSPlasH), but not as a fluid viscoelastic model | Good (Akinci et al. 2013) | Good, but not turnkey for viscoelastic fluid |
| PBF / XPBD | XSPH only (not physical); approximate | Conformation constraints (Barreiro et al. 2017) or Clavet-style springs as XPBD constraints | Excellent | Good for stylized slime; physical parameterization weak |
| FleX | XSPH only | None for fluids | Excellent (`cohesion`, `adhesion`) | Sticky goo, not viscoelastic |
| WCSPH (Clavet et al. 2005) | Approximate | Springs with plasticity (rest-length update) and double-density relaxation | Good | Classic, cheap real-time slime; weak water |
| MLS-MPM | Native (Herschel–Bulkley, Yue et al. 2015) | Native (Ram et al. 2015; Su et al. 2021) | Sticky BCs natural; cohesion via the model | **Best fidelity** for slime. Weakly compressible water and time-step limits make it a poor all-round choice. |

---

## 10. Weighted comparison matrix

### 10.1 Weights and justification (sum = 100)

| # | Criterion | Weight | Why this weight for gameplay liquids |
|---|---|---|---|
| 1 | Real-time GPU suitability | **12** | Hard requirement: 60 FPS with a ~2–2.5 ms solver slice on an 8 GB laptop GPU. A method that can't reach real time disqualifies itself. |
| 2 | Incompressibility | **6** | Visible as "springy" or compressible water; drives correct hydrostatic pressure, and therefore buoyancy. Gameplay tolerates a few % error, so it's not top-weighted. |
| 3 | Volume preservation | **5** | Long-term drift is a gameplay bug (puddles vanish, containers lose water). It can be mitigated in all methods, so it's moderate. |
| 4 | Dynamic rigid-body interaction | **10** | Mission-critical: two-way Chaos coupling, buoyancy, drag, floating and sinking. Second-highest weight. |
| 5 | Moving boundaries | **6** | Moving containers, characters, doors. Needed, but all candidates have workable approaches. |
| 6 | Viscosity | **6** | Water → blood → slime needs a configurable viscosity range. |
| 7 | Surface tension | **4** | Matters at small scale (droplets, blood beads). At gameplay resolution, only partially visible. |
| 8 | Cohesion / adhesion | **4** | Blood wetting and slime sticking are listed requirements. Lower weight because they can be added or faked partly in rendering and decals. |
| 9 | Viscoelasticity | **4** | Slime stringing is listed as "potential" / "where physically appropriate", so it's not core for Phase 1. |
| 10 | Numerical stability | **9** | Gameplay can't tolerate explosions; players do adversarial things (fast objects, teleports, huge forces). |
| 11 | Computational complexity | **8** | Distinct from #1: cost per unit of visual resolution, which sets particle and grid counts within the budget. |
| 12 | Memory | **2** | All candidates fit in 8 GB at gameplay scale (§7). |
| 13 | Scalability | **5** | Quality presets and LOD across hardware; larger levels. |
| 14 | Rendering compatibility | **5** | Rendering is a separate module (mandate §8), so this is moderate. It eases world-space surfaces and gameplay queries. |
| 15 | Implementation difficulty | **6** | A small team; Phase 1 must produce a prototype. |
| 16 | Maintenance | **4** | UE version churn; in-house code is preferred. |
| 17 | Licensing | **4** | Gating rather than graded. All candidates except FleX are reimplemented from papers, so differences are small. |

Simulation-physics criteria (2–10) total 54. Engineering/performance criteria (1, 11–17) total 46. This deliberately balances "does it behave like liquid" against "can we ship it at 60 FPS".

### 10.2 Scores (1 = poor, 5 = excellent; higher is always better, including "easier" for difficulty and "cheaper" for complexity)

| Criterion (weight) | FLIP | APIC | DFSPH | PBF/XPBD | FleX | WCSPH | MLS-MPM |
|---|---|---|---|---|---|---|---|
| GPU suitability (12) | 4 | 4 | 3 | 5 | 4 | 2 | 3 |
| Incompressibility (6) | 4 | 4 | 4 | 3 | 3 | 2 | 2 |
| Volume preservation (5) | 3 | 3 | 4 | 3 | 3 | 3 | 3 |
| Rigid-body interaction (10) | 3 | 3 | 4 | 3 | 3 | 3 | 4 |
| Moving boundaries (6) | 4 | 4 | 3 | 4 | 4 | 3 | 4 |
| Viscosity (6) | 4 | 4 | 4 | 2 | 2 | 3 | 3 |
| Surface tension (4) | 3 | 3 | 4 | 4 | 4 | 4 | 2 |
| Cohesion/adhesion (4) | 2 | 2 | 4 | 5 | 5 | 4 | 3 |
| Viscoelasticity (4) | 2 | 3 | 3 | 3 | 2 | 3 | 5 |
| Numerical stability (9) | 4 | 5 | 3 | 4 | 4 | 2 | 3 |
| Computational complexity (8) | 4 | 4 | 3 | 4 | 4 | 2 | 3 |
| Memory (2) | 4 | 4 | 4 | 5 | 4 | 4 | 3 |
| Scalability (5) | 3 | 3 | 4 | 4 | 3 | 3 | 3 |
| Rendering compatibility (5) | 4 | 4 | 3 | 3 | 4 | 3 | 4 |
| Implementation difficulty (6) | 3 | 3 | 3 | 4 | 4 | 5 | 3 |
| Maintenance (4) | 4 | 4 | 4 | 4 | 1 | 4 | 4 |
| Licensing (4) | 5 | 5 | 5 | 5 | 3 | 5 | 5 |
| **Weighted total (/5)** | **3.58** | **3.71** | **3.54** | **3.80** | **3.42** | **2.99** | **3.31** |
| **Rank** | 3 | 2 | 4 | **1** | 5 | 7 | 6 |

Totals were computed by script: Σ(weight × score) / 100. The script is in the scratchpad and isn't part of the deliverable.

### 10.3 Score rationale (the contestable cells)

* **PBF rigid = 3 (not 4).** PBF couples easily, but against external Chaos bodies the force comes from iteration-limited position corrections. Hydrostatic pressure at depth is therefore under-resolved at budget iteration counts, which degrades buoyancy fidelity. FleX's unified two-way coupling works only with its own particle rigids. DFSPH gets 4 because it computes an actual pressure acting on boundary particles.
* **FLIP/APIC rigid = 3.** Pressure integration over face fractions gives physically good buoyancy and drag, but GPU two-way coupling is weak (explicit) coupling with latency. Monolithic coupling (Batty et al. 2007; Robinson-Mosher et al. 2008) is impractical on GPU in Phase 1.
* **FLIP/APIC rendering = 4 (not 5).** The grid gives an SDF directly, but FLIP Fluids itself meshes from particles, and particle methods can splat to a grid too. The advantage is real but modest.
* **APIC stability = 5 vs FLIP 4.** APIC avoids FLIP-delta noise; pure FLIP needs a PIC blend to stay clean.
* **APIC viscoelastic = 3 vs FLIP 2.** C provides the velocity gradient for stress transport (§9.2).
* **PBF viscosity = 2.** XSPH isn't a physical viscosity; a water-to-slime range with physical parameters isn't available without adding an implicit SPH viscosity solve, which costs DFSPH-like amounts.
* **FleX maintenance = 1.** Closed binary, last code change 2018, CUDA 9.2 runtime.
* **FleX licensing = 3.** Permissive, but no patent grant and no source.
* **MLS-MPM rigid = 4.** CPIC two-way coupling is demonstrated (Hu et al. 2018), and grid momentum exchange is natural.

---

## 11. Sensitivity analysis

Recomputed with the same scores and alternative weights (script output):

| Weight scenario | Ranking (top 4) | Winner changes? |
|---|---|---|
| Base (§10.1) | PBF 3.80 · APIC 3.71 · FLIP 3.58 · DFSPH 3.54 | — |
| All criteria equal | PBF 3.82 · APIC 3.65 · DFSPH 3.65 · FLIP 3.53 | No |
| Slime-heavy (VE 12, coh/adh 8, visc 10, GPU 8, cost 6) | PBF 3.67 · APIC 3.59 · DFSPH 3.56 · MLS-MPM 3.43 | No |
| Rigid coupling ×2 (20) | PBF 3.73 · APIC 3.65 · DFSPH 3.58 · FLIP 3.53 | No |
| Implementation/maintenance-heavy (14 / 8) | PBF 3.82 · APIC 3.67 · FLIP 3.55 · DFSPH 3.52 | No |
| Performance-heavy (GPU 20, cost 14) | PBF 3.89 · APIC 3.75 · FLIP 3.63 · FleX 3.49 | No |
| **Water-fidelity (incomp 10, vol 8, visc 8, render 8)** | **APIC 3.71 · PBF 3.70** · FLIP 3.60 · DFSPH 3.56 | **Yes → APIC** |
| Cohesion/adhesion and surface tension ×2 | PBF 3.85 · APIC 3.62 · DFSPH 3.57 · FLIP 3.50 | No (PBF lead grows) |
| Ignore licensing and maintenance | PBF 3.74 · APIC 3.64 · FleX 3.54 · FLIP 3.50 | No |

**Single-criterion break-even (minimum added weight that changes the winner):**

* Incompressibility +9 → APIC. Viscosity **+5** → APIC. Stability +9 → APIC. Rendering +9 → APIC.
* Volume preservation +26 or rigid coupling +26 → DFSPH. Viscoelasticity +25 → MLS-MPM.
* GPU suitability, moving boundaries, surface tension, cohesion/adhesion, cost, memory, scalability, implementation, maintenance, licensing: no change up to +60.
* Setting GPU suitability or cohesion/adhesion weight to **0** → APIC wins.

**Interpretation.**
1. The PBF vs APIC decision is **weight-sensitive**. A modest emphasis on water fidelity flips it. A modest emphasis on engineering speed or stickiness reinforces PBF.
2. **No reasonable weighting lets FleX, WCSPH or MLS-MPM win.** FleX is penalized by maintenance and integration, WCSPH by time step, and MLS-MPM by compressible water and time step. MLS-MPM wins only when viscoelasticity carries about 29% of the total weight, which supports it as a **slime-specific constitutive extension**, not the base solver.
3. DFSPH needs about 3x weight on rigid coupling or volume preservation to win. It's a credible, more physically rigorous alternative to PBF, at higher cost.

---

## 12. Recommendation

### 12.1 Primary: GPU-native APIC on a dense MAC grid (with optional FLIP-delta blend)

**This is a judgment that overrides a 0.09-point matrix lead for PBF.** The reasons, in order of weight:

1. **Intrinsic vs additive deficits.** PBF's weaknesses for this brief are compressibility and buoyancy fidelity at budget iteration counts, and the lack of a physically parameterized viscosity. Both are tied to its core solver. Fixing them means more iterations or substeps and an added implicit viscosity solve, which pushes PBF toward DFSPH cost and erodes the GPU-suitability advantage that gives PBF its lead. APIC's weaknesses (cohesion, adhesion, surface tension) are **additive**: boundary conditions, free-surface terms, and secondary particles. They don't change the core solver.
2. **Slime growth path.** APIC's per-particle C matrix is the velocity gradient an MPM-style viscoelastic or elastoplastic stress model needs. The same P2G/G2P kernels can later carry stress for slime (APIC + per-particle stress hybrid, §9.2). That gives one shared solver for water, blood and slime, answering mandate question 7.
3. **Validated reference.** FLIP Fluids' MIT engine implements the same discretization, including both FLIP and APIC transfers ([Verified]). It gives CPU-vs-GPU parity tests for Experiments 1–2 under Strategy C.
4. **Gameplay queries and rendering.** Local velocity, fluid level, "how much fluid in region" and submerged fraction are grid lookups and reductions. World-space SDF surfaces come straight from the grid pipeline. (Not scored separately; partially in rendering.)

**Conditions and known risks:** dense-grid cost scales with domain volume (use a domain sized to the gameplay space, a compacted fluid-cell solve, and later narrow-band or sparse); pressure-solve dispatch count; P2G atomics in HLSL; volume drift (budget for density correction, Kugelstadt et al. 2021); weak cohesion and adhesion until additive models land.

### 12.2 Fallback: PBF/XPBD, in-house HLSL on RDG

* Highest raw matrix score, simplest GPU implementation, fewest dispatches, best cohesion and adhesion, proven real-time lineage (FleX).
* References: PositionBasedDynamics (MIT, CPU), SPlisHSPlasH PBF (MIT, CPU), PhysX 5 particle system (BSD-3, CUDA source, verified present). **Do not adopt the FleX binary.**
* When to switch: the APIC gates in §6.4 fail, or the project owner reprioritizes toward stylized, sticky liquids and engineering speed. In that case the matrix already favors PBF.
* Upgrade options if water fidelity is short: XPBD substepping (Macklin et al. 2019), or add DFSPH-style divergence solve and Weiler implicit viscosity on the same neighbor infrastructure.

### 12.3 Not recommended

* **NVIDIA FleX as a dependency:** closed binary core, unmaintained, CUDA 9.2-era, Blackwell compatibility unknown, owns its own rigids, outside RDG. Keep it as a behavioral reference only.
* **WCSPH as the base solver:** time-step-bound. Its Clavet-style slime ideas remain useful.
* **MLS-MPM as the base solver:** weakly compressible water and time-step limits. **Keep it as the reference for the slime constitutive extension.**
* **PolyPIC:** cost is not justified at gameplay resolution.

### 12.4 Decision gate for Phase 1

Build the APIC prototype first, plus a deliberately minimal PBF prototype on the same RDG scaffolding, sharing the emitter, colliders, sort, and debug rendering. Run Experiments 2, 3 and 6 on both. Pass/fail criteria: §6.4 sub-budgets; volume drift; a floating box at the correct waterline (density ratio test); stability under a fast-moving collider. Finalize the choice on measured data. This costs one extra small prototype and removes the weight-sensitivity risk shown in §11.

---

## 13. Unresolved questions and required validation

1. **UE5.8 float atomics / 64-bit atomics availability on DX12 SM6** → `UE58_INTEGRATION.md`. Determines the P2G strategy.
2. **Dispatch + barrier overhead on the RTX 5060 Laptop under RDG** → Experiment 6. Determines Jacobi vs MGPCG vs fused solver.
3. **RTX 5060 Laptop bus width and L2 size** (assumed 128-bit; L2 unknown). Confirm with GPU-Z or the vendor spec.
4. **FleX on Blackwell:** do the D3D12 binaries run on driver 617.x, and do the CUDA binaries embed PTX? Only relevant if FleX is used as a behavioral reference demo.
5. **FLIP Fluids PIC/APIC ratio semantics and the addon default transfer method** → `FLIP_FLUIDS_SOURCE_AUDIT.md`.
6. **Patent search** on PBF/unified particle physics (NVIDIA) and APIC/MPM transfers → legal review via `LICENSING_AUDIT.md`.
7. **PhysX 5 PxParticleSystem / PBD API deprecation status** (not reviewed). It matters only if used as a reference.
8. **All §6.3 particle-count ranges and §8.3 APIC overhead percentages are estimates** that need Experiment 6.

---

## 14. References

### Grid / hybrid particle-grid

* Harlow, F. H., Welch, J. E. "Numerical Calculation of Time-Dependent Viscous Incompressible Flow of Fluid with Free Surface." *Physics of Fluids* 8(12), 1965.
* Brackbill, J. U., Ruppel, H. M. "FLIP: A Method for Adaptively Zoned, Particle-in-Cell Calculations of Fluid Flows in Two Dimensions." *Journal of Computational Physics* 65(2), 1986.
* Zhu, Y., Bridson, R. "Animating Sand as a Fluid." *ACM Transactions on Graphics* 24(3) (SIGGRAPH), 2005.
* Bridson, R. *Fluid Simulation for Computer Graphics*, 2nd ed. CRC Press / A K Peters, 2015.
* Jiang, C., Schroeder, C., Selle, A., Teran, J., Stomakhin, A. "The Affine Particle-In-Cell Method." *ACM Transactions on Graphics* 34(4) (SIGGRAPH), Article 51, 2015.
* Jiang, C., Schroeder, C., Teran, J. "An Angular Momentum Conserving Affine-Particle-In-Cell Method." *Journal of Computational Physics* 338, 2017.
* Fu, C., Guo, Q., Gast, T., Jiang, C., Teran, J. "A Polynomial Particle-In-Cell Method." *ACM Transactions on Graphics* 36(6) (SIGGRAPH Asia), Article 222, 2017.
* Ding, O., Shinar, T., Schroeder, C. "Affine Particle in Cell Method for MAC Grids and Fluid Simulation." *Journal of Computational Physics* 408, 109311, 2020. (Verified via search.)
* Kugelstadt, T., Longva, A., Thuerey, N., Bender, J. "Implicit Density Projection for Volume Conserving Liquids." *IEEE Transactions on Visualization and Computer Graphics* 27(4), 2021. (Verified via search.)
* Ferstl, F., Ando, R., Wojtan, C., Westermann, R., Thuerey, N. "Narrow Band FLIP for Liquid Simulations." *Computer Graphics Forum* 35(2) (Eurographics), 2016.
* Chentanez, N., Müller, M. "Real-Time Eulerian Water Simulation Using a Restricted Tall Cell Grid." *ACM Transactions on Graphics* 30(4) (SIGGRAPH), 2011.
* Chentanez, N., Müller, M., Kim, T.-Y. "Coupling 3D Eulerian, Heightfield and Particle Methods for Interactive Simulation of Large Scale Liquid Phenomena." *IEEE TVCG* 21(10), 2015 (earlier version at SCA 2014).
* McAdams, A., Sifakis, E., Teran, J. "A Parallel Multigrid Poisson Solver for Fluids Simulation on Large Grids." *ACM SIGGRAPH/Eurographics Symposium on Computer Animation (SCA)*, 2010.
* Batty, C., Bertails, F., Bridson, R. "A Fast Variational Framework for Accurate Solid-Fluid Coupling." *ACM Transactions on Graphics* 26(3) (SIGGRAPH), 2007.
* Batty, C., Bridson, R. "Accurate Viscous Free Surfaces for Buckling, Coiling, and Rotating Liquids." *SCA*, 2008.
* Larionov, E., Batty, C., Bridson, R. "Variational Stokes: A Unified Pressure-Viscosity Solver for Accurate Viscous Liquids." *ACM Transactions on Graphics* 36(4) (SIGGRAPH), 2017.
* Robinson-Mosher, A., Shinar, T., Gretarsson, J., Su, J., Fedkiw, R. "Two-Way Coupling of Fluids to Rigid and Deformable Solids and Shells." *ACM Transactions on Graphics* 27(3) (SIGGRAPH), 2008.
* Carlson, M., Mucha, P. J., Turk, G. "Rigid Fluid: Animating the Interplay Between Rigid Bodies and Fluid." *ACM Transactions on Graphics* 23(3) (SIGGRAPH), 2004.
* Goktekin, T. G., Bargteil, A. W., O'Brien, J. F. "A Method for Animating Viscoelastic Fluids." *ACM Transactions on Graphics* 23(3) (SIGGRAPH), 2004.

### SPH family

* Monaghan, J. J. "Simulating Free Surface Flows with SPH." *Journal of Computational Physics* 110(2), 1994.
* Müller, M., Charypar, D., Gross, M. "Particle-Based Fluid Simulation for Interactive Applications." *SCA*, 2003.
* Becker, M., Teschner, M. "Weakly Compressible SPH for Free Surface Flows." *SCA*, 2007.
* Solenthaler, B., Pajarola, R. "Predictive-Corrective Incompressible SPH." *ACM Transactions on Graphics* 28(3) (SIGGRAPH), 2009.
* Ihmsen, M., Cornelis, J., Solenthaler, B., Horvath, C., Teschner, M. "Implicit Incompressible SPH." *IEEE TVCG* 20(3), 2014.
* Ihmsen, M., Orthmann, J., Solenthaler, B., Kolb, A., Teschner, M. "SPH Fluids in Computer Graphics." *Eurographics State of the Art Reports*, 2014.
* Bender, J., Koschier, D. "Divergence-Free Smoothed Particle Hydrodynamics." *SCA*, 2015.
* Bender, J., Koschier, D. "Divergence-Free SPH for Incompressible and Viscous Fluids." *IEEE TVCG* 23(3), 2017.
* Akinci, N., Ihmsen, M., Akinci, G., Solenthaler, B., Teschner, M. "Versatile Rigid-Fluid Coupling for Incompressible SPH." *ACM Transactions on Graphics* 31(4) (SIGGRAPH), 2012.
* Akinci, N., Akinci, G., Teschner, M. "Versatile Surface Tension and Adhesion for SPH Fluids." *ACM Transactions on Graphics* 32(6) (SIGGRAPH Asia), 2013.
* Koschier, D., Bender, J. "Density Maps for Improved SPH Boundary Handling." *SCA*, 2017.
* Bender, J., Kugelstadt, T., Weiler, M., Koschier, D. "Volume Maps: An Implicit Boundary Representation for SPH." *Motion, Interaction and Games (MIG)*, 2019.
* Weiler, M., Koschier, D., Brand, M., Bender, J. "A Physically Consistent Implicit Viscosity Solver for SPH Fluids." *Computer Graphics Forum* 37(2) (Eurographics), 2018.
* Peer, A., Ihmsen, M., Cornelis, J., Teschner, M. "An Implicit Viscosity Formulation for SPH Fluids." *ACM Transactions on Graphics* 34(4) (SIGGRAPH), 2015.
* Peer, A., Gissler, C., Band, S., Teschner, M. "An Implicit SPH Formulation for Incompressible Linearly Elastic Solids." *Computer Graphics Forum* 37(6), 2018.
* Kugelstadt, T., Bender, J., Fernández-Fernández, J. A., Jeske, S. R., Löschner, F., Longva, A. "Fast Corotated Elastic SPH Solids with Implicit Zero-Energy Mode Control." *Proc. ACM Comput. Graph. Interact. Tech.* 4(3) (SCA), 2021. (Author list not re-verified.)
* Gissler, C., Peer, A., Band, S., Bender, J., Teschner, M. "Interlinked SPH Pressure Solvers for Strong Fluid-Rigid Coupling." *ACM Transactions on Graphics* 38(1), 2019.
* Koschier, D., Bender, J., Solenthaler, B., Teschner, M. "A Survey on SPH Methods in Computer Graphics." *Computer Graphics Forum* 41(2) (Eurographics STAR), 2022.
* Clavet, S., Beaudoin, P., Poulin, P. "Particle-Based Viscoelastic Fluid Simulation." *SCA*, 2005.

### Position-based

* Macklin, M., Müller, M. "Position Based Fluids." *ACM Transactions on Graphics* 32(4) (SIGGRAPH), Article 104, 2013.
* Macklin, M., Müller, M., Chentanez, N., Kim, T.-Y. "Unified Particle Physics for Real-Time Applications." *ACM Transactions on Graphics* 33(4) (SIGGRAPH), Article 153, 2014.
* Macklin, M., Müller, M., Chentanez, N. "XPBD: Position-Based Simulation of Compliant Constrained Dynamics." *Motion in Games (MIG)*, 2016.
* Macklin, M., Storey, K., Lu, M., Terdiman, P., Chentanez, N., Jeschke, S., Müller, M. "Small Steps in Physics Simulation." *SCA*, 2019.
* Müller, M., Macklin, M., Chentanez, N., Jeschke, S., Kim, T.-Y. "Detailed Rigid Body Simulation with Extended Position Based Dynamics." *Computer Graphics Forum* 39(8) (SCA), 2020.
* Barreiro, H., García-Fernández, I., Alduán, I., Otaduy, M. A. "Conformation Constraints for Efficient Viscoelastic Fluid Simulation." *ACM Transactions on Graphics* 36(6) (SIGGRAPH Asia), Article 221, 2017. (Verified via search.)

### Material Point Method

* Stomakhin, A., Schroeder, C., Chai, L., Teran, J., Selle, A. "A Material Point Method for Snow Simulation." *ACM Transactions on Graphics* 32(4) (SIGGRAPH), 2013.
* Ram, D., Gast, T., Jiang, C., Schroeder, C., Stomakhin, A., Teran, J., Kavehpour, P. "A Material Point Method for Viscoelastic Fluids, Foams and Sponges." *SCA*, 2015.
* Yue, Y., Smith, B., Batty, C., Zheng, C., Grinspun, E. "Continuum Foam: A Material Point Method for Shear-Dependent Flows." *ACM Transactions on Graphics* 34(5), 2015.
* Hu, Y., Fang, Y., Ge, Z., Qu, Z., Zhu, Y., Pradhana, A., Jiang, C. "A Moving Least Squares Material Point Method with Displacement Discontinuity and Two-Way Rigid Body Coupling." *ACM Transactions on Graphics* 37(4) (SIGGRAPH), Article 150, 2018.
* Gao, M., Wang, X., Wu, K., Pradhana, A., Sifakis, E., Yuksel, C., Jiang, C. "GPU Optimization of Material Point Methods." *ACM Transactions on Graphics* 37(6) (SIGGRAPH Asia), 2018. (Author list not re-verified.)
* Su, H., Xue, T., Han, C., Jiang, C., Aanjaneya, M. "A Unified Second-Order Accurate in Time MPM Formulation for Simulating Viscoelastic Liquids with Phase Change." *ACM Transactions on Graphics* 40(4) (SIGGRAPH), 2021. (Not re-verified.)
* Fei, Y. R., Guo, Q., Wu, R., Huang, L., Gao, M. "Revisiting Integration in the Material Point Method: A Scheme for Easier Separation and Less Dissipation." *ACM Transactions on Graphics* 40(4) (SIGGRAPH), 2021. (Not re-verified.)

### Rendering (as it bears on solver output)

* van der Laan, W. J., Green, S., Sainz, M. "Screen Space Fluid Rendering with Curvature Flow." *ACM Symposium on Interactive 3D Graphics and Games (I3D)*, 2009.
* Yu, J., Turk, G. "Reconstructing Surfaces of Particle-Based Fluids Using Anisotropic Kernels." *ACM Transactions on Graphics* 32(1), 2013.

### Software and licenses (inspected 2026-11-03)

* SPlisHSPlasH — https://github.com/InteractiveComputerGraphics/SPlisHSPlasH — MIT — commit `dba6b26b`.
* PositionBasedDynamics — https://github.com/InteractiveComputerGraphics/PositionBasedDynamics — MIT — commit `10a70bc1`.
* NVIDIA FleX 1.2.0 — https://github.com/NVIDIAGameWorks/FleX — NVIDIA Source Code License (1-Way Commercial) — commit `b1ea0f87` (2021-04-15).
* NVIDIA PhysX SDK — https://github.com/NVIDIA-Omniverse/PhysX — BSD-3-Clause — commit `b97365c6`, release `110.3-omni-and-physx-5.9.1`.
* Blender FLIP Fluids — https://github.com/rlguy/Blender-FLIP-Fluids — commit `70a0e954` (license scope → `LICENSING_AUDIT.md`).
* Press and secondary sources for PhysX GPU source release: NVIDIA Developer Blog, "Open Source Simulation Expands with NVIDIA PhysX 5 Release" (2022); CG Channel, "NVIDIA open-sources PhysX's GPU simulation code" (2025-04-22).

*Citation note:* Bibliographic details marked "verified via search" were checked in this session. The rest are from domain knowledge and should be spot-checked before external publication. Entries flagged "not re-verified" carry the most uncertainty.
