// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshSkeletalColliderComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY_STATIC(LogSloshSkeletalCollider, Log, All);

namespace
{
	struct FFitShape
	{
		ESloshColliderShapeType Shape = ESloshColliderShapeType::Capsule;
		FTransform ComponentSpace;    // bind pose, mesh component space, unit scale
		float Radius = 0.0f;
		float HalfHeight = 0.0f;      // capsule: cylindrical half height
		FVector Extent = FVector::ZeroVector;   // box: half extents
	};

	// Largest eigenvector of a symmetric 3x3 matrix by power iteration.
	FVector PowerIterate(const double C[3][3], FVector Start)
	{
		FVector V = Start.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		for (int32 It = 0; It < 48; ++It)
		{
			const FVector N(
				C[0][0] * V.X + C[0][1] * V.Y + C[0][2] * V.Z,
				C[1][0] * V.X + C[1][1] * V.Y + C[1][2] * V.Z,
				C[2][0] * V.X + C[2][1] * V.Y + C[2][2] * V.Z);
			const FVector Next = N.GetSafeNormal();
			if (Next.IsNearlyZero())
			{
				break;
			}
			V = Next;
		}
		return V;
	}

	double Rayleigh(const double C[3][3], const FVector& V)
	{
		const FVector N(
			C[0][0] * V.X + C[0][1] * V.Y + C[0][2] * V.Z,
			C[1][0] * V.X + C[1][1] * V.Y + C[1][2] * V.Z,
			C[2][0] * V.X + C[2][1] * V.Y + C[2][2] * V.Z);
		return FVector::DotProduct(V, N);
	}

	float Percentile(TArray<float>& Values, float P)
	{
		if (Values.IsEmpty())
		{
			return 0.0f;
		}
		const int32 K = FMath::Clamp(FMath::RoundToInt(P * (Values.Num() - 1)), 0, Values.Num() - 1);
		Values.Sort();
		return Values[K];
	}

	// Capsule along the principal axis; a box instead when the cross-section is strongly flattened (torsos, hips).
	FFitShape FitPoints(const TArray<FVector>& Points, float RadiusPercentile, float LengthTrim)
	{
		FVector Mean = FVector::ZeroVector;
		for (const FVector& P : Points)
		{
			Mean += P;
		}
		Mean /= Points.Num();
		double C[3][3] = {};
		for (const FVector& P : Points)
		{
			const FVector D = P - Mean;
			for (int32 I = 0; I < 3; ++I)
			{
				for (int32 J = 0; J < 3; ++J)
				{
					C[I][J] += D[I] * D[J];
				}
			}
		}
		for (auto& Row : C)
		{
			for (double& X : Row)
			{
				X /= Points.Num();
			}
		}
		const int32 Big = C[0][0] >= C[1][1] && C[0][0] >= C[2][2] ? 0 : (C[1][1] >= C[2][2] ? 1 : 2);
		const FVector E1 = PowerIterate(C, FVector(C[Big][0], C[Big][1], C[Big][2]) + FVector(0.01, 0.02, 0.03));
		const double L1 = Rayleigh(C, E1);
		// Deflate and find the second axis.
		double C2[3][3];
		for (int32 I = 0; I < 3; ++I)
		{
			for (int32 J = 0; J < 3; ++J)
			{
				C2[I][J] = C[I][J] - L1 * E1[I] * E1[J];
			}
		}
		FVector Seed = FVector::CrossProduct(E1, FMath::Abs(E1.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector);
		FVector E2 = PowerIterate(C2, Seed);
		E2 = (E2 - FVector::DotProduct(E2, E1) * E1).GetSafeNormal();
		const FVector E3 = FVector::CrossProduct(E1, E2).GetSafeNormal();
		const double L2 = FMath::Max(Rayleigh(C, E2), 1e-6);
		const double L3 = FMath::Max(Rayleigh(C, E3), 1e-6);

		TArray<float> T1, T2, T3;
		T1.Reserve(Points.Num());
		T2.Reserve(Points.Num());
		T3.Reserve(Points.Num());
		for (const FVector& P : Points)
		{
			const FVector D = P - Mean;
			T1.Add(float(FVector::DotProduct(D, E1)));
			T2.Add(float(FVector::DotProduct(D, E2)));
			T3.Add(float(FVector::DotProduct(D, E3)));
		}
		TArray<float> Sorted = T1;
		const float Lo1 = Percentile(Sorted, LengthTrim), Hi1 = Percentile(Sorted, 1.0f - LengthTrim);

		FFitShape Out;
		const bool bFlat = FMath::Sqrt(L3 / L2) < 0.55 && FMath::Sqrt(L2 / L1) > 0.45;
		if (bFlat)
		{
			// Boxes overshoot an elliptical cross-section at the corners, so measure slightly inside.
			Sorted = T2;
			const float Lo2 = Percentile(Sorted, 0.06f), Hi2 = Percentile(Sorted, 0.94f);
			Sorted = T3;
			const float Lo3 = Percentile(Sorted, 0.06f), Hi3 = Percentile(Sorted, 0.94f);
			Out.Shape = ESloshColliderShapeType::Box;
			Out.Extent = FVector((Hi1 - Lo1) * 0.5, (Hi2 - Lo2) * 0.5, (Hi3 - Lo3) * 0.5);
			const FVector Center = Mean + E1 * ((Lo1 + Hi1) * 0.5) + E2 * ((Lo2 + Hi2) * 0.5) + E3 * ((Lo3 + Hi3) * 0.5);
			Out.ComponentSpace = FTransform(FRotationMatrix::MakeFromXY(E1, E2).ToQuat(), Center);
			return Out;
		}

		// Radius: distance from the axis line, using only points inside the trimmed length.
		TArray<float> Dist;
		Dist.Reserve(Points.Num());
		for (int32 I = 0; I < Points.Num(); ++I)
		{
			if (T1[I] >= Lo1 && T1[I] <= Hi1)
			{
				Dist.Add(FMath::Sqrt(T2[I] * T2[I] + T3[I] * T3[I]));
			}
		}
		const float Radius = Percentile(Dist, RadiusPercentile);
		const float HalfLength = (Hi1 - Lo1) * 0.5f;
		Out.Shape = ESloshColliderShapeType::Capsule;
		Out.Radius = Radius;
		Out.HalfHeight = FMath::Max(HalfLength - Radius, 0.0f);
		Out.ComponentSpace = FTransform(FQuat::FindBetweenNormals(FVector::UpVector, E1), Mean + E1 * ((Lo1 + Hi1) * 0.5));
		return Out;
	}
}

USloshSkeletalColliderComponent::USloshSkeletalColliderComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

USkinnedMeshComponent* USloshSkeletalColliderComponent::GetTargetMesh() const
{
	if (USkinnedMeshComponent* Parent = Cast<USkinnedMeshComponent>(GetAttachParent()))
	{
		return Parent;
	}
	return GetOwner() ? GetOwner()->FindComponentByClass<USkinnedMeshComponent>() : nullptr;
}

void USloshSkeletalColliderComponent::BeginPlay()
{
	Super::BeginPlay();
	if (Colliders.IsEmpty())
	{
		RebuildColliders();
	}
}

void USloshSkeletalColliderComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearColliders();
	Super::EndPlay(EndPlayReason);
}

void USloshSkeletalColliderComponent::ClearColliders()
{
	for (USloshColliderComponent* Collider : Colliders)
	{
		if (Collider)
		{
			Collider->DestroyComponent();
		}
	}
	Colliders.Reset();
}

int32 USloshSkeletalColliderComponent::RebuildColliders()
{
	ClearColliders();
	USkinnedMeshComponent* Skinned = GetTargetMesh();
	const USkinnedAsset* Asset = Skinned ? Skinned->GetSkinnedAsset() : nullptr;
	FSkeletalMeshRenderData* RenderData = Skinned ? Skinned->GetSkeletalMeshRenderData() : nullptr;
	if (!Asset || !RenderData || RenderData->LODRenderData.IsEmpty())
	{
		UE_LOG(LogSloshSkeletalCollider, Warning, TEXT("%s: no skinned mesh / render data to fit"), *GetPathName());
		return 0;
	}
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
	const FSkinWeightVertexBuffer* Weights = Skinned->GetSkinWeightBuffer(0);
	const FPositionVertexBuffer& PositionBuffer = LOD.StaticVertexBuffers.PositionVertexBuffer;
	if (!Weights || PositionBuffer.GetNumVertices() == 0 || !PositionBuffer.GetVertexData() || Weights->GetNumVertices() == 0)
	{
		UE_LOG(LogSloshSkeletalCollider, Warning, TEXT("%s: LOD0 vertex data is not CPU-readable; enable Allow CPU Access on the mesh"), *GetPathName());
		return 0;
	}

	const FReferenceSkeleton& RefSkeleton = Asset->GetRefSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();
	const TArray<FTransform>& RefPose = RefSkeleton.GetRefBonePose();

	// Bind pose in component space; parents always precede children in the reference skeleton.
	TArray<FTransform> BindCS;
	TArray<bool> Excluded;
	BindCS.SetNum(NumBones);
	Excluded.Init(false, NumBones);
	for (int32 B = 0; B < NumBones; ++B)
	{
		const int32 Parent = RefSkeleton.GetParentIndex(B);
		BindCS[B] = Parent >= 0 ? RefPose[B] * BindCS[Parent] : RefPose[B];
		Excluded[B] = ExcludeBones.Contains(RefSkeleton.GetBoneName(B)) || (Parent >= 0 && Excluded[Parent]);
	}

	// Each vertex belongs to its most-weighted bone.
	const int32 NumVertices = int32(PositionBuffer.GetNumVertices());
	const uint32 MaxInfluences = Weights->GetMaxBoneInfluences();
	TArray<int32> Owner;
	Owner.Init(INDEX_NONE, NumVertices);
	TArray<int32> Count;
	Count.Init(0, NumBones);
	for (const FSkelMeshRenderSection& Section : LOD.RenderSections)
	{
		const int32 End = FMath::Min(int32(Section.BaseVertexIndex + Section.NumVertices), NumVertices);
		for (int32 V = int32(Section.BaseVertexIndex); V < End; ++V)
		{
			const FSkinWeightInfo Info = Weights->GetVertexSkinWeights(uint32(V));
			int32 Best = INDEX_NONE;
			uint16 BestWeight = 0;
			for (uint32 K = 0; K < MaxInfluences; ++K)
			{
				if (Info.InfluenceWeights[K] > BestWeight && Info.InfluenceBones[K] < Section.BoneMap.Num())
				{
					BestWeight = Info.InfluenceWeights[K];
					Best = Section.BoneMap[Info.InfluenceBones[K]];
				}
			}
			if (Best >= 0 && Best < NumBones && !Excluded[Best])
			{
				Owner[V] = Best;
				++Count[Best];
			}
		}
	}

	// Small bones hand their vertices to the parent (children have higher indices, so one descending pass suffices).
	const int32 MinVertices = FMath::Max(12, FMath::CeilToInt(MinVertexFraction * NumVertices));
	TArray<int32> Assign;
	Assign.SetNum(NumBones);
	for (int32 B = 0; B < NumBones; ++B)
	{
		Assign[B] = B;
	}
	for (int32 B = NumBones - 1; B > 0; --B)
	{
		const int32 Parent = RefSkeleton.GetParentIndex(B);
		if (Count[B] > 0 && Count[B] < MinVertices && Parent >= 0)
		{
			Count[Parent] += Count[B];
			Count[B] = 0;
			Assign[B] = Parent;
		}
	}
	TArray<int32> Final;
	Final.SetNum(NumBones);
	for (int32 B = 0; B < NumBones; ++B)
	{
		Final[B] = Assign[B] == B ? B : Final[Assign[B]];
	}

	// Keep the bones with the most vertices.
	TArray<int32> Candidates;
	for (int32 B = 0; B < NumBones; ++B)
	{
		if (Assign[B] == B && Count[B] >= MinVertices)
		{
			Candidates.Add(B);
		}
	}
	Candidates.Sort([&Count](int32 A, int32 B) { return Count[A] > Count[B]; });
	Candidates.SetNum(FMath::Min(Candidates.Num(), MaxCapsules));
	TArray<int32> Slot;
	Slot.Init(INDEX_NONE, NumBones);
	for (int32 I = 0; I < Candidates.Num(); ++I)
	{
		Slot[Candidates[I]] = I;
	}
	TArray<TArray<FVector>> Points;
	Points.SetNum(Candidates.Num());
	for (int32 V = 0; V < NumVertices; ++V)
	{
		if (Owner[V] != INDEX_NONE)
		{
			const int32 S = Slot[Final[Owner[V]]];
			if (S != INDEX_NONE)
			{
				Points[S].Add(FVector(PositionBuffer.VertexPosition(uint32(V))));
			}
		}
	}

	FString Summary;
	for (int32 I = 0; I < Candidates.Num(); ++I)
	{
		if (Points[I].Num() < 4)
		{
			continue;
		}
		const int32 Bone = Candidates[I];
		const FName BoneName = RefSkeleton.GetBoneName(Bone);
		const FFitShape Fit = FitPoints(Points[I], RadiusPercentile, LengthTrim);

		USloshColliderComponent* Collider = NewObject<USloshColliderComponent>(GetOwner(), MakeUniqueObjectName(GetOwner(), USloshColliderComponent::StaticClass(), *FString::Printf(TEXT("SloshBone_%s"), *BoneName.ToString())));
		Collider->CreationMethod = EComponentCreationMethod::Instance;
		Collider->Shape = Fit.Shape;
		Collider->Radius = FMath::Max(Fit.Radius * RadiusScale, MinRadius);
		Collider->CapsuleHalfHeight = Fit.HalfHeight;
		Collider->BoxExtent = (Fit.Extent * RadiusScale).ComponentMax(FVector(MinRadius));
		Collider->CouplingMode = CouplingMode;
		Collider->bReportFeedbackWhenKinematic = bReportWetness;
		Collider->EnterThreshold = 0.02f;
		Collider->SetupAttachment(Skinned, BoneName);
		Collider->SetRelativeTransform(Fit.ComponentSpace.GetRelativeTransform(BindCS[Bone]));
		Collider->RegisterComponent();
		Colliders.Add(Collider);
		Summary += FString::Printf(TEXT(" %s(%s %d v, r=%.1f h=%.1f)"), *BoneName.ToString(), Fit.Shape == ESloshColliderShapeType::Box ? TEXT("box") : TEXT("cap"),
			Points[I].Num(), Fit.Shape == ESloshColliderShapeType::Box ? float(Collider->BoxExtent.GetMin()) : Collider->Radius,
			Fit.Shape == ESloshColliderShapeType::Box ? float(Collider->BoxExtent.GetMax()) : Collider->CapsuleHalfHeight);
	}
	UE_LOG(LogSloshSkeletalCollider, Display, TEXT("%s: %d colliders from %d vertices (min %d per bone):%s"), *GetNameSafe(Skinned->GetSkinnedAsset()),
		Colliders.Num(), NumVertices, MinVertices, *Summary);
	return Colliders.Num();
}

void USloshSkeletalColliderComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bDrawCapsules || !GetWorld())
	{
		return;
	}
	for (const USloshColliderComponent* Collider : Colliders)
	{
		if (!Collider)
		{
			continue;
		}
		const FTransform T = Collider->GetComponentTransform();
		const float S = T.GetScale3D().GetAbsMax();
		if (Collider->Shape == ESloshColliderShapeType::Box)
		{
			DrawDebugBox(GetWorld(), T.GetLocation(), Collider->BoxExtent * S, T.GetRotation(), FColor::Cyan, false, -1.0f, 0, 0.3f);
		}
		else
		{
			DrawDebugCapsule(GetWorld(), T.GetLocation(), (Collider->CapsuleHalfHeight + Collider->Radius) * S, Collider->Radius * S, T.GetRotation(), FColor::Cyan, false, -1.0f, 0, 0.3f);
		}
	}
}
