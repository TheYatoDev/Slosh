// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshVolume.h"
#include "SloshColliderComponent.h"
#include "SloshEmitterComponent.h"
#include "SloshManipulatorComponent.h"
#include "SloshSubsystem.h"
#include "SloshRenderer.h"
#include "SloshSurfaceMesh.h"
#include "SloshSurfaceMeshComponent.h"
#include "SloshPaintPreset.h"
#include "Components/BoxComponent.h"
#include "Components/LightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"

ASloshVolume::ASloshVolume()
{
	PrimaryActorTick.bCanEverTick = true;
	// After physics so collider transforms/velocities are this frame's results; feedback forces are
	// applied by the colliders in the next frame's pre-physics tick.
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	Bounds = CreateDefaultSubobject<UBoxComponent>(TEXT("Bounds"));
	Bounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Bounds->SetCanEverAffectNavigation(false);
	Bounds->SetHiddenInGame(true);
	Bounds->ShapeColor = FColor(40, 140, 255);
	RootComponent = Bounds;
}

void ASloshVolume::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Bounds->SetBoxExtent(GetDomainSize() * 0.5, false);
}

FVector ASloshVolume::GetDomainSize() const
{
	return FVector(GridResolution) * CellSize;
}

FTransform ASloshVolume::GetSimToWorld() const
{
	// Sim meters -> local cm (domain min corner at -HalfSize) -> world (actor location/rotation, no scale).
	const FTransform SimToLocal(FQuat::Identity, -GetDomainSize() * 0.5, FVector(100.0));
	const FTransform LocalToWorld(GetActorQuat(), GetActorLocation());
	return SimToLocal * LocalToWorld;
}

bool ASloshVolume::ContainsPoint(FVector WorldLocation) const
{
	const FVector Local = GetActorTransform().InverseTransformPositionNoScale(WorldLocation);
	const FVector Half = GetDomainSize() * 0.5;
	return FMath::Abs(Local.X) <= Half.X && FMath::Abs(Local.Y) <= Half.Y && FMath::Abs(Local.Z) <= Half.Z;
}

float ASloshVolume::GetLiquidVolumeLiters() const
{
	const double CellM = CellSize * 0.01;
	return float(LatestResults.FluidCells * CellM * CellM * CellM * 1000.0);
}

float ASloshVolume::GetParticleVolumeLiters() const
{
	const double CellM = CellSize * 0.01;
	const double Density = LatestResults.MeanInteriorDensity > 0.0f ? LatestResults.MeanInteriorDensity : 1.0;
	return float(double(GetParticleCount()) / 8.0 / Density * CellM * CellM * CellM * 1000.0);
}

int32 ASloshVolume::GetFeedbackLatencyFrames() const
{
	return LatestResults.SimFrame > 0 ? int32(SubmittedFrames - LatestResults.SimFrame) : -1;
}

FSloshSolverSettings ASloshVolume::BuildSolverSettings() const
{
	const FSloshPhysicalProperties& Phys = GetPhysical();
	FSloshSolverSettings S;
	S.GridDim = GridResolution;
	S.CellSize = CellSize * 0.01f;
	S.ParticleCapacity = uint32(FMath::Max(ParticleCapacity, 1024));
	S.TransferMode = Phys.Transfer == ESloshTransfer::APIC ? ESloshTransferMode::APIC : ESloshTransferMode::FLIP;
	S.FlipRatio = Phys.FlipRatio;
	S.Density = Phys.Density;
	S.KinematicViscosity = Phys.KinematicViscosity;
	S.BoundaryFriction = Phys.BoundaryFriction;
	S.Adhesion = Phys.Adhesion;
	S.AdhesionDistance = Phys.AdhesionDistance * 0.01f;
	S.DensityCorrection = Phys.DensityCorrection;
	const int64 Cells = int64(GridResolution.X) * GridResolution.Y * GridResolution.Z;
	const bool bUseMG = PressureSolver == ESloshPressureSolverType::MGPCG
		|| (PressureSolver == ESloshPressureSolverType::Auto && Cells > AutoSolverCellThreshold);
	S.PressureSolver = bUseMG ? ESloshPressureSolver::MGPCG : ESloshPressureSolver::RBGS;
	S.PCGIterations = PCGIterations;
	S.PressureIterations = PressureIterations;
	S.SORWeight = SORWeight;
	S.ViscosityIterations = ViscosityIterations;
	S.MaxSubsteps = MaxSubsteps;
	S.MaxFrameDt = MaxFrameDt;
	S.CFL = CFL;
	S.SortInterval = SortInterval;
	S.bSmoothCouplingForce = bSmoothCouplingForce;
	S.bGhostFluid = bGhostFluid;
	S.DensityDeadband = DensityDeadband;
	S.bTwoSidedDensityProjection = bTwoSidedDensityProjection;
	S.bSparseBricks = bSparseBricks;
	S.SparseMaxActiveFraction = SparseMaxActiveFraction;
	S.SparseStorageBudget = SparseStorageBudget;
	S.DensityRelaxation = DensityRelaxation;
	S.DensityProjectionInterval = DensityProjectionInterval;
	S.MaxSpeed = MaxParticleSpeed * 0.01f;

	const double GravityZ = GetWorld() ? GetWorld()->GetGravityZ() : -980.0;
	S.Gravity = FVector3f(GetActorQuat().UnrotateVector(FVector(0, 0, GravityZ * 0.01)));
	return S;
}

void ASloshVolume::QueueFillLocalBox(const FBox& LocalBoxCm)
{
	const FVector Half = GetDomainSize() * 0.5;
	const FIntVector Res = GridResolution;
	// Keep the wall layer (cells 0 and N-1) empty.
	auto ToCell = [&](const FVector& Local, bool bCeil)
	{
		const FVector C = (Local + Half) / CellSize;
		return FIntVector(
			FMath::Clamp(bCeil ? FMath::CeilToInt(C.X) : FMath::FloorToInt(C.X), 1, Res.X - 1),
			FMath::Clamp(bCeil ? FMath::CeilToInt(C.Y) : FMath::FloorToInt(C.Y), 1, Res.Y - 1),
			FMath::Clamp(bCeil ? FMath::CeilToInt(C.Z) : FMath::FloorToInt(C.Z), 1, Res.Z - 1));
	};
	const FIntVector Min = ToCell(LocalBoxCm.Min, false);
	const FIntVector Max = ToCell(LocalBoxCm.Max, true);
	const FIntVector Dims = Max - Min;
	if (Dims.X <= 0 || Dims.Y <= 0 || Dims.Z <= 0)
	{
		return;
	}
	FSloshEmitRequest Emit;
	Emit.Shape = ESloshEmitShape::FillCells;
	Emit.CellMin = Min;
	Emit.CellDims = Dims;
	PendingEmits.Add(Emit);
	TotalEmittedGameThread += int64(Dims.X) * Dims.Y * Dims.Z * 8;
}

void ASloshVolume::FillBox(FVector WorldCenter, FVector Extent)
{
	const FVector LocalCenter = GetActorTransform().InverseTransformPositionNoScale(WorldCenter);
	QueueFillLocalBox(FBox(LocalCenter - Extent, LocalCenter + Extent));
}

void ASloshVolume::EmitSphere(FVector WorldCenter, float Radius, FVector WorldVelocity, int32 Count)
{
	if (Count <= 0 || Radius <= 0.0f)
	{
		return;
	}
	const FTransform WorldToSim = GetSimToWorld().Inverse();
	FSloshEmitRequest Emit;
	Emit.Shape = ESloshEmitShape::Sphere;
	Emit.Center = FVector3f(WorldToSim.TransformPosition(WorldCenter));
	Emit.Extent = FVector3f(Radius * 0.01f, 0.0f, 0.0f);
	Emit.Velocity = FVector3f(WorldToSim.TransformVector(WorldVelocity));
	Emit.Count = uint32(Count);
	PendingEmits.Add(Emit);
	TotalEmittedGameThread += Count;
}

UMaterialInterface* ASloshVolume::GetEffectiveSurfaceMaterial() const
{
	if (SurfaceMaterial)
	{
		return SurfaceMaterial;
	}
	if (const USloshPaintPreset* Preset = Cast<USloshPaintPreset>(Material))
	{
		return Preset->Material;
	}
	return nullptr;
}

void ASloshVolume::ResetSimulation()
{
	bPendingReset = true;
	PendingEmits.Reset();
	TotalEmittedGameThread = 0;
	LatestResults = FSloshResults();
	ColliderHistory.Reset();
}

void ASloshVolume::BeginPlay()
{
	Super::BeginPlay();
	Proxy = MakeShared<FSloshProxy, ESPMode::ThreadSafe>();
	if (USloshSubsystem* Subsystem = USloshSubsystem::Get(GetWorld()))
	{
		Subsystem->RegisterVolume(this);
	}
	bPendingReset = true;
	if (bFillOnBeginPlay)
	{
		QueueFillLocalBox(InitialFillBox);
	}
	UpdateRenderRegistration();
}

void ASloshVolume::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (USloshSubsystem* Subsystem = USloshSubsystem::Get(GetWorld()))
	{
		Subsystem->UnregisterVolume(this);
	}
	if (SurfaceMeshComponent)
	{
		SurfaceMeshComponent->DestroyComponent();
		SurfaceMeshComponent = nullptr;
	}
	SurfaceMeshBuffers.Reset();
	if (Proxy.IsValid())
	{
		SloshRender::RemoveVolume_GameThread(Proxy.Get());
		FSloshProxy::ReleaseOnRenderThread(Proxy);
	}
	Super::EndPlay(EndPlayReason);
}

void ASloshVolume::UpdateRenderRegistration()
{
	if (!Proxy.IsValid() || !GetWorld())
	{
		return;
	}
	const FSloshVisualProperties& Vis = GetVisual();
	FSloshRenderSettings Settings;
	Settings.bEnabled = bRenderLiquid;
	Settings.AbsorptionPerMeter = FVector3f(Vis.AbsorptionPerMeter);
	Settings.ScatterColor = FVector3f(Vis.ScatterColor.R, Vis.ScatterColor.G, Vis.ScatterColor.B);
	Settings.SkyZenithColor = FVector3f(SkyZenithColor.R, SkyZenithColor.G, SkyZenithColor.B);
	Settings.SkyHorizonColor = FVector3f(SkyHorizonColor.R, SkyHorizonColor.G, SkyHorizonColor.B);
	Settings.RefractionStrength = Vis.RefractionStrength;
	Settings.FresnelF0 = Vis.FresnelF0;
	Settings.SpecularPower = Vis.SpecularPower;
	Settings.ParticleRadiusScale = Vis.ParticleRadiusScale;
	Settings.SmoothingRadius = Vis.SmoothingRadius;
	Settings.SmoothingScale = Vis.SmoothingScale;
	Settings.SmoothingIterations = Vis.SmoothingIterations;
	Settings.EdgeFadeThickness = Vis.EdgeFadeThickness;
	Settings.ScatterLighting = Vis.ScatterLighting;
	Settings.Anisotropy = Vis.Anisotropy;
	Settings.AnisotropyMaxStretch = Vis.AnisotropyMaxStretch;
	Settings.AnisotropySmoothing = Vis.AnisotropySmoothing;
	Settings.SurfaceMode = SurfaceMode == ESloshSurfaceMode::Raymarched ? ESloshSurfaceRenderMode::Raymarch
		: SurfaceMode == ESloshSurfaceMode::Mesh ? ESloshSurfaceRenderMode::Mesh : ESloshSurfaceRenderMode::Splats;
	Settings.FieldResolution = FieldResolution;
	Settings.SurfaceSmoothing = SurfaceSmoothing;
	Settings.IsoLevel = SurfaceIsoLevel;
	Settings.LocalOffsetCm = FVector3f(-GetDomainSize() * 0.5);

	// Mesh mode: a surface mesh component sharing GPU buffers with the renderer, created on demand.
	if (SurfaceMode == ESloshSurfaceMode::Mesh && bRenderLiquid)
	{
		const uint32 Capacity = uint32(FMath::Max(MaxSurfaceVertices, 1024));
		if (!SurfaceMeshBuffers.IsValid() || SurfaceMeshBuffers->GetMaxVertices() != FMath::Clamp<uint32>(Capacity, 1024u, 8u * 1024u * 1024u))
		{
			SurfaceMeshBuffers = FSloshSurfaceMeshBuffers::Create(Capacity);
		}
		if (!SurfaceMeshComponent)
		{
			SurfaceMeshComponent = NewObject<USloshSurfaceMeshComponent>(this, TEXT("SloshSurfaceMesh"));
			SurfaceMeshComponent->SetupAttachment(Bounds);
			SurfaceMeshComponent->RegisterComponent();
		}
		SurfaceMeshComponent->Configure(SurfaceMeshBuffers, GetDomainSize() * 0.5, GetEffectiveSurfaceMaterial());
		Settings.MeshBuffers = SurfaceMeshBuffers;
	}
	else if (SurfaceMeshComponent)
	{
		SurfaceMeshComponent->DestroyComponent();
		SurfaceMeshComponent = nullptr;
		SurfaceMeshBuffers.Reset();
	}

	if (!SunLight)
	{
		for (TActorIterator<ADirectionalLight> It(GetWorld()); It; ++It)
		{
			SunLight = *It;
			break;
		}
	}
	if (SunLight && SunLight->GetLightComponent())
	{
		const ULightComponent* Light = SunLight->GetLightComponent();
		Settings.LightDirectionWorld = FVector3f(Light->GetDirection());
		const FLinearColor C = Light->GetLightColor() * FMath::Min(Light->Intensity, 10.0f) * 0.5f;
		Settings.LightColor = FVector3f(C.R, C.G, C.B);
	}
	SloshRender::AddOrUpdateVolume_GameThread(Proxy, GetWorld()->Scene, Settings);
}

void ASloshVolume::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Proxy.IsValid())
	{
		return;
	}

	USloshSubsystem* Subsystem = USloshSubsystem::Get(GetWorld());
	const FTransform SimToWorld = GetSimToWorld();
	const FTransform WorldToSim = SimToWorld.Inverse();

	// Emitters inside the domain: continuous rate, bursts, lifetime limit, spread and inherited motion.
	if (Subsystem && bSimulate)
	{
		const double CellM = CellSize * 0.01;
		const double ParticlesPerLiter = 8.0 / (CellM * CellM * CellM * 1000.0);
		for (const TWeakObjectPtr<USloshEmitterComponent>& WeakEmitter : Subsystem->GetEmitters())
		{
			USloshEmitterComponent* Emitter = WeakEmitter.Get();
			if (!Emitter)
			{
				continue;
			}
			const FVector Location = Emitter->GetComponentLocation();
			// Motion is measured once per game frame (every volume ticks over every emitter), and an emitter feeds
			// only the first volume that contains it, even where volumes overlap.
			if (Emitter->VelocityFrame != GFrameCounter)
			{
				Emitter->VelocityFrame = GFrameCounter;
				Emitter->MeasuredVelocity = Emitter->bHasPreviousLocation && DeltaSeconds > UE_KINDA_SMALL_NUMBER
					? (Location - Emitter->PreviousLocation) / DeltaSeconds : FVector::ZeroVector;
				Emitter->PreviousLocation = Location;
				Emitter->bHasPreviousLocation = true;
			}
			const FVector EmitterVelocity = Emitter->MeasuredVelocity;
			if (Emitter->EmitFrame == GFrameCounter || !ContainsPoint(Location))
			{
				continue;
			}
			Emitter->EmitFrame = GFrameCounter;
			int64 Count = 0;
			if (Emitter->bEmitting)
			{
				const double Rate = Emitter->RateMode == ESloshEmitRateMode::LitersPerSecond
					? double(Emitter->LitersPerSecond) * ParticlesPerLiter : double(Emitter->ParticlesPerSecond);
				Emitter->Carry += float(Rate * DeltaSeconds);
				Count = FMath::FloorToInt64(Emitter->Carry);
				Emitter->Carry -= float(Count);
			}
			Count += Emitter->PendingBurst + FMath::RoundToInt64(double(Emitter->PendingBurstLiters) * ParticlesPerLiter);
			Emitter->PendingBurst = 0;
			Emitter->PendingBurstLiters = 0.0f;
			if (Emitter->MaxParticles > 0)
			{
				Count = FMath::Min<int64>(Count, FMath::Max<int64>(int64(Emitter->MaxParticles) - Emitter->EmittedParticles, 0));
			}
			if (Count <= 0)
			{
				continue;
			}
			const FTransform EmitterTransform = Emitter->GetComponentTransform();
			const float Scale = EmitterTransform.GetScale3D().GetAbsMax();
			const FVector Velocity = Emitter->GetForwardVector() * Emitter->Speed + (Emitter->bInheritVelocity ? EmitterVelocity : FVector::ZeroVector);
			FSloshEmitRequest Emit;
			Emit.Center = FVector3f(WorldToSim.TransformPosition(Location));
			Emit.Velocity = FVector3f(WorldToSim.TransformVector(Velocity));
			if (Emitter->Shape == ESloshEmitterShape::Box)
			{
				const FQuat SimRot = WorldToSim.GetRotation() * EmitterTransform.GetRotation();
				Emit.Shape = ESloshEmitShape::Box;
				Emit.Extent = FVector3f(Emitter->BoxExtent * Scale * 0.01);
				Emit.Rotation = FVector4f(SimRot.X, SimRot.Y, SimRot.Z, SimRot.W);
			}
			else
			{
				Emit.Shape = ESloshEmitShape::Sphere;
				Emit.Extent = FVector3f(Emitter->Radius * Scale * 0.01f, 0.0f, 0.0f);
			}
			Emit.VelocitySpread = FMath::DegreesToRadians(Emitter->VelocitySpread);
			Emit.SpeedJitter = Emitter->SpeedVariation;
			Emit.Count = uint32(FMath::Min<int64>(Count, MAX_int32));
			PendingEmits.Add(Emit);
			TotalEmittedGameThread += Emit.Count;
			Emitter->EmittedParticles += Emit.Count;
		}
	}

	FSloshFrameInput Input;
	Input.Settings = BuildSolverSettings();
	// Drains and force fields overlapping the domain.
	if (Subsystem)
	{
		const FBox ManipulatorBox = GetComponentsBoundingBox(true);
		for (const TWeakObjectPtr<USloshManipulatorComponent>& WeakManipulator : Subsystem->GetManipulators())
		{
			const USloshManipulatorComponent* Manipulator = WeakManipulator.Get();
			FSloshManipulatorGPU GPU;
			if (Manipulator && Input.Manipulators.Num() < int32(Slosh::MaxManipulators)
				&& ManipulatorBox.ExpandBy(Manipulator->GetReach()).IsInside(Manipulator->GetComponentLocation())
				&& Manipulator->BuildGPU(WorldToSim, GPU))
			{
				Input.Manipulators.Add(GPU);
			}
		}
	}
	Input.SimToWorld = SimToWorld;
	Input.DeltaTime = DeltaSeconds;
	Input.bReset = bPendingReset;
	Input.bPaused = !bSimulate;
	Input.Emits = MoveTemp(PendingEmits);
	PendingEmits.Reset();
	bPendingReset = false;

	TArray<TWeakObjectPtr<USloshColliderComponent>> FrameColliders;
	if (Subsystem)
	{
		const FBox DomainBox = GetComponentsBoundingBox(true).ExpandBy(CellSize * 2.0);
		for (const TWeakObjectPtr<USloshColliderComponent>& WeakCollider : Subsystem->GetColliders())
		{
			USloshColliderComponent* Collider = WeakCollider.Get();
			if (!Collider || Input.Colliders.Num() >= int32(Slosh::MaxColliders))
			{
				continue;
			}
			const float Reach = FMath::Max3(Collider->Radius + Collider->CapsuleHalfHeight, float(Collider->BoxExtent.GetMax()) * 1.75f, 1.0f) * Collider->GetComponentScale().GetAbsMax();
			if (!DomainBox.ExpandBy(Reach).IsInside(Collider->GetComponentLocation()))
			{
				continue;
			}
			Input.Colliders.Add(Collider->BuildGPUCollider(WorldToSim, DeltaSeconds));
			FrameColliders.Add(Collider);
		}
	}

	++SubmittedFrames;
	ColliderHistory.Add(SubmittedFrames, MoveTemp(FrameColliders));
	Proxy->SubmitFrame_GameThread(MoveTemp(Input));

	UpdateRenderRegistration();
	DistributeFeedback();
}

void ASloshVolume::DistributeFeedback()
{
	LatestResults = Proxy->GetResults();
	const uint64 Frame = LatestResults.SimFrame;
	if (Frame == 0 || Frame == LastDistributedFrame)
	{
		return;
	}
	LastDistributedFrame = Frame;

	if (const TArray<TWeakObjectPtr<USloshColliderComponent>>* Snapshot = ColliderHistory.Find(Frame))
	{
		const FTransform SimToWorldNoScale(GetActorQuat(), GetActorLocation());
		const int32 Age = int32(SubmittedFrames - Frame);
		const int32 Num = FMath::Min(Snapshot->Num(), LatestResults.Colliders.Num());
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (USloshColliderComponent* Collider = (*Snapshot)[Index].Get())
			{
				Collider->ReceiveFeedback(LatestResults.Colliders[Index], SimToWorldNoScale, CellSize * 0.01f, GetPhysical().Density, Age);
			}
		}
	}
	// Drop history the GPU has already reported on.
	for (auto It = ColliderHistory.CreateIterator(); It; ++It)
	{
		if (It.Key() <= Frame || SubmittedFrames - It.Key() > 32)
		{
			It.RemoveCurrent();
		}
	}
}
