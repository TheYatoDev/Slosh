// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshPaintComponent.h"
#include "SloshPaintPreset.h"
#include "SloshPaintMesher.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Tasks/Task.h"

using namespace Slosh::Paint;

USloshPaintPreset::FOnPresetChanged USloshPaintPreset::OnPresetChanged;

#if WITH_EDITOR
void USloshPaintPreset::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	OnPresetChanged.Broadcast(this);
}
#endif

/** Cached geometry of one stroke: one piece per bone run (a single piece for non-skeletal paint). */
struct FSloshPaintStrokeCache
{
	uint32 Key = 0;
	struct FPiece
	{
		FName Bone;
		FStrokeResult Result;
	};
	// Heap-allocated: FDynamicMesh3's attribute set points back at its mesh, and UE containers relocate elements bitwise.
	TArray<TUniquePtr<FPiece>> Pieces;
};

struct USloshPaintComponent::FBuildState
{
	struct FStrokeJobSet
	{
		int32 StrokeId = 0;
		uint32 Key = 0;
		TArray<FName> Bones;
		TArray<FStrokeJob> Jobs;   // one per bone run
	};

	// Inputs (game thread -> worker)
	TArray<FStrokeJobSet> Jobs;
	TArray<TSharedPtr<const FSloshPaintStrokeCache>> Reused;
	FBuildSettings Settings;
	TArray<TObjectPtr<UMaterialInterface>> Materials;   // slot table
	TSet<FName> ShadowGroups;

	// Outputs (worker -> game thread)
	TMap<int32, TSharedPtr<const FSloshPaintStrokeCache>> Built;
	struct FGroup
	{
		UE::Geometry::FDynamicMesh3 Mesh;
		TMap<TPair<int32, bool>, TArray<FTransform>> Layers;   // (material slot, rods) -> instances
	};
	TMap<FName, TUniquePtr<FGroup>> Groups;   // heap-allocated for the same reason as FSloshPaintStrokeCache::Pieces

	UE::Tasks::FTask Task;
};

namespace
{
	FPresetParams MakeParams(const USloshPaintPreset& Preset, TFunctionRef<int32(UMaterialInterface*)> SlotOf)
	{
		FPresetParams P;
		P.UnitCm = Preset.GetUnitCm();
		P.Scale = Preset.Scale;
		P.Thickness = Preset.Thickness;
		P.Roughness = Preset.Roughness;
		P.Flatness = Preset.Flatness;
		P.Offset = Preset.Offset;
		P.Fading = Preset.Fading;
		P.MaterialUVScale = Preset.MaterialUVScale;
		P.Quality = Preset.Quality;
		P.SmoothIterations = Preset.SmoothIterations;
		P.DropMode = uint8(Preset.DropMode);
		P.DropLength = Preset.DropLength;
		P.DropSize = Preset.DropSize;
		P.DropExponent = Preset.DropExponent;
		P.DropMove = Preset.DropMove;
		P.bDropMoveRandom = Preset.bDropMoveRandom;
		P.DropMoveSeed = Preset.DropMoveSeed;
		P.DropSpeed = Preset.DropSpeed;
		P.bDropSpeedRandom = Preset.bDropSpeedRandom;
		P.DropSpeedSeed = Preset.DropSpeedSeed;
		const FSloshPaintLayer* Layers[2] = { &Preset.LayerA, &Preset.LayerB };
		for (int32 I = 0; I < 2; ++I)
		{
			FLayerParams& L = P.Layers[I];
			L.bEnabled = Layers[I]->bEnabled && Layers[I]->Material != nullptr;
			L.bRods = Layers[I]->bRods;
			L.bInside = Layers[I]->bInside;
			L.Size = Layers[I]->Size;
			L.SizeRandom = Layers[I]->SizeRandom;
			L.Amount = Layers[I]->Amount;
			L.Shift = Layers[I]->Shift;
			L.Seed = Layers[I]->Seed;
			L.MaterialSlot = L.bEnabled ? SlotOf(Layers[I]->Material) : INDEX_NONE;
		}
		return P;
	}

	UStaticMesh* LayerMesh(bool bRods)
	{
		static TSoftObjectPtr<UStaticMesh> Sphere(FSoftObjectPath(TEXT("/Engine/BasicShapes/Sphere.Sphere")));
		static TSoftObjectPtr<UStaticMesh> Cylinder(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
		return bRods ? Cylinder.LoadSynchronous() : Sphere.LoadSynchronous();
	}
}

USloshPaintComponent::USloshPaintComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	bTickInEditor = true;
	bAutoActivate = true;
}

USloshPaintComponent* USloshPaintComponent::FindOrAddPaintComponent(USceneComponent* Target)
{
	if (!Target || !Target->GetOwner())
	{
		return nullptr;
	}
	for (USceneComponent* Child : Target->GetAttachChildren())
	{
		if (USloshPaintComponent* Existing = Cast<USloshPaintComponent>(Child))
		{
			return Existing;
		}
	}
	AActor* Owner = Target->GetOwner();
	const bool bEditorWorld = Owner->GetWorld() && !Owner->GetWorld()->IsGameWorld();
	USloshPaintComponent* Paint = NewObject<USloshPaintComponent>(Owner, MakeUniqueObjectName(Owner, StaticClass(), TEXT("SloshPaint")),
		bEditorWorld ? RF_Transactional : RF_NoFlags);
	Paint->SetupAttachment(Target);
	if (bEditorWorld)
	{
		Paint->CreationMethod = EComponentCreationMethod::Instance;
		Owner->AddInstanceComponent(Paint);
	}
	Paint->RegisterComponent();
	return Paint;
}

USkinnedMeshComponent* USloshPaintComponent::GetSkinnedTarget() const
{
	return Cast<USkinnedMeshComponent>(GetAttachParent());
}

FTransform USloshPaintComponent::GetStrokeSpaceTransform(FName Bone) const
{
	if (!Bone.IsNone())
	{
		if (const USkinnedMeshComponent* Skinned = GetSkinnedTarget())
		{
			if (Skinned->GetBoneIndex(Bone) != INDEX_NONE)
			{
				return Skinned->GetSocketTransform(Bone, RTS_World);
			}
		}
	}
	return GetComponentTransform();
}

FSloshPaintPoint USloshPaintComponent::MakePoint(const FVector& WorldLocation, const FVector& WorldNormal, float Pressure, FName Bone) const
{
	FSloshPaintPoint Point;
	Point.Bone = (GetSkinnedTarget() && !Bone.IsNone() && GetSkinnedTarget()->GetBoneIndex(Bone) != INDEX_NONE) ? Bone : NAME_None;
	const FTransform Space = GetStrokeSpaceTransform(Point.Bone);
	Point.Position = Space.InverseTransformPosition(WorldLocation);
	Point.Normal = Space.InverseTransformVectorNoScale(WorldNormal.GetSafeNormal(1e-6, FVector::UpVector)).GetSafeNormal();
	Point.Pressure = FMath::Clamp(Pressure, 0.1f, 5.0f);
	return Point;
}

FSloshPaintStroke* USloshPaintComponent::FindStroke(int32 StrokeId)
{
	return Strokes.FindByPredicate([StrokeId](const FSloshPaintStroke& S) { return S.Id == StrokeId; });
}

int32 USloshPaintComponent::BeginStroke(USloshPaintPreset* Preset, FVector WorldLocation, FVector WorldNormal, float Pressure, FName Bone)
{
	FSloshPaintStroke& Stroke = Strokes.AddDefaulted_GetRef();
	Stroke.Preset = Preset;
	Stroke.Id = NextStrokeId++;
	Stroke.Seed = FMath::Rand();
	Stroke.Points.Add(MakePoint(WorldLocation, WorldNormal, Pressure, Bone));
	MarkDirty();
	return Stroke.Id;
}

bool USloshPaintComponent::AddStrokePoint(int32 StrokeId, FVector WorldLocation, FVector WorldNormal, float Pressure, FName Bone)
{
	FSloshPaintStroke* Stroke = FindStroke(StrokeId);
	if (!Stroke)
	{
		return false;
	}
	Stroke->Points.Add(MakePoint(WorldLocation, WorldNormal, Pressure, Bone));
	++Stroke->Revision;
	MarkDirty();
	return true;
}

void USloshPaintComponent::EndStroke(int32 StrokeId)
{
	if (FSloshPaintStroke* Stroke = FindStroke(StrokeId))
	{
		++Stroke->Revision;
		MarkDirty();
	}
}

int32 USloshPaintComponent::AddStroke(USloshPaintPreset* Preset, const TArray<FVector>& WorldLocations, const TArray<FVector>& WorldNormals, float Pressure, FName Bone)
{
	if (WorldLocations.IsEmpty())
	{
		return 0;
	}
	auto NormalAt = [&](int32 I) { return WorldNormals.IsValidIndex(I) ? WorldNormals[I] : (WorldNormals.IsEmpty() ? FVector::UpVector : WorldNormals[0]); };
	const int32 Id = BeginStroke(Preset, WorldLocations[0], NormalAt(0), Pressure, Bone);
	FSloshPaintStroke* Stroke = FindStroke(Id);
	for (int32 I = 1; I < WorldLocations.Num(); ++I)
	{
		Stroke->Points.Add(MakePoint(WorldLocations[I], NormalAt(I), Pressure, Bone));
	}
	EndStroke(Id);
	return Id;
}

bool USloshPaintComponent::RemoveStroke(int32 StrokeId)
{
	const int32 Removed = Strokes.RemoveAll([StrokeId](const FSloshPaintStroke& S) { return S.Id == StrokeId; });
	Cache.Remove(StrokeId);
	if (Removed > 0)
	{
		MarkDirty();
	}
	return Removed > 0;
}

void USloshPaintComponent::UndoLastStroke()
{
	if (!Strokes.IsEmpty())
	{
		RemoveStroke(Strokes.Last().Id);
	}
}

void USloshPaintComponent::ClearStrokes()
{
	Strokes.Reset();
	Cache.Reset();
	MarkDirty();
}

void USloshPaintComponent::SetReveal(float NewReveal)
{
	NewReveal = FMath::Clamp(NewReveal, 0.0f, 1.0f);
	if (NewReveal != Reveal)
	{
		Reveal = NewReveal;
		MarkDirty();
	}
}

void USloshPaintComponent::SetDropTime(float NewDropTime)
{
	if (NewDropTime != DropTime)
	{
		DropTime = NewDropTime;
		if (HasAnimatedDrops())
		{
			MarkDirty();
		}
	}
}

void USloshPaintComponent::Rebuild()
{
	NotifyStrokesChanged(true);
}

void USloshPaintComponent::NotifyStrokesChanged(bool bClearCache)
{
	if (bClearCache)
	{
		Cache.Reset();
	}
	for (FSloshPaintStroke& Stroke : Strokes)
	{
		NextStrokeId = FMath::Max(NextStrokeId, Stroke.Id + 1);
	}
	MarkDirty();
}

bool USloshPaintComponent::HasAnimatedDrops() const
{
	return Strokes.ContainsByPredicate([](const FSloshPaintStroke& S) { return S.Preset && S.Preset->DropMode == ESloshPaintDropMode::Animated; });
}

void USloshPaintComponent::OnRegister()
{
	Super::OnRegister();
	PresetChangedHandle = USloshPaintPreset::OnPresetChanged.AddUObject(this, &USloshPaintComponent::HandlePresetChanged);
	// Ids must be unique; strokes added through the details panel start at 0.
	TSet<int32> Seen;
	for (FSloshPaintStroke& Stroke : Strokes)
	{
		if (Stroke.Id <= 0 || Seen.Contains(Stroke.Id))
		{
			Stroke.Id = NextStrokeId++;
		}
		Seen.Add(Stroke.Id);
		NextStrokeId = FMath::Max(NextStrokeId, Stroke.Id + 1);
	}
	MarkDirty();
}

void USloshPaintComponent::OnUnregister()
{
	USloshPaintPreset::OnPresetChanged.Remove(PresetChangedHandle);
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (Pending.IsValid())
	{
		Pending->Task.Wait();
		Pending.Reset();
	}
	DestroyGeneratedComponents();
	Super::OnUnregister();
}

void USloshPaintComponent::PostLoad()
{
	Super::PostLoad();
	for (const FSloshPaintStroke& Stroke : Strokes)
	{
		NextStrokeId = FMath::Max(NextStrokeId, Stroke.Id + 1);
	}
}

#if WITH_EDITOR
void USloshPaintComponent::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	NotifyStrokesChanged(true);
}

void USloshPaintComponent::PostEditUndo()
{
	Super::PostEditUndo();
	NotifyStrokesChanged(true);
}
#endif

void USloshPaintComponent::HandlePresetChanged(USloshPaintPreset* Preset)
{
	if (Strokes.ContainsByPredicate([Preset](const FSloshPaintStroke& S) { return S.Preset == Preset; }))
	{
		MarkDirty();   // cache keys include the preset's parameters
	}
}

void USloshPaintComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const UWorld* World = GetWorld();
	if (bAnimateDrops && World && World->IsGameWorld() && HasAnimatedDrops())
	{
		DropTime += DeltaTime * DropTimeScale;
		MarkDirty();
	}
}

void USloshPaintComponent::MarkDirty()
{
	bDirty = true;
	if (!Pending.IsValid() && IsRegistered())
	{
		StartBuild();
	}
}

void USloshPaintComponent::StartBuild()
{
	bDirty = false;
	TSharedPtr<FBuildState> State = MakeShared<FBuildState>();
	State->Settings.Reveal = Reveal;
	State->Settings.DropTime = DropTime;
	State->Settings.QualityScale = QualityScale;

	auto SlotOf = [&State](UMaterialInterface* Material)
	{
		int32 Index = State->Materials.IndexOfByKey(Material);
		if (Index == INDEX_NONE)
		{
			Index = State->Materials.Add(Material);
		}
		return Index;
	};

	TMap<int32, TSharedPtr<const FSloshPaintStrokeCache>> NewCache;
	for (const FSloshPaintStroke& Stroke : Strokes)
	{
		if (!Stroke.bVisible || !Stroke.Preset || Stroke.Points.IsEmpty())
		{
			continue;
		}
		const USloshPaintPreset& Preset = *Stroke.Preset;
		FPresetParams Params = MakeParams(Preset, SlotOf);
		Params.UnitCm *= FMath::Max(UnitScale, 0.01f);
		const int32 FluidSlot = SlotOf(Preset.Material);
		const bool bAnimated = Preset.DropMode == ESloshPaintDropMode::Animated;

		uint32 Key = HashCombineFast(GetTypeHash(Stroke.Revision), GetTypeHash(Stroke.Points.Num()));
		Key = HashCombineFast(Key, Params.Hash());
		Key = HashCombineFast(Key, GetTypeHash(FluidSlot));
		Key = HashCombineFast(Key, GetTypeHash(Stroke.Seed));
		Key = HashCombineFast(Key, GetTypeHash(Reveal));
		Key = HashCombineFast(Key, GetTypeHash(QualityScale));
		if (bAnimated)
		{
			Key = HashCombineFast(Key, GetTypeHash(DropTime));
		}
		if (Preset.bCastShadows)
		{
			for (const FSloshPaintPoint& Point : Stroke.Points)
			{
				State->ShadowGroups.Add(Point.Bone);
			}
		}

		if (const TSharedPtr<const FSloshPaintStrokeCache>* Hit = Cache.Find(Stroke.Id); Hit && (*Hit)->Key == Key)
		{
			State->Reused.Add(*Hit);
			NewCache.Add(Stroke.Id, *Hit);
			continue;
		}

		// Split into runs of points that share a bone; neighbouring runs overlap by one point (converted with the
		// current pose) so pieces meet without a gap.
		FBuildState::FStrokeJobSet& Set = State->Jobs.AddDefaulted_GetRef();
		Set.StrokeId = Stroke.Id;
		Set.Key = Key;
		const TArray<FSloshPaintPoint>& Points = Stroke.Points;
		int32 RunStart = 0;
		while (RunStart < Points.Num())
		{
			int32 RunEnd = RunStart;
			while (RunEnd + 1 < Points.Num() && Points[RunEnd + 1].Bone == Points[RunStart].Bone)
			{
				++RunEnd;
			}
			const FName Bone = Points[RunStart].Bone;
			const FTransform Space = GetStrokeSpaceTransform(Bone);
			FStrokeJob Job;
			Job.Preset = Params;
			Job.Seed = Stroke.Seed + Set.Jobs.Num() * 977;
			Job.MaterialSlot = FluidSlot;
			auto AddPoint = [&](const FSloshPaintPoint& Point)
			{
				FInputPoint In;
				if (Point.Bone == Bone)
				{
					In.Position = Point.Position;
					In.Normal = Point.Normal;
				}
				else
				{
					const FTransform From = GetStrokeSpaceTransform(Point.Bone);
					In.Position = Space.InverseTransformPosition(From.TransformPosition(Point.Position));
					In.Normal = Space.InverseTransformVectorNoScale(From.TransformVectorNoScale(Point.Normal));
				}
				In.Pressure = Point.Pressure;
				Job.Points.Add(In);
			};
			if (RunStart > 0)
			{
				AddPoint(Points[RunStart - 1]);
			}
			for (int32 I = RunStart; I <= RunEnd; ++I)
			{
				AddPoint(Points[I]);
			}
			if (RunEnd + 1 < Points.Num())
			{
				AddPoint(Points[RunEnd + 1]);
			}
			Set.Bones.Add(Bone);
			Set.Jobs.Add(MoveTemp(Job));
			RunStart = RunEnd + 1;
		}
	}
	Cache = MoveTemp(NewCache);

	Pending = State;
	FBuildState* Raw = State.Get();
	State->Task = UE::Tasks::Launch(TEXT("SloshPaintBuild"), [Raw]()
	{
		for (FBuildState::FStrokeJobSet& Set : Raw->Jobs)
		{
			TSharedPtr<FSloshPaintStrokeCache> Entry = MakeShared<FSloshPaintStrokeCache>();
			Entry->Key = Set.Key;
			for (int32 I = 0; I < Set.Jobs.Num(); ++I)
			{
				TUniquePtr<FSloshPaintStrokeCache::FPiece> Piece = MakeUnique<FSloshPaintStrokeCache::FPiece>();
				Piece->Bone = Set.Bones[I];
				if (BuildStroke(Set.Jobs[I], Raw->Settings, Piece->Result))
				{
					Entry->Pieces.Add(MoveTemp(Piece));
				}
			}
			Raw->Built.Add(Set.StrokeId, Entry);
		}
		auto Assemble = [Raw](const FSloshPaintStrokeCache& Entry)
		{
			for (const TUniquePtr<FSloshPaintStrokeCache::FPiece>& Piece : Entry.Pieces)
			{
				TUniquePtr<FBuildState::FGroup>& Slot = Raw->Groups.FindOrAdd(Piece->Bone);
				if (!Slot)
				{
					Slot = MakeUnique<FBuildState::FGroup>();
				}
				FBuildState::FGroup& Group = *Slot;
				AppendStrokeMesh(Group.Mesh, Piece->Result.Mesh);
				for (const FLayerInstances& Layer : Piece->Result.Layers)
				{
					Group.Layers.FindOrAdd(TPair<int32, bool>(Layer.MaterialSlot, Layer.bRods)).Append(Layer.Transforms);
				}
			}
		};
		for (const TSharedPtr<const FSloshPaintStrokeCache>& Entry : Raw->Reused)
		{
			Assemble(*Entry);
		}
		for (const TPair<int32, TSharedPtr<const FSloshPaintStrokeCache>>& Entry : Raw->Built)
		{
			Assemble(*Entry.Value);
		}
	});

	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USloshPaintComponent::PollBuild));
	}
}

bool USloshPaintComponent::PollBuild(float)
{
	if (!Pending.IsValid())
	{
		TickerHandle.Reset();
		return false;
	}
	if (!Pending->Task.IsCompleted())
	{
		return true;
	}
	TSharedPtr<FBuildState> State = MoveTemp(Pending);
	Pending.Reset();
	for (const TPair<int32, TSharedPtr<const FSloshPaintStrokeCache>>& Entry : State->Built)
	{
		// Only keep results for strokes that still exist.
		if (FindStroke(Entry.Key))
		{
			Cache.Add(Entry.Key, Entry.Value);
		}
	}
	if (IsRegistered())
	{
		ApplyBuild(*State);
		OnPaintBuilt.Broadcast(this);
	}
	if (bDirty && IsRegistered())
	{
		StartBuild();
		return true;
	}
	TickerHandle.Reset();
	return false;
}

void USloshPaintComponent::DestroyGeneratedComponents()
{
	for (TPair<FName, TObjectPtr<UDynamicMeshComponent>>& Pair : GroupMeshes)
	{
		if (Pair.Value)
		{
			Pair.Value->DestroyComponent();
		}
	}
	GroupMeshes.Reset();
	for (UInstancedStaticMeshComponent* Layer : LayerComponents)
	{
		if (Layer)
		{
			Layer->DestroyComponent();
		}
	}
	LayerComponents.Reset();
}

void USloshPaintComponent::ApplyBuild(FBuildState& State)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}
	USkinnedMeshComponent* Skinned = GetSkinnedTarget();
	auto AttachFor = [this, Skinned](USceneComponent* Child, FName Bone)
	{
		if (!Bone.IsNone() && Skinned)
		{
			Child->SetupAttachment(Skinned, Bone);
		}
		else
		{
			Child->SetupAttachment(this);
		}
	};
	constexpr EObjectFlags GeneratedFlags = RF_Transient | RF_TextExportTransient | RF_DuplicateTransient;

	TArray<UMaterialInterface*> MaterialSet;
	for (const TObjectPtr<UMaterialInterface>& Material : State.Materials)
	{
		MaterialSet.Add(Material.Get());
	}

	// Group meshes: reuse components for bones that persist, drop the rest.
	TMap<FName, TObjectPtr<UDynamicMeshComponent>> Kept;
	for (TPair<FName, TUniquePtr<FBuildState::FGroup>>& Pair : State.Groups)
	{
		TObjectPtr<UDynamicMeshComponent> Mesh;
		GroupMeshes.RemoveAndCopyValue(Pair.Key, Mesh);
		if (!Mesh)
		{
			Mesh = NewObject<UDynamicMeshComponent>(Owner, NAME_None, GeneratedFlags);
			AttachFor(Mesh, Pair.Key);
			Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Mesh->SetGenerateOverlapEvents(false);
			Mesh->SetTangentsType(EDynamicMeshComponentTangentsMode::AutoCalculated);
			Mesh->RegisterComponent();
		}
		Mesh->SetMesh(MoveTemp(Pair.Value->Mesh));
		Mesh->ConfigureMaterialSet(MaterialSet);
		Mesh->SetCastShadow(State.ShadowGroups.Contains(Pair.Key));
		Kept.Add(Pair.Key, Mesh);
	}
	for (TPair<FName, TObjectPtr<UDynamicMeshComponent>>& Stale : GroupMeshes)
	{
		if (Stale.Value)
		{
			Stale.Value->DestroyComponent();
		}
	}
	GroupMeshes = MoveTemp(Kept);

	// Layer instances: rebuilt wholesale (cheap).
	for (UInstancedStaticMeshComponent* Layer : LayerComponents)
	{
		if (Layer)
		{
			Layer->DestroyComponent();
		}
	}
	LayerComponents.Reset();
	for (TPair<FName, TUniquePtr<FBuildState::FGroup>>& Pair : State.Groups)
	{
		for (TPair<TPair<int32, bool>, TArray<FTransform>>& Layer : Pair.Value->Layers)
		{
			const bool bRods = Layer.Key.Value;
			UInstancedStaticMeshComponent* Instances = NewObject<UInstancedStaticMeshComponent>(Owner, NAME_None, GeneratedFlags);
			AttachFor(Instances, Pair.Key);
			Instances->SetStaticMesh(LayerMesh(bRods));
			Instances->SetMaterial(0, State.Materials.IsValidIndex(Layer.Key.Key) ? State.Materials[Layer.Key.Key].Get() : nullptr);
			Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Instances->SetGenerateOverlapEvents(false);
			Instances->SetCastShadow(false);
			Instances->RegisterComponent();
			// Engine basic shapes: sphere radius 50 cm, cylinder radius 50 cm x height 100 cm (centred).
			const FVector ShapeScale = bRods ? FVector(1.0 / 50.0, 1.0 / 50.0, 1.0 / 100.0) : FVector(1.0 / 50.0);
			for (FTransform& Transform : Layer.Value)
			{
				Transform.SetScale3D(Transform.GetScale3D() * ShapeScale);
			}
			Instances->AddInstances(Layer.Value, false, false);
			LayerComponents.Add(Instances);
		}
	}
}
