# GPU FLIP ↔ Chaos Rigid-Body Coupling: Strategy Research

Companion to `UE58_INTEGRATION_PHYSICS.md`, which has the verified engine signatures.

Legend:
- **[Observed]**: verified in the UE 5.8 source (cross-referenced to the companion file).
- **[Inferred]**: design reasoning, numerical-methods knowledge, or the literature. Not engine-verified.

Almost everything here about the numerics is [Inferred] from the literature. The engine-facing facts that shape the design are [Observed]:
- Game-thread `AddForce` / `AddForceAtPosition` forces are constant over the frame's physics interval and are averaged over game frames that produce no physics step.
- `bAllowSubstepping` is ignored.
- Chaos state has no GPU access.
- Results come back through `FRHIGPUBufferReadback`.

---

## 1. Object → fluid (solids as boundary conditions)

### 1.1 Representation: analytic SDF primitives [Inferred design, Observed sources]
Upload one compact record per collider primitive every fluid step:

```
struct FFluidSolidPrimitive            // 64–96 B, StructuredBuffer on the GPU
{
    uint32  Type;          // Sphere / Box / Capsule / (later) SDF-volume
    uint32  BodyIndex;     // which rigid body it belongs to (for force reduction)
    float3  Center;        // world, at the END of the fluid step (t+dt)
    float4  Rotation;      // quat, world
    float3  Extents;       // sphere: r; box: half-extents (X/2,Y/2,Z/2!); capsule: (r, halfLen, -)
    float3  PrevCenter;    // world, at t (for swept tests)
    float4  PrevRotation;
};
struct FFluidRigidBody                  // one per body
{
    float3 COM;  float3 LinVel;  float3 AngVel;   // world; ω in rad/s
    float  InvMass; float3 InvInertiaDiag; float4 InertiaRot;   // only needed for implicit/strong coupling
};
```
Sources [Observed]: `UBodySetup::AggGeom.{SphereElems,BoxElems,SphylElems}` scaled via `GetFinalScaled`. Note that `FKBoxElem::X/Y/Z` are **full** lengths and the capsule axis is local Z. Body state comes from `GetCOMPosition`, `GetUnrealWorldVelocity`, `GetUnrealWorldAngularVelocityInRadians`. For kinematic or animated bodies, compute v and ω by finite difference of the uploaded transforms.

Primitive SDFs are a few ALU ops each: sphere `|p−c|−r`, box (Quilez rounded-box formula with r=0), capsule (segment distance minus r). For each grid cell, evaluate `φ_solid(x) = min_k φ_k(x)` once per step and store it with the owning primitive or body id (the argmin) in a 3D texture. Cull with each body's world AABB (`FBodyInstance::GetBodyBounds`). Convex hulls can become a set of planes (max of plane distances: exact inside, conservative outside), or be pre-baked into a small local SDF volume per mesh.

Solid velocity at any point: `u_s(x) = v_b + ω_b × (x − c_b)`.

### 1.2 MAC-face solid velocity boundary condition [Inferred, standard]
For a face whose fluid side borders a solid, set the normal component `u_f = u_s(x_f)·n_axis` (free-slip, no-penetration). Tangential velocity is left alone, or blended toward `u_s` for a no-slip option. Apply this:
1. after particle-to-grid transfer, before pressure (so the divergence uses solid-consistent fluxes), and
2. after pressure, before grid-to-particle, so FLIP deltas don't inject solid-normal velocity.

After the solve, extrapolate fluid velocity into the solid band (2–3 cells) so particles near walls interpolate sensibly. Then project particles that ended up with `φ_solid<0` back out along `∇φ_solid`, and **replace their relative normal velocity**: `v_p ← v_p − min(0,(v_p−u_s)·n) n`.

### 1.3 Fractional solid weights in the pressure solve (Batty et al. 2007) [Inferred]
Binary voxelised solids give stair-stepped boundaries, fluid that sticks to sloped walls, and fluid that leaks through thin, fast objects. Batty's variational method minimises kinetic energy subject to incompressibility. Each face gets a weight `w_f ∈ [0,1]`, the fraction of the face **open to fluid**. In 3D, estimate it from `φ_solid` at the 4 face corners, with the 2D "fraction inside" formula per edge pair, or by supersampling. The pressure equation for fluid cell *i* becomes

```
Σ_f∈faces(i)  w_f · (Δt / (ρ Δx²)) · (p_i − p_nbr(f))
      = −(1/Δx) Σ_f ± [ w_f · u*_f + (1 − w_f) · u_s,f · n_f ]
```

After the solve, `u_f = u*_f − (Δt/ρΔx) (p_nbr − p_i)` only where `w_f > 0`. Faces with `w_f = 0` take `u_s·n`. The matrix stays SPD and fits the same GPU PCG/multigrid solver, because the weights only scale existing coefficients. Combine with the ghost-fluid liquid–air boundary (`θ` fractions) for the free surface. Cost is negligible: a weights texture plus a few extra multiplies.

The **two-way** extension (also in Batty 2007, and generalised by Robinson-Mosher 2008) adds the rigid body's 6 DOF as unknowns. Coupling term: `J^T p`, with `J` mapping (v, ω) to face fluxes `(1−w_f) n_f (v + ω×r_f)`. The result is a symmetric indefinite system, or an SPD Schur complement `A + J M⁻¹ J^T`, solved for pressure and body velocity together. That is what "synchronized two-way" means in §3. It needs body mass and inertia on the GPU, plus a dense 6×6 block per body inside the Krylov loop.

### 1.4 High-speed penetration [Inferred]
- **Fluid substeps.** Pick the fluid dt with CFL against both fluid and solid velocity: `dt ≤ C·Δx / max(|u_fluid|, |u_s|)`, with C ≈ 1–2 for FLIP. Interpolate solid transforms between the previous and current poses for each substep (slerp the rotation). That is why the primitive record carries `Prev*`.
- **Swept volumes.** For a primitive moving more than about Δx per substep, rasterise the union of its poses at the substep fractions. A sphere's sweep is exactly a capsule. Boxes and capsules can be conservatively approximated by 2–4 intermediate samples.
- **Particle CCD-like handling.** Transform each particle into the solid's local frame at t and t+dt and test the segment. If it crossed the surface, place it at the exit point at t+dt with the solid's normal velocity. That catches particles tunnelled by thin, fast plates.
- **Thin walls.** Require colliders to be at least 2Δx thick for the grid solve (inflate the SDF by `0.5–1·Δx` if needed), and rely on particle projection for the rest.

### 1.5 Rotating containers [Inferred]
- Model the container as an inverted SDF (fluid allowed where `φ_container_interior < 0`). The boundary condition is `u·n = (ω × r)·n`. Free-slip means a spinning cylinder won't spin up the fluid. If that matters, add a tangential no-slip blend in the wall band, or a wall-viscosity term.
- For long-duration spinning (centrifuges, mixers), consider simulating in the container's rotating frame. Fictitious forces `−2ω×u − ω×(ω×r) − ω̇×r` avoid the large boundary velocities that drive the CFL [Inferred; standard rotating-frame CFD].
- Container motion is often kinematic (animated or player-driven), so it is pure one-way rigid→fluid. That is cheap and stable.

---

## 2. Fluid → object (forces on rigid bodies)

### 2.1 (a) Pressure integration / momentum exchange on the GPU [Inferred]
**Pressure force.** `F = −∮ p n dA` (plus viscous traction, usually negligible for water at game scales). In the Batty framework, the discrete force consistent with the solve is `F = Jᵀ p`:

```
for every face f between fluid cell i and a (partially) solid neighbour, owned by body b:
    A_f   = (1 − w_f) · Δx²                // solid-occupied area of that face
    F_b  += −p_i · A_f · n_f               // n_f = outward normal of the solid, i.e. toward the fluid
    τ_b  += (x_f − c_b) × (−p_i · A_f · n_f)
```
- Because p contains the hydrostatic part (gravity is in the momentum equation), **buoyancy emerges automatically**, along with dynamic pressure (lift, slamming) and correct torques.
- Caveat: with a free surface and the ghost-fluid method, partially submerged bodies get `p=0` above the surface, which is correct.
- Caveat: grid pressure near a moving solid is noisy at low resolution. Temporal smoothing (an EMA over 2–4 frames) is usually required.

**Momentum exchange (alternative).** Accumulate `Δ(m_p v_p)` of particles the body pushed during particle projection, plus the velocity-BC correction on faces. Then `F_b = −Σ Δmomentum / dt`. This conserves momentum by construction, but it is noisier and resolution-dependent.

**GPU reduction.** One thread per boundary face or particle computes its contribution and reduces within the wave (`WaveActiveSum`). Lane 0 then does an `InterlockedAdd` into per-body accumulators. Use **fixed-point int64** accumulators (scaled, for example 2⁻¹⁶ N) so results are order-independent and deterministic. With many bodies, first sort or compact by body id (the argmin from §1.1) and do a segmented reduction.

### 2.2 (b) Analytic buoyancy and drag from GPU-measured quantities [Inferred]
Measure on the GPU per body:
- `V_sub`: count of fluid-marked cells (or particles × particle volume) with `φ_body < 0`, times Δx³.
- `c_sub`: centroid of those cells (the centre of buoyancy).
- `ū_f`: mean fluid velocity in a shell `0 < φ_body < k·Δx` (k≈1–2) around the submerged part.
- Optionally the wetted-area proxy `A_wet`: the count of boundary faces adjacent to fluid.

Then the CPU computes, **using the current body state, not lagged state**:
```
F_buoy  = −ρ_f · V_sub · g                        (applied at c_sub → produces righting torque)
v_rel   = ū_f − v_body(c_sub)
F_drag  = ½ ρ_f C_d A_ref |v_rel| v_rel   (quadratic)  or  k_lin · v_rel (linear, more stable)
τ_drag  = −c_rot · ρ_f · V_sub^{5/3} · ω_body      (rotational damping proxy)
```
This is robust, cheap, and tunable (C_d, added-mass coefficient). It cannot capture lift, wave slamming, or flow-induced torques beyond buoyancy and drag. Most games want exactly this level.

**Hybrid (recommended).** Use (b) for buoyancy, drag, and stability. Optionally add the *dynamic* part of (a): `F_dyn = F_pressure − F_hydrostatic`, where `F_hydrostatic ≈ −ρ_f V_sub g`. Filter it and scale it by a user gain for splash, wave-push, and current effects.

### 2.3 Readback of a small per-body result buffer [Observed API, Inferred pipeline]
```
struct FFluidBodyResult { int64 Fx,Fy,Fz; int64 Tx,Ty,Tz;          // fixed-point pressure F/τ (about COM at upload time)
                          uint  SubCells; float3 SubCentroidSum;    // V_sub, c_sub
                          float3 MeanFluidVel; uint ShellCells;
                          uint  FrameId; uint BodyGeneration; };   // ~96 B/body → 256 bodies ≈ 24 KB
```
- Render graph: `AddEnqueueCopyPass(GraphBuilder, Readback[k], ResultBuffer, NumBytes)` (`RenderGraphUtils.h:847`). Keep a ring of N = 3–4 `FRHIGPUBufferReadback`s (`RHIGPUReadback.h:132`).
- Each render frame, poll the oldest ring entry: `IsReady()` → `Lock(NumBytes)` → copy → `Unlock()`. Push the result to the game thread with `FrameId` through a lock-free single-producer/single-consumer queue or a triple buffer. Never block.
- Game thread: in `TG_PrePhysics`, or in an `FPhysScene_Chaos::OnPhysScenePreTick` handler, take the newest result. Map `BodyIndex → FBodyInstance*` with a generation check against despawned bodies, then call:
  - `BI->AddForceAtPosition(F, c_sub)`, `BI->AddTorqueInRadians(τ_extra)` [Observed signatures], or
  - `BI->AddForce(F)` + `BI->AddTorqueInRadians(τ)` when the torque was already reduced about the COM.
- Typical latency [Inferred]: the game thread packs body state in frame N, the render thread runs it in N (1-frame-behind RT), the GPU finishes in N+1, and the readback is ready for the game thread in N+2 or N+3. Treat it as **2–3 frames** at 60 Hz (33–50 ms).

---

## 3. Comparison of coupling schemes

| Scheme | Fidelity | Stability | Performance | CPU/GPU sync | Latency | Momentum conservation | Responsiveness | Complexity |
|---|---|---|---|---|---|---|---|---|
| **One-way rigid→fluid** (bodies are moving boundaries only) | Fluid reacts correctly; bodies ignore fluid | Excellent (no feedback loop) | Best: upload only | None (upload only) | 0 (fluid sees poses of the same frame or 1 frame old) | Not conserved (infinite-mass solids) | Fluid responsive; bodies don't float | Low |
| **CPU gameplay proxies** (analytic buoyancy and drag from a CPU-sampled height or velocity field: a coarse readback or analytic waves) | Low–medium: no splash forces | Very good (fully CPU, implicit drag possible) | Cheap; CPU cost per body | Small field readback, or none | 0 for proxy physics; visuals lag | Not conserved | Immediate body response | Low–medium |
| **Approximate two-way, async with delayed GPU force feedback** (§2.2 hybrid, 1–3 frame lag) | Medium: buoyancy, drag, wave push, approximate torques | Good **with** mitigations (§5); unstable for light bodies without them | Good: one reduction pass plus a small readback | Async readback, no stalls | 2–3 frames on fluid→body forces | Approximate: equal and opposite only if the same forces are fed back to the fluid; lag breaks exactness | Good for heavy and medium bodies; light bodies feel "floaty or late" | Medium |
| **Approximate two-way, same-frame** (GPU waits for the body force within a frame, CPU stalls on readback) | Medium | Better than lagged (one lag step, not 3) | Poor: GPU→CPU stall every frame (several ms) | **Blocking** sync | ≈0–1 frame | Approximate | Good | Medium, plus scheduling pain |
| **Synchronized (monolithic) two-way** (Batty 2007 / Robinson-Mosher 2008: bodies solved inside the GPU pressure system, results written back to Chaos) | High: correct added mass, light bodies, stacks in fluid | Excellent for light bodies (implicit) | Medium–high cost: extra Krylov terms; Chaos contacts still separate | Bodies must be **integrated on the GPU** or results read back before Chaos steps, so a blocking sync or GPU-owned bodies | 0 inside the fluid step; Chaos contacts lag | Conserved in the solve (up to the Chaos split) | Best | High: dual ownership of body state against Chaos |
| **Asynchronous, delayed force feedback with GPU-side "shadow" body integration** (GPU integrates a shadow copy of each body under fluid forces each substep, and the CPU receives a velocity correction instead of a force) | Medium–high | Good (implicit on GPU), but shadow and Chaos state diverge under contacts | Medium | Async | 2–3 frames for corrections | Approximate | Good | High |

Notes [Inferred]:
- Strong coupling inside Chaos is not possible. Chaos runs on CPU tasks with no GPU hooks [Observed: no RHI in Chaos]. A monolithic coupled solve means either GPU-owned body integration with Chaos reduced to collision, or a per-frame blocking readback. Both fight the engine.
- Robinson-Mosher 2008 is fully implicit and handles thin shells and deformables. It is overkill for game rigid bodies, but it is the reference if precision matters (ship hulls at low mass ratio, light flotsam).

---

## 4. Recommendation

### 4.1 Initial approach [Inferred]
**Asynchronous approximate two-way coupling with 1–3-frame-delayed GPU readback, applied on the game thread through `FBodyInstance`.**
1. **Rigid→fluid every fluid step.** Analytic SDF primitives (sphere, box, capsule from `AggGeom`) with per-body v and ω and a previous pose. MAC normal-velocity boundary condition. Batty fractional face weights in the pressure solve. Substep interpolation and particle push-out for fast bodies.
2. **Fluid→rigid every frame.** A GPU reduction produces per body: `V_sub`, `c_sub`, mean shell fluid velocity, and the filtered dynamic-pressure force and torque. These are written to a small fixed-point buffer and read back through a ring of 3 `FRHIGPUBufferReadback`.
3. **Apply on the game thread** before physics (`TG_PrePhysics` or `OnPhysScenePreTick`). Recompute buoyancy, drag, and added-mass compensation **with the current Chaos body state**, which is fresher than the GPU data. Then call `AddForceAtPosition(F, c_sub)` and `AddTorqueInRadians(τ)`. Skip calls with zero force so bodies can sleep. Chaos treats the force as constant across that frame's physics steps and averages it over frames without a step [Observed].
4. **Stability limits** (§5): implicit-equivalent drag clamp, added-mass inflation, force and acceleration clamp, EMA filtering of the GPU dynamic term, and a mass-ratio threshold below which a body is demoted to proxy-only coupling.
5. **Abstraction.** All engine access goes through `IFluidRigidBodyAdapter` (§6). A Jolt (or other) backend can implement the same interface later.

### 4.2 Conditions that justify more accurate coupling [Inferred]
- Light bodies matter: density ratio `ρ_b/ρ_f < ~0.5`, such as foam, wood planks, leaves, or hollow boats with small wall mass. Their added mass dominates and lagged explicit forces blow up or need heavy damping. In that case, move to per-physics-step application through a `TSimCallbackObject` (`PreIntegrate`), using an implicit drag and added-mass formulation on the physics thread. If that is still not enough, use GPU-side shadow integration or a monolithic solve.
- Gameplay relies on precise fluid-driven motion: water wheels, sluice gates, fluid-driven puzzles where torque accuracy and phase matter, or high-speed boats where slamming and planing lift come from the pressure field.
- Tight multi-body stacks or articulated bodies in fluid. Contact and fluid forces must be solved together, otherwise stacks jitter.
- Fixed-timestep, networked, or deterministic physics. Lagged, non-deterministic GPU forces must be quantised and replicated, or replaced by deterministic CPU proxies.

---

## 5. Added-mass instability and mitigations [Inferred]

**Mechanism.** A body accelerating at `a` in a fluid has to accelerate surrounding fluid too. The fluid exerts a reaction force of about `−m_a·a`, where `m_a = C_a·ρ_f·V_sub` (C_a = 0.5 for a sphere, about 1 for a cylinder moving broadside, larger near walls or the free surface). In a **partitioned explicit** scheme the body sees the fluid force computed from its acceleration `k` frames ago:

```
m_b · a_{n} = F_ext − m_a · a_{n−k}      ⇒   a_n = (F_ext − m_a a_{n−k}) / m_b
```

The homogeneous part has an amplification factor of magnitude `m_a/m_b` per k-step. It **diverges when m_a/m_b > 1**, i.e. `ρ_b < C_a·ρ_f`, **regardless of the time step**. Halving dt does not help. Delay (k = 2–3) makes it worse: the feedback becomes an oscillating, delayed loop, and the stability margin shrinks further for drag-like terms too. The same staggered-coupling result is well known in FSI (Causin, Gerbeau & Nobile 2005; Förster, Wall & Ramm 2007). Drag has the analogous explicit limit `c·dt/m_b < 2` without delay, and is much stricter with a k-frame delay.

**Mitigations, in order of preference.**
1. **Implicit added mass on the CPU (mass inflation).** Treat `m_a` as part of the body's inertia. Desired: `(m_b + m_a) a = F_g + F_dyn`, where `F_dyn` = buoyancy + drag + filtered pressure *excluding* the added-mass reaction. Chaos integrates `m_b a = F_g + F_applied`, so apply
   `F_applied = (m_b·F_dyn − m_a·F_g) / (m_b + m_a)`.
   If you use the measured GPU pressure force, first remove its lagged added-mass content: `F_dyn ≈ F_meas + m_a·a_lag`, with `a_lag` from the body's velocity history at the frame the GPU sampled. Do the same for rotation with an added-inertia estimate.
2. **Implicit drag from current velocity.** Compute drag on the CPU with the **current** body velocity and the lagged fluid velocity, using the exact exponential decay: `Δv = (ū_f − v)(1 − e^{−c·dt/(m_b+m_a)})`, so `F = (m_b+m_a)·Δv/dt`. This can never overshoot the relative velocity. Applied once per frame with game-frame dt, it is exact for constant forcing [Observed: Chaos holds the force constant across the frame's steps].
3. **Clamp.** Per frame, `|ΔV| = |F|·dt/m_b ≤ min(|v_rel| + |g|·dt·(ρ_f/ρ_b), a_max·dt)`, and `|F| ≤ k_max·m_b·|g|` (k_max ≈ 5–20). Clamp torque similarly via `|Δω|`. Clamp `V_sub ≤ V_body` too.
4. **Filter the GPU dynamic term.** Use an EMA with time constant ≥ the latency (τ ≈ 2–4 frames). Optionally add Aitken-style under-relaxation `F ← F_prev + α(F_new − F_prev)`, α≈0.3–0.6.
5. **Extra damping.** Increase `LinearDamping` and `AngularDamping` while the body is submerged, proportional to the submerged fraction.
6. **Mass-ratio gate.** Below `ρ_b/ρ_f ≈ 0.3`, or for very small bodies (`V_body < ~8Δx³`), drop the measured pressure term. Use only the analytic proxy (buoyancy + implicit drag + inflated mass), and optionally make the body one-way, i.e. the fluid still sees it.
7. **Per-step application** (if needed). Move steps 1–2 into a `TSimCallbackObject<…, PreIntegrate>` on the physics thread. Pass the latest GPU aggregates as the input struct. Each substep then uses the current internal velocity, which removes the remaining frame-level lag.

**Momentum note.** For visible reaction (body pushes water and water pushes back), the fluid side already gets the body's motion through the boundary condition. Don't also add an equal-and-opposite body force into the fluid, or you double count. Accept that the lagged scheme only conserves momentum approximately.

---

## 6. Physics-backend abstraction (Jolt-compatible, no Jolt dependency) [Inferred]

Design constraints:
- Units are engine units (cm, kg, s). Everything is world space. Angular values are in radians.
- No backend types in the interface. Body handles are opaque `uint64` values with a generation counter.
- Thread contract is explicit: all methods are game-thread unless suffixed `_PhysicsThread`.
- A backend reports its own timing: fixed dt, latency, whether forces are per frame or per step.

```cpp
// FluidRigidBodyAdapter.h  (plugin Runtime module; depends only on Core/CoreUObject/Engine math types)
#pragma once
#include "CoreMinimal.h"

enum class EFluidShapeType : uint8 { Sphere, Box, Capsule, ConvexPlanes, SdfVolume };

struct FFluidColliderShape
{
    EFluidShapeType Type = EFluidShapeType::Sphere;
    FTransform      LocalToBody;          // scale already baked into extents (e.g. via GetFinalScaled)
    FVector3f       Extents = FVector3f::ZeroVector; // sphere: (r,0,0) box: half-extents capsule: (r, halfSegLen, 0) — capsule axis = local Z
    TArray<FPlane4f> Planes;              // ConvexPlanes only (body space)
    bool            bAffectsFluid = true;
};

struct FFluidBodyHandle { uint64 Id = 0; uint32 Generation = 0; bool IsValid() const { return Id != 0; } };

struct FFluidBodyState
{
    FTransform BodyToWorld;               // no scale
    FVector    CenterOfMassWorld;
    FVector    LinearVelocity;            // cm/s
    FVector    AngularVelocity;           // rad/s, world
    float      Mass = 0.f;                // kg (0 => static / infinite)
    FVector    InertiaDiagLocal;          // kg·cm², principal
    FQuat      InertiaRotation;           // principal axes → world
    bool       bDynamic = false;          // receives forces
    bool       bKinematic = false;        // moves but ignores forces
    bool       bAwake = false;
};

struct FFluidSyncTiming
{
    double PhysicsResultsTime = 0.0;      // time the reported state corresponds to (Chaos: GetPhysicsResultsTime_External)
    double FixedStep = 0.0;               // 0 if variable
    bool   bForcesConstantOverFrame = true;  // Chaos GT forces: true (Observed); Jolt AddForce: per-step, cleared after step
    bool   bSupportsPhysicsThreadCallback = false;
};

class IFluidRigidBodyAdapter
{
public:
    virtual ~IFluidRigidBodyAdapter() = default;

    // Discovery / lifetime (game thread)
    virtual void GatherBodiesInBounds(const FBox& WorldBounds, TArray<FFluidBodyHandle>& Out) = 0;
    virtual bool IsBodyValid(FFluidBodyHandle Body) const = 0;

    // Read (game thread; implementations may batch under one lock)
    virtual bool GetBodyState(FFluidBodyHandle Body, FFluidBodyState& Out) const = 0;
    virtual void GetBodyShapes(FFluidBodyHandle Body, TArray<FFluidColliderShape>& Out) const = 0; // cached; shapes rarely change
    virtual void GetBodyStatesBatch(TConstArrayView<FFluidBodyHandle> Bodies, TArrayView<FFluidBodyState> Out) const
    { for (int32 i = 0; i < Bodies.Num(); ++i) GetBodyState(Bodies[i], Out[i]); }

    // Write (game thread). Forces in N_engine (kg·cm/s²), positions world.
    virtual void AddForceAtPosition(FFluidBodyHandle Body, const FVector& Force, const FVector& WorldPos) = 0;
    virtual void AddForceAndTorque(FFluidBodyHandle Body, const FVector& Force, const FVector& TorqueAboutCOM) = 0;

    // Optional per-physics-step hook (backend calls back on its physics thread)
    using FPhysicsStepFn = TFunction<void(double /*Dt*/, double /*SimTime*/)>;
    virtual bool RegisterPhysicsStepCallback_PhysicsThread(FPhysicsStepFn Fn) { return false; }
    virtual bool GetBodyState_PhysicsThread(FFluidBodyHandle, FFluidBodyState&) const { return false; }
    virtual bool AddForceAndTorque_PhysicsThread(FFluidBodyHandle, const FVector&, const FVector&) { return false; }

    // Timing
    virtual FFluidSyncTiming GetSyncTiming() const = 0;
};
```

**Chaos implementation mapping** [Observed signatures, see companion file]:

| Adapter method | Chaos call |
|---|---|
| `GatherBodiesInBounds` | `UWorld::OverlapMultiByObjectType` (World.h:2430), then `UPrimitiveComponent::GetBodyInstance()`. Handle = `FBodyInstance*` + a generation from a side table, or a weak `UPrimitiveComponent` + bone index. |
| `GetBodyState` | `GetUnrealWorldTransform`, `GetCOMPosition`, `GetUnrealWorldVelocity`, `GetUnrealWorldAngularVelocityInRadians`, `GetBodyMass`, `GetBodyInertiaTensor`, `GetMassSpaceToWorldSpace().GetRotation()`, `IsInstanceSimulatingPhysics`, `IsInstanceAwake`. |
| `GetBodyShapes` | `GetBodySetup()->AggGeom.{Sphere,Box,Sphyl,Convex}Elems[i].GetFinalScaled(BI->Scale3D, RelativeTM)`. |
| `AddForceAtPosition` | `FBodyInstance::AddForceAtPosition(F, P)` (default timestamp) |
| `AddForceAndTorque` | `FBodyInstance::AddForce(F)` + `AddTorqueInRadians(T)` |
| `RegisterPhysicsStepCallback_PhysicsThread` | `Solver->CreateAndRegisterSimCallbackObject_External<TSimCallbackObject<In,Out,PreIntegrate>>()`. Use `FRigidBodyHandle_Internal` inside. |
| `GetSyncTiming` | `GetPhysicsResultsTime_External()`, `UPhysicsSettings::Get()->bTickPhysicsAsync / AsyncFixedTimeStepSize`. |

**Jolt mapping** (for later; from Jolt's public API as I know it, not verified here) [Inferred]:
- `BodyInterface::GetCenterOfMassPosition / GetLinearVelocity / GetAngularVelocity / GetWorldTransform`
- `BodyInterface::AddForce(BodyID, Vec3 force, RVec3 point)` and `AddTorque`. Jolt clears accumulated forces after each `PhysicsSystem::Update` step, so the adapter must re-apply each step or each collision substep, i.e. `bForcesConstantOverFrame = false`.
- Shapes come from `SphereShape / BoxShape (half-extent) / CapsuleShape (half-height along Y!)` under `RotatedTranslatedShape`. The adapter must convert axis conventions (Jolt Y-up capsule → local Z) and units (m → cm).

---

## 7. References

- Batty, C., Bertails, F., & Bridson, R. (2007). A Fast Variational Framework for Accurate Solid-Fluid Coupling. *ACM Transactions on Graphics*, 26(3), Article 100 (Proc. SIGGRAPH 2007). https://doi.org/10.1145/1276377.1276502
- Robinson-Mosher, A., Shinar, T., Gretarsson, J., Su, J., & Fedkiw, R. (2008). Two-way Coupling of Fluids to Rigid and Deformable Solids and Shells. *ACM Transactions on Graphics*, 27(3), Article 46 (Proc. SIGGRAPH 2008). https://doi.org/10.1145/1360612.1360645
- Akinci, N., Ihmsen, M., Akinci, G., Solenthaler, B., & Teschner, M. (2012). Versatile Rigid-Fluid Coupling for Incompressible SPH. *ACM Transactions on Graphics*, 31(4), Article 62 (Proc. SIGGRAPH 2012). https://doi.org/10.1145/2185520.2185558. Uses boundary particles weighted by volume Ψ; the rigid body receives the negated sum of the fluid pressure and friction forces on its boundary particles. Directly applicable if the plugin uses particle-level (rather than grid) coupling.
- Carlson, M., Mucha, P. J., & Turk, G. (2004). Rigid Fluid: Animating the Interplay Between Rigid Bodies and Fluid. *ACM Transactions on Graphics*, 23(3), 377–384. https://doi.org/10.1145/1015706.1015733
- Guendelman, E., Selle, A., Losasso, F., & Fedkiw, R. (2005). Coupling Water and Smoke to Thin Deformable and Rigid Shells. *ACM Transactions on Graphics*, 24(3), 973–981. https://doi.org/10.1145/1073204.1073299
- Causin, P., Gerbeau, J.-F., & Nobile, F. (2005). Added-mass effect in the design of partitioned algorithms for fluid–structure problems. *Computer Methods in Applied Mechanics and Engineering*, 194(42–44), 4506–4527. https://doi.org/10.1016/j.cma.2004.12.005
- Förster, C., Wall, W. A., & Ramm, E. (2007). Artificial added mass instabilities in sequential staggered coupling of nonlinear structures and incompressible viscous flows. *Computer Methods in Applied Mechanics and Engineering*, 196(7), 1278–1293. https://doi.org/10.1016/j.cma.2006.09.002
- Bridson, R. (2015). *Fluid Simulation for Computer Graphics* (2nd ed.). CRC Press / A K Peters. (MAC boundary conditions, variational pressure, FLIP.)

---

## Addendum — Prototype findings (2026-11-03, measured)

The Phase 1 prototype (`USloshColliderComponent`, `ProjectCS`, `SubmergedCS`) implements the
asynchronous, delayed-feedback scheme recommended above: forces are read back through a 4-slot
`FRHIGPUBufferReadback` ring, latency is 3 frames, and they are applied in TG_PrePhysics through
`UPrimitiveComponent::AddForce` / `AddTorqueInRadians` with the implicit added-mass formula. The
observed results are recorded in EXPERIMENT_PLAN.md E3:

| Observation | Evidence |
|---|---|
| Sinking body (ρ ≈ 2140) behaves correctly: rests on the bottom, submerged fraction 0.85–0.93 | all runs |
| Buoyant body (ρ ≈ 550) is buoyed but bobs in a sustained limit cycle (≈2.4 s, 25–160 cm) | trace logs |
| Pressure-integral coupling cannot float a body resting on the domain floor (net −230…−450 N, noisy ±400 N laterally) | `Bench/coupling_pressure.log` |
| Four solver/sensor defects created positive feedback and were fixed: solid-interior no-slip, +Z submersion walk, sparse-liquid counting, solid-seeded extrapolation | shader history, EXPERIMENT_PLAN E3 |

Implications for the recommendation above:
1. The analytic route (GPU submerged volume plus CPU buoyancy and drag) stays the default, but its
   sensor must be sturdier. Planned replacement: a per-collider **local liquid surface height**
   (column-top search around the body), followed by an exact CPU computation of the submerged volume
   below that plane. This removes the dependence on cell-walk heuristics.
2. Latency must be compensated. Options: apply forces on the physics thread with an
   `ISimCallbackObject`, so they are evaluated with the current body velocity at every physics step;
   or predict the submerged volume forward by `v_body · latency`.
3. Pressure-integral coupling needs fractional solid weights (Batty 2007) and a converged pressure
   solve (MGPCG) before it can be trusted. Re-run E3 in PressureAndDrag mode as the gate.

## Addendum 2 — Phase 2 results (2026-11-03, measured)

- The pressure field is hydrostatically accurate. A fixed submerged probe measures a time-averaged Fz/ρgV of 0.92–1.06
  (RBGS and MGPCG, 64³ and 128³).
- The Phase 1 "pressure coupling fails" result came from the body resting on the domain floor, where no fluid
  cells lie beneath it, together with tank walls that had no collision. It was not caused by solver error.
- **PressureAndDrag** settles a ρ ≈ 550 kg/m³ box at a submerged fraction of 0.564 (Archimedes 0.547), height SD 7.6 cm.
  The pressure response is immediate in the grid and needs no submersion sensor, so the delayed-sensor
  limit cycle of BuoyancyAndDrag does not occur. It is now the default.
- Open:
  (1) no lift when a body is sealed against the domain floor. Planned fix: fractional solid faces, so a thin
      fluid layer can exist under a body, or a hybrid floor-contact rule;
  (2) lateral forces of 50–180 N in strong currents. These are physical in direction, but staircase faces inflate them.
      Fractional face weights (Batty 2007) are the next fix.

## Addendum 3 — Floor-contact lift (2026-11-03, measured)

**Problem.** Binary cells leave no liquid layer between a body and the domain floor, or between two bodies. The faces
between them are solid–solid and received no pressure. A buoyant body resting on the bottom therefore felt only the
pressure on its top and sides, a net downward force, and never lifted off.

**Fix — seepage pressure** (`EstimateSeepagePressure`, `AccumulateContact` in `Slosh.usf`, applied in `ProjectCS`).
Every face between a two-way collider's cell and a *different* solid (a wall, or another body) receives the
pressure of the liquid surrounding the body at that height. To find it, walk horizontally out of the body from
the contact cell (up to 16 cells in ±X/±Y) and average the pressure of the fluid cells reached. If no liquid is
reached (a dry contact), no force is applied. Contact faces add pressure force and torque only, not wetted-surface or
drag statistics. This is a modelling assumption: real sealed contacts, such as suction cups, are not represented, and
are rarely wanted in games.

**Result** (`Saved/Bench/seep_*.log`):

| Test | Before | After |
|---|---|---|
| ρ ≈ 550 box placed on the floor under ~60 cm of water (`-SloshFloorStart`) | stuck at z = 19.5 cm indefinitely | lifts within 0.27 s (463 N up); floats at mean z 101 cm, SD 18 cm |
| Hard drop from 2.3 m | (previously could pin to the floor) | floats, mean z 100.6 cm, SD 11.4 cm |
| Gentle drop | mean z 88–96 cm, SD 7.6–8.4 cm | mean z 100.4 cm, SD 12.4 cm (within run-to-run spread) |
| Heavy sphere (ρ ≈ 2140) | rests on the bottom, sub ≈ 0.88 | unchanged: rests on the bottom, sub 0.79–0.88 |
| Fixed probe Fz/ρgV | 1.0–1.3 | 1.05–1.37 (probe has no contacts; spread is flow noise) |

The ProjectCS cost is unchanged at 0.02 ms (64³).

**Remaining coupling caveats.**
- The `sub` sensor reads ~0.43–0.46 at 64³ against 0.547 expected. It counts only dense, supported liquid cells,
  so it under-reads near the surface (Inferred). The body's height is the more reliable measure.
- Lateral forces of 50–180 N in strong currents; fractional face weights (Batty 2007) are the planned fix.

## Addendum 4 — Lateral force noise (2026-11-03, measured)

**Metric.** Raw solver pressure force is logged every frame (`rawforce` trace, `-SloshTraceBox`). For frames
where the delayed readback changed, "jitter" is the RMS of the frame-to-frame change (high-frequency noise)
and "std" is the standard deviation (total variability). The moving body is the floating box; the fixed body is
the kinematic probe, which cannot have staircase-flip noise. 64³, t ≥ 8 s.

| Configuration | Probe lateral jitter | Probe vertical jitter | Probe Fz std | Probe mean Fz / ρgV | Box lateral jitter | Liquid volume drift (12→24 s) |
|---|---|---|---|---|---|---|
| Binary faces, integer-count density correction (Phase 2) | 435 N | 418 N | 284 N | 1.17 | 90 N | (volume inflated ~70%) |
| SDF-smoothed faces, integer-count correction | 411 N | 393 N | 266 N | 1.14 | 102 N | — |
| MGPCG ×16 instead of RBGS | 393 N | 391 N | 264 N | 1.12 | 102 N | — |
| No density correction | **140 N** | **146 N** | 104 N | **1.00** | 108 N | **−10%** (−8% in 8 s) |
| **Smooth density correction (new default, 0.15)** | **212 N** | **222 N** | 155 N | 1.10 | 104 N | **−1.9%** |
| Smooth correction 0.05 / 0.30 | 203 / 234 N | 213 / 256 N | 145 / 176 N | 1.09 / 1.11 | — | −3.6% / −2.0% |

**Findings.**
1. **The dominant noise source was the density correction.** Its divergence target was computed from the
   *integer* particle count per cell, which flickers as particles cross cell boundaries. The pressure field
   then oscillated from frame to frame: the probe's frame-to-frame change exceeded √2·std, so successive frames
   were anti-correlated. The fix computes density from the trilinear P2G weight sums on the cell's six faces
   (`FaceWeight` buffer; rest value = particles per cell). That halves the probe jitter and keeps volume
   drift at −1.9% per 12 s.
2. **Staircase quantization was not the main source.** An SDF-weighted force estimator (`CouplingForceCS`:
   `F += −p·h²·Σ w_f n_f`, `w_f = saturate(0.5 + φ/h)`, hybrid with the binary seepage rule for solid–solid contacts)
   gave no measurable gain on either body. It remains enabled (`bSmoothCouplingForce`, cost 0.01 ms) because
   it is continuous for slow sub-cell motion, but that benefit is below the current noise floor and is **not demonstrated**.
3. **The pressure solver is not the source.** MGPCG at 16 iterations leaves the noise unchanged.
4. **The remaining ~100 N of lateral jitter on the moving box** is the same with and without density correction. It is the
   flow-noise floor of this agitated demo pool (paddle, 6,000 particle/s jet), plus free-surface cell flicker
   (Inferred). The ghost-fluid free surface (Stage 2 item 2) is the next candidate.
5. **The old correction inflated the liquid by ~70%.** For the same 262k particles, it held ~7,000 L of fluid cells;
   with smooth correction the figure is ~5,300 L and with none ~4,200–4,600 L (rest ≈ 4,100 L plus surface cells). Earlier
   floating heights (z ≈ 100 cm) reflected that raised pool. With smooth correction the box floats at z ≈ 79 cm with
   **submerged fraction 0.531 vs 0.547 expected (−3%)**.
6. **Known bias.** Any density correction adds ~+10% to the measured buoyancy of a submerged body. The correction's
   source term is stronger in the denser, deeper liquid. This is independent of strength between 0.05 and 0.30.
   The trade is volume conservation against force bias; both are documented here.

## Addendum 5 — Ghost-fluid free surface and a calm-pool ground truth (2026-11-03, measured)

**Ground-truth scene (`-SloshCalm`).** A still pool 100 cm deep across the whole tank, with no paddle, emitter or
sphere; 615k particles at 64³. The surface height is computed from the particle volume at rest density, which gives
the Archimedes equilibrium height of the 40 cm, 35 kg box (density ratio 0.547). The probe sits fully submerged
(spanning 35–75 cm under a 110 cm surface).

> **Correction to Addenda 2–4:** after the smooth density correction removed the old volume inflation, the
> agitated demo pool's surface (~53–60 cm) no longer covered the probe (25–65 cm). Probe "Fz/ρgV" values from
> those runs measured a partly emerged probe in a choppy pool and must not be read as hydrostatic accuracy.
> The calm-pool numbers below replace them.

**Changes in this round.**
1. **Ghost-fluid free surface** (Gibou et al. 2002; Bridson 2015 §5.4). Previously the solver imposed p = 0 at
   the centre of each air cell; now it imposes it at the reconstructed interface. The interface is the 0.5 iso-level
   of the smooth P2G-weight density, so θ = (ρ_f − 0.5)/(ρ_f − ρ_a), clamped to [0.05, 1]. Air neighbours add 1/θ to the
   diagonal (`CellDiag`, used by RBGS and by MGPCG's level-0 operator), and the projection uses the matching ghost
   pressure. `bGhostFluid` defaults to on; `-SloshNoGhostFluid` turns it off.
2. **Coupling pressure evaluated on the body surface.** Fluid cells shift their pressure to the closest surface point
   using the local pressure gradient; body surface cells extrapolate from their fluid neighbours. Previously the
   pressure was read at cell centres half a cell outside the body, which inflated the vertical pressure difference by
   (H + h)/H, i.e. +12.5% for a 40 cm box at 5 cm cells.
3. **Density-correction dead-band.** Correction now applies only above 1.05× rest density. A one-sided correction
   with no dead-band slowly inflates the liquid, because under-dense pockets are never pulled back.

**Calm-pool results** (t ≥ 14 s):

| Configuration | Box height error | Box SD | Probe Fz/ρgV | Probe jitter (lateral) | Volume drift |
|---|---|---|---|---|---|
| Classic free surface, old estimator | +11.7 cm | 2.6 cm | 1.095 | 14 N | — |
| Ghost fluid, old estimator | +8.9 cm | 1.5 cm | 1.096 | 15 N | — |
| Ghost fluid, MGPCG, old estimator | +6.2 cm | 0.8 cm | 1.102 | 48 N | — |
| Ghost + surface pressure, dead-band 0 | +7.8 cm | 1.1 cm | 0.985 | 11 N | +3.8% / 12 s (inflating) |
| **Ghost + surface pressure + dead-band 0.05 (default)** | **+4.4 cm** | **0.9 cm** | **0.984** | **7 N** | **+0.4%** |
| Ghost + surface pressure, no density correction | **−1.2 cm** | 0.5 cm | 0.985 | — | 0.0% |

**Agitated pool (default settings):** volume drift −1.9% per 12 s, unchanged by the dead-band. Floor-start lift
still works. Probe jitter is ~10% lower with ghost fluid on. The ProjectCS, CouplingForce and Divergence costs are unchanged
(hundredths of a millisecond).

**Interpretation.**
- With density correction off, solver plus estimator float the box within −1.2 cm (≈ ¼ cell) of Archimedes, and
  the submerged probe force is within −1.5%. The hydrostatic coupling path is accurate.
- The default's +4.4 cm comes from the density correction's residual ~5% inflation. That is the price of keeping
  agitated-pool volume drift at −1.9% instead of −10% per 12 s. A two-sided (implicit) density projection is the principled
  fix (Kugelstadt et al. 2019) [Design].
- Ghost fluid is a clear improvement: box error 11.7 → 8.9 cm and bobbing SD 2.6 → 1.5 cm on otherwise identical settings.

## Addendum 6 — Two-sided density projection (2026-11-03, measured)

**Method** (after Kugelstadt et al. 2019, "Implicit density projection for volume conserving liquids").
Once per frame, on the last substep:
1. Compute the right-hand side b = s·clamp(ρ − 1, ±0.5) per fluid cell, with s = `DensityRelaxation` (0.5).
   - ρ is the smooth P2G-weight density, skipping faces shared with solids, because their weight only comes
     from one side.
   - Cells touching air may only expand. A partly filled surface cell reads low without being genuinely under-dense.
2. Solve A q = b with the pressure operator, including the ghost-fluid diagonal, by reusing the RBGS or MGPCG code
   with `Pressure := q` and `Divergence := rhs`. RBGS uses 30 sweeps; MGPCG uses 2 PCG iterations.
3. Shift particle **positions** by the trilinearly interpolated face displacement −(q_b − q_a)·h, clamped to
   0.5 cells. Velocities are untouched.

This replaces the one-sided velocity correction when `bTwoSidedDensityProjection` is set (the default). The old path
stays available via `-SloshVelocityDensityCorrection`.

**Results** (64³):

| Test | One-sided velocity correction (Addendum 5) | Two-sided position projection |
|---|---|---|
| Calm: box height vs Archimedes | +4.4 cm | **−0.0 cm** (SD 0.9 cm); −0.1 cm at s = 0.25 |
| Calm: probe Fz/ρgV | 0.984 | 0.985 |
| Calm: volume drift (12→24 s) | +0.4% (pool ~5% inflated) | −0.1% |
| Agitated: liquid volume (fluid cells; rest = 4,096 L for 262k particles) | ~5,200 L | 4,100–4,170 L |
| Agitated: volume drift (12→24 s) | −1.9% | +1.8% |
| Agitated: floating box force jitter, lateral / vertical | 145 / 230 N | **103 / 108 N** |
| Floor-start lift | lifts | lifts (floats at z ≈ 56 cm in the shallower, rest-volume pool) |
| Cost per frame | — | +0.37–0.41 ms (RBGS 30), 0.85 ms at 128³ (MGPCG 2) |

**Interpretation.**
- The remaining float error of Addendum 5 was entirely density-correction inflation. With a two-sided, position-only
  correction, the solver, the ghost-fluid surface and the surface-pressure estimator float the box at its Archimedes
  height to within measurement resolution.
- The liquid now holds its rest volume in both calm and agitated pools. Volume drift is bounded at about ±2% per 12 s
  in the agitated pool. Longer-horizon drift (minutes) has not been measured.
- The agitated probe (0.785) is not comparable: the pool is at its true rest level, so the probe protrudes further.
  Use the calm pool for hydrostatic accuracy.

## Addendum 7 — Multi-minute volume drift (2026-11-03, measured)

**Metric.** A new GPU statistic reports the mean relative density of interior liquid: fluid cells not touching air,
using the solid-aware P2G-weight density, read back once per frame (`GetMeanInteriorDensity`). With a constant
particle count, the particle-implied volume is particles / 8 / ρ_mean · h³ (`GetParticleVolumeLiters`). Unlike the
fluid-cell count, it does not include splash cells. Analysis script: least-squares drift over t ≥ 20 s.
Logs: `Saved/Bench/long_calm.log`, `long_agitated.log`. Default settings: two-sided density projection, ghost fluid.

| 300 s run | Mean interior density | Particle-implied volume | Drift | Fluid-cell volume trend | Other |
|---|---|---|---|---|---|
| Calm pool, 615k particles | 0.9999–1.0000 | 9,610.4–9,610.9 L | **+0.000 %/min** | −0.006 %/min | box error −0.2 cm, trend +0.007 cm/min; probe Fz/ρgV 0.983 → 0.985 |
| Agitated pool, 262k particles (paddle, jet, splashes) | 0.9975–1.0001 (mean 0.9987) | 4,095–4,106 L | **−0.007 %/min** | −0.028 %/min (instantaneous ±2% splash noise) | — |

**Interpretation.**
- Volume is conserved to within 0.03% over 5 minutes, even in the agitated pool. The agitated liquid sits about 0.1%
  below rest density on average, which is ordinary FLIP particle-distribution noise.
- The "±2% per 12 s" figures in Addenda 4–6 came from the fluid-cell count, which fluctuates with splashes. They were
  noise, not drift.
- Not measured: hour-scale runs, and scenes with continuous emission plus drains.
