// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshRenderer.h"
#include "SloshRenderShaders.h"
#include "SloshSurfaceShaders.h"
#include "SloshSurfaceMesh.h"
#include "SceneViewExtension.h"
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
IMPLEMENT_GLOBAL_SHADER(FSloshAnisoBinCS, SLOSH_RENDER_USF, "AnisoBinCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshAnisoComputeCS, SLOSH_RENDER_USF, "AnisoComputeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshSplatEllipsoidsCS, SLOSH_RENDER_USF, "SplatEllipsoidsCS", SF_Compute);

#define SLOSH_SURFACE_USF "/Plugin/Slosh/Private/SloshSurface.usf"
IMPLEMENT_GLOBAL_SHADER(FSloshDensitySplatCS, SLOSH_SURFACE_USF, "DensitySplatCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshFieldResolveCS, SLOSH_SURFACE_USF, "FieldResolveCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshRaymarchCS, SLOSH_SURFACE_USF, "RaymarchCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshSurfaceNetsVerticesCS, SLOSH_SURFACE_USF, "SurfaceNetsVerticesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshSurfaceNetsFacesCS, SLOSH_SURFACE_USF, "SurfaceNetsFacesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshSurfaceNetsArgsCS, SLOSH_SURFACE_USF, "SurfaceNetsArgsCS", SF_Compute);
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

	// Per-particle ellipsoids (centre + three axes, sim metres). Strength 0 gives spheres of the splat radius.
	FRDGBufferRef AddEllipsoidPasses(FRDGBuilder& GraphBuilder, FGlobalShaderMap* ShaderMap, const FSloshProxy& Proxy,
		const FSloshRenderSettings& Settings, float Strength)
	{
		const uint32 NumParticles = Proxy.GetNumParticles_RenderThread();
		FRDGBufferRef Particles = GraphBuilder.RegisterExternalBuffer(Proxy.GetParticleBuffer_RenderThread());
		const float CellMeters = Proxy.GetCellSize_RenderThread();
		// Open-addressed table at most half full: a power of two >= 2 x particles.
		const uint32 TableSize = FMath::Clamp(FMath::RoundUpToPowerOfTwo(NumParticles * 2u), 4096u, 1u << 22);
		FRDGBufferRef HashKeys = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TableSize), TEXT("Slosh.AnisoKeys"));
		FRDGBufferRef HashMoments = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TableSize * 10u), TEXT("Slosh.AnisoMoments"));
		FRDGBufferRef Ellipsoids = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), NumParticles * 4u), TEXT("Slosh.Ellipsoids"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(HashKeys), 0xFFFFFFFFu);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(HashMoments), 0u);
		const FIntVector ParticleGroups = Slosh::GroupCount1D(NumParticles, SplatGroupSize);
		{
			auto* P = GraphBuilder.AllocParameters<FSloshAnisoBinCS::FParameters>();
			P->NumParticles = NumParticles;
			P->BinSize = CellMeters;
			P->TableMask = TableSize - 1u;
			P->Particles = GraphBuilder.CreateSRV(Particles);
			P->HashKeys = GraphBuilder.CreateUAV(HashKeys);
			P->HashMoments = GraphBuilder.CreateUAV(HashMoments);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("AnisoBin"), TShaderMapRef<FSloshAnisoBinCS>(ShaderMap), P, ParticleGroups);
		}
		{
			auto* P = GraphBuilder.AllocParameters<FSloshAnisoComputeCS::FParameters>();
			P->NumParticles = NumParticles;
			P->BinSize = CellMeters;
			P->TableMask = TableSize - 1u;
			P->SplatRadiusSim = CellMeters * Settings.ParticleRadiusScale;
			P->AnisoStrength = FMath::Clamp(Strength, 0.0f, 1.0f);
			P->AnisoMaxStretch = FMath::Clamp(Settings.AnisotropyMaxStretch, 1.0f, 8.0f);
			P->AnisoSmoothing = FMath::Clamp(Settings.AnisotropySmoothing, 0.0f, 1.0f);
			// Weighted count: a full interior neighbourhood is ~125 at 8 particles per cell. Sparse spray (falling
			// columns, lone droplets) stays round instead of stretching into rods.
			P->AnisoMinNeighbours = 35.0f;
			P->Particles = GraphBuilder.CreateSRV(Particles);
			P->HashKeys = GraphBuilder.CreateUAV(HashKeys);
			P->HashMoments = GraphBuilder.CreateUAV(HashMoments);
			P->Ellipsoids = GraphBuilder.CreateUAV(Ellipsoids);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("AnisoCompute"), TShaderMapRef<FSloshAnisoComputeCS>(ShaderMap), P, ParticleGroups);
		}
		return Ellipsoids;
	}

	struct FSloshDensityField
	{
		FRDGTextureRef Field = nullptr;
		FRDGBufferRef BrickOccupancy = nullptr;
		FIntVector Dim = FIntVector::ZeroValue;
		FIntVector BrickDim = FIntVector::ZeroValue;
		float VoxelMeters = 0.0f;
	};

	// Particle surface field on a voxel grid covering the domain (Zhu & Bridson; 0.5 = particle-radius surface).
	bool BuildDensityField(FRDGBuilder& GraphBuilder, FGlobalShaderMap* ShaderMap, const FSloshProxy& Proxy,
		const FSloshRenderSettings& Settings, FSloshDensityField& Out)
	{
		const uint32 NumParticles = Proxy.GetNumParticles_RenderThread();
		const FIntVector GridDim = Proxy.GetGridDim_RenderThread();
		if (NumParticles == 0 || GridDim.X <= 1 || GridDim.Y <= 1 || GridDim.Z <= 1)
		{
			return false;
		}
		const float Resolution = FMath::Clamp(Settings.FieldResolution, 0.5f, 3.0f);
		const float CellMeters = Proxy.GetCellSize_RenderThread();
		Out.VoxelMeters = CellMeters / Resolution;
		Out.Dim = FIntVector(FMath::CeilToInt(GridDim.X * Resolution), FMath::CeilToInt(GridDim.Y * Resolution), FMath::CeilToInt(GridDim.Z * Resolution));
		Out.BrickDim = FIntVector(FMath::DivideAndRoundUp(Out.Dim.X, 8), FMath::DivideAndRoundUp(Out.Dim.Y, 8), FMath::DivideAndRoundUp(Out.Dim.Z, 8));
		const uint32 NumVoxels = uint32(Out.Dim.X) * Out.Dim.Y * Out.Dim.Z;
		const uint32 NumBricks = uint32(Out.BrickDim.X) * Out.BrickDim.Y * Out.BrickDim.Z;

		FRDGBufferRef Ellipsoids = AddEllipsoidPasses(GraphBuilder, ShaderMap, Proxy, Settings, Settings.Anisotropy);
		// Five fixed-point moments per voxel: weight, weighted particle offset (xyz), weighted radius.
		FRDGBufferRef Accum = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), NumVoxels * 5u), TEXT("Slosh.FieldAccum"));
		Out.BrickOccupancy = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumBricks), TEXT("Slosh.FieldBricks"));
		Out.Field = GraphBuilder.CreateTexture(FRDGTextureDesc::Create3D(Out.Dim, PF_R16F, FClearValueBinding::None,
			TexCreate_ShaderResource | TexCreate_UAV), TEXT("Slosh.Field"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Accum), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Out.BrickOccupancy), 0u);

		const float SupportScale = FMath::Clamp(Settings.SurfaceSmoothing, 0.5f, 6.0f);
		{
			auto* P = GraphBuilder.AllocParameters<FSloshDensitySplatCS::FParameters>();
			P->FieldDim = Out.Dim;
			P->NumParticles = NumParticles;
			P->InvVoxelSize = 1.0f / Out.VoxelMeters;
			P->SupportScale = SupportScale;
			P->MaxFootprint = 4.0f;
			P->ParticleRadiusVoxels = Settings.ParticleRadiusScale * Resolution;
			P->EllipsoidsIn = GraphBuilder.CreateSRV(Ellipsoids);
			P->FieldAccum = GraphBuilder.CreateUAV(Accum);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("DensitySplat"), TShaderMapRef<FSloshDensitySplatCS>(ShaderMap), P,
				Slosh::GroupCount1D(NumParticles, SplatGroupSize));
		}
		{
			auto* P = GraphBuilder.AllocParameters<FSloshFieldResolveCS::FParameters>();
			P->FieldDim = Out.Dim;
			P->BrickDim = Out.BrickDim;
			P->FieldAccumIn = GraphBuilder.CreateSRV(Accum);
			P->FieldOut = GraphBuilder.CreateUAV(Out.Field);
			P->BrickOccupancy = GraphBuilder.CreateUAV(Out.BrickOccupancy);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("FieldResolve"), TShaderMapRef<FSloshFieldResolveCS>(ShaderMap), P,
				FComputeShaderUtils::GetGroupCount(Out.Dim, FIntVector(4, 4, 4)));
		}
		return true;
	}

	void FillFieldParameters(FRDGBuilder& GraphBuilder, const FSloshDensityField& Field, const FSloshRenderSettings& Settings, FSloshFieldParameters& P)
	{
		P.FieldDim = Field.Dim;
		P.BrickDim = Field.BrickDim;
		P.IsoLevel = FMath::Clamp(Settings.IsoLevel, 0.02f, 0.98f);
		P.InvVoxelSize = 1.0f / Field.VoxelMeters;
		P.Field = Field.Field;
		P.FieldSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		P.BrickOccupancyIn = GraphBuilder.CreateSRV(Field.BrickOccupancy);
	}

	// Mesh mode: surface nets into the volume's persistent vertex/index buffers, before the base pass draws them.
	void BuildSurfaceMesh(FRDGBuilder& GraphBuilder, const FSloshProxy& Proxy, const FSloshRenderSettings& Settings, FSloshSurfaceMeshBuffers& Mesh)
	{
		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		Mesh.EnsureInitialized(GraphBuilder.RHICmdList, GMaxRHIFeatureLevel);

		FRDGBufferRef Positions = GraphBuilder.RegisterExternalBuffer(Mesh.Positions);
		FRDGBufferRef Tangents = GraphBuilder.RegisterExternalBuffer(Mesh.Tangents);
		FRDGBufferRef TexCoords = GraphBuilder.RegisterExternalBuffer(Mesh.TexCoords);
		FRDGBufferRef Indices = GraphBuilder.RegisterExternalBuffer(Mesh.Indices);
		FRDGBufferRef Args = GraphBuilder.RegisterExternalBuffer(Mesh.Args);
		FRDGBufferRef Counters = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 2), TEXT("Slosh.SurfaceCounters"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Counters), 0u);

		FSloshDensityField Field;
		if (BuildDensityField(GraphBuilder, ShaderMap, Proxy, Settings, Field))
		{
			const FIntVector CellDim = Field.Dim - FIntVector(1);
			FRDGBufferRef CellVertex = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32),
				uint32(CellDim.X) * CellDim.Y * CellDim.Z), TEXT("Slosh.SurfaceCellVertex"));
			const FIntVector CellGroups = FComputeShaderUtils::GetGroupCount(CellDim, FIntVector(4, 4, 4));
			{
				auto* P = GraphBuilder.AllocParameters<FSloshSurfaceNetsVerticesCS::FParameters>();
				FillFieldParameters(GraphBuilder, Field, Settings, P->FieldParams);
				P->MaxVertices = Mesh.GetMaxVertices();
				P->VoxelSizeCm = Field.VoxelMeters * 100.0f;
				P->LocalOffsetCm = Settings.LocalOffsetCm;
				P->CellVertex = GraphBuilder.CreateUAV(CellVertex);
				P->Counters = GraphBuilder.CreateUAV(Counters);
				P->PositionsOut = GraphBuilder.CreateUAV(Positions, PF_R32_FLOAT);
				P->TangentsOut = GraphBuilder.CreateUAV(Tangents, PF_R8G8B8A8_SNORM);
				P->TexCoordsOut = GraphBuilder.CreateUAV(TexCoords, PF_G32R32F);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SurfaceNetsVertices"), TShaderMapRef<FSloshSurfaceNetsVerticesCS>(ShaderMap), P, CellGroups);
			}
			{
				auto* P = GraphBuilder.AllocParameters<FSloshSurfaceNetsFacesCS::FParameters>();
				FillFieldParameters(GraphBuilder, Field, Settings, P->FieldParams);
				P->MaxIndices = Mesh.GetMaxIndices();
				P->CellVertex = GraphBuilder.CreateUAV(CellVertex);
				P->Counters = GraphBuilder.CreateUAV(Counters);
				P->IndicesOut = GraphBuilder.CreateUAV(Indices, PF_R32_UINT);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SurfaceNetsFaces"), TShaderMapRef<FSloshSurfaceNetsFacesCS>(ShaderMap), P, CellGroups);
			}
		}
		{
			auto* P = GraphBuilder.AllocParameters<FSloshSurfaceNetsArgsCS::FParameters>();
			P->MaxIndices = Mesh.GetMaxIndices();
			P->Counters = GraphBuilder.CreateUAV(Counters);
			P->ArgsOut = GraphBuilder.CreateUAV(Args, PF_R32_UINT);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SurfaceNetsArgs"), TShaderMapRef<FSloshSurfaceNetsArgsCS>(ShaderMap), P, FIntVector(1, 1, 1));
		}
		// The base pass reads these outside RDG (mesh draw commands), so hand them over in read-only states.
		GraphBuilder.UseExternalAccessMode(Positions, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
		GraphBuilder.UseExternalAccessMode(Tangents, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
		GraphBuilder.UseExternalAccessMode(TexCoords, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
		GraphBuilder.UseExternalAccessMode(Indices, ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask);
		GraphBuilder.UseExternalAccessMode(Args, ERHIAccess::IndirectArgs | ERHIAccess::SRVMask);
		Mesh.MarkBuilt(GFrameCounterRenderThread);
	}

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
		const FMatrix SimToViewD = SimToTranslatedWorld * TranslatedWorldToView;
		ViewParameters.SimToView = FMatrix44f(SimToViewD);
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

		const bool bRaymarch = Settings.SurfaceMode == ESloshSurfaceRenderMode::Raymarch;
		FRDGTextureRef Src = DepthA;
		FRDGTextureRef Dst = DepthB;
		FRDGTextureRef Normals = nullptr;
		const uint32 NumParticles = Proxy.GetNumParticles_RenderThread();
		if (bRaymarch)
		{
			FSloshDensityField Field;
			Normals = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, PF_FloatRGBA, FClearValueBinding::None, Flags), TEXT("Slosh.SurfaceNormal"));
			if (BuildDensityField(GraphBuilder, ShaderMap, Proxy, Settings, Field))
			{
				auto* P = GraphBuilder.AllocParameters<FSloshRaymarchCS::FParameters>();
				FillFieldParameters(GraphBuilder, Field, Settings, P->FieldParams);
				P->ViewRectMin = Rect.Min;
				P->ViewSize = ViewSize;
				P->SimToView = FMatrix44f(SimToViewD);
				P->ViewToSim = FMatrix44f(SimToViewD.Inverse());
				P->ViewToClip = FMatrix44f(Params.ProjMatrix);
				P->InvDeviceZToWorldZTransform = View.InvDeviceZToWorldZTransform;
				P->MaxSteps = 768;
				P->SceneDepthTexture = SceneDepth;
				P->DepthOut = GraphBuilder.CreateUAV(DepthA);
				P->ThicknessOut = GraphBuilder.CreateUAV(Thickness);
				P->NormalOut = GraphBuilder.CreateUAV(Normals);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("RaymarchSurface"), TShaderMapRef<FSloshRaymarchCS>(ShaderMap), P,
					FComputeShaderUtils::GetGroupCount(ViewSize, FIntPoint(8, 8)));
			}
			else
			{
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DepthA), 3.0e38f);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Thickness), 0.0f);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Normals), FVector4f(0, 0, -1, 0));
			}
		}
		else if (Settings.Anisotropy > 0.0f && NumParticles > 0)
		{
			FRDGBufferRef Ellipsoids = AddEllipsoidPasses(GraphBuilder, ShaderMap, Proxy, Settings, Settings.Anisotropy);
			const FIntVector ParticleGroups = Slosh::GroupCount1D(NumParticles, SplatGroupSize);
			{
				auto* P = GraphBuilder.AllocParameters<FSloshSplatEllipsoidsCS::FParameters>();
				P->View = ViewParameters;
				P->NumParticles = NumParticles;
				P->MaxSplatPixels = MaxSplatPixels;
				P->EllipsoidsIn = GraphBuilder.CreateSRV(Ellipsoids);
				P->SceneDepthTexture = SceneDepth;
				P->FluidDepthUint = GraphBuilder.CreateUAV(DepthUint);
				P->FluidThicknessUint = GraphBuilder.CreateUAV(ThicknessUint);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SplatEllipsoids(%u)", NumParticles),
					TShaderMapRef<FSloshSplatEllipsoidsCS>(ShaderMap), P, ParticleGroups);
			}
		}
		else
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
		if (!bRaymarch)
		{
		{
			auto* P = GraphBuilder.AllocParameters<FSloshResolveCS::FParameters>();
			P->View = ViewParameters;
			P->DepthUintIn = DepthUint;
			P->ThicknessUintIn = ThicknessUint;
			P->DepthOut = GraphBuilder.CreateUAV(DepthA);
			P->ThicknessOut = GraphBuilder.CreateUAV(Thickness);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Resolve"), TShaderMapRef<FSloshResolveCS>(ShaderMap), P, PixelGroups);
		}
		// Smoothing footprint: SmoothingScale splat radii, converted to pixels per unit view depth.
		const float RadiusPerViewZ = Settings.SmoothingScale > 0.0f
			? Settings.SmoothingScale * ParticleViewRadius * float(Params.ProjMatrix.M[0][0]) * 0.5f * float(ViewSize.X) : 0.0f;
		const int32 MaxRadius = FMath::Clamp(Settings.SmoothingRadius, 0, 32);
		const FIntPoint Directions[2] = { FIntPoint(1, 0), FIntPoint(0, 1) };
		auto AddSmooth = [&](FRDGTextureRef In, FRDGTextureRef Guide, FRDGTextureRef Out, const FIntPoint& Direction, float Falloff)
		{
			auto* P = GraphBuilder.AllocParameters<FSloshSmoothDepthCS::FParameters>();
			P->View = ViewParameters;
			P->FilterDirection = Direction;
			P->FilterRadius = MaxRadius;
			P->RadiusPerViewZ = RadiusPerViewZ;
			P->bSlopeAware = Settings.SmoothingScale > 0.0f ? 1u : 0u;
			P->DepthFalloff = Falloff;
			P->DepthIn = In;
			P->GuideDepth = Guide;
			P->DepthOut = GraphBuilder.CreateUAV(Out);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SmoothSurface"), TShaderMapRef<FSloshSmoothDepthCS>(ShaderMap), P, PixelGroups);
		};
		if (MaxRadius > 0)
		{
			for (int32 Iteration = 0; Iteration < FMath::Clamp(Settings.SmoothingIterations, 1, 4); ++Iteration)
			{
				for (const FIntPoint& Direction : Directions)
				{
					AddSmooth(Src, Src, Dst, Direction, FMath::Max(ParticleViewRadius * 2.0f, 0.1f));
					Swap(Src, Dst);
				}
			}
			// Adaptive mode only: thickness drives the colour, so blur it too and single splats do not show as spots.
			if (Settings.SmoothingScale > 0.0f)
			{
			FRDGTextureRef ThicknessTmp = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Extent, PF_R32_FLOAT, FClearValueBinding::None, Flags), TEXT("Slosh.ThicknessTmp"));
			AddSmooth(Thickness, Src, ThicknessTmp, Directions[0], 0.0f);
			AddSmooth(ThicknessTmp, Src, Thickness, Directions[1], 0.0f);
			}
		}
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
			P->EdgeFadeThickness = Settings.EdgeFadeThickness;
			P->ScatterLighting = FMath::Clamp(Settings.ScatterLighting, 0.0f, 1.0f);
			if (!Normals)
			{
				Normals = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_FloatRGBA, FClearValueBinding::None, Flags), TEXT("Slosh.NoNormal"));
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Normals), FVector4f(0, 0, -1, 0));
				P->bHasFluidNormal = 0u;
			}
			else
			{
				P->bHasFluidNormal = 1u;
			}
			P->FluidNormal = Normals;
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
			if (!Entry.Settings.bEnabled || Entry.Settings.SurfaceMode == ESloshSurfaceRenderMode::Mesh || Entry.Scene != Scene
				|| Proxy->GetNumParticles_RenderThread() == 0 || !Proxy->GetParticleBuffer_RenderThread().IsValid())
			{
				continue;
			}
			RenderVolume(GraphBuilder, View, Params, *Proxy, Entry.Settings);
		}
	}
}

namespace
{
	// Builds surface meshes before the scene renders (and so before the base pass draws them).
	class FSloshSceneViewExtension final : public FSceneViewExtensionBase
	{
	public:
		explicit FSloshSceneViewExtension(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}

		virtual void SetupViewFamily(FSceneViewFamily&) override {}
		virtual void SetupView(FSceneViewFamily&, FSceneView&) override {}
		virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}

		virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& Family) override
		{
			for (const FSloshRenderEntry& Entry : GSloshRenderEntries)
			{
				if (Entry.Settings.SurfaceMode != ESloshSurfaceRenderMode::Mesh || !Entry.Settings.bEnabled || !Entry.Settings.MeshBuffers.IsValid()
					|| Entry.Scene != Family.Scene)
				{
					continue;
				}
				FSloshProxyPtr Proxy = Entry.Proxy.Pin();
				FSloshSurfaceMeshBuffers& Mesh = *Entry.Settings.MeshBuffers;
				if (!Proxy.IsValid() || !Proxy->GetParticleBuffer_RenderThread().IsValid() || Mesh.GetLastBuiltFrame() == GFrameCounterRenderThread)
				{
					continue;
				}
				RDG_EVENT_SCOPE_STAT(GraphBuilder, SloshRender, "SloshSurfaceMesh");
				BuildSurfaceMesh(GraphBuilder, *Proxy, Entry.Settings, Mesh);
			}
		}
	};

	TSharedPtr<FSloshSceneViewExtension, ESPMode::ThreadSafe> GSloshViewExtension;
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
		if (!GSloshViewExtension.IsValid())
		{
			GSloshViewExtension = FSceneViewExtensions::NewExtension<FSloshSceneViewExtension>();
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
		GSloshViewExtension.Reset();
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
