# RISK_REGISTER.md

Severity/likelihood: H/M/L. "Validated by" names the experiment in `EXPERIMENT_PLAN.md`. Status reflects
evidence from the Phase 1 prototype run on 2026-11-03 (RTX 5060 Laptop, UE 5.8.4).

| ID | Risk | Sev | Lik | Mitigation | Validated by | Status |
|---|---|---|---|---|---|---|
| R-01 | FLIP Fluids engine cannot be used directly in UE: MinGW-only, offline frame-based, double-precision serial PCG | H | H (confirmed) | Use it as an algorithmic reference only (Strategy C) | E1 | Confirmed by source audit; mitigated by architecture |
| R-02 | CPU solver performance far from 60 FPS | H | H | Do not ship CPU path; keep as offline validation tool | E1 | Accepted |
| R-03 | GPU port complexity (sparse grids, multigrid, meshing) overruns | H | M | Dense-grid baseline first (done), add one subsystem at a time behind measurements | E2, E6 | Baseline **working** |
| R-04 | Sparse grid needed for large domains | M | M | Sparse compute over active 8³ bricks (grid passes and MGPCG level 0) plus sparse **storage** (brick pool, `SparseStorageBudget`) | E6 | **Mitigated:** 128×128×96 shallow pool grid memory 226 → 76 MiB at unchanged cost; 256×256×128 runs in 351 MiB (dense ≈ 1.2 GiB). Budget must cover splash peaks (overflow is counted) |
| R-05 | Pressure solver convergence / free-surface accuracy | M | **L** | Warm start, density correction, MGPCG (Auto above 400k cells), ghost-fluid surface | E2, E3e | **Mitigated:** submerged probe within −1.6% of hydrostatic in a calm pool |
| R-06 | Rigid-body coupling accuracy, instability and noise | H | **L** | PressureAndDrag default, surface-extrapolated pressure, ghost-fluid surface, two-sided density projection, seepage contacts, implicit added mass, accel clamp | E3, E3c–E3e | **Mitigated:** calm-pool float within 0.1 cm of Archimedes, probe −1.5%, agitated box force jitter ~105 N |
| R-07 | GPU/CPU synchronisation stalls | M | L | Non-blocking readback ring (4 slots); never `Lock` before `IsReady` | E3, E6 | Measured latency 3 frames, no stalls observed |
| R-08 | Dynamic collision geometry beyond analytic primitives (skeletal meshes, arbitrary meshes) | M | H | Primitives from `UBodySetup::AggGeom` + physics assets; baked SDF volumes for static meshes | E4 | Open |
| R-09 | Fluid volume loss/gain over long sessions | M | **L** | Two-sided position-based density projection | E2b, E3e | **Mitigated (measured 5 min):** calm +0.000 %/min, agitated −0.007 %/min; hour-scale and emitter+drain scenes untested |
| R-10 | Numerical instability at high velocity | M | M | CFL substeps from lagged max speed, speed clamp, RK2 | E2, E4 | No blow-ups in 20 s runs |
| R-11 | Renderer integration breaks across engine versions | M | M | Use the public post-opaque delegate only | — | Low exposure |
| R-12 | Transparency/refraction limitations (translucency order, no off-screen refraction, no motion vectors) | M | H | Accept for prototype; world-space mesh path in Phase 3 | E6b | Known |
| R-13 | Reliance on `FViewInfo`→`FSceneView` layout in the post-opaque callback | L | L | Single-inheritance cast; fall back to `ViewMatrix`/`ProjMatrix` fields + `CreateInvDeviceZToWorldZTransform` | build on each engine upgrade | Accepted |
| R-14 | Unreal Engine version changes (RDG, GPU profiler rework: `RDG_GPU_STAT_SCOPE` removed in 5.8) | M | H | Pin to 5.8; isolate engine calls in `SloshProxy`/`SloshRenderer` | build | Mitigated |
| R-15 | Third-party licensing: GPL addon code, unlicensed Batty-derived files, Mixbox non-commercial | H | L (if rules followed) | No code copied; MIT notice kept; legal review of LICENSING_AUDIT §questions | legal | Open (legal) |
| R-16 | Memory consumption of dense grids + particle buffers | M | M | ~42 MB at default; ~1.2 GB worst case at 256³ → cap grid; sparse later | E6 | Open |
| R-17 | Performance scaling beyond 262k particles / 64³ | H | **L–M** | Spatial sort, tiled P2G, conditional clears, trimmed APIC gather, MGPCG | E6 | **Mitigated:** 96³ ≈ 5–6.4 ms; 128³ ≈ 6.2 ms at CFL 4 (≈ 16 ms at CFL 2); larger domains need sparse bricks |
| R-18 | Platform compatibility (Vulkan/Metal/consoles: atomics, wave ops, scene texture formats) | M | M | Only int atomics + SM5 features used; no wave intrinsics | port test | Open |
| R-19 | Multiplayer synchronisation of GPU liquid | M | H | Local visual sim + replicated events + server-authoritative proxies (NETWORKING below) | — | Design |
| R-20 | Screen-space splat cost explodes for close-up cameras (per-thread pixel loops up to 64×64) | M | M | MaxSplatPixels cap (32 px); raster sprite path | E6b | Open |
| R-21 | High resolution (256×256×128, 1.25 cm cells, CFL 4): body impact drove particles to the 20 m/s clamp for good, pinning 4 substeps (≈ 50 ms frames) | M | M | RK2 midpoint blends to Euler when the midpoint is > 1 cell away, and unknown faces take the particle velocity (PERFORMANCE_PLAN §10d); `GetClampedParticleCount()` monitors it | E6 | **Mitigated:** 0 clamped particles, 1–2 substeps ≈ 2.5 s after impact, 20–31 ms frames |
| R-22 | Slow frames simulate more time, need more substeps and slow further (feedback spiral) | M | M | `MaxFrameDt` caps simulated time per frame at 1/30 s (slow motion below 30 FPS) | E6 | **Mitigated:** 9.8M-particle calm frame 70 → 36 ms (1 substep). Trade-off: below 30 FPS the liquid is not real-time |
| R-23 | Slosh Paint presets are ported from Fluid Painter, a paid Blender add-on whose zips carry no licence text; the repository is public | H | M | Keep preset content local until redistribution rights are confirmed (PAINTER_DESIGN §5) | — | Open |
| R-24 | Slosh Paint meshing is CPU marching cubes; animated drops and Reveal rebuild strokes every frame | M | M | Worker task, one build in flight, per-stroke cache (only changed strokes rebuild), `MaxCellsPerStroke` cap, `QualityScale` | paint test | Open: static paint costs nothing per frame; animated strokes may lag 1–2 frames |

## Networking and determinism (brief §12)

GPU floating-point results are not bit-identical across vendors, drivers or shader compilers. Causes include
fused multiply-add contraction, transcendental precision, and different wave sizes and scheduling. Float
atomics would also make summation order nondeterministic. The prototype's P2G uses integer atomics, so its
sums are order-independent. It is still not cross-hardware deterministic, because every other stage is float.
Recommendation:
- **Local visual simulation** on every client, seeded by **replicated fluid events** (emit, fill, reset,
  large impulses). This is the feasible default.
- **Server-authoritative gameplay proxies** for anything that affects game state, e.g. per-volume water level,
  "is submerged" flags computed server-side from a cheap CPU heightfield or from the server's own sim readback.
- Full GPU state replication (MBs per frame) and deterministic GPU sims are not feasible. A deterministic CPU
  simulation is feasible only at toy resolutions.
- Keep the liquid out of rollback state.
