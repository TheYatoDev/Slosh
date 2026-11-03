# ARCHITECTURE_DECISION.md

**Decision date:** 2026-11-03 · **Status:** accepted for Phase 1 (prototype built); Phase 2 conditions below.

## Decision

Build a **native GPU FLIP/APIC solver on a dense MAC grid in UE 5.8 RDG compute (Strategy C)**.
Blender FLIP Fluids serves as the validated algorithmic reference, not as linked code. Rigid bodies are coupled
by **asynchronous delayed feedback**: the GPU reduces per-body quantities, a non-blocking readback brings them to the CPU, and
forces are applied to Chaos bodies on the game thread. Liquid is rendered by **screen-space fluid rendering** through
the renderer's public post-opaque delegate. Simulation, rendering and gameplay live in separate modules.

## Alternatives considered

| Option | Verdict | Evidence |
|---|---|---|
| A: link FLIP Fluids CPU engine | Rejected for runtime | MinGW-only (CMake `FATAL_ERROR` on MSVC), offline frame-based design, serial MIC(0)-PCG at up to 900 iterations: FLIP_FLUIDS_FEASIBILITY.md. Kept as an offline reference (E1). |
| B: hybrid CPU/GPU | Rejected | No CPU stage is fast enough to stay on the critical path; GPU↔CPU transfers of particle state each frame would cost more than the compute |
| C: native GPU reimplementation | **Chosen** | 64³ / 262k particles: 2.25–2.72 ms sim + 0.68 ms render [Meas] |
| PBF/XPBD | Fallback | Narrow matrix lead (3.80 vs 3.71), but compressible at real-time iteration counts and no physical viscosity: SOLVER_COMPARISON.md |
| DFSPH, MLS-MPM, FleX | Not chosen | Neighbour-search cost (DFSPH); grid-transfer cost plus viscoelastic focus (MPM, a candidate slime layer later); FleX is binary-only CUDA 9.2 |
| Niagara Fluids | Out of scope as core | Brief forbids dependency |
| `PrePostProcessPass` view extension | Not chosen | Needs Renderer/Internal headers (`bTreatAsEngineModule`) and runs after translucency |

## Rationale
1. FLIP Fluids' MIT engine provides a complete, readable reference for step ordering, APIC transfer and
   boundary handling (FLIP_FLUIDS_SOURCE_AUDIT.md). Its implementation is unsuitable for real-time use.
2. A grid solver makes gameplay queries (submersion, levels, local velocity) and world-space surfaces cheap.
3. APIC's per-particle velocity gradient is the hook for a future viscoelastic slime model.
4. Measured cost at gameplay resolution fits the 4 ms budget with headroom on a mid-range laptop GPU.

## Consequences / accepted limitations
- Dense grids cap practical size at ~64³–96³ per volume until sparse bricks and narrow-band particles exist.
- Coupling forces arrive 2–4 frames late, so the API exposes feedback age explicitly.
- GPU simulation is not deterministic across hardware, so liquid stays out of replicated or rollback state.

## Unresolved decisions
1. Pressure solver: MGPCG vs a fixed RBGS budget (pending E2/E3 after MGPCG).
2. Buoyancy sensor: surface-height sampling vs pressure integral (pending E3 rework).
3. Force application thread: game thread (current) vs `ISimCallbackObject` on the physics thread.
4. World-space surface for shadows and captures: Phase 3 or never (pending art-direction needs).
5. Legal sign-off on the MIT reference and the "no code copied" policy (LICENSING_AUDIT.md).
