// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

BEGIN_SHADER_PARAMETER_STRUCT(FSloshRenderViewParameters, )
	SHADER_PARAMETER(FIntPoint, ViewRectMin)
	SHADER_PARAMETER(FIntPoint, ViewSize)
	SHADER_PARAMETER(FMatrix44f, SimToView)
	SHADER_PARAMETER(FMatrix44f, ViewToClip)
	SHADER_PARAMETER(FMatrix44f, ViewToWorld)
	SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
END_SHADER_PARAMETER_STRUCT()

class FSloshRenderShaderBase : public FGlobalShader
{
public:
	FSloshRenderShaderBase() = default;
	FSloshRenderShaderBase(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FSloshSplatParticlesCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshSplatParticlesCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshSplatParticlesCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FSloshRenderViewParameters, View)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(float, ParticleViewRadius)
		SHADER_PARAMETER(float, MaxSplatPixels)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSloshParticlePacked>, Particles)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, FluidDepthUint)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, FluidThicknessUint)
	END_SHADER_PARAMETER_STRUCT()
};

class FSloshAnisoBinCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshAnisoBinCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshAnisoBinCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(float, BinSize)
		SHADER_PARAMETER(uint32, TableMask)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSloshParticlePacked>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, HashKeys)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, HashMoments)
	END_SHADER_PARAMETER_STRUCT()
};

class FSloshAnisoComputeCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshAnisoComputeCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshAnisoComputeCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(float, BinSize)
		SHADER_PARAMETER(uint32, TableMask)
		SHADER_PARAMETER(float, SplatRadiusSim)
		SHADER_PARAMETER(float, AnisoStrength)
		SHADER_PARAMETER(float, AnisoMaxStretch)
		SHADER_PARAMETER(float, AnisoSmoothing)
		SHADER_PARAMETER(float, AnisoMinNeighbours)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSloshParticlePacked>, Particles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, HashKeys)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, HashMoments)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, Ellipsoids)
	END_SHADER_PARAMETER_STRUCT()
};

class FSloshSplatEllipsoidsCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshSplatEllipsoidsCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshSplatEllipsoidsCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FSloshRenderViewParameters, View)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(float, MaxSplatPixels)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, EllipsoidsIn)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, FluidDepthUint)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, FluidThicknessUint)
	END_SHADER_PARAMETER_STRUCT()
};

class FSloshResolveCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshResolveCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshResolveCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FSloshRenderViewParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, DepthUintIn)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, ThicknessUintIn)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DepthOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, ThicknessOut)
	END_SHADER_PARAMETER_STRUCT()
};

class FSloshSmoothDepthCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshSmoothDepthCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshSmoothDepthCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FSloshRenderViewParameters, View)
		SHADER_PARAMETER(FIntPoint, FilterDirection)
		SHADER_PARAMETER(int32, FilterRadius)
		SHADER_PARAMETER(float, RadiusPerViewZ)
		SHADER_PARAMETER(float, DepthFalloff)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, DepthIn)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, GuideDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DepthOut)
	END_SHADER_PARAMETER_STRUCT()
};

class FSloshCompositeCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshCompositeCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshCompositeCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FSloshRenderViewParameters, View)
		SHADER_PARAMETER(FVector3f, LightDirectionView)
		SHADER_PARAMETER(FVector3f, LightColor)
		SHADER_PARAMETER(FVector3f, AbsorptionPerMeter)
		SHADER_PARAMETER(FVector3f, ScatterColor)
		SHADER_PARAMETER(FVector3f, SkyZenithColor)
		SHADER_PARAMETER(FVector3f, SkyHorizonColor)
		SHADER_PARAMETER(float, RefractionStrength)
		SHADER_PARAMETER(float, SpecularPower)
		SHADER_PARAMETER(float, FresnelF0)
		SHADER_PARAMETER(float, EdgeFadeThickness)
		SHADER_PARAMETER(uint32, bHasFluidNormal)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, FluidNormal)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, FluidDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, FluidThickness)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputColor)
	END_SHADER_PARAMETER_STRUCT()
};
