// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshRenderer.h"
#include "SloshRenderShaders.h"
#include "SloshTypes.h"
#include "EngineModule.h"
#include "RendererInterface.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"

DECLARE_GPU_STAT(SloshRender);

#define SLOSH_RENDER_USF "/Plugin/Slosh/Private/SloshRender.usf"
IMPLEMENT_GLOBAL_SHADER(FSloshSplatParticlesCS, SLOSH_RENDER_USF, "SplatParticlesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshResolveCS, SLOSH_RENDER_USF, "ResolveCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshSmoothDepthCS, SLOSH_RENDER_USF, "SmoothDepthCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshCompositeCS, SLOSH_RENDER_USF, "CompositeCS", SF_Compute);

namespace
{
	struct FSloshRenderEntry
	{
		const FSloshProxy* Key = nullptr;
		FSloshProxyWeakPtr Proxy;
		FSceneInterface* Scene = nullptr;
		FSloshRenderSettings Settings;
	};

	// Render thread only.
	TArray<FSloshRenderEntry> GSloshRenderEntries;
	// Game thread only.
	FDelegateHandle GPostOpaqueHandle;

	constexpr int32 SplatGroupSize = 128;
	constexpr float MaxSplatPixels = 32.0f;

	void RenderVolume(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostOpaqueRenderParameters& Params,
		const FSloshProxy& Proxy, const FSloshRenderSettings& Settings)
	{
		const FIntRect Rect = Params.ViewportRect;
		const FIntPoint ViewSize = Rect.Size();
		if (ViewSize.X <= 0 || ViewSize.Y <= 0)
		{
			return;
		}
		FRDGTextureRef SceneColor = Params.ColorTexture;
		FRDGTextureRef SceneDepth = Params.DepthTexture;
		const FIntPoint Extent = SceneColor->Desc.Extent;

		// Sim space -> translated world -> view, composed in double precision before the float cast.
		const FMatrix SimToWorld = Proxy.GetSimToWorld_RenderThread().ToMatrixWithScale();
		const FMatrix SimToTranslatedWorld = SimToWorld * FTranslationMatrix(View.ViewMatrices.GetPreViewTranslation());
		const FMatrix TranslatedWorldToView = View.ViewMatrices.GetTranslatedViewMatrix();

		FSloshRenderViewParameters ViewParameters;
		ViewParameters.ViewRectMin = Rect.Min;
		ViewParameters.ViewSize = ViewSize;
		ViewParameters.SimToView = FMatrix44f(SimToTranslatedWorld * TranslatedWorldToView);
		ViewParameters.ViewToClip = FMatrix44f(Params.ProjMatrix);
		ViewParameters.ViewToWorld = FMatrix44f(View.ViewMatrices.GetInvTranslatedViewMatrix());
		ViewParameters.InvDeviceZToWorldZTransform = View.InvDeviceZToWorldZTransform;

		const float ParticleViewRadius = Proxy.GetCellSize_RenderThread() * float(Proxy.GetSimToWorld_RenderThread().GetScale3D().X) * Settings.ParticleRadiusScale;

		const ETextureCreateFlags Flags = TexCreate_ShaderResource | TexCreate_UAV;
		FRDGTextureRef DepthUint = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, PF_R32_UINT, FClearValueBinding::None, Flags), TEXT("Slosh.DepthUint"));
		FRDGTextureRef ThicknessUint = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, PF_R32_UINT, FClearValueBinding::None, Flags), TEXT("Slosh.ThicknessUint"));
		FRDGTextureRef DepthA = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, PF_R32_FLOAT, FClearValueBinding::None, Flags), TEXT("Slosh.DepthA"));
		FRDGTextureRef DepthB = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, PF_R32_FLOAT, FClearValueBinding::None, Flags), TEXT("Slosh.DepthB"));
		FRDGTextureRef Thickness = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, PF_R32_FLOAT, FClearValueBinding::None, Flags), TEXT("Slosh.Thickness"));
		FRDGTextureRef Output = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, SceneColor->Desc.Format, FClearValueBinding::None, Flags), TEXT("Slosh.Composite"));

		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DepthUint), 0xFFFFFFFFu);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(ThicknessUint), 0u);

		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		const FIntVector PixelGroups = FComputeShaderUtils::GetGroupCount(ViewSize, FComputeShaderUtils::kGolden2DGroupSize);

		{
			FRDGBufferRef Particles = GraphBuilder.RegisterExternalBuffer(Proxy.GetParticleBuffer_RenderThread());
			auto* P = GraphBuilder.AllocParameters<FSloshSplatParticlesCS::FParameters>();
			P->View = ViewParameters;
			P->NumParticles = Proxy.GetNumParticles_RenderThread();
			P->ParticleViewRadius = ParticleViewRadius;
			P->MaxSplatPixels = MaxSplatPixels;
			P->Particles = GraphBuilder.CreateSRV(Particles);
			P->SceneDepthTexture = SceneDepth;
			P->FluidDepthUint = GraphBuilder.CreateUAV(DepthUint);
			P->FluidThicknessUint = GraphBuilder.CreateUAV(ThicknessUint);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SplatParticles(%u)", P->NumParticles),
				TShaderMapRef<FSloshSplatParticlesCS>(ShaderMap), P,
				Slosh::GroupCount1D(P->NumParticles, SplatGroupSize));
		}
		{
			auto* P = GraphBuilder.AllocParameters<FSloshResolveCS::FParameters>();
			P->View = ViewParameters;
			P->DepthUintIn = DepthUint;
			P->ThicknessUintIn = ThicknessUint;
			P->DepthOut = GraphBuilder.CreateUAV(DepthA);
			P->ThicknessOut = GraphBuilder.CreateUAV(Thickness);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Resolve"), TShaderMapRef<FSloshResolveCS>(ShaderMap), P, PixelGroups);
		}
		const FIntPoint Directions[2] = { FIntPoint(1, 0), FIntPoint(0, 1) };
		FRDGTextureRef Src = DepthA;
		FRDGTextureRef Dst = DepthB;
		for (const FIntPoint& Direction : Directions)
		{
			auto* P = GraphBuilder.AllocParameters<FSloshSmoothDepthCS::FParameters>();
			P->View = ViewParameters;
			P->FilterDirection = Direction;
			P->FilterRadius = FMath::Clamp(Settings.SmoothingRadius, 0, 24);
			P->DepthFalloff = FMath::Max(ParticleViewRadius * 2.0f, 0.1f);
			P->DepthIn = Src;
			P->DepthOut = GraphBuilder.CreateUAV(Dst);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SmoothDepth"), TShaderMapRef<FSloshSmoothDepthCS>(ShaderMap), P, PixelGroups);
			Swap(Src, Dst);
		}
		{
			const FVector LightView = TranslatedWorldToView.TransformVector(FVector(Settings.LightDirectionWorld)).GetSafeNormal();
			auto* P = GraphBuilder.AllocParameters<FSloshCompositeCS::FParameters>();
			P->View = ViewParameters;
			P->LightDirectionView = FVector3f(LightView);
			P->LightColor = Settings.LightColor;
			P->AbsorptionPerMeter = Settings.AbsorptionPerMeter;
			P->ScatterColor = Settings.ScatterColor;
			P->SkyZenithColor = Settings.SkyZenithColor;
			P->SkyHorizonColor = Settings.SkyHorizonColor;
			P->RefractionStrength = Settings.RefractionStrength;
			P->SpecularPower = Settings.SpecularPower;
			P->FresnelF0 = Settings.FresnelF0;
			P->FluidDepth = Src;
			P->FluidThickness = Thickness;
			P->SceneColorTexture = SceneColor;
			P->SceneDepthTexture = SceneDepth;
			P->OutputColor = GraphBuilder.CreateUAV(Output);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Composite"), TShaderMapRef<FSloshCompositeCS>(ShaderMap), P, PixelGroups);
		}

		FRHICopyTextureInfo CopyInfo;
		CopyInfo.Size = FIntVector(ViewSize.X, ViewSize.Y, 1);
		CopyInfo.SourcePosition = FIntVector(Rect.Min.X, Rect.Min.Y, 0);
		CopyInfo.DestPosition = CopyInfo.SourcePosition;
		AddCopyTexturePass(GraphBuilder, Output, SceneColor, CopyInfo);
	}

	void RenderPostOpaque(FPostOpaqueRenderParameters& Params)
	{
		if (GSloshRenderEntries.Num() == 0 || !Params.GraphBuilder || !Params.ColorTexture || !Params.DepthTexture || !Params.View)
		{
			return;
		}
		if (Params.ColorTexture->Desc.NumSamples > 1)
		{
			return; // MSAA (forward shading) is not supported by this prototype.
		}
		// FViewInfo's only base is FSceneView; the delegate exposes it as an opaque pointer.
		const FSceneView& View = *reinterpret_cast<const FSceneView*>(Params.View);
		FSceneInterface* Scene = View.Family ? View.Family->Scene : nullptr;

		FRDGBuilder& GraphBuilder = *Params.GraphBuilder;
		RDG_EVENT_SCOPE_STAT(GraphBuilder, SloshRender, "SloshRender");
		for (int32 Index = GSloshRenderEntries.Num() - 1; Index >= 0; --Index)
		{
			const FSloshRenderEntry& Entry = GSloshRenderEntries[Index];
			FSloshProxyPtr Proxy = Entry.Proxy.Pin();
			if (!Proxy.IsValid())
			{
				GSloshRenderEntries.RemoveAtSwap(Index);
				continue;
			}
			if (!Entry.Settings.bEnabled || Entry.Scene != Scene || Proxy->GetNumParticles_RenderThread() == 0 || !Proxy->GetParticleBuffer_RenderThread().IsValid())
			{
				continue;
			}
			RenderVolume(GraphBuilder, View, Params, *Proxy, Entry.Settings);
		}
	}
}

namespace SloshRender
{
	void Startup()
	{
		check(IsInGameThread());
		if (!GPostOpaqueHandle.IsValid())
		{
			GPostOpaqueHandle = GetRendererModule().RegisterPostOpaqueRenderDelegate(FPostOpaqueRenderDelegate::CreateStatic(&RenderPostOpaque));
		}
	}

	void Shutdown()
	{
		check(IsInGameThread());
		if (GPostOpaqueHandle.IsValid())
		{
			GetRendererModule().RemovePostOpaqueRenderDelegate(GPostOpaqueHandle);
			GPostOpaqueHandle.Reset();
		}
		ENQUEUE_RENDER_COMMAND(SloshRenderClear)([](FRHICommandListImmediate&) { GSloshRenderEntries.Reset(); });
	}

	void AddOrUpdateVolume_GameThread(const FSloshProxyPtr& Proxy, FSceneInterface* Scene, const FSloshRenderSettings& Settings)
	{
		if (!Proxy.IsValid())
		{
			return;
		}
		ENQUEUE_RENDER_COMMAND(SloshRenderUpdate)(
			[Key = Proxy.Get(), Weak = FSloshProxyWeakPtr(Proxy), Scene, Settings](FRHICommandListImmediate&)
			{
				FSloshRenderEntry* Entry = GSloshRenderEntries.FindByPredicate([Key](const FSloshRenderEntry& E) { return E.Key == Key; });
				if (!Entry)
				{
					Entry = &GSloshRenderEntries.AddDefaulted_GetRef();
					Entry->Key = Key;
				}
				Entry->Proxy = Weak;
				Entry->Scene = Scene;
				Entry->Settings = Settings;
			});
	}

	void RemoveVolume_GameThread(const FSloshProxy* Proxy)
	{
		ENQUEUE_RENDER_COMMAND(SloshRenderRemove)(
			[Proxy](FRHICommandListImmediate&)
			{
				GSloshRenderEntries.RemoveAllSwap([Proxy](const FSloshRenderEntry& E) { return E.Key == Proxy; });
			});
	}
}
