# RENDERING_ARCHITECTURE.md — Liquid Surface Rendering in UE 5.8

**[Impl]** built and run in the prototype · **[Meas]** measured · **[Design]** proposed · **[Lit]** literature.

## 1. Decision

**Prototype: screen-space fluid rendering (SSFR)**, compute-only, injected through the renderer's public
post-opaque delegate. **Production target: SSFR first, with a world-space surface (GPU marching cubes
over a particle-splatted SDF) added in Phase 3** for shadows, multiple cameras, ray tracing and Lumen.

Reasons: SSFR cost scales with screen pixels, not particle or grid count. It needs no vertex factory or
primitive scene proxy. It costs 0.67 ms at 1280×720 with 262k particles **[Meas]**. It keeps rendering a
pure consumer of simulation output.

## 2. Integration point [Impl]

- `IRendererModule::RegisterPostOpaqueRenderDelegate` (`RenderCore/Public/RendererInterface.h:329`).
  It is called from `FRendererModule::RenderPostOpaqueExtensions` (`Renderer/Private/SceneRendering.cpp:5624`)
  once per view, after opaque lighting and fog and **before translucency**
  (`DeferredShadingRenderer.cpp:3925`).
- `FPostOpaqueRenderParameters` (`RendererInterface.h:85`) supplies `ColorTexture` (scene colour target),
  `DepthTexture`, `ViewportRect`, `ViewMatrix`, `ProjMatrix` and the `GraphBuilder`. All of it is public.
- To filter by world and read the device-Z transform and pre-view translation, the opaque
  `const FViewInfo* View` is reinterpreted as `const FSceneView*`. `FViewInfo`'s only base is
  `FSceneView` (`Renderer/Private/SceneRendering.h:1254`). This depends on a private class layout
  and is flagged in `RISK_REGISTER.md` (R-13). The alternative is `FSceneViewExtensionBase::PrePostProcessPass_RenderThread`,
  which needs `Renderer/Internal/PostProcess/PostProcessInputs.h`. That means `bTreatAsEngineModule`,
  and it runs after translucency (UE58_INTEGRATION.md).
- Rejected: Niagara as the core path (the brief forbids it as a dependency). A custom `FPrimitiveSceneProxy`
  with a vertex factory is the Phase 3 mesh path.

## 3. SSFR pipeline [Impl] (`Shaders/Private/SloshRender.usf`)

| Pass | Work | Cost @1280×720, 262k particles [Meas] |
|---|---|---|
| Clear | R32_UINT depth (to 0xFFFFFFFF) and thickness | 0.013 ms |
| `SplatParticlesCS` | Per particle: project and splat a view-facing sphere. `InterlockedMin` on float-as-uint view depth, with scene-depth occlusion test. `InterlockedAdd` of fixed-point thickness | 0.560 ms |
| `ResolveCS` | uint → float depth/thickness | 0.017 ms |
| `SmoothDepthCS` ×2 | Separable bilateral filter (Gaussian × depth range) | 0.038 ms |
| `CompositeCS` | Normal from one-sided depth differences, Schlick Fresnel, sky-gradient reflection, refraction by offsetting the scene colour lookup, Beer–Lambert absorption + scatter tint, Blinn-style specular from the scene's directional light | 0.019 ms |
| Copy | Composite → SceneColor (view rect only) | 0.021 ms |

Splat cost dominates. It scales with particles × projected pixel area, so it gets more expensive near the camera.
A rasterised point-sprite path with hardware depth test is the Phase 2 optimisation [Design].

## 4. Feature coverage

| Feature | Status | Notes |
|---|---|---|
| Depth/thickness reconstruction | [Impl] | sphere splat, van der Laan et al. 2009 |
| Surface smoothing | [Impl] | bilateral. Curvature flow or narrow-range filter (Truong & Yuksel 2018) planned |
| Normals | [Impl] | screen-space |
| Refraction | [Impl] | screen-space offset; falls back to unrefracted when the sample is in front of the liquid |
| Reflection | [Impl] partial | analytic sky gradient only. SSR / reflection captures / Lumen not used |
| Transparency + absorption | [Impl] | Beer–Lambert per RGB from the visual material |
| Foam / bubbles / spray | [Design] | diffuse particles (FLIP Fluids' `diffuseparticlesimulation.cpp` is the reference); render as additive sprites |
| Motion vectors | [Design] | none; TSR may smear fast flow. Write velocity into the GBuffer velocity target or draw before TSR with reprojection |
| Shadows cast by the liquid | [Design] | needs world-space surface or a shadow-depth splat pass |
| Shadows received | partial | refraction shows scene shadows; the liquid surface itself is unshadowed |
| Underwater rendering | [Design] | needs a camera-inside test against the liquid SDF |
| Multiple viewpoints | [Impl] | delegate runs per view; textures are transient per view |
| Translucent objects | limitation | liquid composites before translucency; translucents draw over it unsorted |
| MSAA / forward | unsupported | early-out when `NumSamples > 1` |
| Mobile | unsupported | post-opaque path uses deferred scene textures |

## 5. Is screen space sufficient for the first prototype?

Yes for interactive gameplay visuals at typical camera distances; this is now demonstrated in the
auto-test screenshot (`SloshDemo/Saved/Screenshots/WindowsEditor/HighresScreenshot00000.png`).
It is not sufficient for: liquid shadows, off-screen reflections/refraction, ray-traced reflections of
the liquid, liquid seen in reflections/scene captures with correct depth, or physics-accurate surface
queries (those must use the grid, never the renderer).

## 6. World-space surface plan [Design]

1. Splat particles into a narrow-band SDF on the sim grid (or 2× grid), with Zhu–Bridson style smoothing.
   FLIP Fluids does the equivalent on the CPU in `particlemesher.cpp` / `polygonizer3d.cpp`.
2. GPU marching cubes over active bricks → vertex/index buffers with indirect draw args.
3. Render via a `UPrimitiveComponent` + `FPrimitiveSceneProxy` using a custom vertex factory or
   `FLocalVertexFactory` with GPU-written buffers. Use a translucent `UMaterialInterface` from the visual asset.
4. Budget [Est]: 0.5–1.5 ms at 64³ for SDF + MC; validated in Experiment 6b.
