// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshSurfaceMesh.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "Rendering/ColorVertexBuffer.h"

namespace
{
	// Wrappers that expose a pooled RDG buffer through the engine's vertex/index buffer resource types.
	class FSloshExternalVertexBuffer : public FVertexBuffer
	{
	public:
		FBufferRHIRef Source;
		virtual void InitRHI(FRHICommandListBase&) override { VertexBufferRHI = Source; }
	};

	class FSloshExternalIndexBuffer : public FIndexBuffer
	{
	public:
		FBufferRHIRef Source;
		virtual void InitRHI(FRHICommandListBase&) override { IndexBufferRHI = Source; }
	};

	FShaderResourceViewRHIRef MakeTypedSRV(FRHICommandListBase& RHICmdList, FRHIBuffer* Buffer, EPixelFormat Format)
	{
		return RHICmdList.CreateShaderResourceView(Buffer,
			FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(Format));
	}
}

struct FSloshSurfaceMeshBuffers::FResources
{
	FSloshExternalVertexBuffer PositionVB;
	FSloshExternalVertexBuffer TangentVB;
	FSloshExternalVertexBuffer TexCoordVB;
	FSloshExternalIndexBuffer IndexBuffer;
	FShaderResourceViewRHIRef PositionSRV;
	FShaderResourceViewRHIRef TangentSRV;
	FShaderResourceViewRHIRef TexCoordSRV;
};

FSloshSurfaceMeshBuffersPtr FSloshSurfaceMeshBuffers::Create(uint32 MaxVertices)
{
	return FSloshSurfaceMeshBuffersPtr(new FSloshSurfaceMeshBuffers(MaxVertices), [](FSloshSurfaceMeshBuffers* Buffers)
	{
		if (IsInRenderingThread())
		{
			delete Buffers;
			return;
		}
		ENQUEUE_RENDER_COMMAND(SloshDeleteSurfaceMesh)([Buffers](FRHICommandListImmediate&) { delete Buffers; });
	});
}

FSloshSurfaceMeshBuffers::FSloshSurfaceMeshBuffers(uint32 InMaxVertices)
	: MaxVertices(FMath::Clamp<uint32>(InMaxVertices, 1024u, 8u * 1024u * 1024u))
{
}

FSloshSurfaceMeshBuffers::~FSloshSurfaceMeshBuffers()
{
	ReleaseRenderResources();
}

const FIndexBuffer* FSloshSurfaceMeshBuffers::GetIndexBuffer() const
{
	return Resources.IsValid() ? &Resources->IndexBuffer : nullptr;
}

void FSloshSurfaceMeshBuffers::ReleaseRenderResources()
{
	if (VertexFactory.IsValid())
	{
		VertexFactory->ReleaseResource();
		VertexFactory.Reset();
	}
	if (Resources.IsValid())
	{
		Resources->PositionVB.ReleaseResource();
		Resources->TangentVB.ReleaseResource();
		Resources->TexCoordVB.ReleaseResource();
		Resources->IndexBuffer.ReleaseResource();
		Resources.Reset();
	}
	Positions.SafeRelease();
	Tangents.SafeRelease();
	TexCoords.SafeRelease();
	Indices.SafeRelease();
	Args.SafeRelease();
}

void FSloshSurfaceMeshBuffers::EnsureInitialized(FRHICommandListBase& RHICmdList, ERHIFeatureLevel::Type FeatureLevel)
{
	check(IsInRenderingThread());
	if (VertexFactory.IsValid())
	{
		return;
	}
	const uint32 MaxIndices = GetMaxIndices();
	const EBufferUsageFlags VertexUsage = EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::UnorderedAccess;

	FRDGBufferDesc PositionDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(float), MaxVertices * 3u);
	PositionDesc.Usage |= VertexUsage;
	Positions = AllocatePooledBuffer(PositionDesc, TEXT("Slosh.SurfacePositions"));

	FRDGBufferDesc TangentDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaxVertices * 2u);
	TangentDesc.Usage |= VertexUsage;
	Tangents = AllocatePooledBuffer(TangentDesc, TEXT("Slosh.SurfaceTangents"));

	FRDGBufferDesc TexCoordDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(FVector2f), MaxVertices);
	TexCoordDesc.Usage |= VertexUsage;
	TexCoords = AllocatePooledBuffer(TexCoordDesc, TEXT("Slosh.SurfaceTexCoords"));

	FRDGBufferDesc IndexDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaxIndices);
	IndexDesc.Usage |= EBufferUsageFlags::IndexBuffer | EBufferUsageFlags::ShaderResource | EBufferUsageFlags::UnorderedAccess;
	Indices = AllocatePooledBuffer(IndexDesc, TEXT("Slosh.SurfaceIndices"));

	Args = AllocatePooledBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDrawIndexedIndirectParameters>(1), TEXT("Slosh.SurfaceArgs"));

	Resources = MakeUnique<FResources>();
	Resources->PositionVB.Source = Positions->GetRHI();
	Resources->TangentVB.Source = Tangents->GetRHI();
	Resources->TexCoordVB.Source = TexCoords->GetRHI();
	Resources->IndexBuffer.Source = Indices->GetRHI();
	Resources->PositionVB.InitResource(RHICmdList);
	Resources->TangentVB.InitResource(RHICmdList);
	Resources->TexCoordVB.InitResource(RHICmdList);
	Resources->IndexBuffer.InitResource(RHICmdList);
	Resources->PositionSRV = MakeTypedSRV(RHICmdList, Positions->GetRHI(), PF_R32_FLOAT);
	Resources->TangentSRV = MakeTypedSRV(RHICmdList, Tangents->GetRHI(), PF_R8G8B8A8_SNORM);
	Resources->TexCoordSRV = MakeTypedSRV(RHICmdList, TexCoords->GetRHI(), PF_G32R32F);

	VertexFactory = MakeUnique<FLocalVertexFactory>(FeatureLevel, "SloshSurfaceMesh");
	FLocalVertexFactory::FDataType Data;
	Data.PositionComponent = FVertexStreamComponent(&Resources->PositionVB, 0, sizeof(FVector3f), VET_Float3);
	Data.PositionComponentSRV = Resources->PositionSRV;
	Data.TangentBasisComponents[0] = FVertexStreamComponent(&Resources->TangentVB, 0, 2 * sizeof(uint32), VET_PackedNormal);
	Data.TangentBasisComponents[1] = FVertexStreamComponent(&Resources->TangentVB, sizeof(uint32), 2 * sizeof(uint32), VET_PackedNormal);
	Data.TangentsSRV = Resources->TangentSRV;
	Data.TextureCoordinates.Add(FVertexStreamComponent(&Resources->TexCoordVB, 0, sizeof(FVector2f), VET_Float2));
	Data.TextureCoordinatesSRV = Resources->TexCoordSRV;
	Data.NumTexCoords = 1;
	Data.LightMapCoordinateIndex = 0;
	FColorVertexBuffer::BindDefaultColorVertexBuffer(VertexFactory.Get(), Data, FColorVertexBuffer::NullBindStride::ZeroForDefaultBufferBind);
	VertexFactory->SetData(RHICmdList, Data);
	VertexFactory->InitResource(RHICmdList);
}
