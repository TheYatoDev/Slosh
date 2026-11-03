# Feasibility: Running FLIP Fluids Without Blender, and Integration Strategies for UE 5.8

- **Subject repo:** `C:\Users\Dae\Documents\SourceStuff\Blender-FLIP-Fluids` @ `70a0e954018fe39e1f9c3631264989569752bb7a` (engine v1.8.8, `CMakeLists.txt:56-58`)
- **Date:** 2026-11-03
- **Target:** 60 FPS interactive liquid in UE 5.8 on an **RTX 5060 Laptop GPU (8 GB)**, which this machine reports. The project brief's Quadro RTX 4000 / i9-10900 is a different dev box; repeat the measurements there if it is used.
- **Rules:** I did not build or run anything in the repo. I only read files and inspected the PE headers of DLLs already present in `build/` with `objdump -p`. Every performance figure is marked **[Inferred]**. No measurements were taken.
- **Companion document:** `LICENSING_AUDIT.md` (same folder).

**Evidence tags**
- **[Observed]**: read in source or in a binary header, cited `path:line`.
- **[Inferred]**: my reasoning.

---

## 1. Can the engine build and run without Blender?

**Verdict: yes.** The simulation engine is a self-contained C++17 library with a pure `extern "C"` ABI. Nothing in `src/engine` depends on Blender or Python.
- Python (`ffengine/*.py`) is only a ctypes client.
- Blender coupling lives entirely in the GPL addon (`src/addon`), which we must not use anyway.

The limitations are a build-system and toolchain issue, not a code-dependency issue.

### 1.1 Build system [Observed]

| Fact | Evidence |
|---|---|
| **MSVC is hard-rejected.** "Compilation using MSVC … is not supported … will result in errors, performance issues, and broken simulation features." | `CMakeLists.txt:110-111` (`message(FATAL_ERROR …)`). Flags for a would-be MSVC build remain at `:112-115` (`/O2 /W4 /EHsc /wd4244`). |
| **MinGW is the only supported Windows compiler.** Flags: `-static -static-libgcc -static-libstdc++ -fopenmp -O3 -std=c++17 -Wall` | `CMakeLists.txt:116-119`; `README.md:63-66` |
| Linux build force-includes a glibc 2.5 symbol-versioning header | `CMakeLists.txt:139-140` |
| **No `add_executable` target.** Two shared libraries are built: `ffengine` (engine + C bindings) and `ffalembicengine` (engine + Alembic exporter). | `CMakeLists.txt:264-276` |
| **Alembic is `REQUIRED` even to build `ffengine`**, because `find_package` is unconditional. | `CMakeLists.txt:247-253` |
| The engine source list is explicit | `CMakeLists.txt:175-232` (`SOURCES_FLUID_ENGINE_LIBRARY`, 43 engine `.cpp` + 11 binding `.cpp` + mixbox stub) |
| **Eigen is not referenced** by any source or by CMake (see the licensing audit, section 2.1) | grep |
| Mixbox is off by default; the stub is compiled | `CMakeLists.txt:42`, `:99-107` |
| CMake configure **writes generated files into the source tree** (`versionutils.cpp`, `__init__.py`, `blender_manifest.toml`, `installation_utils.py`) | `CMakeLists.txt:157-172` |
| `build.py` simply wraps `cmake -G "MinGW Makefiles"` + `make` on Windows | `build.py:26-49` (`:34`) |

**Local state [Observed]:** a MinGW build already exists in `build/`. It was configured with MSYS2 `C:/msys64/mingw64/bin/c++.exe` (GCC 16.2.0 per strings in the DLL), CMake 4.4.5, and `Alembic_DIR=C:/msys64/mingw64/lib/cmake/Alembic`. Its output is:

```
build/bl_flip_fluids/flip_fluids_addon/ffengine/lib/
  libffengine.dll (6.1 MB)  libffalembicengine.dll (6.7 MB)  libAlembic.dll  libImath-3_2.dll
  libhdf5-320.dll  libsz-2.dll  libz-ng2.dll  libstdc++-6.dll  libgcc_s_seh-1.dll  libwinpthread-1.dll
```

What `objdump -p libffengine.dll` shows:
- PE32+ x86-64.
- Imports only **`KERNEL32.dll` and `msvcrt.dll`**. libstdc++, libgcc and libgomp are statically linked; `GOMP_*` strings are present.
- **738 named exports:**
  - 1 `CBindings_*`
  - 592 `FluidSimulation_*`
  - 52 `ForceField*`
  - 50 `MeshFluidSource_*`
  - 40 `MeshObject_*`
  - 3 `Mixbox_*`

These DLLs were not produced by me. They are timestamped 2026-11-03 11:57, and a side effect of that configure is the modified `src/addon/utils/installation_utils.py` in `git status`.

### 1.2 `src/engine/main.cpp` [Observed]

- A 103-line demo, not a CLI tool (`main.cpp:69-103`). It:
  - creates a `FluidSimulation(64, 64, 64, 0.125)` (`:73-77`);
  - sets surface subdivision 2 (`:79`);
  - builds a box `TriangleMesh` covering the middle third of the domain (`:84-89`, helper `getTriangleMeshFromAABB` at `:46-67`);
  - wraps it in a `MeshObject` and calls `addMeshFluid` (`:90-92`);
  - adds body force `(0, -25, 0)` (`:94`). **Y is up.**
  - calls `initialize()` (`:95`), then `for(;;) { update(1/30); writeSurfaceMesh(); }` (`:96-101`). That is an **infinite loop**.
- `writeSurfaceMesh` dumps `getSurfaceData()` bytes to a **hard-coded `C:/tmp/NNNNNN.ply`** (`:30-41`).
- **No CMake target builds it as an executable.** It is not in `SOURCES_FLUID_ENGINE_LIBRARY`. However, the Alembic target's `file(GLOB "src/engine/*.cpp")` (`CMakeLists.txt:256-262`) does compile `main.cpp` into `libffalembicengine.dll` as a stray `main` symbol.
- `src/alembic_io/src/main.cpp:30-45` is a stub that prints "This feature will become available in a future release."

### 1.3 The C ABI (`src/engine/c_bindings`) [Observed]

**Conventions**
- Every function is `extern "C"` with `EXPORTDLL` = `__declspec(dllexport)` on `_WIN32` (`fluidsimulation_c.cpp:36-42`).
- Nearly every call takes a trailing `int *err`: `1` = SUCCESS, `0` = FAIL (`cbindings.cpp:35-36`).
- `CBindings_get_error_message()` returns the last message (`cbindings.cpp:99-101`).
- C++ exceptions are converted to `err` by the `safe_execute_*` templates, which catch `std::exception&` only (`cbindings.h:44-148`).

**Key exports** (`fluidsimulation_c.cpp` unless noted)

| Purpose | Function (line) |
|---|---|
| Create / destroy | `FluidSimulation* FluidSimulation_new_from_dimensions(int isize,int jsize,int ksize,double dx,int*err)` (56); `FluidSimulation_new_from_empty` (43); `void FluidSimulation_destroy(FluidSimulation*)` (71, no try/catch) |
| Init | `FluidSimulation_initialize` (97); `FluidSimulation_is_initialized` (101); `FluidSimulation_upscale_on_initialization` (82) |
| Step | `void FluidSimulation_update(FluidSimulation*, double dt, int*err)` (115). This advances **one frame** of length `dt` with internal CFL substeps. `FluidSimulation_get_current_frame` (133), `FluidSimulation_is_current_frame_finished` (160) |
| Domain / config | `FluidSimulation_get_simulation_dimensions` (198); `FluidSimulation_set_domain_offset` (562) / `FluidSimulation_set_domain_scale` (576); `FluidSimulation_set_surface_subdivision_level` (478); `FluidSimulation_disable_surface_reconstruction` (626); `FluidSimulation_enable/disable_asynchronous_meshing` (640/647); `FluidSimulation_set_pressure_solver_max_iterations` (337); `FluidSimulation_set_min/max_time_steps_per_frame` (2972/2988); `FluidSimulation_set_max_thread_count` (2666) |
| Forces | `FluidSimulation_add_body_force(obj, double fx, double fy, double fz, int*err)` (2674); `FluidSimulation_reset_body_force` (2690); force-field objects in `forcefield*_c.cpp` |
| Obstacles | `MeshObject* MeshObject_new(int i,int j,int k,double dx,int*err)` (`meshobject_c.cpp:38`); `MeshObject_update_mesh_static(MeshObject*, TriangleMesh_t, int*)` (`:56`); `MeshObject_update_mesh_animated(obj, prev, cur, next, err)` (`:71`); `MeshObject_destroy` (`:52`); `FluidSimulation_add_mesh_obstacle(sim, MeshObject*, err)` (3199); `FluidSimulation_remove_mesh_obstacle(s)` (3207/3215) |
| Fluid sources | `MeshFluidSource_new` (`meshfluidsource_c.cpp:38`); `MeshFluidSource_update_mesh_static/animated` (`:56/:71`); `MeshFluidSource_set_inflow/outflow` (`:120/:126`); `MeshFluidSource_set_velocity` (`:217`); `FluidSimulation_add_mesh_fluid_source` (3176); one-shot fluid volume `FluidSimulation_add_mesh_fluid(sim, MeshObject*, Vector3_t velocity, err)` (3222) |
| Particle data | `size_t FluidSimulation_get_num_marker_particles` (3237); `FluidSimulation_get_marker_particle_position_data_size` / `FluidSimulation_get_marker_particle_position_data(obj, char* out, err)` (4064 / 4970); `FluidSimulation_get_marker_particle_*_data_range(obj, start, end, char* out, err)` (4761-4904; position, velocity, affine x/y/z, age, color …); legacy `FluidSimulation_get_marker_particles(obj, start, end, MarkerParticle_t* out, err)` (3244); diffuse/whitewater equivalents (3289, 4915-5012) |
| Surface mesh | `size_t FluidSimulation_get_surface_data_size` (3360) + `FluidSimulation_get_surface_data(obj, char* out, err)` (4106). The output is a **serialized file blob**: PLY by default (`fluidsimulation.h:2385`, `_getTriangleMeshFileData` `fluidsimulation.cpp:9190-9195`), or BOBJ via `FluidSimulation_set_mesh_output_format_as_bobj` (591). Preview, blur, and per-vertex attribute meshes follow the same pattern (3373-3581, 4118-4310). |
| Stats / timing | `FluidSimulationFrameStats FluidSimulation_get_frame_stats_data(obj, err)` (4754), returned **by value**. It includes `FluidSimulationTimingStats {total, mesh, advection, particles, pressure, diffuse, viscosity, objects}` (`fluidsimulation.h:69-78`) and pressure/viscosity iterations and error (`:80-100`). |
| State reload | `FluidSimulation_load_marker_particle_*_data` (5019-5107) |

**C structs** (all `float`)
- `Vector3_t {x,y,z}` (`vector3_c.h:26-30`)
- `AABB_t` (`aabb_c.h`)
- `MarkerParticle_t {pos, vel}` (`markerparticle_c.h`)
- `MeshUtils::TriangleMesh_t {float* vertices; int* triangles; int numVertices; int numTriangles;}` (`meshutils.h:34-39`)
- `GridIndex_t` (int)

### 1.4 Memory ownership across the ABI [Observed, with risks Inferred]

1. **Opaque handles** (`FluidSimulation*`, `MeshObject*`, `MeshFluidSource*`, `ForceField*`) are allocated with `new` inside the DLL and freed with `delete` inside the DLL via the `*_destroy` functions (`fluidsimulation_c.cpp:71-73`, `meshobject_c.cpp:52-54`). No cross-CRT `free`.
2. **Inputs are copied.** `MeshObject_update_mesh_static` converts the caller's `TriangleMesh_t` into an internal `TriangleMesh` (`meshobject_c.cpp:56-69`, `MeshUtils::structToTriangleMesh`). The caller may free its arrays after the call. `addMeshFluid` takes the `MeshObject` **by value** (`fluidsimulation.h:1331-1332`).
3. **Obstacles and sources are registered by raw, non-owning pointer** (`FluidSimulation::addMeshObstacle` pushes `obstacle` into `_obstacles`, `fluidsimulation.cpp:3062-3076`; `addMeshFluidSource` likewise, `:3021-3033`). The host must keep these objects alive until `remove_*`, and must never destroy them while `update()` runs.
4. **Outputs use a size-then-copy pattern into caller-allocated buffers.** Example: `get_surface_data_size` then `memcpy(c_data, data->data(), data->size())` (`fluidsimulation_c.cpp:3360-3371`, `4106-4116`). The Python side allocates `(c_char * size)()` (`ffengine/fluidsimulation.py:3032-3043`). Particle positions are packed 12-byte `float3` (`ffengine/fluidsimulation.py:3045-3053`, `size_of_vector = 12`). The DLL never hands heap memory to the caller.
5. **Error string:**
   - a pointer to a **process-global static** `char[4096]` (`cbindings.cpp:37`), so it is not thread-safe;
   - `set_error_message` copies `msg.length()` bytes without clamping to 4095 (`cbindings.cpp:39-43`), so it **can overflow** on long messages [Inferred from code];
   - only `std::exception` is caught. `*_destroy` functions are unguarded. [Inferred] An exception on one of the engine's internal `std::thread`s calls `std::terminate`, which would take down the UE editor if the DLL is in-process.
6. **Struct returns by value** (`Vector3_t`, `AABB_t`, the large `FluidSimulationFrameStats`). [Inferred] Both mingw-w64 GCC and MSVC follow the Microsoft x64 calling convention for `extern "C"`: more than 8 bytes goes through a hidden return pointer. They should interoperate, but `FluidSimulationFrameStats` has C++ default member initializers, so the host must declare a layout-identical plain C struct. **Verify in the experiment.**

### 1.5 `ffengine/pybindings.py` / `ffengine.py` [Observed]

- `FFEngineLib` lazily loads the first `libffengine*.dll|.dylib|.so` found in `ffengine/lib/` via `ctypes.cdll.LoadLibrary`, sorting by name length (`ffengine/ffengine.py:49-134`, load at `:107`).
- `pybindings.execute_lib_func` appends a `c_int` `err` by reference, calls the function, and raises `RuntimeError(CBindings_get_error_message())` on failure (`ffengine/pybindings.py:24-59`).
- The Python package imports only stdlib + ctypes. No `bpy` (grep of imports).
- **The engine therefore already runs headless from plain Python 3** (ctypes, cdecl). On Win64 there is a single calling convention, so `cdll` is fine.

### 1.6 Minimum components to run without Blender [Observed]

- `src/engine/*.cpp` minus `main.cpp`, i.e. the 43 files listed in `CMakeLists.txt:176-218`, including `versionutils.cpp`, which is pre-generated in-tree.
- `src/engine/c_bindings/*.cpp` (11 files).
- `src/engine/mixbox/mixbox_stub.cpp` (required, because `mixbox_c.cpp` and `fluidsimulation.cpp` reference the API). Compile with `-DWITH_MIXBOX=0`.
- `src/engine/pcgsolver/*.h` (headers).
- **Not needed:** `Eigen/`, `alembic_io/`, Alembic/Imath/HDF5, `glibc_version_header/` (Linux), `mingw32_threads/` (32-bit only), `ffengine/*.py` (unless driving from Python), and all of `src/addon`.

### 1.7 Planned experiment: dam break outside Blender (NOT executed)

Run everything in a scratch directory outside the repo. Copy the sources so CMake's in-tree `configure_file` writes cannot touch the checkout.

**Option 1 (fastest): MSVC host + existing prebuilt `libffengine.dll`.** This directly tests MinGW-DLL / MSVC-host interop, which is what Strategy A needs.
1. Copy `build/bl_flip_fluids/flip_fluids_addon/ffengine/lib/libffengine.dll` to a scratch folder. It has no third-party DLL dependencies beyond `msvcrt.dll`.
2. Write `ffengine_c.h`, a hand-written C prototype subset from 1.3, and `dambreak.c`. Compile with `cl /O2` (and clang-cl), using `LoadLibraryW` + `GetProcAddress`.
3. Call sequence (Y up, domain 64×32×32 at dx = 0.05 m, i.e. 3.2 × 1.6 × 1.6 m):
   - `sim = FluidSimulation_new_from_dimensions(64,32,32,0.05,&err)`
   - `FluidSimulation_set_max_thread_count(sim, N)`. Sweep N.
   - Optionally `FluidSimulation_disable_surface_reconstruction` for a particle-only timing pass.
   - Build a box `TriangleMesh_t` for x ∈ [0, 0.4·W], y ∈ [0, 0.6·H], z ∈ [0, D] (12 triangles, like `main.cpp:46-67`).
   - `MeshObject_new(64,32,32,0.05,&err)`, `MeshObject_update_mesh_static`, then `FluidSimulation_add_mesh_fluid(sim, mo, (Vector3_t){0,0,0}, &err)`. Then `MeshObject_destroy(mo)` is safe, because the mesh object is copied by value.
   - Domain walls are closed by default (`_openBoundary* = false`, `fluidsimulation.h:2525-2530`).
   - `FluidSimulation_add_body_force(sim, 0, -9.81, 0, &err)`, then `FluidSimulation_initialize(sim, &err)`.
   - Loop 300 frames with `FluidSimulation_update(sim, 1.0/60.0, &err)`. Each frame:
     - `FluidSimulation_get_frame_stats_data`: log `substeps`, `timing.total/pressure/advection/particles/mesh`, `pressureSolverIterations`;
     - `QueryPerformanceCounter` wall time;
     - `get_num_marker_particles`;
     - every 10th frame, `get_surface_data_size/get_surface_data`, written to a `.ply`.
   - `FluidSimulation_destroy(sim)`.
4. Repeat at 64³ and 128³, with subdivision levels 1/2, meshing on/off, and thread counts 1/4/8/hardware.
5. **Acceptance:** stable run with no crash. The PLY opens in a viewer. The CSV of timings gives the real per-frame cost that replaces the inferences in section 2.

**Option 2: rebuild `main.cpp` as an exe with MinGW** (scratch copy):
- `g++ -O3 -std=c++17 -fopenmp -DWITH_MIXBOX=0 -static <engine .cpp list> c_bindings/*.cpp mixbox/mixbox_stub.cpp main_dambreak.cpp`
- Edit the copy to: bound the loop (`main.cpp:97`), replace `C:/tmp/` (`main.cpp:36`), move the box to the dam-break position, and print `getFrameStatsData()`.

This avoids Alembic entirely. A trivial `CMakeLists` with just `add_library(ffengine SHARED …)` would also avoid the `find_package(Alembic REQUIRED)` problem.

**Option 3: Python** headless driver using `src/engine/ffengine` (MIT) and the prebuilt DLL. This is the quickest sanity check. Do **not** reuse `src/addon/bake.py`, which is GPL.

---

## 2. Integration issues for UE 5.8

| Issue | Finding | Impact |
|---|---|---|
| **Compiler** | [Observed] MSVC is rejected (`CMakeLists.txt:110-111`). UE Win64 builds with MSVC (clang-cl optional). | [Inferred] Either (a) ship a MinGW-built DLL behind the C ABI, which is loadable via `FPlatformProcess::GetDllHandle`, or (b) port the engine to compile under MSVC. The authors warn of "broken simulation features" with MSVC, so (b) is unquantified work. |
| **CRT** | [Observed] The DLL links legacy **`msvcrt.dll`**. UE uses the **UCRT**. | [Inferred] Separate heaps and stdio. This is OK because the ABI copies out into caller buffers (1.4). Never pass `FILE*`, `std::` types, or memory to be freed across the boundary. `std::cout` logging from the DLL (`logfile.cpp:151-155`) goes nowhere useful in the editor. |
| **Exceptions** | [Observed] Only `std::exception` is caught at the boundary. Worker threads are raw `std::thread`. | [Inferred] Any escaping exception or `std::terminate` kills the UE process. An **out-of-process host** (helper .exe + shared memory) would isolate crashes, the CRT, and threads at the cost of an IPC copy. |
| **Threads** | [Observed] 67 call sites create `std::vector<std::thread> threads(numthreads)` per operation (e.g. `velocityadvector.cpp:212,260`). Count defaults to `std::thread::hardware_concurrency()` (`threadutils.cpp:39`); `set_max_thread_count` exists (`fluidsimulation_c.cpp:2666`). OpenMP appears only in the disabled Jacobi path (`pressuresolver.cpp:875-876` `useJacobiSolve=false`; `#pragma omp` at `:937-1000`), yet libgomp is statically linked. | [Inferred] Per-call thread creation plus oversubscription against UE's TaskGraph workers means jitter. These threads cannot be mapped onto UE tasks without modifying the engine. |
| **Precision** | [Observed] Mixed. Particles and vectors use `float` (`vmath.h:41,57`); grids are `Array3d<float>`. The **pressure PCG is `double`** (`pressuresolver.cpp:881` `PCGSolver<double>`, default tolerance 1e-9 and max 900 iterations at `fluidsimulation.h:2436-2438`). The viscosity PCG is `float` (`viscositysolver.cpp:835`). `dt`, `dx` and forces are `double` in the API. | [Inferred] A GPU port must go to float32: consumer GPUs run FP64 at a small fraction of FP32 rate. The tolerance must be relaxed accordingly. |
| **Pressure solver** | [Observed] PCG with **Modified Incomplete Cholesky (0)** (`pcgsolver/pcgsolver.h:15-19`). The triangular solves are sequential (`solveLower`, `solveLowerTransposeInPlace` `:212,:228`). No threading in `pcgsolver.h`. | [Inferred] Likely the dominant CPU cost at scale. MIC(0) is **inherently serial** and does not map to GPU. A GPU path needs Jacobi/RBGS, a multigrid-preconditioned CG, or similar. |
| **Offline, frame-based design** | [Observed] `update(dt)` = one *frame*, subdivided by CFL (`fluidsimulation.cpp:11167-11260`; `_calculateNextTimeStep` `:10988-11025`; CFL 5, min 1 / max 6 substeps `fluidsimulation.h:2236-2240`). Substep pipeline: `fluidsimulation.cpp:10835-10880`. Animated obstacles need **previous, current and next** meshes (`meshobject_c.cpp:71-88`), i.e. a frame of lookahead. Surface meshing is launched only on substep 0 (`fluidsimulation.cpp:10808-10829`). Output is serialized PLY/BOBJ (`:9190-9195`). The API also carries `set_blend_filepath_string`, timeline start/end, and log files. | [Inferred] Built for baking to a disk cache. Real time needs: extrapolated obstacle motion (no lookahead), surface mesh parsing on the game thread or a worker, and per-frame upload of particles and mesh to the GPU. |
| **Per-frame cost** | [Observed] The source has instrumentation (`FluidSimulationTimingStats`, `fluidsimulation.h:69-78`) but **no recorded timings**. | [Inferred, unmeasured] For production-quality resolutions (≥128³ with meshing), CPU cost per frame is very likely **well above a 16.7 ms budget**, plausibly hundreds of ms to seconds. That fits the offline intent and the serial MIC(0) solve. Coarse grids (32³-64³) without meshing might approach interactive rates. **Run experiment 1.7 to replace this with data.** |
| **Data transfer** | [Observed] Particle positions are 12 B each; the surface comes as a file blob. | [Inferred] 1 M particles means 12 MB per frame, about 0.7 GB/s at 60 FPS of CPU-to-GPU upload, plus mesh rebuilds. That is feasible but eats into frame time. |
| **Coordinates** | [Observed] Y-up, metres (`main.cpp:94`); domain offset and scale are available (`fluidsimulation.h:2383-2384`). | UE is Z-up, cm, left-handed. Convert at the boundary. |

---

## 3. Strategy evaluation

**Hardware context [Inferred]**
- RTX 5060 Laptop GPU: Blackwell, 8 GB, 128-bit GDDR7, roughly 350-450 GB/s depending on SKU and power limit.
- Laptop TGP varies widely, so performance must be profiled on this exact machine.
- The GPU also renders the UE scene (Lumen/Nanite/TSR). A realistic sim budget is about 3-6 ms of the 16.7 ms frame.

### Strategy A: Direct CPU integration (ship the FLIP engine DLL, call it via the C ABI)

- **Complexity: low to moderate.** The ABI exists. You need:
  - a UE module that loads the DLL;
  - a worker-thread runner (`isCurrentFrameFinished` exists for that, `fluidsimulation.h:283-288`) with double-buffered outputs;
  - PLY/BOBJ parsing into `UDynamicMesh` / `FMeshDescription`, or particles into Niagara;
  - collider extraction to `TriangleMesh_t`.
- **Performance [Inferred]:** bounded by the CPU engine. Not 60 FPS at useful resolutions, because of the serial double-precision MIC(0) PCG, per-call thread creation, and offline meshing. It cannot use the GPU. It would compete with the game thread and the render thread.
- **Maintainability:** good for staying in sync with upstream (a drop-in DLL), but it means a MinGW toolchain in a UE project. Debugging across the toolchain boundary is painful.
- **Risks:**
  - crash propagation (exceptions, `std::terminate`);
  - CRT mismatch;
  - Fab acceptance of a GCC-built binary and GCC runtime-exception licensing (see `LICENSING_AUDIT.md` §4.5 and §6);
  - no real-time obstacle lookahead.
- **Best use:** an **editor-time bake / validation oracle**, not runtime.

### Strategy B: Hybrid CPU/GPU

Two sub-variants:
- **B1:** CPU sim + GPU rendering. GPU surface reconstruction, e.g. screen-space fluid rendering or GPU marching cubes from uploaded particles. This removes the CPU meshing cost, but the simulation loop (pressure, advection) stays on the CPU, so the 60 FPS ceiling remains (A's limits). [Inferred]
- **B2:** offload only the pressure solve to GPU compute. Each substep would need to upload the matrix/RHS (or grid) and read back pressure. At 128³ that is about 8-16 MB each way per substep, with a GPU sync and readback stall. This needs UE RHI readback inside the engine's substep loop, plus re-architecting the engine. [Inferred] **Worst of both worlds**: PCIe round trips and serialization dominate. Not recommended.
- **Complexity:** high, because it touches engine internals and needs synchronization. **Maintainability:** poor, as a fork of upstream. **Risk:** high.

### Strategy C: Native GPU reimplementation (UE RDG compute shaders, FLIP engine as reference)

- **Complexity: high.** You write:
  - APIC/FLIP P2G (atomic scatter or sorted gather) and G2P;
  - MAC grid ops;
  - an SDF/level set for colliders (UE Distance Fields or a custom voxelizer);
  - a pressure solver (MGPCG, or Jacobi/RBGS with warm start; the variational solid-fraction weights from the reference are valuable);
  - particle advection, emitters and sinks;
  - a rendering path (screen-space fluid or GPU marching cubes).
- Scope it to a core feature set first. Whitewater, viscosity, surface tension and sheeting come later.
- **Performance [Inferred, must be prototyped]:**
  - Grid passes are bandwidth-bound. One 128³ float field is 8 MB, so about 10 field sweeps per substep is about 80 MB, which is sub-millisecond at around 400 GB/s.
  - The pressure solve dominates. With a multigrid preconditioner, a few V-cycles are needed; pure Jacobi needs tens to hundreds of iterations for comparable divergence reduction.
  - P2G atomics contention and particle sorting are the other hot spots.
  - A 64³-128³ domain with roughly 0.5-2 M particles at 1-2 substeps is a plausible 60 FPS target in a 3-6 ms budget, but this is **not guaranteed**.
- **The GPU is not automatically faster:**
  - small domains (≤48³) underutilize the GPU, and dispatch overhead per pass adds up across about 20-40 dispatches per substep;
  - irregular work (narrow-band level sets, sparse fluid cells, emitter voxelization) is harder;
  - FP64 is effectively unavailable;
  - gameplay readback (buoyancy, splash events) introduces latency of one or more frames;
  - the sim shares the GPU with rendering;
  - laptop thermals throttle.
- **Memory:** an 8 GB budget is ample at this scale. A 128³ MAC grid with about 15-20 fields is roughly 120-160 MB. 2 M APIC particles at about 60 B each is roughly 120 MB.
- **Maintainability:** good. Native UE code, MSVC/clang, Unreal Insights and GPU profiling, no foreign runtime, and no Fab binary questions. The divergence from upstream is intentional.
- **Risks:**
  - numerical stability and volume loss at float32 with relaxed tolerances;
  - solver convergence at large CFL;
  - team effort;
  - reaching visual parity with an offline mesher (the FLIP Fluids mesher is not a real-time design).
- **Licensing:** cleanest. Keep the MIT notice anyway (`LICENSING_AUDIT.md` §4.4).

### Comparison

| | A: CPU DLL | B1: CPU sim + GPU render | B2: GPU pressure only | **C: native GPU** |
|---|---|---|---|---|
| Engineering complexity | Low-Med | Med | High | High |
| Expected 60 FPS at ≥64³ with ~10⁵-10⁶ particles [Inferred] | Unlikely | Unlikely | Unlikely | **Plausible (prototype to confirm)** |
| Fidelity to FLIP Fluids output | Exact | Exact sim, different render | Near | Approximate (float32, different solver) |
| Maintainability in UE | Med (foreign toolchain) | Med | Poor | **Good** |
| Crash / ABI / CRT risk | High (in-proc) | High | High | Low |
| Licensing / Fab friction | Med (GCC binary) | Med | Med | **Low** |

---

## 4. Recommendation

**Adopt Strategy C (native GPU reimplementation in UE 5.8 RDG compute)** for the runtime product. Use the FLIP Fluids MIT engine as the **algorithmic reference** and as a **CPU ground-truth oracle**, which is a scoped use of Strategy A as an internal dev tool only.

1. **Week-0 gate (cheap, data first).** Run experiment 1.7 Option 1: an MSVC host with the prebuilt DLL. It yields:
   - real CPU per-frame timings (`FluidSimulationTimingStats`), replacing every [Inferred] number above;
   - confirmation of MinGW-DLL/MSVC interop;
   - reference dam-break particle and mesh sequences for validating the GPU solver (volume over time, front position versus time).
2. **Prototype the GPU core** (APIC P2G/G2P, MAC advection, solid SDF, pressure via MGPCG versus Jacobi) at 64³ and 128³ on the RTX 5060 Laptop. Measure GPU ms per pass in Unreal Insights / `ProfileGPU` **before** committing to secondary features. Decide 128³ versus 64³ plus sparse/narrow-band from data.
3. **Reuse from the reference as designs, not code.** Translate closely only where it is clearly beneficial, and if so keep the MIT notices. Candidates:
   - variational solid weights for pressure (`pressuresolver.*`);
   - APIC transfer (`velocityadvector.cpp:539+`);
   - CFL/substep logic (`fluidsimulation.cpp:10988-11025`);
   - whitewater emission heuristics (`diffuseparticlesimulation.*`).

   Implement the Batty-derived pieces from the papers (provenance gap, `LICENSING_AUDIT.md` §3.3). **Never consult `src/addon`.**
4. **Optional later:** an editor-only "bake with FLIP engine" path (Strategy A, out-of-process helper .exe) for cinematic offline quality. Legal must clear the GCC-binary and Fab questions first.

**Key uncertainties to retire early:**
- the actual CPU cost (experiment 1.7);
- GPU pressure-solve convergence and cost at float32;
- the surface rendering approach within budget;
- laptop GPU thermal headroom while UE renders a full scene.
