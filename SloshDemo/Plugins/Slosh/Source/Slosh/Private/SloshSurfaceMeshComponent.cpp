// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshSurfaceMeshComponent.h"
#include "SloshSurfaceMesh.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveViewRelevance.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "SceneManagement.h"
#include "SceneInterface.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"

namespace
{
	class FSloshSurfaceMeshSceneProxy final : public FPrimitiveSceneProxy
	{
	public:
		FSloshSurfaceMeshSceneProxy(USloshSurfaceMeshComponent* Component, UMaterialInterface* InMaterial)
			: FPrimitiveSceneProxy(Component)
			, Buffers(Component->GetBuffers())
			, Material(InMaterial ? InMaterial : UMaterial::GetDefaultMaterial(MD_Surface))
			, MaterialRelevance(Material->GetRelevance_Concurrent(GetScene().GetShaderPlatform()))
		{
		}

		virtual SIZE_T GetTypeHash() const override
		{
			static size_t UniquePointer;
			return reinterpret_cast<size_t>(&UniquePointer);
		}

		virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

		virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
		{
			FPrimitiveViewRelevance Result;
			Result.bDrawRelevance = IsShown(View);
			Result.bShadowRelevance = IsShadowCast(View);
			Result.bDynamicRelevance = true;
			Result.bRenderInMainPass = ShouldRenderInMainPass();
			Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
			Result.bRenderCustomDepth = ShouldRenderCustomDepth();
			MaterialRelevance.SetPrimitiveViewRelevance(Result);
			Result.bVelocityRelevance = false;
			return Result;
		}

		virtual bool CanBeOccluded() const override { return false; }

		virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap,
			FMeshElementCollector& Collector) const override
		{
			if (!Buffers.IsValid() || !Buffers->HasMesh() || !Buffers->GetVertexFactory() || !Buffers->GetIndexBuffer() || !Buffers->GetIndirectArgsBuffer())
			{
				return;
			}
			for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
			{
				if ((VisibilityMap & (1u << ViewIndex)) == 0)
				{
					continue;
				}
				FMeshBatch& Mesh = Collector.AllocateMesh();
				FMeshBatchElement& Element = Mesh.Elements[0];
				Element.IndexBuffer = Buffers->GetIndexBuffer();
				Mesh.VertexFactory = Buffers->GetVertexFactory();
				Mesh.MaterialRenderProxy = Material->GetRenderProxy();

				FDynamicPrimitiveUniformBuffer& PrimitiveUniformBuffer = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
				FPrimitiveUniformShaderParametersBuilder Builder;
				BuildUniformShaderParameters(Builder);
				PrimitiveUniformBuffer.Set(Collector.GetRHICommandList(), Builder);
				Element.PrimitiveUniformBufferResource = &PrimitiveUniformBuffer.UniformBuffer;

				// Triangle count comes from the GPU (surface nets) through the indirect arguments.
				Element.FirstIndex = 0;
				Element.NumPrimitives = 0;
				Element.IndirectArgsBuffer = Buffers->GetIndirectArgsBuffer();
				Element.IndirectArgsOffset = 0;
				Element.MinVertexIndex = 0;
				Element.MaxVertexIndex = Buffers->GetMaxVertices() - 1;
				Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
				Mesh.Type = PT_TriangleList;
				Mesh.DepthPriorityGroup = SDPG_World;
				Mesh.bCanApplyViewModeOverrides = false;
				Collector.AddMesh(ViewIndex, Mesh);
			}
		}

	private:
		FSloshSurfaceMeshBuffersPtr Buffers;
		UMaterialInterface* Material;
		FMaterialRelevance MaterialRelevance;
	};
}

USloshSurfaceMeshComponent::USloshSurfaceMeshComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	CastShadow = true;
	bUseAsOccluder = false;
}

void USloshSurfaceMeshComponent::Configure(const FSloshSurfaceMeshBuffersPtr& InBuffers, const FVector& InHalfExtent, UMaterialInterface* InMaterial)
{
	const bool bChanged = Buffers != InBuffers || SurfaceMaterial != InMaterial || !HalfExtent.Equals(InHalfExtent);
	Buffers = InBuffers;
	HalfExtent = InHalfExtent;
	SurfaceMaterial = InMaterial;
	if (bChanged)
	{
		UpdateBounds();
		MarkRenderStateDirty();
	}
}

FPrimitiveSceneProxy* USloshSurfaceMeshComponent::CreateSceneProxy()
{
	return Buffers.IsValid() ? new FSloshSurfaceMeshSceneProxy(this, GetMaterial(0)) : nullptr;
}

FBoxSphereBounds USloshSurfaceMeshComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	return FBoxSphereBounds(FBox(-HalfExtent, HalfExtent)).TransformBy(LocalToWorld);
}

void USloshSurfaceMeshComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	if (UMaterialInterface* Material = GetMaterial(0))
	{
		OutMaterials.Add(Material);
	}
}

UMaterialInterface* USloshSurfaceMeshComponent::GetMaterial(int32 ElementIndex) const
{
	return SurfaceMaterial ? SurfaceMaterial.Get() : UMaterial::GetDefaultMaterial(MD_Surface);
}

void USloshSurfaceMeshComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* InMaterial)
{
	if (SurfaceMaterial != InMaterial)
	{
		SurfaceMaterial = InMaterial;
		MarkRenderStateDirty();
	}
}
