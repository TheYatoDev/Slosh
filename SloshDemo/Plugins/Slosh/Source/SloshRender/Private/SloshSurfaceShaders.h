// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "SloshRenderShaders.h"

// Shaders in SloshSurface.usf: density field, raymarched surface and surface-nets mesh.

class FSloshDensitySplatCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshDensitySplatCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshDensitySplatCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, FieldDim)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(float, InvVoxelSize)
		SHADER_PARAMETER(float, SupportScale)
		SHADER_PARAMETER(float, MaxFootprint)
		SHADER_PARAMETER(float, ParticleRadiusVoxels)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, EllipsoidsIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, FieldAccum)
	END_SHADER_PARAMETER_STRUCT()
};

class FSloshFieldResolveCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshFieldResolveCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshFieldResolveCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntVector, FieldDim)
		SHADER_PARAMETER(FIntVector, BrickDim)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, FieldAccumIn)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, FieldOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BrickOccupancy)
	END_SHADER_PARAMETER_STRUCT()
};

BEGIN_SHADER_PARAMETER_STRUCT(FSloshFieldParameters, )
	SHADER_PARAMETER(FIntVector, FieldDim)
	SHADER_PARAMETER(FIntVector, BrickDim)
	SHADER_PARAMETER(float, IsoLevel)
	SHADER_PARAMETER(float, InvVoxelSize)
	SHADER_PARAMETER_RDG_TEXTURE(Texture3D<float>, Field)
	SHADER_PARAMETER_SAMPLER(SamplerState, FieldSampler)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BrickOccupancyIn)
END_SHADER_PARAMETER_STRUCT()

class FSloshRaymarchCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshRaymarchCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshRaymarchCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FSloshFieldParameters, FieldParams)
		SHADER_PARAMETER(FIntPoint, ViewRectMin)
		SHADER_PARAMETER(FIntPoint, ViewSize)
		SHADER_PARAMETER(FMatrix44f, SimToView)
		SHADER_PARAMETER(FMatrix44f, ViewToSim)
		SHADER_PARAMETER(FMatrix44f, ViewToClip)
		SHADER_PARAMETER(FVector4f, InvDeviceZToWorldZTransform)
		SHADER_PARAMETER(uint32, MaxSteps)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, DepthOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, ThicknessOut)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, NormalOut)
	END_SHADER_PARAMETER_STRUCT()
};

class FSloshSurfaceNetsVerticesCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshSurfaceNetsVerticesCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshSurfaceNetsVerticesCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FSloshFieldParameters, FieldParams)
		SHADER_PARAMETER(uint32, MaxVertices)
		SHADER_PARAMETER(float, VoxelSizeCm)
		SHADER_PARAMETER(FVector3f, LocalOffsetCm)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellVertex)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Counters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, PositionsOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, TangentsOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float2>, TexCoordsOut)
	END_SHADER_PARAMETER_STRUCT()
};

class FSloshSurfaceNetsFacesCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshSurfaceNetsFacesCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshSurfaceNetsFacesCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FSloshFieldParameters, FieldParams)
		SHADER_PARAMETER(uint32, MaxIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellVertex)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Counters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, IndicesOut)
	END_SHADER_PARAMETER_STRUCT()
};

class FSloshSurfaceNetsArgsCS : public FSloshRenderShaderBase
{
public:
	DECLARE_GLOBAL_SHADER(FSloshSurfaceNetsArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FSloshSurfaceNetsArgsCS, FSloshRenderShaderBase);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MaxIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Counters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, ArgsOut)
	END_SHADER_PARAMETER_STRUCT()
};
