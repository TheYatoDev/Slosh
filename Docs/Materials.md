# Liquid Materials and Presets

This guide covers how Slosh describes a liquid, how a volume decides which description to use, what each parameter does to the simulation and the image, how the Fluid Painter presets were turned into simulation materials, and a cookbook of starting values for common liquids.

Related pages: [Reference §4](./Reference.md#4-usloshmaterialasset-fsloshphysicalproperties-fsloshvisualproperties) (property tables), [Simulation](./Simulation.md) (solver), [Rendering](./Rendering.md) (surface modes and shading), [Slosh Paint](./SloshPaint.md) (painting with presets). Design background: `Research/LIQUID_MATERIALS.md`.

---

## 1. Physical and visual are separate

A liquid type is a `USloshMaterialAsset` (`SloshMaterialAsset.h`), a `UDataAsset` holding two structs:

| Struct | Read by | Module | Effect |
|---|---|---|---|
| `FSloshPhysicalProperties Physical` | Solver only, through `ASloshVolume::BuildSolverSettings` into `FSloshSolverSettings` | `SloshCore` | How the liquid moves |
| `FSloshVisualProperties Visual` | Renderer only, through `ASloshVolume::UpdateRenderRegistration` into `FSloshRenderSettings` | `SloshRender` | How it looks |

The separation is enforced by module boundaries: `SloshCore` doesn't depend on `SloshRender`, and the renderer never sees physical values. In practice:
- You can tune the look of a running simulation without changing its motion, and the other way round.
- "Water that looks like honey" or "honey that looks like water" is two independent edits.
- The renderer doesn't know the viscosity, so a thick liquid needs its *visual* smoothing set explicitly (§5.2).

---

## 2. How a volume picks its liquid

`ASloshVolume` has three liquid properties in category `Slosh`:

| Property | Type | EditCondition |
|---|---|---|
| `Material` | `USloshMaterialAsset*` | — |
| `Physical` | `FSloshPhysicalProperties` (inline) | `Material == nullptr` |
| `Visual` | `FSloshVisualProperties` (inline) | `Material == nullptr` |

The resolution rule (`SloshVolume.h:310-311`):

```cpp
const FSloshPhysicalProperties& GetPhysical() const { return Material ? Material->Physical : Physical; }
const FSloshVisualProperties&   GetVisual()   const { return Material ? Material->Visual   : Visual;   }
```

- **All or nothing.** An assigned asset replaces *both* structs. There is no per-field override. To change one value, duplicate the asset.
- **The inline values are kept but ignored** while `Material` is set (they show greyed out). Clearing `Material` brings them back.
- **Live.** Both structs are read every tick, so editing the asset or the inline values during PIE changes the simulation and the image on the next frame. Several volumes can share one asset, and editing it changes them all.
- **Mesh surface mode** also takes the surface's UE material from the asset when the asset is a Fluid Painter preset and `SurfaceMaterial` is empty (`ASloshVolume::GetEffectiveSurfaceMaterial`).

### The classic default: inline water

A newly placed volume with no `Material` uses the struct defaults. These defaults are the **classic water look**, restored on purpose because the viscous-liquid render settings over-smoothed water and removed its ripples:

| Visual field | Classic default (inline) | Viscous look (all presets) |
|---|---|---|
| `SmoothingRadius` | 7 px | 24 px |
| `SmoothingScale` | 0 (fixed-pixel filter) | 4 splat radii |
| `SmoothingIterations` | 1 | 3 |
| `Anisotropy` | 0 (round splats) | 0.85 |
| `AnisotropyMaxStretch` / `AnisotropySmoothing` | 4 / 0.5 | 4 / 0.5 |
| `EdgeFadeThickness` | 0 cm | 1.5 cm |
| `ScatterLighting` | 0 | 1 |
| `ParticleRadiusScale` | 0.6 | 0.9 |
| `AbsorptionPerMeter` | (4.0, 1.2, 0.8) | derived per preset (§4) |
| `ScatterColor` | (0.02, 0.08, 0.10) | derived per preset |
| `RefractionStrength` | 0.02 | derived per preset |
| `FresnelF0` | 0.02 | derived per preset |
| `SpecularPower` | 400 | derived per preset |

Physical defaults: `Density 1000`, `KinematicViscosity 0`, `Transfer APIC`, `FlipRatio 0.95`, `BoundaryFriction 0`, `Adhesion 0`, `AdhesionDistance 2.5`, `DensityCorrection 0.15`.

---

## 3. Physical properties in depth

| Property | Default | Range / units | What it really does | Tuning notes |
|---|---|---|---|---|
| `Density` | 1000 | ≥ 1 kg/m³ | Converts solver pressure to pascals (`PressureToPascal = Density · h / dt`) and sets buoyancy in `BuoyancyAndDrag` mode. | In a single-liquid domain the liquid's own flow doesn't depend on density. It only decides what floats and how hard the liquid pushes bodies. Use real values. |
| `KinematicViscosity` | 0 | ≥ 0 m²/s | Implicit viscosity, solved with `ViscosityIterations` Jacobi sweeps (volume setting, default 12). | The grid's numerical viscosity already exceeds water's 1e-6, so ν only becomes visible once ν·dt/h² ≳ 0.05. That works out to about 3e-2 at 5 cm cells, about 8e-3 at 2.5 cm and about 3e-3 at 1.6 cm (at dt ≈ 1/240 s). Above about 0.1, raise `ViscosityIterations` to 30 to 50, because 12 sweeps under-damp a stiff solve and the liquid looks thinner than its value. |
| `Transfer` | APIC | APIC / FLIP | APIC: low noise, conserves angular momentum, slightly damped. FLIP: livelier and noisier. | Use APIC for everything viscous and for gameplay water. Use FLIP 0.95 to 0.98 for showy splashes. |
| `FlipRatio` | 0.95 | 0..1 (FLIP only) | PIC/FLIP blend. | Lower values (0.8 to 0.9) calm FLIP noise. |
| `BoundaryFriction` | 0 | 0..1 | At collider contact, removes that fraction of the tangential relative velocity of penetrating particles. At **domain walls** it multiplies tangential velocity by (1 − friction) every step. | It is what makes a viscous liquid pool on the floor instead of sliding out into a one-particle layer. Values of 0.8 or more read as "sticky". It applies per step, so even 0.2 is noticeable on walls. |
| `Adhesion` | 0 | 0..2 (1 = 1.5 g) | Within `ParticleRadius + AdhesionDistance` of a **collider** (not the domain walls), slows particles moving away from the surface and adds a small approach velocity, with linear falloff. It never pushes particles into the body faster than one substep can absorb. | Makes coats and drips cling to bodies and overhangs. At 1 a film holds on an overhang against gravity; 2 is glue. Pair it with `BoundaryFriction`, because adhesion without friction slides off. |
| `AdhesionDistance` | 2.5 | 0.1..20 cm | Range of adhesion beyond the particle radius (0.25 cells). | About one cell. Larger values pull distant droplets onto the body unnaturally. |
| `DensityCorrection` | 0.15 | 0..1 | Strength of the **velocity-based** density correction. | **Ignored while the volume's `bTwoSidedDensityProjection` is on (the default):** the proxy sets it to 0 (`SloshProxy.cpp:421`). Volume is then held by the position-based projection (`DensityRelaxation`, `DensityProjectionInterval` on the volume). The value only matters if you turn the two-sided projection off. |

Volume-level settings that interact with the liquid, from [Reference §3.4](./Reference.md#34-properties-solver-sloshsolver):
- `ViscosityIterations` (thick liquids).
- `CellSize` (film thickness; see §5.3).
- `MaxParticleSpeed`.
- `CFL` (higher values cost less but splash and couple less accurately).

What the physical model does **not** have yet (Research/LIQUID_MATERIALS.md §4-5):
- Surface tension and cohesion.
- Shear thinning or yield stress (Bingham, Herschel–Bulkley).
- Viscoelastic strings.
- Several liquids in one domain.

Honey works well with high Newtonian viscosity. Slime approximated this way looks like thick honey, not stretchy slime.

---

## 4. Every Fluid Painter preset is a Slosh material

`USloshPaintPreset` (`SloshPaintPreset.h`) **derives from `USloshMaterialAsset`**. Each of the 43 presets in `/Slosh/Paint/Presets` therefore carries, alongside its painting parameters (thickness, drops, layers, paint `Material`), a full `Physical` and `Visual` tuned to match its painted look. Assign any preset to a volume's `Material` slot and the simulated liquid behaves and looks like that preset. In `Mesh` surface mode it is drawn with the preset's paint material as well.

Folders:
- `Presets/Regular`: 34 presets (water, milk, blood, oils, syrups, honey, sauces, chocolate, slime, mud, wax, metals, soap, foam, and more).
- `Presets/NSFW`: a separate folder of presets flagged `bAdultContent = true`, kept apart so it can be excluded from a build.
- `Presets/Layers/Materials`: bubble and sugar instance materials used by preset layers.

### 4.1 How the preset simulation values were derived

The values were generated once by an editor script (run through the Unreal MCP server; see [Testing §7](./Testing.md#7-optional-authoring-editor-content-through-the-unreal-mcp-server)) and saved into the assets.

**Physical values from the liquid type.** The preset name is matched against a keyword table; the first match wins. Every preset uses `Transfer = APIC`, `AdhesionDistance = 2.5`, `DensityCorrection = 0.15`.

| Keyword | Density | ν (m²/s) | BoundaryFriction | Adhesion | Class |
|---|---|---|---|---|---|
| WaterDrops, Water | 1000 | 1e-6 | 0.05 | 0.15 | thin |
| Milk | 1030 | 3e-6 | 0.10 | 0.20 | thin |
| Blood | 1060 | 4e-6 | 0.20 | 0.30 | thin |
| Oil | 900 | 1e-4 | 0.20 | 0.30 | thin, slick |
| OilCrude | 950 | 5e-3 | 0.40 | 0.50 | medium |
| Foam | 400 | 2e-3 | 0.50 | 0.60 | light, medium |
| Soap | 1020 | 2e-3 | 0.40 | 0.50 | medium |
| Lube | 1050 | 5e-3 | 0.30 | 0.70 | medium, clingy |
| MapleSyrup | 1330 | 5e-3 | 0.70 | 0.60 | medium-thick |
| Glaze | 1300 | 1e-2 | 0.80 | 0.80 | thick |
| Ketchup | 1100 | 2e-2 | 0.90 | 0.70 | thick |
| Chocolate | 1250 | 2e-2 | 0.90 | 0.80 | thick |
| Mud | 1600 | 2e-2 | 0.80 | 0.60 | thick, heavy |
| Honey | 1420 | 3e-2 | 0.95 | 1.00 | thick, sticky |
| Mustard | 1050 | 3e-2 | 0.90 | 0.80 | thick |
| Slime | 1050 | 3e-2 | 0.80 | 1.00 | thick, sticky |
| Mayo | 910 | 5e-2 | 1.00 | 0.90 | very thick |
| Wax | 900 | 1e-1 | 1.00 | 1.00 | near-solid |
| Gummy | 1200 | 2e-1 | 1.00 | 1.00 | near-solid |
| Metal | 6000 | 5e-7 | 0.10 | 0.00 | heavy, runny, non-wetting |
| *(no keyword: Default, Biscuit, Glitter, …)* | 1050 | 5e-3 | 0.60 | 0.50 | medium |

The list order matters: `OilCrude` is checked before `Oil`, and `MapleSyrup` doesn't match anything earlier. "Plus" and "Anim" variants inherit their base liquid's values.

**Visual values from each preset's paint material.** The script read `BaseColor`, `Opacity`, `Roughness`, `IOR` and `Metallic` from the preset's material instance, then computed:

| Visual field | Formula | Reasoning |
|---|---|---|
| (input clamps) | colour channels to [0.02, 1]; opacity to [0, 1]; roughness to [0.03, 1]; IOR ≥ 1; metallic to [0, 1] | Avoids `ln(0)` and infinite specular. |
| depth `d` | `d = 0.12 m + (0.01 − 0.12) · opacity²`, so 0.12 m (clear) down to 0.01 m (opaque) | The depth at which the liquid reaches its base colour: clear liquids tint over about 12 cm, opaque ones within about 1 cm. |
| `AbsorptionPerMeter` | `−ln(colour) / d` per channel, capped at 400 | Beer–Lambert: transmittance `exp(−σ·d) = colour` at depth `d`. |
| `ScatterColor` | `colour · (0.25 + 0.75 · opacity²)` | Opaque liquids show their base colour as diffuse body colour. Clear ones show only a faint tint. |
| `FresnelF0` | `((IOR − 1)/(IOR + 1))²`, then lerped toward 0.8 by `Metallic` | Standard dielectric reflectance; metals become mirror-like. |
| `SpecularPower` | `clamp(2 / roughness⁴ − 2, 20, 300)` | Blinn-Phong equivalent of GGX roughness, capped at 300, so glossy presets all land at 300. |
| `RefractionStrength` | `0.03 · (1 − opacity) + 0.004` | Clear liquids refract, opaque ones barely. |
| `ParticleRadiusScale` | 0.9 | Fuller splats for coats and films. |
| `SmoothingRadius` / `SmoothingScale` / `SmoothingIterations` | 24 / 4 / 3 | The "viscous look": depth-adaptive, slope-aware smoothing. |
| `Anisotropy` | 0.85 | Ellipsoid splats, so films render as sheets. |
| `EdgeFadeThickness` | 1.5 cm | Hides stray particles and softens film edges. |
| `ScatterLighting` | 1 | Lit body colour keeps thick, opaque liquids readable in shape. |

The viscous render values are stored **explicitly in every preset**, so presets look the same no matter what the struct defaults are.

To regenerate or retune presets in bulk, apply the same formulas to the material parameters and write `Physical`/`Visual` with `set_properties` (see [Testing §7](./Testing.md#7-optional-authoring-editor-content-through-the-unreal-mcp-server)). For one-off tweaks, edit the preset asset in the details panel. Its `PostEditChangeProperty` broadcasts `OnPresetChanged`, so painted strokes rebuild and volumes pick up the change on the next tick.

---

## 5. Visual properties in depth

### 5.1 Colour and optics (Splats and Raymarched)

| Property | What to change it for |
|---|---|
| `AbsorptionPerMeter` (1/m, RGB) | The **colour seen through depth**. A channel's transmittance after `x` metres is `exp(−σ·x)`. Water's (4, 1.2, 0.8) removes red fastest, so deep water turns blue-green. For a target colour `c` reached at depth `d`, use `σ = −ln(c)/d`. Values in the hundreds make the liquid opaque within millimetres. |
| `ScatterColor` | The colour a **thick** liquid tends toward: milk, paint, sauces. Keep it near black for clear liquids, or they look milky. |
| `ScatterLighting` (0..1) | 0 gives flat scatter (the classic look). 1 gives wrapped-diffuse lit scatter, which shows the shape of opaque liquids. Water looks better at 0. |
| `RefractionStrength` | Screen-space refraction offset. 0.02 for water, 0.004 or less for opaque liquids. Off-screen content can't be refracted. |
| `FresnelF0` | Reflectance at normal incidence: water 0.02, oils and syrups 0.035 to 0.04, metals 0.6 to 0.9. |
| `SpecularPower` | Sun highlight tightness: 300 to 400 glossy, 20 to 50 matte (mud, paste). The highlight comes from `SunLight` (auto-detected directional light). |

### 5.2 Shape of the surface

| Property | Splats | Raymarched / Mesh | Guidance |
|---|---|---|---|
| `ParticleRadiusScale` | Splat radius (× CellSize) | Field kernel radius | 0.6 for water and 0.9 for coats. Larger values are fatter and blobbier. |
| `SmoothingRadius` | Depth-filter cap, px | — | 7 keeps ripples. 24 smooths close-ups of viscous liquid. |
| `SmoothingScale` | 0 = fixed-pixel filter; above 0 = depth-adaptive, slope-aware filter plus thickness blur | — | Leave at 0 for water. 3 to 4 suits honey, slime and paint. |
| `SmoothingIterations` | Repeat passes | — | 1 for water, 2 to 3 for viscous liquids. |
| `Anisotropy`, `AnisotropyMaxStretch`, `AnisotropySmoothing` | Ellipsoid splats | Ellipsoids feed the field | 0.85 / 4 / 0.5 make films into sheets and streams into rods. Costs about 0.7 ms at 250k particles. Spray stays round (fewer than 35 weighted neighbours). |
| `EdgeFadeThickness` | Thin-liquid fade, cm | Same | 0 for crisp water edges, 1 to 2 for coats. |

Settings that are **not** in the material but shape the surface (volume, `Slosh|Surface`): `SurfaceMode`, `FieldResolution`, `SurfaceSmoothing`, `SurfaceIsoLevel`, `SurfaceMaterial`. See [Rendering](./Rendering.md).

### 5.3 Resolution is part of the material

A "blobby" or "beaded" look is mostly a **cell-size** problem, not a render-setting problem. Films are 1 to 2 particles thick, so at 2.5 cm cells a coat on a body breaks into beads, while at 1.6 cm it becomes continuous (about 9 ms per frame in a character-pour test). Before raising smoothing, try a smaller `CellSize`. Heavy smoothing on coarse liquid gives melted, wobbly surfaces, and on water it removes the ripples.

---

## 6. Tuning cookbook

The physical columns follow the preset keyword table where one exists, otherwise Research/LIQUID_MATERIALS.md. The visual columns apply the §4.1 formulas to a typical base colour and opacity, so they match what a preset of that colour would get. Rows with ν at or above about 1e-2 need small cells to show it (§3). All rows use APIC unless noted.

| Liquid | Density | ν (m²/s) | Friction | Adhesion | Absorption /m (RGB) | ScatterColor | F0 | Spec | Render look | Cell | Why |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Water (clear) | 1000 | 0 | 0 | 0 | (4.0, 1.2, 0.8) | (0.02, 0.08, 0.10) | 0.02 | 400 | classic (7/0/1, aniso 0, fade 0, SL 0) | 2.5 to 5 cm | Struct defaults. Ripples need the fixed-pixel filter. FLIP 0.95 for splashier water. |
| Muddy / flood water | 1000 | 1e-6 | 0.05 | 0.15 | (5.4, 7.3, 10.9) | (0.18, 0.14, 0.10) | 0.02 | 300 | classic | 2.5 to 5 cm | Brown tint over about 11 cm. Still thin and splashy. |
| Milk | 1030 | 3e-6 | 0.10 | 0.20 | (4.0, 5.1, 7.8) | (0.85, 0.83, 0.79) | 0.02 | 300 | classic, SL 1 | 2.5 cm | Opaque white body comes from scatter, not absorption. Lit scatter shows the shape. |
| Blood | 1060 | 4e-6 (real) to 5e-3 (gameplay) | 0.2 to 0.4 | 0.3 | (12, 59, 59) | (0.28, 0.01, 0.01) | 0.02 | 300 | classic; viscous look for pools | 1.6 to 2.5 cm | Red passes and green/blue are absorbed within centimetres. Raise ν and friction for a thicker, staining feel. |
| Cooking oil | 900 | 1e-4 (try 5e-3 to see it) | 0.2 | 0.3 | (1.4, 3.0, 13.7) | (0.23, 0.19, 0.05) | 0.036 | 300 | classic | 2.5 cm | Clear golden. Higher IOR gives slightly stronger reflections. Floats on nothing, because Slosh is single-liquid. |
| Crude oil | 950 | 5e-3 | 0.4 | 0.5 | (350, 370, 390) | (0.03, 0.025, 0.02) | 0.04 | 300 | viscous look, SL 0.3 | 2.5 cm | Opaque black with a glossy highlight. Keep scatter near black. |
| Maple syrup | 1330 | 5e-3 | 0.7 | 0.6 | (5.6, 13, 28) | (0.19, 0.085, 0.017) | 0.039 | 300 | viscous look | 1.6 to 2.5 cm | Amber through depth, dark when deep. Flows but sticks to surfaces. |
| Honey | 1420 | 3e-2 | 0.95 | 1.0 | (1.7, 7.1, 26) formula, or (6, 18, 60) hand-tuned | (0.34, 0.20, 0.03) or (0.35, 0.17, 0.02) | 0.04 | 250 to 300 | viscous look | 1.6 cm | Slow, coiling, coats bodies. The hand-tuned set is the `-SloshSkelTest` default and reads deeper orange. Raise `ViscosityIterations` toward 30. |
| Ketchup | 1100 | 2e-2 | 0.9 | 0.7 | (60, 322, 391) | (0.55, 0.04, 0.02) | 0.028 | 300 | viscous look | 1.6 to 2.5 cm | Opaque red paste. Shape reads through lit scatter. |
| Slime | 1050 to 1100 | 3e-2 to 0.3 | 0.8 | 1.0 | (13, 1.8, 20.5) | (0.13, 0.37, 0.07) | 0.025 | 300 | viscous look | 1.6 to 2.5 cm | Translucent green. Without elasticity it behaves like thick honey (no strings). |
| Mud | 1600 | 2e-2 | 0.8 | 0.6 | (139, 177, 230) | (0.25, 0.17, 0.10) | 0.02 | 20 to 50 | viscous look, SL 1 | 2.5 cm | Heavy, opaque, matte. Low specular sells it. |
| Wax / gummy (near-solid) | 900 to 1200 | 0.1 to 0.2 | 1.0 | 1.0 | from colour | from colour | 0.03 | 100 to 300 | viscous look | 1.6 cm | Barely flows. Needs `ViscosityIterations` of 40 or more, or it looks runnier than intended. |
| Liquid metal | 6000 (mercury: 13,534) | 5e-7 | 0.1 | 0 | (10, 10, 8) | (0.90, 0.90, 0.92) | 0.8 | 300 | classic, SL 0 | 2.5 cm | Runny, non-wetting (no adhesion), mirror-like Fresnel. Heavy bodies float in it. |

Notes:
- **"Render look"** refers to the two bundles in §2: *classic* is `7/0/1, Anisotropy 0, EdgeFade 0, ScatterLighting 0, radius 0.6`, and *viscous look* is `24/4/3, Anisotropy 0.85, EdgeFade 1.5, ScatterLighting 1, radius 0.9`.
- **Surface mode.** Water usually looks best as Splats. Honey, slime and coats look best as Raymarched, or as Mesh when engine lighting and shadows are needed (keep `SurfaceIsoLevel` at 0.5).
- **Shortcut.** For any row that matches a Regular preset (Milk, BloodPlus, OilPlus, OilCrudePlus, MapleSyrup, Honey, Ketchup, Slime, MudPlus, Wax, Gummy, MetalSilver), assign the preset instead of typing values.

### Quick fixes by symptom

| The liquid looks… | Change |
|---|---|
| Too thin, splashes like water although ν is high | Smaller `CellSize`; raise `ViscosityIterations`; raise ν further (§3 threshold). |
| Slides off bodies | `BoundaryFriction` 0.8 to 1 **and** `Adhesion` 0.6 to 1. |
| Spreads into a single sparse layer on the floor | Raise `BoundaryFriction` (it applies at domain walls too). |
| Milky when it should be clear | Lower `ScatterColor`; set `ScatterLighting` to 0. |
| Too dark in deep areas | Lower `AbsorptionPerMeter` (or derive it with a larger depth `d`). |
| Beady thin films | Smaller `CellSize` first. Then `Anisotropy` 0.85, `EdgeFadeThickness` 1 to 2, or Raymarched mode. |
| Water lost its ripples, or looks melted | Return to the classic look: `SmoothingScale` 0, `SmoothingRadius` 7, `SmoothingIterations` 1. |
