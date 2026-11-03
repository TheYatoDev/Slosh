# UE 5.8 Rendering / RHI / Threading / Plugin Integration (verified)

Verified against the installed engine: `C:\Program Files\Epic Games\UE_5.8` (Build.version 5.8.4, CL 58902052, `++UE5+Release-5.8`).
Unless noted, paths are relative to `Engine/Source/Runtime/`. Line numbers come from that install.

> **Physics integration: see UE58_INTEGRATION_PHYSICS.md**

Legend: **[PUBLIC]** = public header, safe for a project plugin. **[INTERNAL]** = `Module/Internal/`, which a *project* plugin cannot include (see section 10). **[PRIVATE]** = `Module/Private/`. Avoid it. **[DEPRECATED-5.8]** = compiles to a deprecation warning that does nothing.

---

## 0. Things that changed and break 5.0–5.3-era code

| Old habit | 5.8 reality | Source |
|---|---|---|
| `RDG_GPU_STAT_SCOPE(GraphBuilder, Stat)` | **[DEPRECATED-5.8]**, does nothing. Use `RDG_EVENT_SCOPE_STAT(GraphBuilder, Stat, "Name")` | `RenderCore/Public/RenderGraphEvent.h:520`, `:480` |
| `SCOPED_GPU_STAT(RHICmdList, Stat)` | **[DEPRECATED-5.8]**, does nothing. Use `RHI_BREADCRUMB_EVENT_STAT` | `RenderCore/Public/ProfilingDebugging/RealtimeGPUProfiler.h:154`, `RHI/Public/RHIBreadcrumbs.h:1302` |
| `FComputeShaderUtils` is a struct | It is a **namespace** | `RenderCore/Public/RenderGraphUtils.h:333` |
| Pass lambda `[](FRHIComputeCommandList&)` | Still accepted. Engine helpers use the `(FRDGAsyncTask, FRHIComputeCommandList&)` form | `RenderCore/Public/RenderGraphPass.h:~580-640` (TLambdaTraits), `RenderGraphDefinitions.h:107` |
| `FRHIGPUMemoryReadback::Lock/Unlock` | Base-class versions are deprecated. Call the `FRHIGPUBufferReadback` versions | `RHI/Public/RHIGPUReadback.h:100,109,139-140` |
| `ISceneViewExtension::SetupViewFamily` etc. pure virtual | All now have `{}` default bodies, so you override only what you need | `Engine/Public/SceneViewExtension.h:145-165` |
| `SubscribeToPostProcessingPass(Pass, Callbacks, bEnabled)` | Deprecated in 5.5. New signature also takes `const FSceneView&` | `SceneViewExtension.h:222,252` |
| `FVector`/`FMatrix` in shader params | Use `FVector3f`, `FVector4f`, `FMatrix44f`. Double variants are rejected | `RenderCore/Public/ShaderParameterMacros.h:82,939,1115` |
| Doc comment shows `FRDGBuilder GraphBuilder(RDG_EVENT_NAME(...), Flags)` | That comment is out of date. The real ctor takes the `RHICmdList` first | `RenderGraphDefinitions.h:85` vs `RenderGraphBuilder.h:66` |

---

## 1. Plugin shader directories

- `RenderCore/Public/ShaderCore.h:1654`
  `extern RENDERCORE_API void AddShaderSourceDirectoryMapping(const FString& VirtualShaderDirectory, const FString& RealShaderDirectory);`
- Checks run in `RenderCore/Private/ShaderCore.cpp:4218-4250`:
  - It must be called on the **game thread**.
  - The virtual path must start with `/`, must **not** end with `/`, and must not contain `.`.
  - Each virtual path can be mapped only once.
  - The real directory must exist.
  - The real directory must not contain a `Generated` subfolder.
  - In cooked builds, or when `!AllowShaderCompiling()`, the call is a no-op. That is harmless.
- `Projects/Public/Interfaces/IPluginManager.h:667` `static PROJECTS_API IPluginManager& Get();` · `:393` `virtual TSharedPtr<IPlugin> FindPlugin(const FStringView Name) = 0;` · `:140` `virtual FString GetBaseDir() const = 0;`
- Loading phase: `ELoadingPhase::PostConfigInit` (`Projects/Public/ModuleDescriptor.h:32`). The mapping must exist before global shaders compile, so the module that calls it must be `PostConfigInit`.
- Engine convention for plugins is `/Plugin/<Name>`. Examples: `/Plugin/Runtime/HairStrands` in `Engine/Plugins/Runtime/HairStrands/Source/HairStrandsCore/Private/HairStrandsCore.cpp:33`, and `/Plugin/FX/Niagara` in `Engine/Plugins/FX/Niagara/Source/NiagaraShader/Private/NiagaraShaderModule.cpp:19`.
- Reference example: `Engine/Plugins/Experimental/ColorCorrectRegions/Source/ColorCorrectRegions/Private/ColorCorrectRegionsModule.cpp:19-23`. Its .uplugin line 21 is `"LoadingPhase": "PostConfigInit"`.

```cpp
// FluidSimShaders module (uplugin: "LoadingPhase": "PostConfigInit")
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"
void FFluidSimShadersModule::StartupModule()
{
    const FString Dir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("FluidSim"))->GetBaseDir(), TEXT("Shaders"));
    AddShaderSourceDirectoryMapping(TEXT("/Plugin/FluidSim"), Dir);
}
```

Recommendation: use a separate light `PostConfigInit` module (for example `FluidSimShaders`) that holds the mapping and the shader classes. Keep the gameplay and runtime module at `Default`, because `PostConfigInit` modules load before most of the engine exists.

---

## 2. Global compute shaders

| Symbol | Location | 5.8 form |
|---|---|---|
| `FGlobalShader` | `RenderCore/Public/GlobalShader.h:282` | default `ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters&, FShaderCompilerEnvironment&)` at `:303` |
| `FGlobalShaderPermutationParameters` | `GlobalShader.h:72` | members: `GlobalShaderName`, plus base `Platform`, `PermutationId`, `Flags` (`EShaderPermutationFlags`, `ShaderPermutation.h:20`) |
| `DECLARE_GLOBAL_SHADER(C)` | `GlobalShader.h:431` | `DECLARE_SHADER_TYPE(C, Global)` |
| `IMPLEMENT_GLOBAL_SHADER(C, "/Plugin/..usf", "MainCS", SF_Compute)` | `GlobalShader.h:433` | |
| `SHADER_USE_PARAMETER_STRUCT(C, FGlobalShader)` | `RenderCore/Public/ShaderParameterStruct.h:62` | |
| `BEGIN_SHADER_PARAMETER_STRUCT(Name, DllStorage)` / `END_SHADER_PARAMETER_STRUCT()` | `ShaderParameterMacros.h:1411` / `:1414` | |
| `SHADER_PARAMETER(Type, Name)` | `:1613` | allowed types include `float`, `uint32`, `int32`, `FVector3f`, `FVector4f`, `FIntVector`, `FUintVector3`, `FMatrix44f` (`:923-1115`) |
| `SHADER_PARAMETER_RDG_BUFFER_SRV(HLSLType, Name)` | `:1745` | |
| `SHADER_PARAMETER_RDG_BUFFER_UAV(HLSLType, Name)` | `:1757` | |
| `SHADER_PARAMETER_RDG_TEXTURE(HLSLType, Name)` | `:1697` | |
| `SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, Name)` | `:1733` | |
| `SHADER_PARAMETER_RDG_UNIFORM_BUFFER(Struct, Name)` | `:1768` | |
| `SHADER_PARAMETER_STRUCT_REF(Struct, Name)` | `:1854` | |
| `RDG_BUFFER_ACCESS(Name, ERHIAccess)` | `:1868` | for indirect-args buffers in raster passes |
| `RENDER_TARGET_BINDING_SLOTS()` | `:1900` | |
| `SHADER_PERMUTATION_BOOL("DEFINE")` | `RenderCore/Public/ShaderPermutation.h:629` | |
| `SHADER_PERMUTATION_INT("DEFINE", N)` | `ShaderPermutation.h:648` | |
| `SHADER_PERMUTATION_RANGE_INT("DEFINE", Start, N)` | `ShaderPermutation.h:658` | |
| `TShaderPermutationDomain<...>` | `ShaderPermutation.h:272,452` | |
| `GetGlobalShaderMap(EShaderPlatform)` | `GlobalShader.h:333` | |
| `GetGlobalShaderMap(ERHIFeatureLevel::Type)` | `GlobalShader.h:351` (inline) | |
| `TShaderMapRef<T>(const ShaderMapType*)` | `RenderCore/Public/Shader.h:2691` | `static_assert`s that the shader has no permutation domain |
| `TShaderMapRef<T>(Map, const FPermutationDomain&)` | `Shader.h:2698` | |
| `ECompilerFlags` | `RenderCore/Public/ShaderCompilerCore.h:110` | X-macro list in `RenderCore/Public/ShaderCompilerFlags.inl` |
| `IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5)` | `RHI/Public/DataDrivenShaderPlatformInfo.h:1076` | |
| `RHISupportsWaveOperations(Platform)` | `DataDrivenShaderPlatformInfo.h:1261` | |

Compiler flags in `ShaderCompilerFlags.inl`:

| Line | Flag |
|---|---|
| 57 | `WaveOperations` |
| 59 | `ForceDXC` |
| 64 | `AllowTypedUAVLoads` |
| 66 | `Wave32` |
| 70 | `AllowRealTypes` |
| 74 | `HLSL2021` |
| 77-78 | `BindlessResources` and `BindlessSamplers` are deprecated in 5.7 |

```cpp
class FFluidP2GCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFluidP2GCS);
    SHADER_USE_PARAMETER_STRUCT(FFluidP2GCS, FGlobalShader);

    class FWaveOps : SHADER_PERMUTATION_BOOL("USE_WAVE_OPS");
    using FPermutationDomain = TShaderPermutationDomain<FWaveOps>;

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(uint32, NumParticles)
        SHADER_PARAMETER(FVector3f, GridOrigin)
        SHADER_PARAMETER(float, InvCellSize)
        SHADER_PARAMETER(FIntVector, GridDim)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Particles)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, RWGridAccum)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, RWPressure)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)
    {
        const FPermutationDomain PV(P.PermutationId);
        if (PV.Get<FWaveOps>() && !RHISupportsWaveOperations(P.Platform)) return false;
        return IsFeatureLevelSupported(P.Platform, ERHIFeatureLevel::SM5);
    }
    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& Env)
    {
        FGlobalShader::ModifyCompilationEnvironment(P, Env);
        Env.SetDefine(TEXT("THREADGROUP_SIZE"), 64);           // ShaderCore.h:603-605 overloads
        if (FPermutationDomain(P.PermutationId).Get<FWaveOps>())
            Env.CompilerFlags.Add(CFLAG_WaveOperations);
    }
};
IMPLEMENT_GLOBAL_SHADER(FFluidP2GCS, "/Plugin/FluidSim/Private/P2G.usf", "MainCS", SF_Compute);
```

Engine pattern for the same wave-ops permutation:
- `Renderer/Private/SingleLayerWaterRendering.cpp:648`: `ShouldCompilePermutation`
- `:662-671`: `CFLAG_WaveOperations`
- `:894-896`: runtime gate `GRHISupportsWaveOperations && RHISupportsWaveOperations(View.GetShaderPlatform())`

---

## 3. RDG

**Builder** (`RenderCore/Public/RenderGraphBuilder.h`)
- `:66` `RENDERCORE_API FRDGBuilder(FRHICommandListImmediate& RHICmdList, FRDGEventName Name = {}, ERDGBuilderFlags Flags = ERDGBuilderFlags::None, EShaderPlatform ShaderPlatform = GMaxRHIShaderPlatform);`
- `:412` `RENDERCORE_API void Execute();`
- `:221` `template<typename P, typename L> FRDGPassRef AddPass(FRDGEventName&& Name, const P* ParameterStruct, ERDGPassFlags Flags, L&& Lambda);`
- `:233` parameterless `AddPass(FRDGEventName&&, ERDGPassFlags, L&&)`. It implicitly adds `NeverCull` and does not allow `AsyncCompute`.
- `:180` `AllocParameters<T>()`
- `:110` `FRDGBufferRef CreateBuffer(const FRDGBufferDesc&, const TCHAR* Name, ERDGBufferFlags = None)`
- `:104` `FRDGTextureRef CreateTexture(const FRDGTextureDesc&, const TCHAR*, ERDGTextureFlags = None)`
- `:138` `CreateUAV(const FRDGBufferUAVDesc&, ...)`. `FRDGBufferUAVDesc(FRDGBufferRef)` is implicit (`RenderGraphResources.h:1166`), so `CreateUAV(Buffer)` works.
- `:140` `CreateUAV(Buffer, EPixelFormat)`
- `:132` `CreateUAV(FRDGTextureRef, ERDGUnorderedAccessViewFlags = None, EPixelFormat = PF_Unknown)`
- `:122/124` `CreateSRV(FRDGBufferSRVDesc)` / `CreateSRV(Buffer, EPixelFormat)`
- `:90` `RegisterExternalBuffer(const TRefCountPtr<FRDGPooledBuffer>&, ERDGBufferFlags = None)`
- `:91` adds an `ERHIAccess AccessFinal` overload
- `:94` adds a `const TCHAR* NameIfNotRegistered` overload
- `:79/84` `RegisterExternalTexture(const TRefCountPtr<IPooledRenderTarget>&, [const TCHAR*,] ERDGTextureFlags = None)`
- `:352/353` `QueueBufferExtraction(FRDGBufferRef, TRefCountPtr<FRDGPooledBuffer>* Out[, ERHIAccess AccessFinal])`
- `:345/346` `QueueTextureExtraction(FRDGTextureRef, TRefCountPtr<IPooledRenderTarget>* Out, ...)`
- `:360` `const TRefCountPtr<FRDGPooledBuffer>& ConvertToExternalBuffer(FRDGBufferRef)`
- `:359` `const TRefCountPtr<IPooledRenderTarget>& ConvertToExternalTexture(FRDGTextureRef)`
- `:309` `QueueBufferUpload(FRDGBufferRef, const void*, uint64, ERDGInitialDataFlags = None)`

**Flags** (`RenderCore/Public/RenderGraphDefinitions.h`)
- `:129` `ERDGPassFlags`: `None, Raster, Compute, AsyncCompute, Copy, NeverCull, SkipRenderPass, NeverMerge, NeverParallel, Readback(=Copy|NeverCull)`
- `:111` `ERDGBuilderFlags`: `None, ParallelSetup, ParallelCompile, ParallelExecute, Parallel`

**Descriptors**
- Buffer descriptors, `RenderCore/Public/RenderGraphResources.h`:
  - `:993` `FRDGBufferDesc::CreateStructuredDesc(uint32 BytesPerElement, uint32 NumElements)`
  - `:1003` `CreateStructuredDesc<T>(N)`
  - `:1010` `CreateBufferDesc(BytesPerElement, N)`. This is a typed/vertex buffer for `Buffer<>`/`RWBuffer<>`.
  - `:941` `CreateByteAddressDesc(NumBytes)`
  - `:983` `CreateIndirectDesc<FRHIDispatchIndirectParameters>(N)`
  - `:959` `CreateIndirectDesc(BytesPerElement, N)`
  - `:968` `CreateRawIndirectDesc(NumBytes)`
- `FRDGPooledBuffer`: `RenderGraphResources.h:1194`
- `IPooledRenderTarget`: `RenderCore/Public/PooledRenderTarget.h:433`
- `RenderGraphDefinitions.h:660` `static FRDGTextureDesc Create3D(FIntVector Size, EPixelFormat Format, FClearValueBinding ClearValue, ETextureCreateFlags Flags, uint8 NumMips = 1, uint32 ExtData = 0)`

**Utilities** (`RenderCore/Public/RenderGraphUtils.h`, namespace `FComputeShaderUtils` at `:333`)
- `:339-367` `GetGroupCount(int32|FIntPoint|FIntVector ThreadCount, int32|FIntPoint|FIntVector GroupSize) -> FIntVector`
- `:388/416` `GetGroupCountWrapped(...)`. Use this for more than 65535 groups in one dimension. The shader side is `GetUnWrappedDispatchGroupId` in `ComputeShaderUtils.ush`.
- `:603` `FRDGPassRef AddPass(FRDGBuilder&, FRDGEventName&&, ERDGPassFlags, const TShaderRef<T>&, typename T::FParameters*, FIntVector GroupCount)`
- `:616` the same without flags. It defaults to `Compute`.
- `:571/628` take a `FRDGDispatchGroupCountCallback`
- `:641` indirect form: `AddPass(GraphBuilder, Name, ERDGPassFlags, Shader, Params, FRDGBufferRef IndirectArgsBuffer, uint32 IndirectArgsOffset, FRDGDispatchLateParamCallback&& = {})`. `PassFlags` must be exactly `Compute` or `AsyncCompute`. The buffer needs `BUF_DrawIndirect`, which means using `CreateIndirectDesc`.
- `:693` `FRDGBufferRef AddIndirectArgsSetupCsPass1D(GraphBuilder, FeatureLevel, FRDGBuffer* InputCountBuffer, const TCHAR* OutName, uint32 Divisor, uint32 InputCountOffset = 0, uint32 Multiplier = 1)`. This is a ready-made counter-to-dispatch-args pass.
- `:758` `AddClearUAVPass(FRDGBuilder&, FRDGBufferUAVRef, uint32 Value, ERDGPassFlags = Compute)`. It calls `ClearUAVUint` and works on structured buffers (`Private/RenderGraphUtils.cpp:419-435`). There is also `AddClearUAVFloatPass`.
- `:762` texture overload with `float`
- `:764` texture overload with `uint32`
- `:772` texture overload with `FUintVector4`
- `:839` `AddCopyBufferPass(GraphBuilder, Dst, DstOffset, Src, SrcOffset, NumBytes)`
- `:841` `AddCopyBufferPass(GraphBuilder, Dst, Src)`
- `:865` `CreateStructuredBuffer(GraphBuilder, Name, BytesPerElement, NumElements, InitialData, InitialDataSize, ERDGInitialDataFlags = None)`. There are TArray overloads at `:890+`.
- `:1392` `TRefCountPtr<FRDGPooledBuffer> AllocatePooledBuffer(const FRDGBufferDesc&, const TCHAR* Name, ...)`. Use it to allocate persistent buffers outside a graph.
- `FRHIDispatchIndirectParameters`: `RHI/Public/RHI.h:549` (padded)
- `FRHIDrawIndirectParameters`: `RHI.h:561`

**Persistent sim buffers pattern (render-thread-owned state)**
```cpp
struct FFluidGPUState { TRefCountPtr<FRDGPooledBuffer> Particles; TRefCountPtr<IPooledRenderTarget> Pressure; uint32 N = 0; FIntVector Dim; };

FRDGBufferRef Particles;
if (!State.Particles) {
    Particles = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), State.N), TEXT("Fluid.Particles"));
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Particles), 0u);
    State.Particles = GraphBuilder.ConvertToExternalBuffer(Particles);   // valid immediately, persists after Execute
} else {
    Particles = GraphBuilder.RegisterExternalBuffer(State.Particles);
}
FRDGTextureRef Pressure = State.Pressure ? GraphBuilder.RegisterExternalTexture(State.Pressure)
    : GraphBuilder.CreateTexture(FRDGTextureDesc::Create3D(State.Dim, PF_R32_FLOAT, FClearValueBinding::Black,
        TexCreate_ShaderResource | TexCreate_UAV), TEXT("Fluid.Pressure"));
if (!State.Pressure) State.Pressure = GraphBuilder.ConvertToExternalTexture(Pressure);

auto* P = GraphBuilder.AllocParameters<FFluidP2GCS::FParameters>();
P->Particles  = GraphBuilder.CreateSRV(Particles);
P->RWPressure = GraphBuilder.CreateUAV(Pressure);
// ...
TShaderMapRef<FFluidP2GCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel), FFluidP2GCS::FPermutationDomain());
FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Fluid.P2G"), ERDGPassFlags::Compute, CS, P,
    FComputeShaderUtils::GetGroupCount((int32)State.N, 64));
```
Note: RDG culls passes whose outputs are never consumed. Writes into a registered or converted external resource count as consumed. Transient buffers that nothing reads get culled together with their producer passes. `r.RDG.CullPasses` is defined at `RenderCore/Private/RenderGraphPrivate.cpp:230`.

**Async compute**
- Tag the pass with `ERDGPassFlags::AsyncCompute`. The lambda must take `FRHIComputeCommandList&`.
- RDG enables async compute only when all of these hold (`RenderCore/Private/RenderGraphPrivate.h:229-234` **[PRIVATE]**):
  - `GRDGAsyncCompute > 0` (`r.RDG.AsyncCompute`, `RenderGraphPrivate.cpp:211-218`: 0 = off, 1 = tagged passes, 2 = all compute)
  - not immediate mode
  - render-pass merging is off
  - `GSupportsEfficientAsyncCompute` (`RHI/Public/RHIGlobals.h:866`)
  - `GRHISupportsSeparateDepthStencilCopyAccess`
- On D3D12, `GSupportsEfficientAsyncCompute` is set at `D3D12RHI/Private/D3D12Adapter.cpp:1447-1481`: AMD yes, NVIDIA only with a caps check, Intel through an extension check. `-ForceAsyncCompute` and `-DisableAsyncCompute` override it.
- On Vulkan it follows `HasAsyncComputeQueue()` (`VulkanRHI.cpp:994`).
- Query with `GraphBuilder.IsAsyncComputeEnabled()` (`RenderGraphBuilder.h:292`).
- If async compute is unsupported, the passes silently run on the graphics pipe.
- Async compute overlaps only with work **in the same graph**. A standalone sim graph overlaps nothing outside itself.

---

## 4. HLSL atomics and shader model

- **D3D12 SM6 always compiles `cs_6_6` with DXC.**
  - `Developer/Windows/ShaderFormatD3D/Private/ShaderFormatD3D.cpp:107-110`: `IsSM66` is true when `ShaderFormat == NAME_PCD3D_SM6`.
  - `D3DShaderCompiler.cpp:131-140`: `cs_6_6` profile.
  - Wave ops are always available on SM6 (`ShaderFormatD3D.cpp:224`: `PLATFORM_SUPPORTS_SM6_0_WAVE_OPERATIONS = bSM6Features`).
  - `CFLAG_WaveOperations` matters only to push SM5 toward SM6.0 (`:113-121`).
  - `SM6_PROFILE` = 1 on SM6.6 (`:227-231`). `PLATFORM_SUPPORTS_REAL_TYPES` (fp16) needs `CFLAG_AllowRealTypes` (`:244`).
- **Vulkan** always defines `PLATFORM_SUPPORTS_SM6_0_WAVE_OPERATIONS=1` with Vulkan 1.1 as the minimum (`Developer/VulkanShaderFormat/Private/SpirVShaderCompiler.inl:1503-1507`).
- **Metal** defines it only when `METAL_SM6_PROFILE == 1` (`Engine/Shaders/Public/Platform/Metal/MetalCommon.ush:35-37`).
- Shader guards are in `Engine/Shaders/Public/Platform.ush`:
  - `:253` `PLATFORM_SUPPORTS_SM6_0_WAVE_OPERATIONS` defaults to 0
  - `:661-689` `COMPILER_SUPPORTS_WAVE_ONCE` and `COMPILER_SUPPORTS_WAVE_VOTE`
- Wave-aggregated atomic helpers are in `Engine/Shaders/Private/WaveOpUtil.ush:44-130`: `WaveInterlockedAdd`, `WaveInterlockedAdd_`, `WaveInterlockedAddScalar`. They fall back to plain `InterlockedAdd` when wave vote is unavailable. Use them for particle counters and append allocators.
- **Float atomics**: the engine has **no** global `InterlockedAddFloat` helper, and HLSL SM6.6 has no portable float `InterlockedAdd`. The engine's own grid-scatter reference is HairStrands:
  - `Engine/Plugins/Runtime/HairStrands/Shaders/Private/NiagaraDataInterfaceVelocityGridTemplate.ush:199-249`
  - `FAST_MODE==1` uses a fixed-point `int` with `InterlockedAdd`.
  - The other branch uses an `asint` CAS loop with `InterlockedCompareExchange`.
  - **Use fixed-point int for P2G (fast, deterministic) and a CAS loop only where range or precision requires it.**
- Engine helpers for 64-bit image atomics: `COMPILER_SUPPORTS_UINT64_IMAGE_ATOMICS` in `Platform/D3D/D3DCommon.ush:117`, `Vulkan/VulkanCommon.ush:13`, `Metal/MetalCommon.ush:107`. The CPU-side gates are `GRHISupportsAtomicUInt64` (`RHIGlobals.h:845`) and `RHISupportsVolumeTextureAtomics(Platform)` (`DataDrivenShaderPlatformInfo.h:1304`). Prefer buffers over 3D-texture atomics.

```hlsl
#include "/Engine/Public/Platform.ush"
RWStructuredBuffer<int> RWGridAccum;          // or RWByteAddressBuffer + .InterlockedAdd(byteOffset, v)
static const float FP_SCALE = 65536.0;         // choose per quantity (range vs. precision)
void AtomicAddFixed(uint Idx, float V) { InterlockedAdd(RWGridAccum[Idx], (int)round(V * FP_SCALE)); }
float ReadFixed(int Q) { return (float)Q / FP_SCALE; }

RWStructuredBuffer<uint> RWGridF;              // exact float add via CAS (slower, contention-sensitive)
void AtomicAddFloatCAS(uint Idx, float V)
{
    uint Cur = RWGridF[Idx], Prev;
    [allow_uav_condition] for (;;) {
        InterlockedCompareExchange(RWGridF[Idx], Cur, asuint(asfloat(Cur) + V), Prev);
        if (Prev == Cur) break; Cur = Prev;
    }
}
```

---

## 5. Running the sim once per frame

Callbacks that receive `FRDGBuilder&` are all in `Engine/Public/SceneViewExtension.h`. Base classes: `FSceneViewExtensionBase` at `:278` (ctor `FSceneViewExtensionBase(const FAutoRegister&)` at `:281`) and `FWorldSceneViewExtension(const FAutoRegister&, UWorld*)` at `:292-295`.

```cpp
virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) {}                 // :175
virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) {}                                   // :180
virtual void PreInitViews_RenderThread(FRDGBuilder& GraphBuilder) {}                                                         // :185
virtual void PreRenderBasePass_RenderThread(FRDGBuilder& GraphBuilder, bool bDepthBufferIsPopulated) {}                     // :190
virtual void PostRenderBasePassDeferred_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView,
        const FRenderTargetBindingSlots& RenderTargets, TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) {} // :195
virtual void PostTLASBuild_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) {}                                    // :205 (needs ESceneViewExtensionFlags::SubscribesToPostTLASBuild)
virtual void PrePostProcessPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& InView, const FPostProcessingInputs& Inputs) {} // :210
virtual void PrePostProcessPassMobile_RenderThread(FRDGBuilder&, const FSceneView&, const FMobilePostProcessingInputs&) {}  // :215
virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView, FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) {} // :222
virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) {}                // :227
virtual void PostRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) {}                                  // :232
virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const { return true; }                   // :261 (IsActiveThisFrame itself is final in the Base)
// Game-thread: SetupViewFamily(FSceneViewFamily&) :145, SetupView(FSceneViewFamily&, FSceneView&) :150, BeginRenderViewFamily(FSceneViewFamily&) :165 — all have {} defaults now.
// Factory: template<class T, class... A> static TSharedRef<T, ESPMode::ThreadSafe> FSceneViewExtensions::NewExtension(A&&...)   :329
```

Call order in the deferred renderer:

| Callback | Call site |
|---|---|
| `PreRenderViewFamily` / `PreRenderView` | `Renderer/Private/SceneRendering.cpp:4299/4303` |
| `GEngine->GetPreRenderDelegateEx()` (`FPreRenderDelegateEx` = `(FRDGBuilder&)`, `Engine/Classes/Engine/Engine.h:704`) | `DeferredShadingRenderer.cpp:2364` |
| `PreRenderBasePass` | `:2792` |
| `PostRenderBasePassDeferred` | `BasePassRendering.cpp:1162` |
| lights | `:3468` |
| fog | `:3823` |
| **post-opaque delegate** | `:3925` |
| main translucency | `:4012` |
| `PrePostProcessPass` | `:4269`, after translucency |
| `PostRenderViewFamily` | `SceneRendering.cpp:4956` |

**All of these fire once per view family** (each viewport, scene capture, or editor viewport). None of them is "once per frame".

Other driver options:
- (b) `ENQUEUE_RENDER_COMMAND(Name)([..](FRHICommandListImmediate& RHICmdList){ FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("FluidSim")); ...; GraphBuilder.Execute(); });`
  - `RenderCore/Public/RenderingThread.h:1087`. Dispatcher accepts `TUniqueFunction<void(FRHICommandListImmediate&)>` (`:993`).
- (c) `FTickableGameObject`, `Engine/Public/Tickable.h`:
  - `:158` `FTickableGameObject(ETickableTickType = ETickableTickType::NewObject)`
  - `:95` `virtual void Tick(float DeltaTime) = 0`
  - `:125` `virtual TStatId GetStatId() const = 0`
  - `:182` `IsTickableInEditor`
  - `:171` `IsTickableWhenPaused`
  - `:194` `GetTickableGameObjectWorld`
  - A `UTickableWorldSubsystem` is equivalent.

**Recommendation:**
- Use **(c)+(b)**: tick once per game frame on the game thread, snapshot parameters, and enqueue a standalone RDG graph. This runs exactly once per frame no matter how many views there are. It is ordered before that frame's scene rendering, because world tick happens before viewport redraw. It needs no Renderer-module coupling.
- Persist state in `TRefCountPtr<FRDGPooledBuffer>` and `TRefCountPtr<IPooledRenderTarget>`, owned by a render-thread proxy.
- Render from a scene view extension or the post-opaque delegate by registering those same pooled resources (see section 8).
- If you later need async-compute overlap with scene rendering, move the step into `PreRenderViewFamily_RenderThread` and guard it with `GFrameCounterRenderThread` (`Core/Public/CoreGlobals.h:534`) so it runs only on the first family each frame.

Engine examples:
- `Engine/Plugins/Experimental/Water/Source/Runtime/Private/WaterViewExtension.cpp:821-857` uses `FWorldSceneViewExtension` with `PreRenderViewFamily`, `PreRenderView`, and `PreRenderBasePass`. It is created at `WaterBodyManager.cpp:12` with `NewExtension<FWaterViewExtension>(World)`.
- `Engine/Plugins/Experimental/ColorCorrectRegions/.../ColorCorrectRegionsSceneViewExtension.cpp:693-714` shows `PrePostProcessPass` reading `(*Inputs.SceneTextures)->SceneColorTexture`.
- `Engine/Plugins/Compositing/CompositeCore/Source/CompositeCore/Private/CompositeCoreSceneViewExtension.cpp`.

---

## 6. GPU to CPU readback

- `RHI/Public/RHIGPUReadback.h`:
  - `:132` `class FRHIGPUBufferReadback final : public FRHIGPUMemoryReadback`
  - `:136` `FRHIGPUBufferReadback(FName RequestName)`
  - `:138` `void EnqueueCopy(FRHICommandList&, FRHIBuffer* SourceBuffer, uint32 NumBytes = 0)`
  - `:139` `void* Lock(uint32 NumBytes)`
  - `:140` `void Unlock()`
  - `:33` `bool IsReady()` (inherited)
- RDG wrapper at `RenderCore/Public/RenderGraphUtils.h:847`: `void AddEnqueueCopyPass(FRDGBuilder&, FRHIGPUBufferReadback* Readback, FRDGBufferRef SourceBuffer, uint32 NumBytes)`. The texture version is at `:844`.
- Latency pattern: keep a ring of 3 or 4 readbacks. Enqueue into slot `Frame % N`, and on the render thread poll the oldest slot with `IsReady()`. Never spin-wait on it. Call `Lock`/`Unlock` on the render thread, then hand the copied data to the game thread through a queue or a game-thread task. Expect 2–3 frames of latency.

---

## 7. GPU profiling (5.8)

- `RDG_EVENT_NAME("Fmt", ...)`: `RenderCore/Public/RenderGraphEvent.h:477`.
- `RDG_EVENT_SCOPE(GraphBuilder, "Fmt", ...)`: `:479`. It compiles to nothing in `WITH_RHI_BREADCRUMBS_MINIMAL` builds (`:500`).
- `RDG_EVENT_SCOPE_STAT(GraphBuilder, StatName, "Fmt", ...)`: `:480`. This is the GPU-stat scope that still works in minimal mode (`:497`).
- `RDG_EVENT_SCOPE_CONDITIONAL_STAT(GraphBuilder, Cond, StatName, "Fmt")`: `:482`.
- `RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, StatName)`: `:524`.
- `DECLARE_GPU_STAT(StatName)`: `RenderCore/Public/ProfilingDebugging/RealtimeGPUProfiler.h:128`. Use it at file scope in exactly one .cpp. It is active only when `HAS_GPU_STATS` (`RHI/Public/RHIDefinitions.h:74`, non-shipping).
- `DECLARE_GPU_STAT_NAMED(StatName, TEXT("Display"))`: `:123`.
- `DECLARE_GPU_STAT_NAMED_EXTERN(...)` in a header plus `DEFINE_GPU_STAT(StatName)` in one .cpp: `:124` / `:133`.
- **Deprecated in 5.8 and no-ops:** `RDG_GPU_STAT_SCOPE` and `RDG_GPU_STAT_SCOPE_VERBOSE` (`RenderGraphEvent.h:520-521`), and `SCOPED_GPU_STAT` and `SCOPED_GPU_STAT_VERBOSE` (`RealtimeGPUProfiler.h:153-154`).
- CPU side:
  - `TRACE_CPUPROFILER_EVENT_SCOPE(Name)`: `Core/Public/ProfilingDebugging/CpuProfilerTrace.h:453`
  - `CSV_DEFINE_CATEGORY(Cat, bDefault)`: `CsvProfiler.h:50`
  - `CSV_SCOPED_TIMING_STAT(Cat, Stat)`: `:120`

```cpp
DECLARE_GPU_STAT(FluidSim);
RDG_EVENT_SCOPE_STAT(GraphBuilder, FluidSim, "FluidSim");
```

---

## 8. Particle and fluid rendering

**(i) Recommended prototype: post-opaque render delegate.** It is public, runs before translucency, and gives both SceneColor and SceneDepth.
- `RenderCore/Public/RendererInterface.h:85` `class FPostOpaqueRenderParameters` has these members:
  - `FIntRect ViewportRect`
  - `FMatrix ViewMatrix, ProjMatrix`
  - `FRDGTexture* ColorTexture, DepthTexture, NormalTexture, VelocityTexture, SmallDepthTexture`
  - `FRDGBuilder* GraphBuilder`
  - `FRHIUniformBuffer* ViewUniformBuffer`
  - `TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformParams`
  - `void* Uid`
  - `const FViewInfo* View`
- `:104-105` `FOnPostOpaqueRender` / `FPostOpaqueRenderDelegate` (one parameter, `FPostOpaqueRenderParameters&`).
- `:329` `IRendererModule::RegisterPostOpaqueRenderDelegate(const FPostOpaqueRenderDelegate&) -> FDelegateHandle`; `:330` `RemovePostOpaqueRenderDelegate`. Get the module with `FModuleManager::GetModuleChecked<IRendererModule>("Renderer")`.
- The delegate is broadcast per view after fog and before main translucency. The fields are filled at `Renderer/Private/SceneRendering.cpp:5643-5658` (`Color.Target` / `Depth.Target`, so these are the MSAA targets when MSAA forward is on). `GPULightmass` uses it (`Engine/Plugins/Experimental/GPULightmass/.../LightmapRenderer.cpp`).
- Inside the delegate: `GraphBuilder.RegisterExternalBuffer(State.Particles)`, then add a Raster pass with `RENDER_TARGET_BINDING_SLOTS()`:

```cpp
P->RenderTargets[0] = FRenderTargetBinding(Params.ColorTexture, ERenderTargetLoadAction::ELoad);           // ShaderParameterMacros.h:460
P->RenderTargets.DepthStencil = FDepthStencilBinding(Params.DepthTexture, ERenderTargetLoadAction::ELoad,
                                    FExclusiveDepthStencil::DepthWrite_StencilNop);                     // :596
GraphBuilder.AddPass(RDG_EVENT_NAME("Fluid.Draw"), P, ERDGPassFlags::Raster, [P, VS, PS, Rect, N](FRHICommandList& RHICmdList)
{
    RHICmdList.SetViewport(Rect.Min.X, Rect.Min.Y, 0.f, Rect.Max.X, Rect.Max.Y, 1.f);
    FGraphicsPipelineStateInitializer PSO; RHICmdList.ApplyCachedRenderTargets(PSO);
    PSO.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;           // CommonRenderResources.h:83
    PSO.BoundShaderState.VertexShaderRHI = VS.GetVertexShader(); PSO.BoundShaderState.PixelShaderRHI = PS.GetPixelShader();
    PSO.PrimitiveType = PT_TriangleList;
    PSO.BlendState = TStaticBlendState<>::GetRHI(); PSO.RasterizerState = TStaticRasterizerState<>::GetRHI();
    PSO.DepthStencilState = TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();             // reversed-Z
    SetGraphicsPipelineState(RHICmdList, PSO, 0);                                                       // RHI/Public/PipelineStateCache.h:165
    SetShaderParameters(RHICmdList, VS, VS.GetVertexShader(), P->VS);                                   // ShaderParameterStruct.h:366
    SetShaderParameters(RHICmdList, PS, PS.GetPixelShader(), P->PS);
    RHICmdList.DrawPrimitive(0, 2, N);  // 2 tris/quad, instanced by SV_InstanceID   RHICommandList.h:3745
    // GPU-driven count: RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs) + RHICmdList.DrawPrimitiveIndirect(P->IndirectArgs->GetRHI(), 0) (:3910)
});
```

Use nested `BEGIN_SHADER_PARAMETER_STRUCT`s for VS and PS, with `SHADER_PARAMETER_STRUCT_INCLUDE` (`ShaderParameterMacros.h:1840`). Pass `ViewMatrix * ProjMatrix` as an `FMatrix44f` instead of binding the View UB. Screen-space fluid (depth splat, bilateral blur, shade) is the same pattern with extra full-screen passes. The full-screen helper is `FPixelShaderUtils::AddFullscreenPass(GraphBuilder, GlobalShaderMap, Name, PS, Params, FIntRect Viewport, Blend, Raster, DepthStencil, StencilRef, AdditionalFlags)` at `RenderCore/Public/PixelShaderUtils.h:155`.

**Scene view extension alternatives:**
- `PostRenderBasePassDeferred_RenderThread` gets the GBuffer and depth `RenderTargets` plus the public SceneTextures UB. Drawing there means writing to the GBuffer, so the fluid gets lit by the engine as an opaque surface.
- `PrePostProcessPass_RenderThread` runs **after translucency**. Its `FPostProcessingInputs` is defined only in **[INTERNAL]** `Renderer/Internal/PostProcess/PostProcessInputs.h:8`. Forward-declaring it is not enough, because reading `Inputs.SceneTextures` needs the full definition.
- Public alternatives:
  - `UE::FXRenderingUtils::GetRawViewRectUnsafe(const FSceneView&)` (`Renderer/Public/FXRenderingUtils.h:41`)
  - `GetOrCreateSceneTextureUniformBuffer(GraphBuilder, Views, FeatureLevel)` (`:48`)
  - `CreateSceneTextureShaderParameters(GraphBuilder, const FSceneView&, ...)` / `GetSceneTextureShaderParameters(const FSceneView&)` (`Renderer/Public/SceneRenderTargetParameters.h:129/134`)
  - `FSceneTextureUniformParameters` (`Engine/Public/SceneTexturesConfig.h:14`)
  - `AddDrawScreenPass` (`Renderer/Public/ScreenPass.h:499`)

**(ii) Niagara:** optional. It needs a custom NDI or a render-target/grid NDI and pulls in Niagara modules. It is not recommended for the prototype.

**(iii) Custom `FPrimitiveSceneProxy` plus vertex factory:** `GetDynamicMeshElements` (`Engine/Public/PrimitiveSceneProxy.h:504`), with `FMeshBatchElement::IndirectArgsBuffer` / `IndirectArgsOffset` (`Engine/Public/MeshBatch.h:265-266`). This gives proper shadows, translucency sorting, and material support, but it means a custom VF, its `.ush`, and material compatibility work. Defer it.

---

## 9. Game thread and render thread

- `ENQUEUE_RENDER_COMMAND(Type)(Lambda)`: `RenderingThread.h:1087`. The lambda takes `FRHICommandListImmediate&`.
- `FlushRenderingCommands()`: `RenderingThread.h:111`.
- `TRenderThreadStruct<T>` (`:1135ff`) creates and deletes a struct on the render thread through render commands.
- `FRenderCommandFence` (`RenderCore/Public/RenderCommandFence.h:14`):
  - `void BeginFence(ESyncDepth = ESyncDepth::RenderThread)` at `:35`
  - `void Wait(bool bProcessGameThreadTasks = false) const` at `:41`
  - `bool IsFenceComplete() const` at `:44`
- `FDeferredCleanupInterface`: `RenderCore/Public/RenderDeferredCleanup.h:10`. `BeginCleanup(FDeferredCleanupInterface*)`: `:32`. It deletes the object after the render thread has finished with it.
- Pattern:
  - The component owns `TSharedPtr<FFluidRenderProxy, ESPMode::ThreadSafe>`.
  - Every parameter change goes by value into a render command.
  - The proxy holds the pooled RDG resources and is touched only on the render thread.
  - On `EndPlay`/`BeginDestroy`, enqueue a command that removes the proxy from the extension or delegate registry and drops the shared pointer. Alternatively, call `BeginCleanup(Proxy)` and use a fence in `IsReadyForFinishDestroy`.
  - Scene view extensions are `TSharedRef<..., ESPMode::ThreadSafe>`. Keep the ref in a subsystem and reset it at world teardown, as Water does with `FWorldDelegates::OnPreWorldFinishDestroy` (`WaterViewExtension.cpp:82`).
  - Call `RemovePostOpaqueRenderDelegate` in shutdown.

---

## 10. Build.cs

```csharp
PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "RenderCore", "RHI", "Renderer", "Projects" });
```

Dependencies by module:

| Module | Provides |
|---|---|
| `RenderCore` | shaders, RDG |
| `RHI` | RHI types, readback |
| `Projects` | `IPluginManager` |
| `Engine` | `SceneViewExtension`, `FSceneTextureUniformParameters`, tickable |
| `Renderer` | `IRendererModule` and `FXRenderingUtils`. Headers like `RendererInterface.h` live in RenderCore, but the implementation module is Renderer |

Notes:
- Engine plugins use the same set (ColorCorrectRegions.Build.cs).
- **Do not** add `Renderer/Private` include paths.
- **`Renderer/Internal` is not visible to project plugins.** UBT adds Internal include paths only when the depending module has the same scope or sets `bTreatAsEngineModule` (`Engine/Source/Programs/UnrealBuildTool/Configuration/UEBuildModule.cs:736-739`). So `PostProcess/PostProcessInputs.h` and `SceneTextures.h` (`FSceneTextures`, `FMinimalSceneTextures`) need `bTreatAsEngineModule = true`, or the plugin must live under `Engine/Plugins`. **Avoid both by using the post-opaque delegate.**

---

## 11. Cross-platform

- Integer `InterlockedAdd`, `InterlockedMin`, `InterlockedMax`, and `InterlockedCompareExchange` on `RWStructuredBuffer<uint|int>` and `RWByteAddressBuffer` are portable across D3D12, Vulkan, and Metal. Build float accumulation on fixed-point int or CAS.
- Native float atomic add exists only through vendor extensions (see `bRequiresVendorExtensionsForAtomics`, `DataDrivenShaderPlatformInfo.h:60,531`). Do not depend on it.
- Wave intrinsics:
  - Always present on SM6 D3D and Vulkan.
  - On Metal only with `METAL_SM6_PROFILE`.
  - Wave size varies (32 on NVIDIA/Intel, 32 or 64 on AMD). Query `GRHIMinimumWaveSize` / `GRHIMaximumWaveSize` (`RHIGlobals.h:923-924`).
  - Never hard-code 32 or 64. Guard with `#if COMPILER_SUPPORTS_WAVE_VOTE` or use `WaveOpUtil.ush`.
- 3D-texture atomics: `RHISupportsVolumeTextureAtomics`.
- 64-bit atomics: `GRHISupportsAtomicUInt64`.
- Indirect args must not cross `PLATFORM_DISPATCH_INDIRECT_ARGUMENT_BOUNDARY_SIZE`. Use `FRHIDispatchIndirectParameters`, which is padded, and offsets that are a multiple of 4. This is checked at `RenderGraphUtils.h:428-439`.
- Max groups per dimension: `GRHIMaxDispatchThreadGroupsPerDimension`. Use `GetGroupCountWrapped` for large particle counts.
