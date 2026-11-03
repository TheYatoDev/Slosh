# FLIP Fluids Engine Source Audit (algorithmic reference for UE5.8 GPU FLIP/APIC plugin)

- Repo: `C:\Users\Dae\Documents\SourceStuff\Blender-FLIP-Fluids` (origin `rlguy/Blender-FLIP-Fluids`)
- Commit: `70a0e954018fe39e1f9c3631264989569752bb7a` (verified with `git rev-parse HEAD`)
- Engine version: 1.8.8 (`CMakeLists.txt:54-56`)
- All paths below are relative to `src/engine/` unless prefixed with `src/addon/` or repo root.
- Tags: **[O]** = Observed (read in source at the cited line). **[I]** = Inferred (reasoned from code, naming or structure, not stated literally).

---

## 0. One-screen summary

| Topic | Answer | Where |
|---|---|---|
| Grid | Uniform dense MAC grid, single `dx`, `Array3d<float>` per face component, x-fastest flat layout | `macvelocityfield.h`, `array3d.h:780` [O] |
| Particles | SoA `ParticleSystem` with named attribute vectors (`POSITION`, `VELOCITY`, optional `AFFINEX/Y/Z`, ...) | `particlesystem.h:305-325`, `fluidsimulation.cpp:4264` [O] |
| Seeding | 8 particles per cell (2x2x2 at +/-0.25dx), jitter +/-0.25dx * jitterFactor (addon default 1.0) | `fluidsimulation.cpp:8673-8713`, `:4374` [O] |
| P2G (FLIP mode) | Per-face-component scatter with a radial Wyvill-type polynomial kernel, radius `0.5*sqrt(3)*dx` (about 0.866dx), normalized by weight sum | `velocityadvector.cpp:467-536` [O] |
| P2G (APIC mode) | Trilinear (tent) 8-node scatter of `v + c.(x_i - x_p)`, normalized | `velocityadvector.cpp:543-623` [O] |
| G2P | FLIP: `v = a*vPIC + (1-a)*(v + vPIC - vSaved)`, `a = PICFLIP ratio = 0.05`. APIC: `v = vPIC`, `c = sum(grad w_i * u_i)` (pure APIC, no blend) | `fluidsimulation.cpp:7216-7288` [O] |
| Transfer enum | `VelocityTransferMethod { FLIP=0, APIC=1 }`, default FLIP | `fluidsimulation.h:1564-1567`, `:2401` [O] |
| Pressure | Batty-Bertails-Bridson variational solid weights (face area fractions from solid SDF) + ghost-fluid free surface (theta = phi_air/phi_liq clamped to 25), 7-point SPD matrix, PCG + MIC(0) (param 0.97), tol 1e-9 relative (inf-norm), cap 900 it | `pressuresolver.cpp:751-867`, `pcgsolver/pcgsolver.h`, `fluidsimulation.h:2436-2438` [O] |
| Viscosity | Batty-Bridson 2008 variational implicit viscosity, coupled u/v/w system (15 nnz/row), PCG+MIC(0) float, tol 1e-4, cap 900 | `viscositysolver.cpp:27-32,50-80,832` [O] |
| Surface tension | Curvature from liquid SDF (upwind reinit + curvature formula), injected as ghost-fluid pressure jump `sigma*kappa` in RHS and pressure gradient; adds timestep limit | `particlelevelset.cpp:196-291`, `pressuresolver.cpp:660-748,1072-1090`, `fluidsimulation.cpp:11005-11008` [O] |
| Obstacles | Triangle meshes -> node-sampled `MeshLevelSet` (exact band 3 cells), per-face solid velocity from closest-triangle barycentric vertex velocity; enters pressure RHS and velocity constraint (with friction blend) | `meshlevelset.cpp:542-640,1320-1374`, `fluidsimulation.cpp:6786-6837` [O] |
| Two-way coupling | **None.** No fluid->solid force, torque, buoyancy for rigid bodies or pressure readback anywhere in engine or addon. One-way kinematic obstacles only | grep results, section 9 [O] |
| Advection | Particles: RK3 (Ralston-type: k at 0, 0.5dt, 0.75dt; weights 2/9,3/9,4/9) through post-projection MAC grid; ray-march collision against solid SDF | `fluidsimulation.cpp:8186-8193,8216-8291` [O] |
| Timestep | `dt_sub = CFL*dx/max|v_p|`, CFL default 5, rounded so frame splits evenly; min/max substeps (engine 1/6, addon 1/24) | `fluidsimulation.cpp:10988-11020,11167-11240` [O] |
| Threading | Raw `std::thread` spawn/join per stage via `ThreadUtils::splitRangeIntoIntervals`; OpenMP only in the disabled Jacobi path; Eigen vendored but unused | `threadutils.cpp:54-73`, `pressuresolver.cpp:875,937` [O] |

---

## 1. Module map and data flow

### 1.1 Standalone C++ engine (no Blender dependencies) [O]
All of `src/engine/*.cpp|h` compiles into the shared library `ffengine` (`CMakeLists.txt:273-276`, `add_library(fluid_engine_objects OBJECT ...)` then `add_library(ffengine SHARED ...)`). Nothing in the engine includes Blender headers or Python. Key classes:

| File | Role |
|---|---|
| `fluidsimulation.cpp/.h` (11300 / 2537 lines) | Orchestrator `FluidSimulation`; owns all grids, particles, sub-solvers; `update(dt)` is the public per-frame entry |
| `macvelocityfield.cpp/.h` | Staggered MAC velocity `MACVelocityField` + `ValidVelocityComponentGrid` |
| `velocityadvector.cpp/.h` | P2G transfer (named "advect" historically) |
| `pressuresolver.cpp/.h` | Pressure projection (matrix assembly, solve, gradient apply) |
| `viscositysolver.cpp/.h` | Implicit variational viscosity |
| `pcgsolver/pcgsolver.h, sparsematrix.h, blaswrapper.h` | CC0 PCG + MIC(0) solver, list-of-lists sparse matrix, threaded BLAS dot/axpy |
| `particlelevelset.cpp/.h` | Liquid SDF from particles, curvature |
| `meshlevelset.cpp/.h` | Solid SDF from triangle meshes (adapted from Batty SDFGen, header `meshlevelset.h:25-52`) |
| `meshobject.cpp/.h` | Obstacle / fluid / inflow mesh object with prev/current/next frame meshes and velocities |
| `meshfluidsource.cpp/.h` | Inflow/outflow sources |
| `particlesheeter.cpp/.h` | Thin-sheet particle seeding |
| `diffuseparticlesimulation.cpp/.h` | Spray/foam/bubble/dust |
| `particlemesher.cpp/.h`, `polygonizer3d.cpp/.h` | Output surface: union-of-spheres SDF + marching cubes |
| `forcefield*.cpp/.h` | Force field objects and `ForceFieldGrid` |
| `levelsetsolver.cpp`, `levelsetutils.cpp`, `gridutils.cpp`, `interpolation.cpp` | Upwind reinit, fraction-inside, extrapolation, trilinear/tricubic |
| `array3d.h`, `blockarray3d.h`, `subdividedarray3d.h`, `fragmentedvector.h`, `boundedbuffer.h` | Containers |
| `threadutils.cpp/.h`, `mingw32_threads/` | Thread count and range splitting; mingw32 thread shim |
| `mixbox/mixbox_stub.cpp` | Stub used when `WITH_MIXBOX` is off (`CMakeLists.txt:98-107`) |
| `main.cpp` | Standalone example program; not part of the library source list (no `main.cpp` reference in `CMakeLists.txt`) [O] |

### 1.2 C bindings (`src/engine/c_bindings/`) [O]
- `extern "C"` exported functions with `EXPORTDLL` (`fluidsimulation_c.cpp:37-42`). 594 exports in `fluidsimulation_c.cpp`, plus `meshobject_c.cpp` (42), `meshfluidsource_c.cpp` (52), `forcefield*_c.cpp`, `mixbox_c.cpp`.
- Each wraps a method through templated try/catch helpers `CBindings::safe_execute_method_*` (`cbindings.h`), returning error codes via `int *err` and a global message buffer `CBINDINGS_ERROR_MESSAGE[4096]`.
- Example: `FluidSimulation_update(obj, dt, err)` -> `FluidSimulation::update` (`fluidsimulation_c.cpp:115-119`).
- Output buffers are pulled by size + memcpy pairs, e.g. `FluidSimulation_get_surface_data` memcpys `getSurfaceData()` into a caller buffer (`fluidsimulation_c.cpp:4106-4116`).

### 1.3 Python ctypes bindings (`src/engine/ffengine/`) [O]
- `ffengine.py:107` loads `libffengine.*` with `ctypes.cdll.LoadLibrary`.
- `fluidsimulation.py` (3470 lines): class `FluidSimulation` mirroring the C API; `pybindings.py:32-45` `init_lib_func` / `execute_lib_func` set argtypes and check the `err` out-param.
- `update(dt)` -> `lib.FluidSimulation_update` (`fluidsimulation.py:140-143`); `_get_output_data(size_fn, data_fn)` allocates `c_char * size` and returns `bytes` (`fluidsimulation.py:3032-3043`).
- CMake copies this package into the addon: `file(COPY src/engine/ffengine/ DESTINATION ${BLENDER_ADDON_DIR}/ffengine)` and the library into `ffengine/lib` (`CMakeLists.txt:245,280`).

### 1.4 Blender-specific code (`src/addon/`) [O]
- Properties (defaults that drive the engine): `src/addon/properties/domain_advanced_properties.py`, `domain_world_properties.py`, `domain_simulation_properties.py`, `obstacle_properties.py`, `domain_surface_properties.py`.
- `src/addon/bake.py` (3406 lines) is the driver: imports `FluidSimulation, MeshObject, MeshFluidSource, ...` from `.ffengine` (`bake.py:26-39`); bake runs on a Python `threading.Thread` started from `operators/bake_operators.py:198`, which calls `bake.bake(...)` (`bake_operators.py:525`).

### 1.5 Data flow [O]
```
Blender scene (bpy) --export--> geometry database / datafile
  -> src/addon/bake.py
       __initialize_fluid_simulation_settings (bake.py:912)   sets every engine param (PICFLIP_ratio, CFL, substeps, ...)
       __add_obstacle_objects / __add_inflow_objects ... (bake.py:1532,1556)   MeshObject(isize,jsize,ksize,dx)
       __run_simulation (bake.py:3249) loop per frame:
           __update_animatable_properties(...)  -> per-frame param + mesh updates
           __update_dynamic_object_mesh -> MeshObject.update_mesh_animated(prev, cur, next) (bake.py:1680-1683)
           fluidsim.update(dt)  (bake.py:3272), dt = time_scale / fps (bake.py:3240-3246)
           __write_simulation_output (bake.py:3212) -> fluidsim.get_*_data() -> files in cache/bakefiles
  -> ffengine/fluidsimulation.py (ctypes)
  -> c_bindings/fluidsimulation_c.cpp (extern "C")
  -> FluidSimulation::update(double dt) (fluidsimulation.cpp:11167)
```

---

## 2. Particle representation

- `ParticleSystem` is a structure-of-arrays container keyed by attribute name. Storage is one `std::vector<std::vector<T>>` per data type (char, uchar, bool, int, size_t ID, uint16, ulonglong, float, vec3) plus per-attribute defaults (`particlesystem.h:305-325`). `AttributeDataType` enum at `particlesystem.h:~34-45`. [O]
- `getAttributeValues("NAME", ptr)` returns a pointer to the raw vector; `update()` re-syncs sizes after push_backs (`particlesystem.cpp:31`); `removeParticles(std::vector<bool>&)` compacts all attributes (`particlesystem.cpp:88`). [O]
- Marker particle attributes created in `FluidSimulation::_initializeParticleSystems` (`fluidsimulation.cpp:4264-4309`) [O]:
  - Always: `POSITION` (vec3), `VELOCITY` (vec3).
  - APIC only: `AFFINEX`, `AFFINEY`, `AFFINEZ` (vec3 each) (`fluidsimulation.cpp:4268-4272`). These are the three rows of the APIC C matrix: `AFFINEX` = gradient of the u component, etc. (see `fluidsimulation.cpp:7255-7286`) [O]. In memory that is 9 floats, i.e. a full 3x3 C per particle.
  - Optional: `AGE`, `LIFETIME`, `COLOR`, `UVW`, `SOURCEID`, `VISCOSITY`, `DENSITY`, `ID` (uint16), `UID` (int).
- `MarkerParticle {vec3 position, velocity}` and `MarkerParticleAffine {affineX, affineY, affineZ}` are plain transfer structs for load/add (`markerparticle.h`) [O].
- Precision: `vmath::vec3` is float (positions stored in float) [I: vmath vec3 components are `float`, used with `(float)` casts throughout].
- Particle volume radius: `_markerParticleRadius = (3V/4pi)^(1/3)` with `V = dx^3/8` (`fluidsimulation.cpp:4487-4493`), i.e. 8 particles per cell by design. `_liquidSDFParticleRadius = 0.5 * scale * dx * sqrt(3)` (scale default 1.0) = 0.866dx (`:4492`, `fluidsimulation.h:2293-2294`) [O].

---

## 3. Grid data structures

### 3.1 MAC grid (`MACVelocityField`, `macvelocityfield.h`) [O]
- Three `Array3d<float>`: `_u` (isize+1, jsize, ksize), `_v` (isize, jsize+1, ksize), `_w` (isize, jsize, ksize+1) (`macvelocityfield.h`, index-range helpers `isIndexInRangeU/V/W`).
- Face positions: `FaceIndexToPositionU(i,j,k) = (i*dx, (j+0.5)dx, (k+0.5)dx)` (`grid3d.h:110-115`); analogous for V/W. Cell centers `(i+0.5)dx`; nodes `i*dx`.
- `evaluateVelocityAtPositionLinear` = per-component trilinear (`macvelocityfield.cpp:635-645`, `Interpolation::trilinearInterpolate` at `:548`); `evaluateVelocityAtPosition` (non-linear) is tricubic (`macvelocityfield.cpp:~448`). All simulation paths (G2P, RK3) use the Linear version.
- `ValidVelocityComponentGrid` = three `Array3d<bool>` validity masks per face (`macvelocityfield.h:~34-47`).
- `extrapolateVelocityField(valid, numLayers)` -> `GridUtils::extrapolateGrid` per component (`macvelocityfield.cpp:671-677`): layer-by-layer BFS; each new cell = average of already-known 6-neighbours (`gridutils.cpp:30-118,177-230`).
- Coarse/fine grid generation exists (`generateCoarseGrid`, `macvelocityfield.cpp:722-`) and was used for an experimental coarse pressure solve that is commented out (`fluidsimulation.cpp:6469-6537`).

### 3.2 `Array3d<T>` [O]
- Dense heap array `T* _grid`; flat index `i + width*(j + height*k)` (x fastest) (`array3d.h:780-783`).

### 3.3 Sparse / blocked structures [O]
- `BlockArray3d<T>` (`blockarray3d.h`): grid split into cubic blocks of `blockwidth`, only `activeblocks` allocated (`blockarray3d.h:31-37,199-246`). Used transiently in P2G (`velocityadvector.cpp:190-237`, block width `_chunkWidth = 10`, `velocityadvector.h:187`), liquid SDF (`particlelevelset.cpp:413`, `_blockwidth = 10`), mesh SDF (`meshlevelset.cpp:920`, `_blockwidth = 10`), particle mesher, attribute transfer.
- `SubdividedArray3d<T>` (`subdividedarray3d.h`): array with sub-cell subdivision level; used only by `FluidMaterialGrid` (`fluidmaterialgrid.h:112`).
- `FluidMaterialGrid`: `enum class Material : char { air=0, fluid=1, solid=2 }` (`fluidmaterialgrid.h:30-34`). Used in pressure gradient application (`pressuresolver.cpp:94-112`), diffuse sim, scalar field. The main pipeline does NOT use a material grid for classification; liquid vs air comes from `liquidSDF < 0` and solid from `MeshLevelSet` weights [O].
- The persistent simulation grids (MAC velocity, liquid SDF, solid SDF, weight grids) are **dense** over the whole domain [O]. Blocks are only a CPU scheduling and cache device.

### 3.4 Scalar grid placements [O]
| Grid | Size | Sample location |
|---|---|---|
| `ParticleLevelSet::_phi` (liquid) | isize x jsize x ksize | cell centers (`particlelevelset.cpp:42-45`, distance computed at `GridIndexToCellCenter`, `:652`) |
| `MeshLevelSet::_phi` (solid) | (isize+1)^3 nodes | grid nodes (`meshlevelset.cpp:35-42`) |
| `WeightGrid {center, U, V, W}` | cells / faces | (`pressuresolver.h:49-60`) |
| `_viscosity` | cells | |

### 3.5 Cell size and dimensions [O]
- `FluidSimulation(isize, jsize, ksize, dx)`; addon `resolution` = voxels along the longest domain side, default 65 (`src/addon/properties/domain_simulation_properties.py:115-122`).
- Domain walls are a solid AABB inset by 1.5 dx on every side: `_getBoundaryAABB` uses `expand(-3*dx - eps)` and `AABB::expand` subtracts `v/2` per side (`fluidsimulation.cpp:5479-5484`, `aabb.cpp:122-128`). This mesh is the negated "domain object" in the solid SDF (`fluidsimulation.cpp:5581-5591`). Pressure unknowns also skip the outer cell layer (`pressuresolver.cpp:139-149`).

---

## 4. Per-frame / per-substep pipeline (actual order)

### 4.1 Frame: `FluidSimulation::update(double dt)` (`fluidsimulation.cpp:11167-11300`) [O]
1. `substepTime = frameDt / _minFrameTimeSteps` (`:11195`).
2. Loop until frame time is consumed:
   - `_currentFrameTimeStep = min(_calculateNextTimeStep(dt), remaining)`.
   - If the step would overshoot the next `minSubsteps` boundary, clamp to `substepTime` (`:11217-11221`).
   - If `stepNumber == _maxFrameTimeSteps - 1`, take all the remaining time (`:11223-11225`). Max substeps is a hard cap, so CFL can be violated on the last substep.
   - `_stepFluid(step)`.
3. Collect solver stats and timings.

### 4.2 Substep: `FluidSimulation::_stepFluid(double dt)` (`fluidsimulation.cpp:10835-10878`) [O]
In exact source order:

| # | Call | What it does | Def line |
|---|---|---|---|
| 1 | `_launchUpdateObstacleObjectsThread(dt)` + `_joinUpdateObstacleObjectsThread()` | `_updateSolidLevelSet` (rebuild solid SDF and solid face velocities when any obstacle changed; animated moving meshes rebuild every substep), `_updateNearSolidGrid`, `_updateMeshingVolumeSDF` | 5854, 5691 |
| 2 | `_launchUpdateLiquidLevelSetThread()` + join | `_liquidSDF.calculateSignedDistanceField(particles, r)`, then on join `postProcessSignedDistanceField(_solidSDF)` | 5890, 5918 |
| 3 | `_launchAdvectVelocityFieldThread()` + join | `_advectVelocityField`: clear MAC, **P2G** (`VelocityAdvector::advect`), then `_extrapolateFluidVelocities` | 5927 |
| 4 | `_launchCalculateFluidCurvatureGridThread()` (only if surface tension, sheeting or whitewater) | Curvature runs concurrently with 5-7 | 6008 |
| 5 | `_saveVelocityField()` | `_savedVelocityField = _MACVelocity` (grid "old" velocity for FLIP delta) | 5975 |
| 6 | `_applyBodyForcesToVelocityField(dt)` | gravity / force-field grid added to all faces except inflow-constrained ones | 6257 |
| 7 | `_applyViscosityToVelocityField(dt)` | `_constrainVelocityField(_MACVelocity)` then implicit viscosity solve | 6287 |
| 8 | `_joinCalculateFluidCurvatureGridThread()` (if surface tension) | | 6042 |
| 9 | `_pressureSolve(dt)` | `_updateWeightGrid`, density grid, `PressureSolver::solve`, `applySolutionToVelocityField`, re-extrapolate | 6456 |
| 10 | `_constrainVelocityFields()` | Set solid faces to solid velocity (and friction blend on partial faces) on both `_savedVelocityField` and `_MACVelocity` | 6839 |
| 11 | join curvature (if whitewater) then `_updateDiffuseMaterial(dt)` | spray/foam/bubble/dust | 6860 |
| 12 | join curvature (if sheeting) then `_updateSheetSeeding()` | add particles in thin sheets | 6931 |
| 13 | `_updateMarkerParticleVelocities()` | **G2P** (FLIP blend or APIC) + inflow velocity constraint | 7377 |
| 14 | `_deleteSavedVelocityField()` | | 5991 |
| 15 | `_advanceMarkerParticles(dt)` | RK3 advection + collision + `_removeMarkerParticles` | 8423 |
| 16 | `_updateFluidObjects()` | add fluid objects, inflow emission, outflow removal, open-boundary removal | 9172 |
| 17 | `_updateMarkerParticleAttributes(dt)` | age, lifetime, color mixing, etc. | 8158 |
| 18 | `_outputSimulationData()` | Only when `_currentFrameTimeStepNumber == 0`: launch async mesher, write whitewater and particles; join mesher on the last substep | 10808 |

Notes:
- The function named `_advectVelocityField` is the P2G transfer, not semi-Lagrangian grid advection. There is no grid advection step: velocity is carried only by particles [O].
- Output data is generated after **substep 0** of the frame (`fluidsimulation.cpp:10809`). The meshing thread runs in parallel with substeps 1..N and is joined on the last substep (`:10827-10829`) [O].
- Steps 1-3 launch a thread and immediately join, so they are effectively serial; only curvature and meshing actually overlap other work [O].

---

## 5. Velocity transfer

### 5.1 Enum and selection [O]
- `enum class VelocityTransferMethod : char { FLIP = 0x00, APIC = 0x01 }` (`fluidsimulation.h:1564-1567`), default `FLIP` (`fluidsimulation.h:2401`). Mirrored by `VelocityAdvectorTransferMethod` (`velocityadvector.h`).
- Addon: `velocity_transfer_method` default `'VELOCITY_TRANSFER_METHOD_FLIP'` (`src/addon/properties/domain_advanced_properties.py:98-102`); applied via `set_velocity_transfer_method_FLIP/APIC` (`bake.py:1322-1326`).

### 5.2 P2G: `VelocityAdvector::advect` (`velocityadvector.cpp:38-43`) [O]
- Processes U, V, W **separately** (`_advectGrid(Direction)`).
- Per component, particles are shifted by the face offset (U: `(0, dx/2, dx/2)`, `velocityadvector.cpp:177-188`), so the component grid becomes "node-centered" in local coordinates.
- Active blocks (10^3 faces) are marked from particle positions and feathered 26-neighbour (`velocityadvector.cpp:190-237`). Particles are counted and sorted into blocks; a particle whose kernel overlaps several blocks is **duplicated into each block** (`velocityadvector.cpp:442-460`). Each block is then a private, race-free accumulation buffer.
- Work distribution: producer threads pop `_numBlocksPerJob = 10` blocks at a time from a `BoundedBuffer`; the main thread writes finished blocks back into the MAC array and marks `valid` where `weight > 1e-6` (`velocityadvector.cpp:114-175`).
- **FLIP-mode kernel** (`_advectionFLIPProducerThread`, `velocityadvector.cpp:467-536`):
  - `r = particleRadius = _liquidSDFParticleRadius = 0.866 dx` (radius passed in from `fluidsimulation.cpp:5939-5949`; scaled by 2.0 only if the smooth surface-tension kernel is enabled).
  - For every face inside the sphere `d^2 < r^2`: `w = 1 - (4/9) d^6/r^6 + (17/9) d^4/r^4 - (22/9) d^2/r^2` (`:474-476,514`). [I: this is the Wyvill "soft object" kernel; compact, C1, w(0)=1, w(r)=0.]
  - Accumulate `sum(w*v)` and `sum(w)`; divide at the end (`:525-530`). That is a normalized weighted average, not a mass-weighted momentum transfer.
- **APIC-mode kernel** (`_advectionAPICProducerThread`, `velocityadvector.cpp:543-623`, adapted from Doyub Kim's fluid-engine-dev per comment at `:538-542`):
  - Trilinear (tent) weights on the 8 surrounding component nodes (`:585-592`).
  - Scatter `w * (v_comp + dot(c_comp, x_node - x_p))` and `w`; normalize (`:601-616`).

### 5.3 G2P (`fluidsimulation.cpp:7216-7308`) [O]
- FLIP (`_updatePICFLIPMarkerParticleVelocitiesThread`, `:7216-7229`):
  ```
  vPIC  = MAC_new(x)                      // trilinear
  vFLIP = v_p + vPIC - MAC_saved(x)       // saved = post-P2G, pre-forces grid
  v_p   = ratioPICFLIP * vPIC + (1 - ratioPICFLIP) * vFLIP      // :7226
  ```
  `_ratioPICFLIP = 0.05` (`fluidsimulation.h:2460`; addon `PICFLIP_ratio` default 0.05, `domain_advanced_properties.py:105-116`).
- APIC (`_updatePICAPICMarkerParticleVelocitiesThread`, `:7236-7288`):
  - `affineX = sum_i gradW_i(x) * U_i` over 8 U-nodes using analytic trilinear weight gradients `_getIndicesAndGradientWeights` (`:7154-7214`); same for Y/V and Z/W.
  - `v_p = MAC_new(x)` (`:7283`). This is **pure APIC**: no FLIP component and no PIC/APIC blend.
  - `_ratioPICAPIC` (default 0.0, `fluidsimulation.h:2461`) has only a getter and setter (`fluidsimulation.cpp:2950,2964`) and is never read in the transfer [O]. The addon property is described as "Placeholder" (`domain_advanced_properties.py:117-123`).
- Then `_constrainMarkerParticleVelocities()` overwrites velocities of particles inside constrained inflows (`:7310-7375`).
- Threading: range-split over particles with `std::thread` (`:7290-7308`).

**APIC evidence summary:** APIC exists (enum value, `AFFINEX/Y/Z` attributes, P2G affine term, G2P gradient reconstruction, save-state load/save of affine data `bake.py:2942,3046,3122`). It is standard APIC (Jiang et al. 2015 form via Kim's implementation) with trilinear weights, not quadratic B-splines.

---

## 6. Pressure projection

Header citation: "A Fast Variational Framework for Accurate Solid-Fluid Coupling" (Batty, Bertails, Bridson), source Fluid3D (`pressuresolver.h:25-33`) [O].

### 6.1 Inputs (`fluidsimulation.cpp:6456-6584`) [O]
- `_updateWeightGrid()` (`:6370-6455`): `weight.U/V/W = clamp(1 - solidSDF.getFaceWeightU/V/W)`, `weight.center = 1 - solidSDF.getCellWeight`. Face weight = `LevelsetUtils::fractionInside` of the 4 face-corner node phis (2D area fraction, `meshlevelset.cpp:357-391`, `levelsetutils.cpp:62`). Cell weight = volume fraction of the 8 node phis (`meshlevelset.cpp:1491-1511`). This is the Batty variational (fractional-area) solid treatment [O].
- `velocityFieldSolid = _solidSDF.getVelocityDataGrid()->field` (per-face solid velocity).
- `liquidSDF = _liquidSDF.getPhiGrid()` (cell-centered).
- `densityGrid`: all 1.0 unless variable-density attribute is enabled (`:6519-6525`).
- Unknowns: interior cells (1..n-2) with `liquidSDF < 0` (`pressuresolver.cpp:139-149`), indexed via `GridIndexKeyMap`.

### 6.2 Conditioning (`PressureSolver::_conditionSolidVelocityField`, `pressuresolver.cpp:161-288`) [O]
Flood-fills connected liquid regions; regions with no face open to air have their surrounding solid face velocities zeroed. This keeps the system compatible (removes inconsistent RHS for sealed pockets).

### 6.3 RHS (`_calculateNegativeDivergenceVectorThread`, `pressuresolver.cpp:624-713`) [O]
```
div = sum_faces  -/+ (1/dx) * weight_f * u_fluid_f
    + sum_faces  +/- (1/dx) * (weight_f - weight_center) * u_solid_f
    + (surface tension) stfactor * weight_f * theta * sigma * kappa   for air neighbours
```
`stfactor = dt/dx^2`.

### 6.4 Matrix (`_calculateMatrixCoefficientsThread`, `pressuresolver.cpp:767-867`) [O]
- `factor = dt/dx^2`, `diag = sum_f (weight_f / rho_f) * factor`.
- Liquid neighbour: off-diagonal `-(weight_f/rho_f)*factor`.
- Air neighbour (ghost fluid): `theta = phi_neighbor / phi_center` clamped to `+/-_maxtheta = 25` (`pressuresolver.h:209`); `diag -= (weight_f/rho_f)*factor*theta`. Because phi_center < 0 and phi_air >= 0, theta <= 0, so the diagonal grows: this is the Gibou/Enright ghost-fluid Dirichlet p=0 condition at the sub-cell interface [I: classic GFM form, equivalent to dividing by the liquid fraction].
- `diag = max(diag, 0)`. 7-point stencil, `SparseMatrixd(matSize, 7)` (`pressuresolver.cpp:83`), double precision.
- Liquid face fractions are **not** used; only the solid face weights are fractional. Free surface uses GFM theta.

### 6.5 Solver (`_solveLinearSystem`, `pressuresolver.cpp:869-916`; `pcgsolver/pcgsolver.h`) [O]
- `useJacobiSolve = false` (`pressuresolver.cpp:875`), so always PCG. An OpenMP Jacobi path exists (`:918-1019`) but is dead code.
- `PCGSolver<double>`: "PCG with Modified Incomplete Cholesky (0) preconditioner" (`pcgsolver.h:15`); MIC parameter 0.97, min diagonal ratio 0.25 (`pcgsolver.h:250-266`); factorization `factorModifiedIncompleteColesky0` (`pcgsolver.h:69`); preconditioner apply = forward + backward triangular solve (`pcgsolver.h:342-345`, `solveLower` at `:343`), inherently sequential.
- Convergence: residual in **infinity norm**; stop when `|r|_inf <= min(tol * |r0|_inf, maxErrorTolerance=1.0)` (`pcgsolver.h:281-310,333`; tol at `:286`). The tolerance is relative.
- `FluidSimulation` passes `tolerance = _pressureSolveTolerance = 1e-9`, `acceptableTolerance = 1.0`, `maxIterations = _maxPressureSolveIterations = 900` (`fluidsimulation.h:2436-2438`; addon `pressure_solver_max_iterations` default 900, `domain_advanced_properties.py:82-88`). `PressureSolver` internal fallback default is 200 (`pressuresolver.h:208`) but it is overwritten by params.
- If max iterations are reached with error < acceptable (1.0), the solve is still accepted (`pressuresolver.cpp:901-905`).
- Initial guess: zero (`pressureGrid` freshly zeroed each substep, `fluidsimulation.cpp:6526`) [O]. No warm start.
- BLAS dot/absMax are threaded in chunks (`blaswrapper.h:39-58`); sparse mat-vec uses `FixedSparseMatrix` (CSR form of the list-of-lists `SparseMatrix`).
- [I] `pcgsolver/` is Robert Bridson's public-domain `pcg_solver.h` / `sparse_matrix.h` / `blas_wrapper.h` lineage (CC0 header, identical structure and names such as `SparseColumnLowerFactor`, `modified_incomplete_cholesky`).

### 6.6 Gradient application (`_applyPressureToVelocityFieldThread`, `pressuresolver.cpp:1047-1205`) [O]
- For faces with `weight > 0` that border fluid: `u -= (dt/dx) * (p2 - p1) / rho_face`; the air-side pressure is ghost-extrapolated `p_ghost = theta_tension*sigma*kappa + theta_pressure*p_liquid`. Marks face valid.
- All other faces are set to 0, then `_extrapolateFluidVelocities` (12 layers at CFL 5: `ceil(sqrt(3)*CFL)+3`, `fluidsimulation.cpp:6586-6590`).

### 6.7 Velocity constraint and friction (`fluidsimulation.cpp:6596-6837`) [O]
- `_constrainVelocityFieldThread`: if `weight_f == 0`, then `u = u_solid`; if `0 < weight_f < 1`, then `u = f*u_solid + (1-f)*u` where `f` = average friction of the closest mesh objects at the face's 4 nodes (`_getFaceFrictionU`, `:6639-6674`). The domain uses per-side `_boundaryFriction*` (all 0.0 by default, `fluidsimulation.h:2281-2286`). Obstacle friction default 0.0 (`meshobject.h:204`; addon `obstacle_properties.py:75-80`).
- Applied to both `_savedVelocityField` and `_MACVelocity` after pressure, so the FLIP delta does not inject solid-velocity jumps.
- Vestigial members `_density = 20.0` and `_minfrac = 0.01` (`fluidsimulation.h:2434-2435`) are not used by the solver [O: only getter/setter references].

---

## 7. Viscosity (`viscositysolver.cpp`) [O]
- "Viscosity solver adapted from Christopher Batty's viscosity3d.cpp ... C. Batty and R. Bridson" with the 2008 paper link (`viscositysolver.cpp:27-32`): variational, implicit, stress-based.
- Pipeline: `_computeFaceStateGrid` (fluid/solid per face from solid SDF), `_computeVolumeGrid` (liquid volume fractions at cell centers and edges, estimated on a 2x supersampled grid, `viscositysolver.cpp:209-378`, `i/2` indexing at `:330`), `_computeMatrixIndexTable`, assemble a **coupled u/v/w system** `SparseMatrixf(matsize, 15)` (`:65`), solve with `PCGSolver<float>` (`:832-864`), write back.
- Per-row coefficients use `dt/dx^2`, cell-center viscosity for normal stress terms and 4-cell averaged edge viscosity for shear terms (`:559-648`).
- Called only if enabled and viscosity is non-zero; first calls `_constrainVelocityField(_MACVelocity)` (`fluidsimulation.cpp:6287-6625`). Variable viscosity via particle attribute splat (`AttributeToGridTransfer`) + extrapolation (`:6293-6318`).
- Defaults: `_viscositySolverErrorTolerance = 1e-4`, `_maxViscositySolveIterations = 900` (`fluidsimulation.h:2422-2423`), acceptable tolerance 10.0 (`viscositysolver.h:251`). Addon: `enable_viscosity` False, `viscosity` 5.0 with an exponent, tolerance exponent 4 meaning 1e-4 (`domain_world_properties.py:139-170`, `bake.py:1135-1136`).

---

## 8. Surface tension [O]
- Enabled by `_isSurfaceTensionEnabled` (default false); constant `_surfaceTensionConstant` (addon `surface_tension` 0.25 with an exponent, `domain_world_properties.py:172-185`).
- Curvature: `ParticleLevelSet::calculateCurvatureGrid` (`particlelevelset.cpp:196-291`): select nodes with `|phi| < 2dx`, expand by blocks, `LevelSetSolver::reinitializeUpwind` over band `3dx` (`levelsetsolver.cpp:82`), compute curvature `_getCurvature` (div of normalized gradient, `particlelevelset.cpp:728-`), extrapolate 3 layers. Runs on its own thread (`fluidsimulation.cpp:6008-6052`).
- Optional "smooth kernel": liquid SDF particle radius x2.0 (`fluidsimulation.cpp:5899-5902`, `fluidsimulation.h:2295`).
- Applied inside the pressure solve as a ghost-fluid pressure jump `sigma * kappa(x_interface)` (`pressuresolver.cpp:715-749`) in both RHS and gradient. Small isolated clusters (< `_surfaceTensionClusterThreshold = 36` cells) are excluded via `_initializeSurfaceTensionClusterData` (`pressuresolver.h:210`, `pressuresolver.cpp:314-606`).
- Timestep restriction: `dt <= 10 * sqrt(dx^3) * sqrt(1/sigma)` (`_surfaceTensionConditionNumber = 10.0`, `fluidsimulation.cpp:11005-11008`, `fluidsimulation.h:2241`).

---

## 9. Level sets, obstacles, coupling

### 9.1 Liquid SDF (`ParticleLevelSet`) [O]
- `_computeSignedDistanceFromParticles` (`particlelevelset.cpp:335-411`): fill phi with `3dx`; for each particle and each cell center within `2r` (`_searchRadiusFactor = 2.0`, `particlelevelset.h:149`), `phi = min(phi, |x - p| - r)` with `r = 0.866dx`. Blocked and threaded like P2G (`_computeExactBandProducerThread`, `:618-666`). This is a union of spheres (Zhu-Bridson style without smoothing) [I].
- `postProcessSignedDistanceField(solid)` (`particlelevelset.cpp:170-194`): cells inside solid with `phi < 0.5dx` are set to `-0.5dx` (extends liquid into solids so the solid boundary acts as wet); clamps `|phi| >= 0.005dx` to avoid division by ~0 in GFM theta.

### 9.2 Solid SDF (`MeshLevelSet`) [O]
- Node-sampled `(n+1)^3` phi, `_closestTriangles`, `_closestMeshObjects`, `VelocityDataGrid` (`meshlevelset.cpp:35-42`).
- `calculateSignedDistanceField(mesh, vertexVelocities, bandwidth)` (`meshlevelset.cpp:547-570`): exact point-triangle distance in a band of `_solidLevelSetExactBand = 3` cells (`fluidsimulation.h:2291`), blocked and threaded (`:781-876,1115-1163`); `_propagateDistanceField` BFS outward via neighbours' closest triangles (`:1236-1296`); signs from `MeshUtils::getGridNodesInsideTriangleMesh` (inside/outside test by ray-parity or intersection counts) (`:1298-1318`); velocity grids (`:1320-1374`). The domain uses `fastCalculateSignedDistanceField` (skips propagation, `:577-601`).
- Multiple obstacles are merged with `calculateUnion` (min phi, merges triangle and object ids, `:603-638`); inverted obstacles are built into a temp SDF, negated, then unioned (`fluidsimulation.cpp:5558-5572`, static path `:5626-5640`).
- Static obstacles can be cached in `_staticSolidSDF` when `_isStaticSolidLevelSetPrecomputed` (`fluidsimulation.cpp:5492-5521,5645-5660`).

### 9.3 Moving obstacle velocity [O]
- Python supplies prev/current/next frame meshes per frame: `MeshObject::updateMeshAnimated(prev, cur, next)` (`meshobject.cpp:61-96`) stores per-vertex translations `cur - prev` and `next - cur`, detects rigid vs deforming vs topology-changing.
- `getMesh(frameInterpolation)` lerps current to next vertex positions (`meshobject.cpp:158-177`). `getVertexVelocities(dt, interp)` = lerped translations / `dt` * `_velocityScale` (`meshobject.cpp:183-220`), with `dt = frame dt` (`_updateSolidLevelSet(_currentFrameDeltaTime)` at `fluidsimulation.cpp:5866`).
- Per-face solid velocity: `MeshLevelSet::getNearestVelocity` finds the closest triangle among the 8 cell nodes and barycentrically interpolates vertex velocities (`meshlevelset.cpp:168-205`); stored as `weight * v` per face (`:1330-1340`) then `normalizeVelocityGrid` (`fluidsimulation.cpp:5724`).
- Animated meshes rebuild the SDF every substep while they move: `_isSolidStateChanged` returns true for `isAnimated && isMeshChanged` (`fluidsimulation.cpp:5662-5670`, `meshobject.cpp:893-911`), and the SDF is built at `frameProgress` for the current substep (`fluidsimulation.cpp:5537-5538`).
- How moving obstacles enter the BCs: (a) solid face velocity in the pressure RHS term `(weight_f - weight_c) * u_solid` (`pressuresolver.cpp:652-657`); (b) `_constrainVelocityFields` sets solid and partial faces to solid velocity (with friction); (c) particle collision projection against the SDF in `_resolveCollision`. Optional `_isAdaptiveObstacleTimeSteppingEnabled` (default false, `fluidsimulation.h:2514`) folds obstacle vertex speed into CFL (`fluidsimulation.cpp:10962-10986,10999`).
- `_resolveSolidLevelSetUpdateCollisionsThread` (pushing particles out after an SDF update) is entirely commented out (`fluidsimulation.cpp:5786-5824`).

### 9.4 Fluid -> obstacle force feedback: NONE [O]
- `grep -i "buoyan|two.way|twoway|rigid.*force|hydrodynamic|pressureforce|getForce|torque"` over `src/engine` (excluding Eigen) finds only force-field weights and diffuse-bubble buoyancy (`diffuseparticlesimulation.cpp:2369-2378`); `grep -i "coupling|two.way"` over `src/addon` and `src/engine` finds only the Batty paper title in `pressuresolver.h:28`.
- The pressure grid is a local inside `_pressureSolve` (`fluidsimulation.cpp:6526`) and is discarded; there is no pressure getter in `fluidsimulation.h` (only `getPressureSolverMaxIterations`, `:359`) and no C binding for pressure or forces.
- **Conclusion:** coupling is strictly one-way (kinematic obstacles drive fluid). The variational solid weights would support two-way rigid coupling (that is the point of the Batty 2007 paper) but it is not implemented. A UE port wanting buoyancy or impulses must integrate `-p n dA` over solid faces itself.

---

## 10. Advection, CFL, substeps

- Particle advection: `_RK3` (`fluidsimulation.cpp:8186-8193`):
  `k1=u(p); k2=u(p+0.5dt k1); k3=u(p+0.75dt k2); p1 = p + dt/9 (2k1+3k2+4k3)` [O]. Velocity used is the post-projection, post-constraint `_MACVelocity` (trilinear). Particle velocity itself is not used for position update [O].
- Collision (`_resolveCollision`, `:8216-8291`): if old or new position is in a near-solid coarse cell (`_nearSolidGrid`, factor 3, feathered by `ceil(CFL/3)` layers, `:5752-5784`), ray-march from old to new in steps of `0.1dx` (`_markerParticleStepDistanceFactor`, `fluidsimulation.h:2523`); at the first `phi<0` sample, project along the SDF gradient to `phi = 0.2dx` (`_solidBufferWidth`, `fluidsimulation.h:2513`), else fall back to the last free position; clamp into the domain AABB inset by the buffer [O].
- Timestep: `_calculateNextTimeStep(frameDt)` (`:10988-11020`): `maxu` = max particle speed (or predicted inflow speed on frame 0), optionally max obstacle speed; `dt = CFL*dx/(maxu+1e-6)`; min with surface-tension and color-mixing limits; then `dt = frameDt / ceil(frameDt/dt)` [O].
- Defaults: `_CFLConditionNumber = 5.0` (`fluidsimulation.h:2240`; addon `CFL_condition_number` int default 5, range 1-30, `domain_advanced_properties.py:125-133`). `_minFrameTimeSteps = 1`, `_maxFrameTimeSteps = 6` (engine, `fluidsimulation.h:2236-2237`); addon overrides to min 1 / **max 24** (`domain_advanced_properties.py:34-46`, applied `bake.py:1302-1304`) [O].

---

## 11. Particle seeding, removal, sheeting, extreme velocity, volume

- **Seeding** (`_addNewFluidCellsThread`, `fluidsimulation.cpp:8673-8713`; also `:8557-8609`, `_addNewFluidCellsAABB` `:8611-8671`): per source cell, 8 candidate positions at the cell center +/-0.25dx in each axis; keep if inside source mesh SDF (`d <= 0`) and outside solid (`solidSDF > 0`); jitter each axis uniformly in `+/-0.25*(jitterFactor-1e-3)*dx` (`_getMarkerParticleJitter`, `:4374-4377`), applied only to interior particles (`d < -dx`) unless `_isJitterSurfaceMarkerParticlesEnabled`. A `ParticleMaskGrid` (sub-cell occupancy) prevents double-seeding (`:8500-8509`) [O]. Engine default jitter factor 0.0 (`fluidsimulation.h:2249`), addon default **1.0** (`domain_advanced_properties.py:63-70`).
- Inflow: emitted every substep (or once per frame if `substepEmissions == 0`), with optional rigid or velocity-field-based initial velocities (`_updateInflowMeshFluidSource`, `:8715-8776`). Constrained inflows overwrite particle velocities after G2P and exclude faces from body forces (`:6064-6129,7310-7375`).
- **Removal** (`_removeMarkerParticles`, `:8343-8421`), run after every advection: inside solid (`trilinearInterpolateSolidPoints`, `:8379`), lifetime expiry, outside open boundaries (buffer `_openBoundaryWidth = 2` cells), **more than `_maxMarkerParticlesPerCell = 250` in a cell** (`:8404`, `fluidsimulation.h:2512`), extreme velocity.
- **Extreme velocity** (`_getMarkerParticleSpeedLimit`, `:8293-8341`): histogram speeds in bins of `CFL*dx/frameDt`; allow removing at most `min(0.05% of particles, 35)` (`fluidsimulation.h:2519-2520`) from the top bins so the frame fits in `maxSubsteps`; plus outlier removal above 0.99999 x max speed when at most 6 outliers. Enabled by default (`fluidsimulation.h:2516`) [O].
- **Sheeting** (`_updateSheetSeeding`, `fluidsimulation.cpp:6931-7152`; `ParticleSheeter::generateSheetParticles`, `particlesheeter.cpp:40-74`): find sparse surface cells in thin sheets (depth tests on `_fluidSurfaceLevelSet`, max sheet depth 2 cells, at most 6 particles/cell, `particlesheeter.h:131-139`), pick seed candidates, accept each with probability `_sheetFillRate = 0.5` (`fluidsimulation.cpp:7063-7076`), new velocity sampled from `_savedVelocityField` (`:7078`). Threshold `_sheetFillThreshold = -0.95` (`fluidsimulation.h:2455`). Default off (`fluidsimulation.h:2454`) [O].
- **Volume preservation:** no explicit volume correction, density-constraint pressure, or reseeding to a target count was found (`grep -i "volume.*correct|reseed|resample|targetvolume"` finds only a sheeter comment) [O]. Mechanisms that partially preserve volume [I]: 8-per-cell seeding, sheeting fill, the per-cell cap of 250 (prevents piling, does not refill), `postProcessSignedDistanceField` wetting near solids, and GFM sub-cell free surface.

---

## 12. Surface meshing [O]
- `_generateOutputSurface` (`fluidsimulation.cpp:9430-9493`) -> `ParticleMesher::meshParticles` (`particlemesher.cpp:36`).
- Scalar field: union-of-spheres distance `min(|x-p| - r)` on a (possibly subdivided, `_outputFluidSurfaceSubdivisionLevel = 1`) node grid, radius `r = _markerParticleRadius * _markerParticleScale` = `0.31dx * 3.0` = about 0.93dx (`fluidsimulation.cpp:9476`, `fluidsimulation.h:2381-2382`; addon multiplies user scale 1.0 by `native_particle_scale = 3.0`, `bake.py:1236`, `domain_surface_properties.py:369`), search radius x1.5 (`particlemesher.h:192`), blocked and threaded producers (`particlemesher.cpp:584-636`), processed in slab "compute chunks" with seam data.
- Solid SDF is combined so the mesh conforms to obstacles; optional obstacle meshing offset (`fluidsimulation.cpp:9443-9467`).
- `Polygonizer3d` = classic marching cubes with Paul Bourke `_edgeTable[256]` / `_triTable[256][16]` (`polygonizer3d.cpp:65,99,619`).
- Post: remove small polyhedra, remove near domain, attribute generation, **Laplacian smoothing** `v += 0.5*(avg_neighbors - v)` for 2 iterations (`TriangleMesh::_smoothTriangleMesh`, `trianglemesh.cpp:622-655`; `_surfaceReconstructionSmoothingValue = 0.5`, `_surfaceReconstructionSmoothingIterations = 2`, `fluidsimulation.h:2378-2379`; addon same, `domain_surface_properties.py:134-148`).
- Runs asynchronously on `_mesherThread` with copies of particles, a minimal solid SDF and the velocity field (`fluidsimulation.cpp:10014-10063`).

---

## 13. Diffuse particles (spray / foam / bubble / dust) [O]
- `DiffuseParticleSimulation::update` (`diffuseparticlesimulation.cpp:51-99`): build material grid -> get emitters -> emit -> `_advanceDiffuseParticles` -> `_updateDiffuseParticleTypes` -> lifetimes -> remove.
- Emission potentials (Ihmsen et al. "Unified Spray, Foam and Bubbles" style [I]): energy potential `_getEnergyPotential(v)`, wavecrest potential from curvature * `dx` and `dot(v_hat, n) >= _wavecrestSharpness = 0.4` (`diffuseparticlesimulation.cpp:1715-1742`), turbulence potential from a `TurbulenceField` (`:1744`). Emission rates 175 (`diffuseparticlesimulation.h:471-472`), max 10e6 particles (`:466`).
- Type by position relative to liquid SDF; spray = ballistic + drag (`_sprayDragCoefficient = 0.0`), bubbles = buoyancy `-4.0 * g` + drag 1.0 toward fluid velocity (`:2369-2378`, `.h:483-484`), foam advected by surface velocity (`_foamAdvectionStrength = 1.0`). One-way: diffuse particles do not affect the liquid.
- Output `.wwp` per type (`bake.py:2462-2481`).

---

## 14. Multithreading [O]
- `ThreadUtils` (`threadutils.h`, `threadutils.cpp:34-73`): `getMaxThreadCount()` = `std::thread::hardware_concurrency()` unless overridden (addon `max_thread_count`, `bake.py:1336-1341`); `splitRangeIntoIntervals(begin, end, n)` gives even contiguous ranges.
- Pattern everywhere: `std::vector<std::thread> threads(n); threads[i] = std::thread(&Class::fooThread, this, intervals[i], intervals[i+1], ...); join all`. A fresh set of threads is created per stage, per component (U/V/W), per call. No thread pool, no task graph [O].
- Producer/consumer `BoundedBuffer<ComputeBlock>` for block jobs (P2G, liquid SDF, mesh SDF, mesher).
- Stage-level concurrency: curvature thread and async meshing thread (section 4.2).
- **OpenMP:** `-fopenmp` on MinGW and Linux (`CMakeLists.txt:117,139`; not on macOS, `:125`). Only use: `#pragma omp parallel/barrier` in `PressureSolver::_solveLinearSystemJacobi` (`pressuresolver.cpp:30,937-1000`), which is disabled (`:875`). In practice the engine does not depend on OpenMP.
- `mingw32_threads/` (`mingw.thread.h`, `mingw.mutex.h`, `mingw.condition_variable.h`) is used only for 32-bit MinGW (`threadutils.h:27-36`, also included in `fluidsimulation.h:28`, `macvelocityfield.h:28`, etc.).
- MSVC builds are a hard `FATAL_ERROR` (`CMakeLists.txt:110-111`). This matters for UE (MSVC toolchain) if any code is reused directly [O].

---

## 15. External dependencies and output / caching

### 15.1 Dependencies [O]
- **Eigen:** a full copy is vendored in `src/engine/Eigen/` but no engine or addon source references it (`grep -rl Eigen` outside `src/engine/Eigen/` returns nothing). Unused.
- **pcgsolver/:** CC0 Bridson-style PCG/MIC(0) (`pcgsolver.h:1-15`) [lineage I].
- **mixbox:** `mixbox_stub.cpp` unless `WITH_MIXBOX` (`CMakeLists.txt:98-107`); only affects color-attribute mixing.
- Batty SDFGen (mesh SDF), Batty Fluid3D (pressure), Batty viscosity3d (viscosity), Doyub Kim fluid-engine-dev (APIC, curl), Paul Bourke (marching cubes), all cited in headers.
- Alembic exporter is a separate library `ffalembicengine` (`CMakeLists.txt:264-269`).

### 15.2 Output and serialization [O]
- Engine fills `std::vector<char>` buffers during `_outputSimulationData`; Python pulls them with size + data C calls (`fluidsim.get_surface_data()` etc.).
- Surface mesh `.bobj`: `[int numVerts][float xyz * N][int numTris][int abc * M]` (`TriangleMesh::getMeshFileDataBOBJ`, `trianglemesh.cpp:234-257`); `bake.py:2362-2378` writes `bakefiles/<frame>.bobj`, `blur<frame>.bobj`, `velocity<frame>.bobj`. PLY is an alternative (`fluidsimulation.cpp:9190-9196`).
- Whitewater `.wwp` (`foam/bubble/spray/dust<frame>.wwp`, `bake.py:2462-2481`); fluid particles `.ffp3` (`fluidparticles<frame>.ffp3` plus per-attribute files, `bake.py:2590-2606`; encoded by `_generateFluidParticleDataFFP3`, `fluidsimulation.cpp:10297`).
- Per frame also: bounds, logfile, frame stats JSON, autosave/savestate (marker particle positions, velocities, affine data when APIC), metadata, "finished" flag (`bake.py:3212-3237,2854`).

---

## 16. Default parameters relevant to a real-time port

| Parameter | Engine default (file:line) | Addon default | Notes |
|---|---|---|---|
| Transfer method | FLIP (`fluidsimulation.h:2401`) | FLIP | APIC available |
| PIC/FLIP ratio | 0.05 (`fluidsimulation.h:2460`) | 0.05 | fraction of PIC |
| PIC/APIC ratio | 0.0 (`:2461`) | 0.0 "Placeholder" | unused by engine |
| CFL number | 5.0 (`:2240`) | 5 (int, 1-30) | cells per substep |
| Min / max substeps | 1 / 6 (`:2236-2237`) | 1 / 24 | last substep absorbs the remainder |
| Surface-tension CFL | 10.0 (`:2241`) | | |
| Pressure tol / acceptable / iters | 1e-9 rel. inf-norm / 1.0 / 900 (`:2436-2438`) | iters 900 | MIC(0) param 0.97 (`pcgsolver.h:256`) |
| GFM theta clamp | 25 (`pressuresolver.h:209`) | | |
| Viscosity tol / iters | 1e-4 / 900 (`:2422-2423`) | 1e-4 / 900 | |
| Particles per cell | 8 (2x2x2, `fluidsimulation.cpp:8679-8689`) | | |
| Jitter factor | 0.0 (`fluidsimulation.h:2249`) | 1.0 | amplitude 0.25dx * factor |
| Max particles per cell | 250 (`:2512`) | | removal cap |
| Liquid SDF particle radius | 0.866 dx (`fluidsimulation.cpp:4492`) | | also the FLIP P2G kernel radius |
| Mesh particle radius | 0.31dx * 3.0 (`fluidsimulation.h:2382`) | scale 1.0 x native 3.0 | |
| Solid exact band | 3 cells (`:2291`) | | |
| Solid buffer | 0.2 dx (`:2513`) | | collision push-out |
| Extrapolation layers | `ceil(sqrt(3)*CFL)+3` = 12 (`fluidsimulation.cpp:6588`) | | |
| Boundary friction (domain) | 0.0 per side (`fluidsimulation.h:2281-2286`) | 0.0 (`domain_world_properties.py:249-254`) | |
| Obstacle friction | 0.0 (`meshobject.h:204`) | 0.0 (`obstacle_properties.py:75-80`) | |
| Extreme velocity removal | on, <= min(0.05%, 35) (`fluidsimulation.h:2516-2522`) | on | |
| Domain resolution | n/a | 65 cells on the longest axis | |
| Mesh smoothing | 0.5 x 2 iterations (`fluidsimulation.h:2378-2379`) | same | |

---

## 17. Implications for a GPU port (UE5.8 RDG compute)

### 17.1 Maps cleanly to compute (data-parallel)
- **P2G:** per-component scatter. GPU options: (a) atomic float add of `w*v` and `w` into face buffers (simple; fine on UE's SM6 `InterlockedAdd` on float via `asuint` CAS or native float atomics where available); (b) sort particles by cell (counting sort / radix sort) then gather per face, which is deterministic and mirrors the engine's block-sorting approach. Keep the engine's normalized average (`sum w v / sum w`). Kernel choice: FLIP mode uses a 0.866dx spherical Wyvill kernel, which touches up to about 27 faces per component; a trilinear (APIC-style) 8-node stencil is cheaper and is what the engine's APIC path uses. Recommended: implement APIC with trilinear first (matches engine), optionally quadratic B-spline later.
- **G2P, RK3 advection, body forces, constraint, friction blend, weight grid, RHS and matrix coefficients, gradient apply:** all embarrassingly parallel per particle or per face/cell.
- **Liquid SDF from particles:** per-cell gather over particles from a cell-sorted list (or atomic-min on `asuint(float)` for non-negative distances with an offset). Union of spheres with r = 0.866dx, band 3dx.
- **Velocity extrapolation (12 layers):** layered Jacobi-style passes (each pass = one dispatch averaging valid 6-neighbours) or a jump-flood / fast-sweeping variant. 12 dispatches at CFL 5; a real-time port should lower CFL (1-2) which lowers the layer count to about 5-7.
- **Marching cubes:** standard GPU MC with the Bourke tables + stream compaction; Laplacian smoothing per vertex needs adjacency (or smooth the SDF instead, which is cheaper on GPU).
- **Diffuse particles:** per-particle potentials and integration; emission via append buffers.

### 17.2 Hard or sequential stages
- **MIC(0) preconditioner** (`pcgsolver.h:69,342-345`): factorization and triangular solves are sequential (row-ordered dependencies). Do not port. Substitutes, in rough order of real-time suitability:
  1. **Geometric multigrid-preconditioned CG (MGPCG)** on the dense grid with the same variational weights and GFM diagonal (McAdams et al. 2010 style), red-black Gauss-Seidel or damped-Jacobi smoothers. Best convergence per ms.
  2. **Plain multigrid V-cycles** as the solver (2-4 cycles per substep) when exact divergence-free is not needed.
  3. **Jacobi / red-black GS or Chebyshev-accelerated Jacobi** with a fixed iteration budget plus warm start from the previous substep's pressure (the engine always cold-starts from zero, `fluidsimulation.cpp:6526`).
  4. **IC with red-black or multicolor ordering**, or an incomplete Poisson (IP) / SPAI-0 preconditioner, if staying with PCG.
  Keep the engine's matrix definition (section 6.4) exactly; only replace the solve. Note the engine's stopping rule is relative inf-norm 1e-9 with a 900 iteration cap; a real-time budget will be about 20-60 iterations, so expect visible compressibility unless multigrid is used.
- **Dot products in CG:** global reductions each iteration; use wave intrinsics + two-pass reduction; minimize readback (keep alpha/beta on GPU).
- **Mesh level-set construction** (`meshlevelset.cpp`): exact-band triangle rasterization is parallelizable (one thread per triangle writing atomic-min into nodes within band, then pick closest triangle), but **sign determination** (`MeshUtils::getGridNodesInsideTriangleMesh`) and the BFS propagation are irregular. GPU substitutes: precompute SDFs offline for rigid obstacles (UE Mesh Distance Fields / baked volume textures) and transform per frame (rigid) using the object transform, deriving solid face velocity analytically `v + w x r`; for deforming meshes use GPU triangle splat + jump flooding + generalized winding numbers or ray-parity with a few rays. UE's Global Distance Field is too coarse for exact solid face fractions but could serve for collision of distant geometry.
- **Isolated-pocket conditioning** (`_conditionSolidVelocityField`) and **surface-tension cluster detection** are flood fills; on GPU use iterative label propagation (connected-components via pointer jumping) or skip (sealed pockets are rare in games; regularize by adding a tiny diagonal epsilon).
- **Variable-length particle add/remove** (seeding, per-cell caps, extreme velocity histogram, sheeting): use append/consume buffers, prefix-sum compaction, and per-cell counters (counts are already needed for sorting). The extreme-velocity histogram is a parallel histogram + small serial scan.
- **Dense grids:** the engine is dense; memory is fine at real-time resolutions (for example 128^3). For larger domains a sparse-brick layout (matching the engine's 10^3 `BlockArray3d` idea, or 8^3 / 4^3 bricks with an indirection table) is recommended, but it complicates multigrid; start dense.
- **Viscosity:** coupled 3-component variational system with volume fractions on a 2x supersampled grid. Expensive; for real time use explicit or a few Jacobi iterations of the implicit diagonal-dominant form, or disable. Port Batty-Bridson only if high-viscosity materials are a requirement.
- **Two-way coupling:** absent in the reference. If needed for UE Chaos rigid bodies, integrate pressure over solid face fractions (`sum p * weight_face_delta * n * dx^2`) on GPU, reduce per body, and feed back as an impulse; or implement the full Batty 2007 coupled system. This is new design work, not a port.
- **Precision:** the engine solves pressure in double. On GPU use float with an MG preconditioner (float is adequate with multigrid); avoid deep plain-CG iterations in float.

### 17.3 Suggested GPU substep (mirrors engine order)
`[obstacle SDF/velocity update] -> [particle sort] -> [liquid SDF] -> [P2G + valid mask] -> [extrapolate] -> [save grid (FLIP)] -> [gravity] -> [constrain] -> [weights + RHS + matrix] -> [MGPCG] -> [gradient apply] -> [extrapolate] -> [constrain saved+new] -> [G2P (FLIP 0.05 or APIC)] -> [RK3 advect + SDF collision] -> [remove/emit/compact]`, with surface reconstruction once per frame.
