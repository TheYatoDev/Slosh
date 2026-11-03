// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshPaintLibrary.h"
#include "SloshPaintComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "UObject/UObjectIterator.h"

namespace
{
	// CPU-skinned positions are reused for every ray against the same component within a frame.
	struct FSkinnedCache
	{
		TWeakObjectPtr<USkinnedMeshComponent> Component;
		uint64 Frame = ~0ull;
		TArray<FVector3f> Positions;
	};
	FSkinnedCache GSkinnedCache;

	bool RayTriangle(const FVector& O, const FVector& D, const FVector& A, const FVector& B, const FVector& C, double& OutT, double& OutU, double& OutV)
	{
		const FVector E1 = B - A;
		const FVector E2 = C - A;
		const FVector P = D.Cross(E2);
		const double Det = E1.Dot(P);
		if (FMath::Abs(Det) < 1e-12)
		{
			return false;
		}
		const double InvDet = 1.0 / Det;
		const FVector S = O - A;
		const double U = S.Dot(P) * InvDet;
		if (U < 0.0 || U > 1.0)
		{
			return false;
		}
		const FVector Q = S.Cross(E1);
		const double V = D.Dot(Q) * InvDet;
		if (V < 0.0 || U + V > 1.0)
		{
			return false;
		}
		OutT = E2.Dot(Q) * InvDet;
		OutU = U;
		OutV = V;
		return OutT > 0.0;
	}
}

bool USloshPaintLibrary::TraceSkinnedSurface(USkinnedMeshComponent* Component, FVector Start, FVector End, FVector& OutLocation, FVector& OutNormal, FName& OutBone, float& OutDistance)
{
	if (!Component || !Component->IsRegistered())
	{
		return false;
	}
	FSkeletalMeshRenderData* RenderData = Component->GetSkeletalMeshRenderData();
	if (!RenderData || RenderData->LODRenderData.IsEmpty())
	{
		return false;
	}
	const int32 LODIndex = 0;
	FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[LODIndex];
	FSkinWeightVertexBuffer* Weights = Component->GetSkinWeightBuffer(LODIndex);
	const FRawStaticIndexBuffer16or32Interface* Indices = LOD.MultiSizeIndexContainer.GetIndexBuffer();
	const uint32 NumVertices = LOD.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
	if (!Weights || !Indices || Indices->Num() == 0 || NumVertices == 0 || !LOD.StaticVertexBuffers.PositionVertexBuffer.GetVertexData())
	{
		return false;   // render data not CPU-accessible
	}

	if (GSkinnedCache.Component.Get() != Component || GSkinnedCache.Frame != GFrameCounter)
	{
		TArray<FMatrix44f> RefToLocals;
		Component->CacheRefToLocalMatrices(RefToLocals);
		GSkinnedCache.Component = Component;
		GSkinnedCache.Frame = GFrameCounter;
		GSkinnedCache.Positions.SetNumUninitialized(NumVertices);
		for (uint32 V = 0; V < NumVertices; ++V)
		{
			GSkinnedCache.Positions[V] = USkinnedMeshComponent::GetSkinnedVertexPosition(Component, int32(V), LOD, *Weights, RefToLocals);
		}
	}
	const TArray<FVector3f>& Positions = GSkinnedCache.Positions;

	const FTransform& ToWorld = Component->GetComponentTransform();
	const FVector O = ToWorld.InverseTransformPosition(Start);
	FVector D = ToWorld.InverseTransformVector(End - Start);
	const double RayLength = D.Length();
	if (RayLength < 1e-6)
	{
		return false;
	}
	D /= RayLength;

	double BestT = RayLength;
	int32 BestSection = INDEX_NONE;
	uint32 BestTri[3] = { 0, 0, 0 };
	double BestU = 0.0;
	double BestV = 0.0;
	for (int32 SectionIndex = 0; SectionIndex < LOD.RenderSections.Num(); ++SectionIndex)
	{
		const FSkelMeshRenderSection& Section = LOD.RenderSections[SectionIndex];
		if (Section.bDisabled)
		{
			continue;
		}
		for (uint32 Tri = 0; Tri < Section.NumTriangles; ++Tri)
		{
			const uint32 I0 = Indices->Get(Section.BaseIndex + Tri * 3 + 0);
			const uint32 I1 = Indices->Get(Section.BaseIndex + Tri * 3 + 1);
			const uint32 I2 = Indices->Get(Section.BaseIndex + Tri * 3 + 2);
			double T, U, V;
			if (RayTriangle(O, D, FVector(Positions[I0]), FVector(Positions[I1]), FVector(Positions[I2]), T, U, V) && T < BestT)
			{
				BestT = T;
				BestSection = SectionIndex;
				BestTri[0] = I0;
				BestTri[1] = I1;
				BestTri[2] = I2;
				BestU = U;
				BestV = V;
			}
		}
	}
	if (BestSection == INDEX_NONE)
	{
		return false;
	}

	const FVector A(Positions[BestTri[0]]);
	const FVector B(Positions[BestTri[1]]);
	const FVector C(Positions[BestTri[2]]);
	FVector Normal = (B - A).Cross(C - A).GetSafeNormal();
	if (Normal.Dot(D) > 0.0)
	{
		Normal = -Normal;
	}
	OutLocation = ToWorld.TransformPosition(O + D * BestT);
	OutNormal = ToWorld.TransformVectorNoScale(Normal).GetSafeNormal();
	OutDistance = float(ToWorld.TransformVector(D * BestT).Length());

	// Dominant bone of the vertex closest to the hit (largest barycentric weight).
	const double Bary[3] = { 1.0 - BestU - BestV, BestU, BestV };
	const int32 Corner = Bary[0] >= Bary[1] && Bary[0] >= Bary[2] ? 0 : (Bary[1] >= Bary[2] ? 1 : 2);
	const uint32 Vertex = BestTri[Corner];
	const FSkelMeshRenderSection& Section = LOD.RenderSections[BestSection];
	uint16 BestWeight = 0;
	int32 BestBone = INDEX_NONE;
	for (uint32 Influence = 0; Influence < Weights->GetMaxBoneInfluences(); ++Influence)
	{
		const uint16 Weight = Weights->GetBoneWeight(Vertex, Influence);
		if (Weight > BestWeight)
		{
			const uint32 Local = Weights->GetBoneIndex(Vertex, Influence);
			if (Section.BoneMap.IsValidIndex(int32(Local)))
			{
				BestWeight = Weight;
				BestBone = Section.BoneMap[Local];
			}
		}
	}
	OutBone = BestBone != INDEX_NONE ? Component->GetBoneName(BestBone) : NAME_None;
	return true;
}

FSloshPaintHit USloshPaintLibrary::TracePaintSurface(const UObject* WorldContextObject, FVector Start, FVector End,
	TEnumAsByte<ECollisionChannel> TraceChannel, const TArray<AActor*>& ActorsToIgnore, bool bIncludeSkinnedWithoutCollision)
{
	FSloshPaintHit Result;
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	if (!World)
	{
		return Result;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(SloshPaintTrace), true);
	Params.AddIgnoredActors(ActorsToIgnore);
	FHitResult Hit;
	double BestDistance = TNumericLimits<double>::Max();
	if (World->LineTraceSingleByChannel(Hit, Start, End, TraceChannel, Params) && Hit.GetComponent())
	{
		Result.bHit = true;
		Result.Location = Hit.ImpactPoint;
		Result.Normal = Hit.ImpactNormal;
		Result.Component = Hit.GetComponent();
		Result.Bone = Hit.BoneName;
		Result.Distance = Hit.Distance;
		BestDistance = Hit.Distance;
	}

	// Skinned meshes: hit the rendered skin, not the physics capsules (and find meshes without collision).
	for (TObjectIterator<USkinnedMeshComponent> It; It; ++It)
	{
		USkinnedMeshComponent* Skinned = *It;
		if (Skinned->GetWorld() != World || !Skinned->IsRegistered() || !Skinned->IsVisible() || ActorsToIgnore.Contains(Skinned->GetOwner()))
		{
			continue;
		}
		const bool bWasHit = Result.Component == Skinned;
		if (!bWasHit && !bIncludeSkinnedWithoutCollision)
		{
			continue;
		}
		const FBox Box = Skinned->Bounds.GetBox();
		FVector HitPoint, HitNormal;
		float HitTime;
		if (!FMath::LineExtentBoxIntersection(Box, Start, End, FVector::ZeroVector, HitPoint, HitNormal, HitTime) && !Box.IsInside(Start))
		{
			continue;
		}
		FVector Location, Normal;
		FName Bone;
		float Distance;
		if (TraceSkinnedSurface(Skinned, Start, End, Location, Normal, Bone, Distance) && (Distance < BestDistance || bWasHit))
		{
			Result.bHit = true;
			Result.Location = Location;
			Result.Normal = Normal;
			Result.Component = Skinned;
			Result.Bone = Bone;
			Result.Distance = Distance;
			BestDistance = Distance;
		}
	}
	return Result;
}

int32 USloshPaintLibrary::PaintDab(const FSloshPaintHit& Hit, USloshPaintPreset* Preset, float Pressure)
{
	if (!Hit.bHit || !Hit.Component || !Preset)
	{
		return 0;
	}
	USloshPaintComponent* Paint = USloshPaintComponent::FindOrAddPaintComponent(Hit.Component);
	if (!Paint)
	{
		return 0;
	}
	const int32 Id = Paint->BeginStroke(Preset, Hit.Location, Hit.Normal, Pressure, Hit.Bone);
	Paint->EndStroke(Id);
	return Id;
}
