# LIQUID_MATERIALS.md — Water, Blood and Slime on One Solver

**Status:** Phase 0 design + what the Phase 1 prototype actually implements.
**Evidence legend:** **[Impl]** implemented in `SloshDemo/Plugins/Slosh` (compiled, see `PHASE1_IMPLEMENTATION_PLAN.md` §Status for runtime validation), **[Design]** proposed, not implemented, **[Lit]** from literature, **[Est]** estimate.

## 1. Can one solver foundation cover all three?

Short answer: **one grid/particle foundation, three constitutive layers.** The FLIP/APIC MAC-grid pipeline
(P2G → forces → viscosity → pressure → G2P) is shared. Water and blood differ only in parameters.
Slime needs at least one extra constitutive term (shear-thinning viscosity and ideally an elastic
stress) that "more viscosity" cannot reproduce (see §4).

| Behaviour | Water | Blood | Slime | Mechanism in shared solver |
|---|---|---|---|---|
| Incompressible bulk flow | ✔ | ✔ | ✔ | Pressure projection **[Impl]** |
| Viscosity | ~1e-6 m²/s (negligible at grid scale) | ~3–4e-6 m²/s Newtonian approx. (real blood is shear-thinning) | 1e-2 … 5e-1 m²/s effective | Implicit viscosity, Jacobi iterations **[Impl]** |
| Splashing | high | medium | low | FLIP ratio / APIC **[Impl]** |
| Surface tension | visible at small scales | important for droplets/pooling | important for strands | Ghost-fluid pressure jump σκ (as FLIP Fluids does, `pressuresolver.cpp:715`) **[Design]** |
| Adhesion / wetting | low | high (stains, film on surfaces) | very high (sticks to walls) | Boundary friction (tangential velocity removal) **[Impl]**, adhesion pressure term **[Design]** |
| Cohesion | low | low–medium | very high | Surface tension + elastic stress **[Design]** |
| Shear thinning | no | mild | strong (Bingham / Herschel-Bulkley) | Strain-rate dependent viscosity per face **[Design]** |
| Viscoelastic stretching / strings | no | no | yes | Per-particle elastic stress (Oldroyd-B / upper-convected Maxwell) on the APIC C matrix **[Design]** |
| Drying / clotting | — | future | future | Per-particle age attribute exists (`FSloshParticle::Age`) **[Impl]**; viscosity ramp by age **[Design]** |

## 2. Parameter model (implemented)

`USloshMaterialAsset` (`Source/Slosh/Public/SloshMaterialAsset.h`) is a `UDataAsset` with two
deliberately separate structs:

- `FSloshPhysicalProperties` — consumed only by the solver (`FSloshSolverSettings`):
  `Density`, `KinematicViscosity`, `Transfer` (APIC/FLIP), `FlipRatio`, `BoundaryFriction`, `DensityCorrection`.
- `FSloshVisualProperties` — consumed only by `SloshRender`: `AbsorptionPerMeter`, `ScatterColor`,
  `RefractionStrength`, `FresnelF0`, `SpecularPower`, `ParticleRadiusScale`, `SmoothingRadius`.

The rendering module never sees physical properties and the solver never sees visual ones
(module boundary: `SloshCore` has no dependency on `SloshRender`).

### Suggested presets (all **[Est]**, to be tuned in Experiment 5)

| Preset | Density | ν (m²/s) | Transfer | Friction | Absorption /m (RGB) | Scatter |
|---|---|---|---|---|---|---|
| Water | 1000 | 0 (numerical viscosity dominates) | FLIP 0.95 or APIC | 0.0 | (4.0, 1.2, 0.8) | (0.02, 0.08, 0.10) |
| Blood | 1060 | 4e-6 … 5e-3 (gameplay scale) | APIC | 0.4 | (2, 60, 60) | (0.25, 0.0, 0.0) |
| Slime | 1100 | 0.05 … 0.3 | APIC | 0.8 | (12, 2, 14) | (0.1, 0.4, 0.05) |

Why the blood/slime ν values far exceed reality: at 5 cm cells, numerical diffusion of PIC/APIC already
exceeds water's physical viscosity by orders of magnitude **[Lit, Bridson 2015 ch.10]**, so viscosity only
becomes visible once ν·dt/h² ≳ 0.05. With dt = 1/240 s and h = 0.05 m that is ν ≳ 3e-2 m²/s.

## 3. Viscosity implementation and its limits [Impl]

`ViscosityJacobiCS` in `Shaders/Private/Slosh.usf` solves `(I − ν·dt·∇²) u = u*` per MAC component
with Jacobi iterations (`alpha = ν dt / h²`). Properties:
- Unconditionally stable per iteration (diagonally dominant), so high-ν slime does not explode.
- Converges slowly for large alpha: 12 iterations damp only the highest frequencies when alpha ≫ 1, i.e.
  slime is *under*-viscous at default settings. A multigrid or CG solve is the Phase 2 fix.
- Decoupled components and a 7-point Laplacian with free-slip treatment of air neighbours. FLIP Fluids
  instead uses the coupled variational solver of Batty & Bridson 2008 (`viscositysolver.cpp:27-32`),
  which correctly handles the free-surface stress-free condition and rotation. Our simplification
  damps rigid rotation of viscous blobs (they "melt" rather than tumble). This is a known artefact.

## 4. Why "just increase viscosity" is not slime

High Newtonian viscosity produces honey: slow, uniform, rate-independent flow that never stretches into
filaments and never springs back. Slime behaviour needs:
1. **Yield stress / shear thinning** (Bingham, Herschel-Bulkley): rests as a blob, flows when sheared.
   Implementation: per-face viscosity μ(γ̇) computed from the grid strain rate before the viscosity solve.
   Cost: one extra pass + variable-coefficient Laplacian. **[Design]**
2. **Elasticity / memory** (Oldroyd-B, Maxwell): stretching strings that thin and recoil.
   Implementation: store an elastic stress or left Cauchy-Green tensor per particle (6 floats), advect it
   with the APIC velocity gradient (the `Cx/Cy/Cz` rows already present per particle **[Impl]**),
   relax it toward identity with time constant λ, and add its divergence as a grid force.
   References: Goktekin, Bargteil & O'Brien 2004; Ram et al. 2015 (MPM foam); MLS-MPM (Hu et al. 2018).
   **[Design]**
3. **Cohesion/surface tension**: σκ pressure jump at the free surface (ghost fluid) **[Design]**.

The APIC choice matters here: APIC gives a per-particle velocity gradient for free, which is exactly
the quantity an elastic-stress model needs. This is one reason APIC is the default transfer.

## 5. Proposed Data Asset evolution [Design]

```
USloshMaterialAsset
  FSloshPhysicalProperties Physical
     Density, KinematicViscosity, Transfer, FlipRatio, BoundaryFriction, DensityCorrection   [Impl]
     SurfaceTension (N/m), Adhesion (contact-angle proxy)                                    [Design]
     ESloshRheology Rheology { Newtonian, Bingham, HerschelBulkley, OldroydB }              [Design]
     YieldStress, FlowIndex, ElasticModulus, RelaxationTime                                  [Design]
     DryingRate / ClotTime (age-driven viscosity ramp)                                       [Design]
  FSloshVisualProperties Visual                                                              [Impl]
     + FoamColor, WetnessDecalMaterial (soft ref)                                             [Design]
  TSoftObjectPtr<UMaterialInterface> SurfaceMaterial (for a future mesh renderer)            [Design]
```
Mixing several liquids in one domain needs a per-particle material index and per-cell averaged
properties (one byte per particle; viscosity becomes variable-coefficient). Deferred to Phase 3.

## 6. Validation

Experiment 5 in `EXPERIMENT_PLAN.md` (column collapse with three presets; measure front position vs time,
final spread radius, and pass count/cost of the viscosity solve) validates this document.
