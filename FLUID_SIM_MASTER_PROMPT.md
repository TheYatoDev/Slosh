# UE5.8 INTERACTIVE FLUID SIMULATION
## PHASE 0: MASTER RESEARCH & ARCHITECTURE MANDATE

**Project:** Unreal Engine 5.8 Interactive Fluid Simulation Plugin  
**Primary Research Target:** Blender FLIP Fluids  
**Development Stage:** Phase 0, Research and Feasibility  
**Primary Language:** C++ / HLSL  
**Target Engine:** Unreal Engine 5.8 (source build)  
**Primary Platform:** Windows PC  
**Primary GPU Architecture:** DirectX 12 / Shader Model 6  
**Agent Role:** Principal Graphics Engineer, Physics Simulation Researcher, Unreal Engine Systems Architect

---

# 1. MISSION

Research, evaluate, and architect a production-oriented, GPU-accelerated liquid simulation system for Unreal Engine 5.8.

The system must support physically interactive liquids suitable for real-time gameplay, including water, blood, slime, and other liquids with configurable physical properties.

The long-term objective is a standalone Unreal Engine plugin providing:

- Real-time liquid simulation.
- GPU-accelerated fluid dynamics.
- Bidirectional interaction between fluids and rigid bodies.
- Static, dynamic, and moving collision geometry.
- Fluid displacement, buoyancy, drag, and momentum transfer.
- Configurable viscosity, density, surface tension, and adhesion.
- Liquid surface reconstruction and high-quality rendering.
- Blueprint-accessible gameplay functionality.
- Modular integration with Unreal's physics and rendering systems.
- Extensible liquid material definitions.
- Performance scalability across supported GPU hardware.

The system must not depend on Niagara Fluids for its core simulation.

Niagara may be investigated as an optional secondary effects system, but it must not become a mandatory simulation dependency.

**This assignment is exclusively for research, source analysis, feasibility assessment, architecture design, and experimental planning.**

Do not begin implementing the production plugin.

---

# 2. PRIMARY RESEARCH PRIORITY: BLENDER FLIP FLUIDS

The first and most important research task is to investigate Blender FLIP Fluids as a potential foundation for the Unreal simulation system.

Primary repository:

https://github.com/rlguy/Blender-FLIP-Fluids

Official website:

https://flipfluids.com/

Investigate the actual repository and its underlying simulation engine.

Do not rely exclusively on documentation, summaries, or assumptions about the implementation.

## 2.1 Repository and Source Architecture

Inspect the complete relevant source tree.

Identify:

1. The standalone C++ simulation engine.
2. Blender-specific integration code.
3. Python bindings and interfaces.
4. Fluid particle representation.
5. Grid data structures.
6. Particle-to-grid transfer.
7. Grid-to-particle transfer.
8. Pressure projection and incompressibility enforcement.
9. Viscosity implementation.
10. Surface reconstruction and meshing.
11. Collision geometry and obstacle representation.
12. Moving obstacle support.
13. Fluid-solid interaction mechanisms.
14. Simulation stepping and substepping.
15. Multithreading and parallelization.
16. External libraries and dependencies.
17. Serialization, caching, and output systems.

Produce an architectural map identifying the important classes, functions, modules, dependencies, and data flows.

Reference exact source file paths and symbols wherever possible.

Distinguish verified implementation details from inferred behavior.

## 2.2 Standalone Engine Feasibility

Determine whether the FLIP Fluids simulation engine can be compiled and executed independently of Blender.

Investigate:

- Build system requirements.
- Platform dependencies.
- Compiler requirements.
- Runtime dependencies.
- CPU threading architecture.
- Simulation initialization.
- Scene and collider construction.
- Particle emission.
- Simulation stepping.
- Access to particle and grid data.
- Output and surface generation.
- Memory ownership.
- External API accessibility.

Determine the minimum components required to operate the simulation engine without Blender.

Evaluate three integration strategies:

**Strategy A: Direct CPU Integration**

Embed or link the existing standalone simulation engine into an Unreal plugin.

**Strategy B: Hybrid CPU/GPU Integration**

Reuse selected CPU systems while migrating computationally expensive workloads to GPU compute.

**Strategy C: Native GPU Reimplementation**

Use the existing engine as a validated algorithmic reference while implementing a GPU-native solver using Unreal Engine 5.8's RDG and RHI.

Compare the engineering complexity, expected performance, maintainability, and technical risks of all three approaches.

Do not assume that a GPU port will automatically outperform the CPU implementation.

## 2.3 Licensing and Code Reuse

Inspect actual license files, source headers, dependency licenses, and relevant distribution requirements.

Determine:

- Licensing of the Blender addon.
- Licensing of the standalone C++ simulation engine.
- Licensing of third-party dependencies.
- Whether the engine can be reused in a separately distributed Unreal plugin.
- Whether modifying and redistributing the engine introduces obligations.
- Whether linking or embedding specific components changes licensing requirements.
- Whether the intended architecture can avoid Blender-specific dependencies.

Create a source-level licensing inventory.

Do not assume that the license applying to the simulation engine also applies to every repository component.

Flag unresolved legal questions for independent review.

---

# 3. FLIP/APIC ALGORITHM INVESTIGATION

After mapping the FLIP Fluids implementation, investigate the mathematical and computational architecture of its solver.

Analyze:

- Particle-grid hybrid simulation.
- FLIP and PIC velocity transfers.
- APIC or other affine transfer techniques, where relevant.
- Pressure projection.
- Divergence computation.
- Incompressibility enforcement.
- Advection.
- Boundary conditions.
- Particle reseeding and maintenance.
- Numerical dissipation.
- Volume preservation.
- Surface tracking.
- Stability under high velocities.
- Simulation timestep restrictions.

Determine which algorithms are implemented by FLIP Fluids and which would require additional development.

Do not assume the existing implementation uses APIC without source evidence.

Evaluate whether APIC could improve the proposed system and under what circumstances.

Document equations, relevant papers, implementation references, and computational costs.

---

# 4. UNREAL ENGINE 5.8 SOURCE INVESTIGATION

Inspect the installed Unreal Engine 5.8 source tree.

The installed engine source is authoritative for engine-specific implementation details.

Do not assume that APIs, rendering hooks, or physics interfaces from UE5.2 or earlier remain compatible.

## 4.1 Rendering Infrastructure

Investigate:

- Render Dependency Graph (RDG).
- Render Hardware Interface (RHI).
- Global shaders.
- HLSL compute shaders.
- Persistent GPU buffers.
- Structured buffers and UAVs.
- GPU resource lifetime.
- Compute pass scheduling.
- GPU synchronization.
- Asynchronous compute.
- GPU profiling.
- Render-thread and game-thread communication.
- Shader compilation and permutation management.

Determine how a persistent GPU fluid simulation should integrate with Unreal's rendering infrastructure.

Identify exact engine classes, functions, source locations, and extension points.

Evaluate cross-platform implications even though Windows/DX12 is the initial target.

Avoid depending unnecessarily on private engine implementation details.

If private engine modifications are required, document them explicitly.

## 4.2 Physics Integration

Investigate Chaos integration for:

- Static rigid bodies.
- Dynamic rigid bodies.
- Kinematic bodies.
- Collision shapes.
- Physics substeps.
- Force and impulse application.
- Physics-thread synchronization.
- Simulation callbacks.
- Collision queries.
- Moving geometry.
- Deforming geometry.

Identify supported public integration points and any engine-level limitations.

Investigate how GPU-computed fluid forces can influence CPU rigid-body simulation without introducing unacceptable synchronization stalls.

## 4.3 Optional Jolt Integration

Design a physics abstraction that could support Jolt alongside Chaos.

The plugin must not depend directly on a specific third-party Jolt plugin.

Investigate how the adapter could support:

- Rigid-body transforms.
- Collision shapes.
- Force application.
- Impulse transfer.
- Simulation synchronization.
- Contact and collision data.

Do not attempt to integrate Jolt during Phase 0.

---

# 5. BIDIRECTIONAL FLUID-RIGID BODY COUPLING

This is a critical research area.

The target system must support fluid-to-object and object-to-fluid interactions.

Investigate the following:

## 5.1 Object-to-Fluid Interaction

- Fluid displacement.
- Moving obstacles.
- Rotating containers.
- Kinematic collision boundaries.
- High-speed object penetration.
- Collision response.
- Fluid splashing.
- Fluid movement caused by rigid bodies.

## 5.2 Fluid-to-Object Interaction

- Buoyancy.
- Hydrodynamic drag.
- Pressure forces.
- Momentum transfer.
- Floating and sinking.
- Object rotation caused by fluid forces.
- Fluid forces acting on constrained rigid bodies.

## 5.3 Coupling Strategies

Compare:

1. One-way rigid-body-to-fluid coupling.
2. Approximate two-way coupling.
3. Synchronized two-way coupling.
4. Asynchronous coupling with delayed force feedback.
5. CPU gameplay proxies combined with GPU fluid simulation.

Evaluate each for:

- Physical fidelity.
- Numerical stability.
- Performance.
- GPU/CPU synchronization.
- Latency.
- Momentum conservation.
- Gameplay responsiveness.
- Implementation complexity.

Identify the most practical initial approach and the conditions under which a more accurate coupling system would be justified.

Do not claim full bidirectional physical coupling unless both directions are supported by the proposed design.

---

# 6. SECONDARY SOLVER RESEARCH

FLIP Fluids is the highest-priority research target, but it is not automatically the selected solution.

After investigating FLIP Fluids, compare it with alternative simulation techniques.

## 6.1 Required Candidates

**DFSPH**

Investigate divergence-free smoothed particle hydrodynamics and its suitability for GPU-based liquid simulation.

Reference:

https://github.com/InteractiveComputerGraphics/SPlisHSPlasH

**PBF / XPBD**

Investigate position-based fluids and extended position-based dynamics.

Reference:

https://github.com/InteractiveComputerGraphics/PositionBasedDynamics

**NVIDIA Flex**

Investigate its unified particle architecture, fluid-rigid-body interaction approach, GPU implementation, and licensing constraints.

**Other Relevant Techniques**

Consider additional approaches only when they offer a concrete advantage for the target system.

Examples include:

- Weakly compressible SPH.
- MLS-MPM.
- Hybrid grid-particle methods.
- Specialized viscoelastic solvers.

## 6.2 Comparison Requirements

Compare candidates on:

- Real-time GPU suitability.
- Incompressibility.
- Volume preservation.
- Dynamic rigid-body interaction.
- Moving boundaries.
- Viscosity.
- Surface tension.
- Cohesion and adhesion.
- Viscoelastic material support.
- Numerical stability.
- Computational complexity.
- Memory requirements.
- Scalability.
- Rendering compatibility.
- Implementation difficulty.
- Maintenance requirements.
- Licensing.

Provide a weighted comparison matrix and explain the weights.

Recommend a primary candidate and a fallback based on evidence.

---

# 7. LIQUID MATERIAL SYSTEM

Design a shared simulation framework capable of representing different liquids through configurable physical models.

Initial target liquids:

**Water**

- Low viscosity.
- Free-flowing behavior.
- Splashing.
- Surface tension.
- Buoyancy.
- Dynamic displacement.

**Blood**

- Configurable viscosity.
- Surface tension.
- Adhesion and wetting.
- Flow and pooling.
- Optional clotting or drying as future extensions.

**Slime**

- High viscosity.
- Cohesion.
- Adhesion.
- Potential non-Newtonian behavior.
- Potential viscoelasticity.
- Stretching and string formation where physically appropriate.

Investigate whether these behaviors can be supported through one solver architecture.

Identify which require additional constitutive models, constraints, or separate simulation techniques.

Do not assume that increasing viscosity alone accurately reproduces slime.

Design a potential Unreal Data Asset-based fluid material interface.

Separate physical properties from visual material properties.

Do not implement the material system during Phase 0.

---

# 8. LIQUID RENDERING RESEARCH

Research rendering techniques capable of producing visually convincing liquid surfaces in Unreal Engine 5.8.

Investigate:

- Screen-space fluid rendering.
- Particle depth and thickness reconstruction.
- Surface smoothing.
- Implicit surface representations.
- GPU marching cubes.
- Sparse surface extraction.
- Surface normals.
- Refraction.
- Reflection.
- Transparency.
- Absorption.
- Foam.
- Bubbles.
- Splash particles.
- Fluid motion vectors.
- Shadows.
- Underwater rendering.
- Multiple camera viewpoints.

Evaluate integration with Unreal's rendering pipeline.

Determine whether screen-space reconstruction is sufficient for the initial prototype or whether world-space surface reconstruction should be prioritized.

Simulation and rendering must remain independent modules.

The renderer must not determine the physical simulation state.

---

# 9. TECHNICAL EXPERIMENT PLAN

Design reproducible experiments that validate the proposed architecture.

Do not implement these experiments during Phase 0.

## Experiment 1: Standalone FLIP Fluids Baseline

Goal: Verify that the underlying simulation engine can operate independently of Blender.

Investigate the minimum build, initialization, simulation, and output requirements.

Define the steps needed to execute a simple liquid simulation outside Blender.

Specify expected outputs and failure conditions.

## Experiment 2: Dam Break

Goal: Validate basic fluid motion, incompressibility, and stability.

Measure:

- Particle count.
- Density or divergence error.
- Volume preservation.
- Simulation timestep.
- CPU/GPU execution time.
- Memory usage.
- Stability.

## Experiment 3: Floating Rigid Body

Goal: Validate bidirectional fluid-rigid-body interaction.

Use objects with different densities and masses.

Measure:

- Displacement.
- Buoyancy.
- Force transfer.
- Object motion.
- Numerical stability.
- Momentum conservation where applicable.

## Experiment 4: Moving Container

Goal: Validate moving boundaries.

Test translation, rotation, acceleration, and spilling.

Measure:

- Boundary penetration.
- Fluid stability.
- Collision accuracy.
- Energy behavior.
- Performance.

## Experiment 5: Variable Liquid Properties

Goal: Validate the material model.

Compare water-like, blood-like, and slime-like configurations.

Measure:

- Flow behavior.
- Viscosity response.
- Cohesion.
- Adhesion.
- Numerical stability.
- Computational overhead.

## Experiment 6: GPU Scaling

Goal: Determine the practical simulation capacity of the proposed architecture.

Evaluate particle counts including:

- 10,000
- 25,000
- 50,000
- 100,000
- 200,000

Use identical scenes and documented hardware configurations.

Profile simulation, neighbor search or grid transfers, pressure solving, collision, coupling, surface reconstruction, and synchronization independently.

Define pass/fail criteria before implementation.

---

# 10. PERFORMANCE REQUIREMENTS

Initial target: 60 FPS on Windows PC.

Initial development hardware:

- NVIDIA Quadro RTX 4000.
- Intel Core i9-10900.
- Approximately 64 GB system RAM.

Investigate a provisional 4 ms total fluid subsystem budget at 60 FPS, including simulation, coupling, and rendering.

This is an aspirational engineering budget, not a guaranteed performance target.

Determine whether the target is realistic for the candidate algorithms and particle counts.

Analyze:

- GPU memory bandwidth.
- Compute occupancy.
- Particle-grid transfer costs.
- Pressure solver iteration counts.
- GPU memory allocations.
- Readback latency.
- Synchronization overhead.
- CPU bottlenecks.
- GPU rendering overhead.
- Frame pacing.
- Scalability settings.

Do not fabricate benchmark results.

If a performance estimate cannot be substantiated, mark it as an estimate and explain the assumptions.

---

# 11. GAMEPLAY INTEGRATION ARCHITECTURE

Design a Blueprint-accessible interface for:

- Fluid emitters.
- Fluid containers.
- Fluid simulation volumes.
- Fluid material assets.
- Collision components.
- Fluid forces.
- Buoyancy.
- Fluid detection.
- Fluid level queries.
- Fluid entry and exit events.
- Fluid gameplay interactions.

Investigate the feasibility of gameplay queries such as:

- Is an object submerged?
- How much of an object is submerged?
- What liquid is touching this actor?
- What is the local fluid velocity?
- How much fluid is present in a region?
- What force is the liquid applying to an object?

Determine which queries can be answered synchronously, which require GPU readback, and which need approximate CPU-side representations.

Define latency and accuracy expectations.

Avoid APIs that imply immediate authoritative answers when only delayed GPU data is available.

Consider world partitioning, level streaming, simulation ownership, and lifecycle management.

---

# 12. NETWORKING AND DETERMINISM

Investigate networking implications without making deterministic GPU fluid simulation a mandatory initial requirement.

Compare:

- Local visual simulation.
- Server-authoritative gameplay proxies.
- Replicated fluid events.
- Replicated fluid state.
- Deterministic CPU simulation.
- Full GPU state synchronization.

Identify which approaches are feasible for multiplayer gameplay.

Explicitly document why GPU floating-point simulation may not reproduce bit-identical results across different hardware.

Keep fluid simulation independent of rollback gameplay state unless a validated deterministic strategy exists.

---

# 13. RISK ASSESSMENT

Create a technical risk register covering at least:

- FLIP Fluids standalone engine limitations.
- CPU solver performance.
- GPU port complexity.
- Sparse grid implementation.
- GPU pressure solver convergence.
- Rigid-body coupling instability.
- GPU/CPU synchronization.
- Dynamic collision geometry.
- Fluid volume loss.
- Numerical instability.
- Renderer integration.
- Transparency and refraction limitations.
- Unreal engine version changes.
- Third-party licensing.
- Memory consumption.
- Performance scaling.
- Platform compatibility.
- Multiplayer synchronization.

For each risk, document severity, likelihood, mitigation, and an experiment capable of validating the concern.

---

# 14. REQUIRED DELIVERABLES

Create a `Research/` directory containing:

1. `FLIP_FLUIDS_SOURCE_AUDIT.md`  
   Source architecture, dependencies, relevant algorithms, and verified implementation details.

2. `FLIP_FLUIDS_FEASIBILITY.md`  
   Standalone engine feasibility, reuse strategies, and Unreal integration options.

3. `LICENSING_AUDIT.md`  
   License boundaries, dependency obligations, and unresolved questions.

4. `SOLVER_COMPARISON.md`  
   FLIP/APIC versus DFSPH, PBF/XPBD, and other relevant candidates.

5. `UE58_INTEGRATION.md`  
   Verified rendering, physics, threading, and plugin integration points.

6. `GPU_ARCHITECTURE.md`  
   Proposed compute architecture, resource management, and execution pipeline.

7. `PHYSICS_COUPLING.md`  
   Bidirectional fluid-rigid-body interaction strategies.

8. `LIQUID_MATERIALS.md`  
   Water, blood, and slime physical model requirements.

9. `RENDERING_ARCHITECTURE.md`  
   Surface reconstruction and Unreal rendering integration.

10. `GAMEPLAY_API.md`  
    Proposed C++ and Blueprint interfaces.

11. `EXPERIMENT_PLAN.md`  
    Reproducible experiments, measurements, and acceptance criteria.

12. `PERFORMANCE_PLAN.md`  
    Benchmark methodology, provisional budgets, and scalability strategy.

13. `RISK_REGISTER.md`  
    Technical risks, mitigation plans, and validation requirements.

14. `ARCHITECTURE_DECISION.md`  
    Recommended architecture, alternatives, rationale, and unresolved decisions.

15. `PHASE1_IMPLEMENTATION_PLAN.md`  
    A staged implementation roadmap for the first approved technical prototype.

16. `RESEARCH_SUMMARY.md`  
    Executive summary of findings and a clear recommendation on whether FLIP Fluids should be used as the primary foundation.

---

# 15. RESEARCH QUALITY REQUIREMENTS

All findings must be evidence-based.

For source-derived claims:

- Reference repository URLs.
- Record the inspected commit hash or version.
- Cite exact source files and relevant symbols.
- Distinguish direct source observations from interpretation.
- Identify outdated or potentially incompatible documentation.
- Record unresolved questions.
- State when claims require experimental validation.

For Unreal Engine findings, use the installed UE5.8 source tree as the primary authority.

For academic algorithms, reference original papers or reliable technical implementations.

For performance claims, distinguish measured results, published results, estimates, and untested hypotheses.

Do not invent measurements, APIs, compatibility claims, or implementation details.

If source access is unavailable, report the limitation instead of substituting unsupported assumptions.

---

# 16. EXECUTION STRATEGY

Use a research-first, multi-stage workflow.

**Stage A: FLIP Fluids Source Audit**

Inspect the FLIP Fluids repository, identify the standalone engine, map its architecture, and verify licensing boundaries.

**Stage B: Standalone and GPU Feasibility**

Evaluate CPU reuse, hybrid integration, and GPU reimplementation.

Identify the major technical barriers and possible solutions.

**Stage C: Unreal Engine 5.8 Investigation**

Inspect RDG, RHI, shader integration, Chaos, threading, and relevant extension points.

**Stage D: Solver and Coupling Comparison**

Compare alternative algorithms and evaluate fluid-rigid-body coupling strategies.

**Stage E: Architecture Design**

Produce the proposed plugin architecture, module boundaries, data flows, rendering pipeline, and gameplay API.

**Stage F: Experimental and Implementation Planning**

Define the smallest experiments capable of validating the architecture.

Produce the Phase 1 roadmap and final recommendations.

Use parallel research agents where beneficial, but require findings to be consolidated and contradictions resolved.

Prioritize evidence quality over research volume.

---

# 17. FINAL DECISION REQUIREMENTS

At the end of Phase 0, provide explicit answers to:

1. Is Blender FLIP Fluids a viable technical foundation for this Unreal plugin?
2. Should its C++ engine be integrated directly, partially reused, or treated as an algorithmic reference?
3. Is a GPU-native FLIP/APIC implementation practical for the target hardware?
4. Is FLIP/APIC preferable to DFSPH or PBF for the intended gameplay?
5. What coupling strategy should be used for Chaos rigid bodies?
6. What rendering architecture best supports the required liquid quality?
7. Can water, blood, and slime share the same simulation foundation?
8. What is the highest-risk technical assumption?
9. What is the smallest experiment that could invalidate the recommended architecture?
10. What should be implemented first in Phase 1?

Conclude with one of the following:

- **GO:** Sufficient evidence supports proceeding with the proposed architecture.
- **CONDITIONAL GO:** Proceed only after specified feasibility experiments.
- **NO-GO:** The proposed approach is unsuitable; recommend an alternative.

A GO recommendation must not be based solely on theoretical feasibility.

---

# 18. STOP CONDITIONS

Do not implement the production Unreal plugin.

Do not begin a full GPU solver port.

Do not modify the Unreal Engine source.

Do not claim successful builds or benchmarks without executing them.

Do not silently replace the requested architecture with Niagara Fluids or another existing Unreal fluid system.

Do not assume Blender FLIP Fluids is the final solution before completing the comparison.

Do not proceed to Phase 1 without explicit approval.

**The final output of this assignment is a technically defensible architecture and implementation plan, not working fluid simulation code.**

Begin with the FLIP Fluids repository source audit. Treat it as Priority #1 and establish its feasibility before investing substantial research effort in alternative solver architectures.