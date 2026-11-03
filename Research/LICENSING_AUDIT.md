# Licensing Audit: Blender FLIP Fluids as a Reference for a Commercial UE 5.8 Liquid Plugin

- **Subject repo:** `C:\Users\Dae\Documents\SourceStuff\Blender-FLIP-Fluids`
- **Origin / commit:** https://github.com/rlguy/Blender-FLIP-Fluids @ `70a0e954018fe39e1f9c3631264989569752bb7a` (2026-08-24, "Updated Discord invite link"). Engine version per `CMakeLists.txt:56-58` = 1.8.8.
- **Audit date:** 2026-11-03
- **Scope:** licensing of every file under `src/engine`, `src/addon`, `src/alembic_io`, plus root `CMakeLists.txt` and `build.py`. I did not modify or build anything in the repo.
- **Status:** this is an engineering inventory, not legal advice. Everything in Section 6 needs review by qualified counsel before release.

**Evidence tags**
- **[Observed]**: read directly from a file in the repo, from the prebuilt binaries already present in `build/`, or from a live web fetch, with the source cited.
- **[Inferred]**: my reasoning, or general knowledge of a license or third-party project that this repo does not state.

---

## 0. Executive verdict

| Component | License | Can a separately distributed commercial UE plugin reuse/port it? |
|---|---|---|
| `src/engine/*.cpp,*.h` (100 files + `versionutils.cpp.in`) | MIT, © 2026 Ryan L. Guy & Dennis Fassbaender | **Yes.** Keep the MIT notice. Ten files also carry embedded third-party notices (Batty SDFGen MIT, Doyub Kim MIT), and some cite Batty repos that publish **no license**. See 3.2. |
| `src/engine/c_bindings/*` (17) | MIT (Guy & Fassbaender) | Yes, with notice. |
| `src/engine/ffengine/*.py` (19) | MIT (Guy & Fassbaender) | Yes, with notice. These are Python ctypes wrappers, mostly useful as reference. |
| `src/engine/pcgsolver/*` (3) | CC0 1.0 (public-domain dedication) | Yes, no obligations. Attribution is courteous. |
| `src/engine/mingw32_threads/*` (4) | BSD-2-Clause, © 2016 Mega Limited | Yes, keep the notice. Only used for **32-bit** MinGW, so it is irrelevant on Win64. |
| `src/engine/glibc_version_header/*` (2) | MIT, © 2018 Tom Mason | Linux-only build shim. Not needed for UE/Windows. |
| `src/engine/Eigen/*` (337, Eigen 3.4.0) | Mostly MPL-2.0, with some BSD and Apache-2.0 files | **Not compiled or used by the engine at all.** Do not ship it. If it is ever used, MPL-2.0 file-level copyleft applies. |
| `src/engine/mixbox/mixbox.h` | **"non-commercial use only"** (Secret Weapons), plus a commercial certificate issued to *1206018 B.C. LTD.*, not to us | **Exclude.** |
| `src/engine/mixbox/mixbox_stub.cpp` | No header (trivial stub) | Exclude along with the Mixbox feature. |
| `src/addon/**/*.py`, `*.in`, `blender_manifest.toml` (122) | **GPL-3.0-or-later** | **Must not be shipped, copied, or translated** into the UE plugin. |
| `src/addon/third_party/cpuinfo.py` | BSD-style (NumPy license), © 2002 Pearu Peterson | Not needed. |
| `src/addon/third_party/mixbox/` | Only `empty_file` | Nothing here. The proprietary plugin is distributed separately. |
| Addon assets: `materials/`, `presets/preset_library/{sys,usr/basic_fluids}/`, `resources/example_scene_data/` | MIT (per each directory's `LICENSE.md`) | Allowed, but they are Blender `.blend`/`.preset`/`.sim` data with little value for UE. |
| `src/addon/resources/media/*.mp4` | CC BY 4.0 | Not needed. Attribution is required if used. |
| `src/addon/resources/sounds/alarm/alarm.ogg` | CC0 | Not needed. |
| `src/alembic_io/src/*` (5) | MIT (Guy & Fassbaender) | Not needed. It links **Alembic** (BSD-3, not vendored). |
| `src/alembic_io/src/nlohmann/json.hpp` | MIT, nlohmann/json 3.12.0 | Not needed. MIT applies if used. |
| `CMakeLists.txt`, `build.py` | MIT (Guy & Fassbaender) | Only useful as a reference. |
| Root `__init__.py` | GPL-3.0-or-later | Exclude. |
| `LICENSE_Standard_Royalty_Free.md` | Covers paid-only content that is **not in this repo** | Not applicable to anything we would take. |

**Bottom line [Inferred]:** the MIT engine can legally be reused, ported, or used as an algorithmic reference in a closed-source commercial UE plugin, provided that:
1. the MIT notices are kept, including the embedded Batty/Kim notices for derived files;
2. every addon GPL file and the Mixbox code are kept out entirely;
3. the provenance of the code "adapted from" Batty's unlicensed repos is resolved, or that code is reimplemented independently from the papers.

---

## 1. Method [Observed]

1. Root license files: `LICENSE_MIT.md` (MIT, "Copyright (C) 2026 Ryan L. Guy & Dennis Fassbaender"), `LICENSE_GPLv3.md` (GPL v3 text), `LICENSE_Standard_Royalty_Free.md` (13 lines).
2. `README.md:43-51` states the split:
   - "The Blender addon code is licensed under the GPL."
   - "The FLIP Fluids simulation engine is licensed under the MIT license."
   - Some paid-only content (example scenes, media, Mixbox) is Standard Royalty Free and "will not be available on the GitHub project page."
   - "Source code files will state their license at the top of the file."
3. A Python scanner (`C:\Users\Dae\AppData\Local\Temp\claude\C--Users-Dae-Desktop-UE5-Fluid-Sim\ee214b94-cb8f-4fa9-bede-11272477611e\scratchpad\scan.py`; raw results in `...\scratchpad\scan.json`) read the first 60 lines of every file under the scoped paths and matched these patterns:
   - `MIT License` / "Permission is hereby granted, free of charge"
   - `GNU General Public License` / `GPL-[23]`
   - `LGPL` / `GNU Lesser`
   - `Mozilla Public` / `\bMPL\b`
   - `BSD` / "Redistribution and use in source and binary forms"
   - `Apache License`
   - `public domain`
   - `Creative Commons` / `CC BY`
   - `SPDX-License-Identifier`
   - and extracted `Copyright` lines.
4. Outliers were reviewed by hand.
5. A whole-tree grep looked for embedded attributions (`adapted from|based on|derived from|Copyright (c)|github.com`).

---

## 2. Source-level inventory

### 2.1 `src/engine` [Observed]

| Directory / component | Files | Header license (count) | Copyright holders | Notes / outliers |
|---|---|---|---|---|
| `src/engine/` (top level) | 101 (44 `.cpp`, 56 `.h`, 1 `.in`) | **MIT = 101** | "Ryan L. Guy & Dennis Fassbaender" (2026) | `versionutils.cpp` is generated from `versionutils.cpp.in` by `CMakeLists.txt:157-160`. `main.cpp` is MIT. Embedded third-party notices are listed in 3.2. |
| `src/engine/c_bindings/` | 17 (11 `.cpp`, 6 `.h`) | **MIT = 17** | Guy & Fassbaender | None. |
| `src/engine/ffengine/` | 19 `.py` | **MIT = 19** | Guy & Fassbaender | Stdlib + ctypes only. No `bpy` imports. |
| `src/engine/pcgsolver/` | 3 (`pcgsolver.h`, `sparsematrix.h`, `blaswrapper.h`) | **CC0 1.0 = 3** | none (public-domain dedication) | Header: "CC0 1.0 Public Domain License … even for commercial purposes" (`pcgsolver.h:1-11`). [Inferred] The code is consistent with Robert Bridson's public-domain PCG/MIC(0) solver used in Batty's Fluid3D, which has a `pcgsolver/` folder (see 3.3). |
| `src/engine/mingw32_threads/` | 4 (`LICENSE` + 3 headers) | **BSD-2-Clause = 4** | © 2016 Mega Limited | Included only `#if __MINGW32__ && !_WIN64` (10 include sites, e.g. `meshlevelset.h` after the license block). Unused on x64. |
| `src/engine/glibc_version_header/` | 2 (`LICENSE.md`, `force_link_glibc_2.5.h`) | MIT = 1 (`LICENSE.md`, © 2018 Tom Mason). The header has no notice. | Tom Mason | Force-included only on Linux (`CMakeLists.txt:140`). |
| `src/engine/mixbox/` | 2 (`mixbox.h`, `mixbox_stub.cpp`) | No OSS license | Secret Weapons (`mixbox.h:1-3`) | `mixbox.h` line 2: "This is for non-commercial use only." The certificate transcription (`mixbox.h:5-32`) grants commercial use to **1206018 B.C. LTD.** for the FLIP Fluids addon only. `mixbox.cpp` is **absent**: only the two files are tracked (`git ls-files`). `WITH_MIXBOX` defaults OFF (`CMakeLists.txt:42`), which selects the stub (`CMakeLists.txt:99-107`). |
| `src/engine/Eigen/` | 337 (Eigen **3.4.0**, `Eigen/src/Core/util/Macros.h:18-20`) | MPL-2.0 = 298; BSD-only = 20; MPL2+BSD = 2; MPL2 + "public domain" = 2; Apache-2.0 = 1; no header = 14 | Gael Guennebaud, Benoit Jacob, Intel, TensorFlow Authors, and others | See 2.2. **No Eigen `COPYING.*` files are vendored.** **Not referenced by any engine/addon/alembic source or by CMake** (grep for `eigen` outside `Eigen/` hits only `src/addon/utils/geometry_utils.py:140`, a NumPy `np.linalg.eigh` call). |

### 2.2 Eigen detail [Observed unless noted]

- **BSD (Intel MKL/LAPACKE bindings), 20 files.** Examples: `Eigen/src/Core/util/MKL_support.h`, `Eigen/src/Core/Assign_MKL.h`, `Eigen/src/*/*_LAPACKE.h`, `Eigen/src/Core/products/*_BLAS.h`, `Eigen/src/PardisoSupport/PardisoSupport.h`, `Eigen/src/misc/lapacke.h`. Copyright "2011, Intel Corporation".
- **MPL2+BSD:** `Eigen/src/Core/arch/Default/Half.h` (Fabian Giesen 2016) and `Eigen/src/Geometry/AlignedBox.h` (Willow Garage 2011-2014).
- **Apache-2.0:** `Eigen/src/Core/arch/Default/BFloat16.h` ("Copyright 2017 The TensorFlow Authors").
- **"Public domain" mention (JAMA-derived):** `Eigen/src/Eigenvalues/EigenSolver.h`, `RealSchur.h`.
- **LGPL heritage, all relicensed to MPL2 in-file:**
  - `Eigen/src/OrderingMethods/Amd.h:11-17` (CSparse, T. Davis: "executed a license with Google LLC … under the Mozilla Public License v. 2.0")
  - `Eigen/src/SparseCholesky/SimplicialCholesky_impl.h:13-15` (LDL, same)
  - `Eigen/src/IterativeLinearSolvers/IncompleteLUT.h:88-93` (SPARSKIT, "Yousef Saad gave us permission to relicense his ILUT code to MPL2")
- **`EIGEN_MPL2_ONLY`:** the guard `Eigen/src/Core/util/NonMPL2.h` exists, but **no Eigen file includes it**, and `EIGEN_MPL2_ONLY` is not defined anywhere in the project. [Inferred] This means this Eigen 3.4.0 subset contains no LGPL-only code. Defining `EIGEN_MPL2_ONLY` would be a cheap safety belt if Eigen were ever adopted.
- **No-header files (14):** module aggregators (`Eigen/Dense`, `Eigen/Eigen`), plugin fragments (`Eigen/src/plugins/*.h`), `DisableStupidWarnings.h`, `ReenableStupidWarnings.h`, `blas.h`, `lapack.h`, `lapacke_mangling.h`, `NonMPL2.h`, `AltiVec/MatrixProductCommon.h`, `NEON/GeneralBlockPanelKernel.h`, `HIP/hcc/math_constants.h`. These are covered by Eigen's top-level licensing (MPL2), whose COPYING files are not vendored.
- **Practical conclusion [Inferred]:** Eigen carries **no obligation** for us, because it is neither compiled into `libffengine` nor needed for a port. Do not copy `src/engine/Eigen` into the plugin. If an Eigen-like library is ever needed in UE, use UE's own bundled Eigen (`Engine/Source/ThirdParty/Eigen`), whose licensing Epic has already handled, or vendor Eigen from upstream with its COPYING files.

### 2.3 `src/addon` [Observed]

| Component | Files | License | Notes |
|---|---|---|---|
| Python sources (all subdirs: top level, `filesystem/`, `materials/`, `objects/`, `operators/`, `presets/`, `properties/`, `resources/`, `ui/`, `utils/`) | 118 `.py` | **GPL-3.0-or-later** ("either version 3 of the License, or (at your option) any later version", e.g. `src/addon/__init__.py.in:1-15`) | Copyright "Ryan L. Guy & Dennis Fassbaender" (one file, `presets/render_passes.py`, is "Dennis Fassbaender" only). |
| Templates | 3 `.in` (`__init__.py.in`, `blender_manifest.toml.in`, `utils/installation_utils.py.in`) | GPL-3.0-or-later | — |
| `blender_manifest.toml(.in)` | 1 (+ template) | `license = ["SPDX:GPL-3.0-or-later"]` (`blender_manifest.toml.in:12`) | — |
| **`third_party/cpuinfo.py`** | 1 | BSD-style "NumPy (BSD style) license", © 2002 Pearu Peterson (`cpuinfo.py:1-22`) | The referenced `LICENSES/cpuinfo.txt` is **not present** in the repo. |
| **`third_party/mixbox/`** | 1 (`empty_file`) | n/a | Placeholder for the separately distributed proprietary "Mixbox.plugin" (`mixbox.h:9-11`). |
| `materials/` assets | `LICENSE.md` (MIT) | MIT for "Material Library Blend files and containing materials" | The `.py` files in that directory are still GPL. |
| `presets/preset_library/sys/`, `presets/preset_library/usr/basic_fluids/` | `LICENSE.md` (MIT) each; 15 `.preset`, 15+ `.png`, `.blend`, `package.info` | MIT | Data files have no headers, so the directory LICENSE governs. |
| `resources/example_scene_data/` | `LICENSE.md` (MIT); 4 `flipdata.sim` | MIT | — |
| `resources/media/` | 22 `.mp4` + `0_LICENSE_CC_BY_ATTRIBUTION_4.0.txt` | CC BY 4.0 | Excluded from builds unless `DISTRIBUTE_MEDIA` (`CMakeLists.txt:43`, `:282-284`). |
| `resources/sounds/alarm/` | `alarm.ogg`, `license.txt`, `sound_data.json` | CC0 (OpenGameArt) | — |
| `icons/` | 2 `.png` | no notice. A PNG metadata hit for "GPL" is a scanner artefact. | [Inferred] Treat as GPL/addon branding, so exclude. |
| Misc. | `.bat`, `versions.json`, `empty_file` ×3 | no notice | Addon-internal. |

### 2.4 `src/alembic_io`, root build files [Observed]

| File | License |
|---|---|
| `src/alembic_io/src/alembic_io.cpp`, `alembic_io.h`, `c_bindings/alembic_io_c.cpp` | MIT, © 2026 Guy & Fassbaender |
| `src/alembic_io/src/main.cpp`, `main.h` | MIT, © 2025 Guy & Fassbaender |
| `src/alembic_io/src/nlohmann/json.hpp`, `LICENSE.MIT` | MIT, `SPDX-License-Identifier: MIT`, © 2013-2025 Niels Lohmann, v3.12.0 |
| `CMakeLists.txt`, `build.py` | MIT, © 2026 Guy & Fassbaender |
| Root `__init__.py` | GPL-3.0-or-later (addon shim) |

### 2.5 External dependencies not vendored

| Dependency | How it enters | License |
|---|---|---|
| **Alembic** | `find_package(Alembic REQUIRED)` (`CMakeLists.txt:248-253`). Linked only into `ffalembicengine` (`:264-270`). | [Inferred] BSD-3-Clause (Lucasfilm / Sony Pictures Imageworks). |
| **Imath 3** | Alembic dependency (`README.md:70-71`) | [Inferred] BSD-3-Clause (ASWF). |
| HDF5, szip/libaec, zlib-ng | **[Observed]** These DLLs appear in the existing local build output `build/bl_flip_fluids/flip_fluids_addon/ffengine/lib/` (`libhdf5-320.dll`, `libsz-2.dll`, `libz-ng2.dll`), pulled in by the MSYS2 Alembic package. | [Inferred] HDF5: BSD-style; libaec: BSD-2; zlib-ng: zlib license. |
| GCC runtime (libstdc++, libgcc, libgomp), winpthreads | **[Observed]** `CMakeLists.txt:117` uses `-static -static-libgcc -static-libstdc++ -fopenmp`. The prebuilt `libffengine.dll` imports only `KERNEL32.dll` and `msvcrt.dll` (objdump), and contains `GOMP_*` strings (statically linked libgomp). `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll` are also present in `lib/`, for the Alembic DLL. | [Inferred] GPLv3 **with GCC Runtime Library Exception**, which permits distributing the compiled result under any terms. winpthreads: MIT/BSD-style. Needs legal confirmation if we ever ship a MinGW-built binary (Strategy A/B). |

None of these are needed for a GPU reimplementation (Strategy C) or for an engine-only DLL built without `alembic_io`.

---

## 3. Third-party code embedded in MIT engine files

### 3.1 Summary of the requested dependencies

| Item | Finding |
|---|---|
| **Eigen** | MPL-2.0 (3.4.0). No LGPL-only files present (`NonMPL2.h` is never included). **Unused by the engine.** **[Observed]** |
| **mingw32_threads** | BSD-2-Clause, © Mega Limited (`mingw32_threads/LICENSE`). 32-bit MinGW only. **[Observed]** |
| **mixbox** | `mixbox.h` is Secret Weapons, "non-commercial use only". `mixbox.cpp` (the real proprietary implementation; [Inferred] publicly CC BY-NC 4.0) is **absent**. `mixbox_stub.cpp` returns zeros. **[Observed]** |
| **pcgsolver** | CC0 1.0. [Inferred] Bridson-derived via Batty's Fluid3D. **[Observed]** for the license text. |
| **glibc_version_header** | MIT, Tom Mason 2018. Linux only. **[Observed]** |
| **Alembic / Imath** | BSD-3 [Inferred]. Only used by `alembic_io`. **[Observed]** for usage. |

### 3.2 Embedded notices and attributions inside MIT engine files [Observed]

Files carrying a **second, full MIT notice**, which must be kept if these files or close translations of them are redistributed:

| File | Embedded notice |
|---|---|
| `src/engine/meshlevelset.h:25-50` | Christopher Batty SDFGen, "The MIT License (MIT) Copyright (c) 2015, Christopher Batty" |
| `src/engine/velocityadvector.h:25-50` | same (Batty SDFGen MIT) |
| `src/engine/attributetogridtransfer.h:25-50` | same (Batty SDFGen MIT) |
| `src/engine/levelsetsolver.h:25-49` | Doyub Kim fluid-engine-dev, "The MIT License (MIT) Copyright (c) 2018 Doyub Kim" |

Files with an **attribution only** ("adapted from" / "based on"), with no license text reproduced:

| File:line | Source |
|---|---|
| `fluidsimulation.cpp:7231-7235`, `velocityadvector.cpp:539-541` | APIC transfer, Doyub Kim fluid-engine-dev ([Observed web] MIT) |
| `macvelocityfield.cpp:679` | Doyub Kim "Fluid Engine Development" |
| `pressuresolver.h:25-33` | Batty, Bertails, Bridson variational pressure paper; "Source Code: https://github.com/christopherbatty/Fluid3D" |
| `viscositysolver.cpp:26-33` | "adapted from Christopher Batty's viscosity3d.cpp" (VariationalViscosity3D) |
| `levelsetutils.cpp:26-30` | Batty Fluid3D `levelset_util.cpp`, VariationalViscosity3D `volume_fractions.cpp` |
| `interpolation.cpp:186-187` | Batty VariationalViscosity3D `array3_utils.h` |
| `forcefieldutils.cpp:26-28` | Batty SDFGen `makelevelset3.cpp` |
| `interpolation.cpp:30` | paulinternet.nl bicubic |
| `polygonizer3d.cpp:618-619` | Paul Bourke marching cubes |
| `collision.cpp:29,77,159,346,394` | lighthouse3d, geometrictools, tavianator, nuclex |
| `aabb.cpp:414` | Akenine-Möller tri-box paper |
| `fragmentedvector.h:166-169` | public-domain quicksort |
| `fluidsimulation.cpp:7717,7763` | StackOverflow answer (code posted on StackOverflow carries a CC BY-SA licence; [Inferred] the snippet is short) |
| `meshlevelset.cpp:394` | hlombaert levelset notes |
| `noisegenerationutils.cpp:26-28` | Catlike Coding Unity tutorials (tutorial code; license not stated) |

### 3.3 Upstream license status of Batty repos [Observed via GitHub API, 2026-11-03]

- `api.github.com/repos/christopherbatty/Fluid3D` reports `"license": null`, last pushed 2018-10-18. Its README and file listing show no license statement.
- `api.github.com/repos/christopherbatty/VariationalViscosity3D` reports `"license": null`, last pushed 2018-02-06. No license statement found either.
- `doyubkim/fluid-engine-dev` reports SPDX `MIT`.
- `christopherbatty/SDFGen` returns no API license (404 on `/license`). FLIP Fluids reproduces an MIT notice for it in-source.

**[Inferred] Risk:** FLIP Fluids relicenses as MIT engine files that it says are "adapted from" repos with no published license (Fluid3D, VariationalViscosity3D). The downstream MIT grant can only be as good as the upstream permission. Academic code like this is widely reused, and the risk is generally considered low, but it is a provenance gap. **Mitigation:** for the pressure solver, viscosity solver, volume-fraction and level-set utilities, implement from the published papers (Batty-Bertails-Bridson 2007; Batty-Bridson 2008) rather than translating FLIP Fluids or Batty code line by line.

---

## 4. Conclusions

### 4.1 Can the MIT engine be reused/ported in a separately distributed commercial UE plugin? [Inferred]

**Yes.** MIT permits use, modification, sublicensing and sale in closed-source products. The single condition is that "the above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software" (`LICENSE_MIT.md`). This applies whether we:
- (A) ship a compiled `libffengine` DLL,
- (B) link parts of it, or
- (C) translate substantial portions to C++/HLSL.

### 4.2 Obligations if MIT code (or a close translation) is shipped [Inferred]

1. Ship `ThirdPartyNotices.txt`, in the plugin root and referenced from the `.uplugin`/Fab listing, containing:
   - the FLIP Fluids MIT notice (Guy & Fassbaender);
   - the Batty SDFGen MIT notice and the Doyub Kim MIT notice, if any of the files in 3.2 (or their logic, translated closely) are used;
   - BSD-2 Mega Limited, only if `mingw32_threads` is shipped (it should not be);
   - CC0 pcgsolver: no obligation, attribution optional.
2. In each ported source/shader file, keep a header comment naming the origin file (e.g. `// Derived from Blender-FLIP-Fluids src/engine/pressuresolver.cpp (MIT, (C) 2026 Ryan L. Guy & Dennis Fassbaender)`).
3. Keep the MIT warranty disclaimer in the notice file.
4. If a MinGW-built binary is shipped, add the GCC Runtime Library Exception and winpthreads notices. Legal should confirm.

### 4.3 GPL boundary: what must be avoided

- **[Observed]** All 118 addon `.py` files, the 3 `.in` templates, `blender_manifest.toml` and root `__init__.py` are GPL-3.0-or-later.
- **[Inferred] Do not:**
  - copy, translate, or "port the logic of" any addon file. That includes `src/addon/bake.py` (the reference orchestration that drives the engine), `operators/*`, `properties/*` (parameter defaults and ranges), and `ui/*`.
  - ship addon icons, or the `.blend`/preset assets that live alongside GPL code (the MIT-licensed ones are allowed but useless for UE).
  - reproduce GPL-licensed default-parameter tables.
- **Allowed:** derive engine-call sequences from the **MIT** sources instead:
  - `src/engine/main.cpp`
  - `src/engine/ffengine/*.py`
  - `src/engine/c_bindings/*`
  - the doc comments in `src/engine/fluidsimulation.h`
- [Inferred] Default values that are set inside the MIT engine (e.g. `fluidsimulation.h:2236-2240`, `:2436-2438`) are MIT and fine to reuse.
- **Process recommendation [Inferred]:** engineers writing the UE plugin should be told not to open `src/addon/` while implementing. Anyone who has studied it should not write the corresponding plugin feature without a written spec produced from MIT sources only.

### 4.4 Does a clean-room / algorithmic reimplementation in HLSL carry obligations? [Inferred]

- **Pure algorithmic reimplementation** carries no copyright obligation: written from papers and an understanding of the method, without translating code structure, identifiers or constants line by line. Algorithms and math are not copyrightable. No patents were identified in the source, but none were searched (see Section 6).
- **Close translation** of FLIP Fluids C++ into HLSL/C++ is plausibly a derivative work, so the MIT notice is required. Close translation means mirroring function decomposition, variable names, magic constants, or order of operations.
- **Recommendation:** keep the FLIP Fluids MIT notice (and the Batty/Kim notices for the files in 3.2) in `ThirdPartyNotices.txt` **anyway**, even for the GPU reimplementation. It costs nothing and removes the need to argue clean-room status later. The parallel algorithm-audit agent's notes will also be "derived from reading MIT code".

### 4.5 Unreal Engine EULA / Fab considerations [Inferred — well-known constraints, flag for legal review]

- The UE EULA prohibits combining UE "Licensed Technology" with code under licenses that would require UE code to be disclosed or licensed on copyleft terms. GPL is the canonical example. **GPL code therefore cannot be shipped inside a UE plugin**, nor linked into a UE module in a way that would subject the engine-linked binary to the GPL. This is another reason the addon must be excluded.
- Fab (formerly the UE Marketplace) accepts code plugins with third-party components under permissive licenses (MIT, BSD, zlib, Apache-2.0, MPL-2.0 file-level) when those components are disclosed and their notices are included. MIT engine code and a CC0 solver are compatible.
- **Open questions for Fab specifically:**
  - Does Fab accept a prebuilt **MinGW/GCC-compiled DLL** inside `Source/ThirdParty/` (Strategies A/B), or require third-party code to build with UE's MSVC/clang toolchain?
  - Is the GCC Runtime Library Exception acceptable to Fab's reviewers?
  - Strategy C (native UE C++/HLSL) avoids both questions.

### 4.6 "Standard Royalty Free" scope [Observed]

- `LICENSE_Standard_Royalty_Free.md` forbids resale or redistribution of the "purchased product" without the author's permission. `README.md:49` scopes it to paid-only content ("example scene files, media, artwork, data, and features that rely on this content (ex: Mixbox …) … will not be available on the GitHub project page").
- A grep for "Royalty" finds only `README.md`, the license file itself, and `CMakeLists.txt:295` (which packages the license file).
- **No file in this checkout is marked Standard Royalty Free.** Do not obtain or use paid FLIP Fluids content (paid example scenes, Mixbox.plugin) for this project.

### 4.7 Mixbox [Observed + Inferred]

- `mixbox.h` is labelled non-commercial and carries a commercial certificate that is personal to 1206018 B.C. LTD.
- `mixbox_c.cpp` (MIT) only exposes `Mixbox_initialize` / `Mixbox_is_initialized` wrappers.
- **Exclude the Mixbox feature, `mixbox.h`, `mixbox_stub.cpp`, `mixbox_c.cpp`, and the `FluidSimulation::initializeMixbox` path** (`fluidsimulation.h:262-265`).
- If pigment-style colour mixing is wanted later, license Mixbox directly from Secret Weapons or implement a different colour model.

### 4.8 Incidental observation [Observed]

- The checkout is **not pristine.** `git status` shows `src/addon/utils/installation_utils.py` modified: `SUPPORT_LICENSE_ID` changed from `379LY` to `781MC`.
- This happened because a CMake configure was previously run in `build/` (CMake 4.4.5, MSYS2 MinGW64 `c++.exe`, `Alembic_DIR=C:/msys64/mingw64/lib/cmake/Alembic`). `CMakeLists.txt:157-172` writes configured files back into the source tree, and `:84-88` generates a random support ID.
- Prebuilt DLLs exist in `build/bl_flip_fluids/flip_fluids_addon/ffengine/lib/` (timestamped 2026-11-03 11:57). I did not create them; I only inspected their PE headers.
- The change has no licensing impact.

---

## 5. Recommended handling matrix [Inferred]

| Action | Allowed? | Conditions |
|---|---|---|
| Read MIT engine sources as an algorithm reference | Yes | — |
| Translate MIT engine C++ to UE C++/HLSL | Yes | Keep the MIT notice, plus the Batty/Kim notices for files in 3.2. Prefer paper-based reimplementation for Batty-derived solvers. |
| Ship a compiled `libffengine.dll` (Strategy A/B) | Yes (license-wise) | MIT notice; GCC Runtime Exception notice; confirm Fab acceptance. Build **without** Alembic and Mixbox. |
| Use `pcgsolver/*` | Yes | None (CC0). |
| Use or ship `Eigen/` from this repo | Avoid | Unneeded. If ever used, take Eigen from upstream or from UE's ThirdParty copy. |
| Read, copy, or translate `src/addon/**` | **No** | GPL-3.0-or-later. Also incompatible with the UE EULA. |
| Use Mixbox | **No** | Non-commercial; certificate is not ours. |
| Use paid FLIP Fluids content | **No** | Standard Royalty Free, no redistribution. |
| Use the MIT addon assets (materials, presets, example `.sim`) | Technically yes | Low value. Keep MIT notice. Avoid to keep a clean boundary. |

---

## 6. Unresolved legal questions for independent review

1. **Batty provenance.** Upstream Fluid3D and VariationalViscosity3D publish no license. Is FLIP Fluids' MIT grant over files "adapted from" them (`pressuresolver.*`, `viscositysolver.cpp`, `levelsetutils.cpp`, `interpolation.cpp`) sufficient for commercial redistribution of close translations? Should we require a paper-only reimplementation of those modules?
2. **Derivative-work threshold.** At what level of structural similarity does a GPU (HLSL compute) re-expression of a CPU algorithm become a derivative work? Given 4.4, is retaining the MIT notice sufficient in all cases?
3. **UE EULA × third-party binaries.** Confirm that shipping a GCC/MinGW-compiled DLL (GPLv3 + GCC Runtime Library Exception for statically linked libstdc++/libgcc/libgomp) in a Fab plugin is permitted under the UE EULA and Fab's content guidelines.
4. **Fab technical policy.** Are precompiled non-MSVC third-party DLLs accepted? Are third-party components required to compile from source with UE's toolchain?
5. **Contamination exposure.** The team (and AI agents) have had access to the GPL addon tree. Is an internal "no-addon-reading" rule plus documented provenance sufficient, or is a formal clean-room process advisable for specific features (whitewater, meshing parameters, sheeting)?
6. **Embedded small snippets.** Do the StackOverflow-derived snippet (`fluidsimulation.cpp:7717,7763`; CC BY-SA on StackOverflow) and the Catlike Coding tutorial-derived noise code (`noisegenerationutils.cpp:26-28`) need separate attribution if ported? Recommendation: reimplement those independently.
7. **Patents.** No patent search was performed. Methods such as APIC, variational pressure/viscosity, and particle-based whitewater are published academic methods, but freedom-to-operate for a commercial product was not assessed.
8. **Trademark.** "FLIP Fluids" is the authors' product name. The plugin's name and marketing should not imply affiliation. Is a nominative "inspired by / derived from" statement in the notices acceptable?
9. **Mixbox header.** Confirm that `mixbox.h` (non-commercial) is fully excluded and that the MIT `mixbox_c.cpp` API wrapper carries no residual obligation if dropped.
