#include "SloshDemoGameMode.h"
#include "SloshVolume.h"
#include "SloshColliderComponent.h"
#include "SloshEmitterComponent.h"
#include "SloshManipulatorComponent.h"
#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "DynamicRHI.h"
#include "HAL/PlatformTime.h"
#include "Modules/ModuleManager.h"
#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/Paths.h"
#include "SloshPaintBrushComponent.h"
#include "SloshPaintComponent.h"
#include "SloshPaintLibrary.h"
#include "SloshPaintPreset.h"
#include "UnrealClient.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "Components/PoseableMeshComponent.h"
#include "SloshSkeletalColliderComponent.h"
#include "SloshSkelTestActors.h"

DEFINE_LOG_CATEGORY_STATIC(LogSloshDemo, Log, All);

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, SloshDemo, "SloshDemo");

namespace
{
	// -SloshSurface=Splats|Raymarched|Mesh  -SloshIso=  -SloshFieldRes=  -SloshSurfaceSmoothing=
	void ApplySurfaceFlags(ASloshVolume* Target)
	{
		FString Mode;
		if (FParse::Value(FCommandLine::Get(), TEXT("SloshSurface="), Mode))
		{
			Target->SurfaceMode = Mode.StartsWith(TEXT("Ray"), ESearchCase::IgnoreCase) ? ESloshSurfaceMode::Raymarched
				: Mode.Equals(TEXT("Mesh"), ESearchCase::IgnoreCase) ? ESloshSurfaceMode::Mesh : ESloshSurfaceMode::Splats;
		}
		FParse::Value(FCommandLine::Get(), TEXT("SloshIso="), Target->SurfaceIsoLevel);
		FParse::Value(FCommandLine::Get(), TEXT("SloshFieldRes="), Target->FieldResolution);
		FParse::Value(FCommandLine::Get(), TEXT("SloshSurfaceSmoothing="), Target->SurfaceSmoothing);
	}
}

ASloshDemoGameMode::ASloshDemoGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
}

AStaticMeshActor* ASloshDemoGameMode::SpawnBody(const TCHAR* MeshPath, const FVector& Location, float Scale, float MassKg, bool bSimulate, bool bSphere)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Actor = GetWorld()->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator, Params);
	UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, MeshPath));
	Mesh->SetWorldScale3D(FVector(Scale));
	if (bSimulate)
	{
		Mesh->SetSimulatePhysics(true);
		Mesh->SetMassOverrideInKg(NAME_None, MassKg, true);
		Mesh->SetLinearDamping(0.05f);
	}

	USloshColliderComponent* Collider = NewObject<USloshColliderComponent>(Actor);
	Collider->Shape = bSphere ? ESloshColliderShapeType::Sphere : ESloshColliderShapeType::Box;
	if (FParse::Param(FCommandLine::Get(), TEXT("SloshBuoyancyCoupling")))
	{
		Collider->CouplingMode = ESloshCouplingMode::BuoyancyAndDrag;
	}
	Collider->Radius = 50.0f;            // basic shapes are 100 cm; the component inherits the mesh scale
	Collider->BoxExtent = FVector(50.0);
	Collider->SetupAttachment(Mesh);
	Collider->RegisterComponent();
	return Actor;
}

void ASloshDemoGameMode::StartPlay()
{
	Super::StartPlay();
	UWorld* World = GetWorld();
	bAutoTest = FParse::Param(FCommandLine::Get(), TEXT("SloshAutoTest"));
	FParse::Value(FCommandLine::Get(), TEXT("SloshAutoTestSeconds="), AutoTestSeconds);
	if (FParse::Param(FCommandLine::Get(), TEXT("SloshPaintTest")))
	{
		StartPaintTest();
		return;
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("SloshSkelTest")))
	{
		StartSkelTest();
		return;
	}
	for (TActorIterator<ASloshVolume> It(World); It; ++It)
	{
		Volume = *It;
		bPlacedScene = true;
		ApplySurfaceFlags(Volume);
		AutoTestSeconds = FMath::Max(AutoTestSeconds, 13.0);
		UE_LOG(LogSloshDemo, Display, TEXT("placed scene: using %s (grid %s, cell %.2f cm); demo scene not spawned"),
			*Volume->GetName(), *Volume->GridResolution.ToString(), Volume->CellSize);
		return;
	}

	FActorSpawnParameters Params;
	Params.bDeferConstruction = true;
	Volume = World->SpawnActor<ASloshVolume>(FVector(0, 0, 125), FRotator::ZeroRotator, Params);
	FString Grid;
	if (FParse::Value(FCommandLine::Get(), TEXT("SloshGrid="), Grid, /*bShouldStopOnSeparator*/ false))
	{
		TArray<FString> Parts;
		Grid.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() == 3)
		{
			Volume->GridResolution = FIntVector(FCString::Atoi(*Parts[0]), FCString::Atoi(*Parts[1]), FCString::Atoi(*Parts[2]));
			Volume->CellSize = 320.0f / Volume->GridResolution.X;
			Volume->ParticleCapacity = Volume->GridResolution.X * Volume->GridResolution.Y * Volume->GridResolution.Z * 2;
		}
	}
	FParse::Value(FCommandLine::Get(), TEXT("SloshIters="), Volume->PressureIterations);
	FParse::Value(FCommandLine::Get(), TEXT("SloshPCGIters="), Volume->PCGIterations);
	FParse::Value(FCommandLine::Get(), TEXT("SloshCFL="), Volume->CFL);
	FParse::Value(FCommandLine::Get(), TEXT("SloshMaxSubsteps="), Volume->MaxSubsteps);
	FParse::Value(FCommandLine::Get(), TEXT("SloshMaxFrameDt="), Volume->MaxFrameDt);
	FParse::Value(FCommandLine::Get(), TEXT("SloshMaxSpeed="), Volume->MaxParticleSpeed);
	FParse::Value(FCommandLine::Get(), TEXT("SloshSortInterval="), Volume->SortInterval);
	if (FParse::Param(FCommandLine::Get(), TEXT("SloshNoGhostFluid")))
	{
		Volume->bGhostFluid = false;   // classic first-order free surface (A/B testing)
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("SloshFaceCoupling")))
	{
		Volume->bSmoothCouplingForce = false;   // legacy binary-face force estimator (A/B testing)
	}
	FParse::Value(FCommandLine::Get(), TEXT("SloshDensityCorrection="), Volume->Physical.DensityCorrection);
	FParse::Value(FCommandLine::Get(), TEXT("SloshDensityDeadband="), Volume->DensityDeadband);
	FParse::Value(FCommandLine::Get(), TEXT("SloshDensityRelaxation="), Volume->DensityRelaxation);
	FParse::Value(FCommandLine::Get(), TEXT("SloshDensityInterval="), Volume->DensityProjectionInterval);
	FParse::Value(FCommandLine::Get(), TEXT("SloshSparseThreshold="), Volume->SparseMaxActiveFraction);
	FParse::Value(FCommandLine::Get(), TEXT("SloshPoolBudget="), Volume->SparseStorageBudget);
	if (FParse::Param(FCommandLine::Get(), TEXT("SloshDenseGrid")))
	{
		Volume->bSparseBricks = false;   // dense grid passes (A/B testing)
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("SloshVelocityDensityCorrection")))
	{
		Volume->bTwoSidedDensityProjection = false;   // previous one-sided velocity correction (A/B testing)
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("SloshNoAniso")))
	{
		Volume->Visual.Anisotropy = 0.0f;   // round splats (A/B rendering comparisons)
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("SloshNoDensityCorrection")))
	{
		Volume->Physical.DensityCorrection = 0.0f;   // isolates pressure-solver accuracy in the divergence metric
	}
	FString Solver;
	if (FParse::Value(FCommandLine::Get(), TEXT("SloshSolver="), Solver))
	{
		Volume->PressureSolver = Solver.Equals(TEXT("RBGS"), ESearchCase::IgnoreCase) ? ESloshPressureSolverType::RBGS
			: Solver.Equals(TEXT("MGPCG"), ESearchCase::IgnoreCase) ? ESloshPressureSolverType::MGPCG : ESloshPressureSolverType::Auto;
	}
	bCalm = FParse::Param(FCommandLine::Get(), TEXT("SloshCalm"));
	if (bCalm)
	{
		// Fill the whole tank 100 cm deep (wall layer excluded) so a 40 cm probe sits fully submerged.
		const FVector HalfSize = Volume->GetDomainSize() * 0.5;
		double CalmDepth = 100.0;
		FParse::Value(FCommandLine::Get(), TEXT("SloshCalmDepth="), CalmDepth);
		Volume->InitialFillBox = FBox(FVector(-HalfSize.X, -HalfSize.Y, -HalfSize.Z), FVector(HalfSize.X, HalfSize.Y, -HalfSize.Z + 5.0 + CalmDepth));
		Volume->ParticleCapacity = Volume->GridResolution.X * Volume->GridResolution.Y * Volume->GridResolution.Z * 4;
	}
	ApplySurfaceFlags(Volume);
	UGameplayStatics::FinishSpawningActor(Volume, FTransform(FVector(0, 0, 125)));

	// Invisible tank walls so rigid bodies cannot leave the liquid domain (the grid walls only bound the liquid).
	const FVector Half = Volume->GetDomainSize() * 0.5;
	const FVector Center(0, 0, 125);
	const FVector WallSpecs[4][2] = {
		// 1 m thick so fast bodies cannot tunnel through.
		{ FVector(Half.X + 50, 0, 0), FVector(1.0, Half.Y * 0.02 + 2.0, Half.Z * 0.02 + 1.0) },
		{ FVector(-Half.X - 50, 0, 0), FVector(1.0, Half.Y * 0.02 + 2.0, Half.Z * 0.02 + 1.0) },
		{ FVector(0, Half.Y + 50, 0), FVector(Half.X * 0.02 + 2.0, 1.0, Half.Z * 0.02 + 1.0) },
		{ FVector(0, -Half.Y - 50, 0), FVector(Half.X * 0.02 + 2.0, 1.0, Half.Z * 0.02 + 1.0) } };
	for (const FVector* Spec : WallSpecs)
	{
		AStaticMeshActor* Wall = World->SpawnActor<AStaticMeshActor>(Center + Spec[0], FRotator::ZeroRotator);
		// Runtime-spawned static-mobility components reject SetStaticMesh; make it movable first.
		Wall->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Wall->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Wall->GetStaticMeshComponent()->SetWorldScale3D(Spec[1]);
		Wall->GetStaticMeshComponent()->SetVisibility(false);
	}

	if (!bCalm)
	{
		HeavySphere = SpawnBody(TEXT("/Engine/BasicShapes/Sphere.Sphere"), FVector(-110, 70, 270), 0.5f, 140.0f, true, true);
		PaddleOrigin = FVector(90, 0, 60);
		Paddle = SpawnBody(TEXT("/Engine/BasicShapes/Cube.Cube"), PaddleOrigin, 0.3f, 0.0f, false, false);
		Paddle->GetStaticMeshComponent()->SetWorldScale3D(FVector(0.2, 1.2, 0.8));
		Paddle->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	// Optional fixed, fully submerged pressure probe: measures the solver's integrated pressure force
	// against Archimedes (rho * g * V) without body dynamics or feedback latency in the loop.
	if (FParse::Param(FCommandLine::Get(), TEXT("SloshPressureProbe")))
	{
		PressureProbe = SpawnBody(TEXT("/Engine/BasicShapes/Cube.Cube"), FVector(0, 100, bCalm ? 55.0 : 45.0), 0.4f, 0.0f, false, false);
		PressureProbe->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		PressureProbe->FindComponentByClass<USloshColliderComponent>()->bReportFeedbackWhenKinematic = true;
	}

	if (!bCalm)
	{
	AActor* EmitterActor = World->SpawnActor<AActor>(FVector(120, 0, 200), FRotator(-20, 180, 0));
	USceneComponent* EmitterRoot = NewObject<USceneComponent>(EmitterActor);
	EmitterActor->SetRootComponent(EmitterRoot);
	EmitterRoot->RegisterComponent();
	EmitterActor->SetActorLocationAndRotation(FVector(120, 0, 200), FRotator(-20, 180, 0));
	USloshEmitterComponent* Emitter = NewObject<USloshEmitterComponent>(EmitterActor);
	Emitter->SetupAttachment(EmitterRoot);
	Emitter->Radius = 8.0f;
	Emitter->ParticlesPerSecond = 6000.0f;
	Emitter->Speed = 250.0f;
	// -SloshManipTest: a spread jet, a floor drain in one corner and a whirlpool in the pool.
	const bool bManipTest = FParse::Param(FCommandLine::Get(), TEXT("SloshManipTest"));
	if (bManipTest)
	{
		Emitter->VelocitySpread = 12.0f;
		Emitter->SpeedVariation = 0.2f;
	}
	Emitter->RegisterComponent();
	if (bManipTest)
	{
		AActor* Manipulators = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator);
		USceneComponent* ManipRoot = NewObject<USceneComponent>(Manipulators);
		Manipulators->SetRootComponent(ManipRoot);
		ManipRoot->RegisterComponent();
		USloshDrainComponent* Drain = NewObject<USloshDrainComponent>(Manipulators, TEXT("Drain"));
		Drain->SetupAttachment(ManipRoot);
		Drain->SetRelativeLocation(FVector(110, 110, 15));
		Drain->Radius = 30.0f;
		Drain->bDrawShape = true;
		Drain->RegisterComponent();
		USloshForceFieldComponent* Swirl = NewObject<USloshForceFieldComponent>(Manipulators, TEXT("Whirlpool"));
		Swirl->SetupAttachment(ManipRoot);
		Swirl->SetRelativeLocation(FVector(-40, -40, 40));
		Swirl->ForceType = ESloshForceType::Swirl;
		Swirl->Radius = 90.0f;
		Swirl->Strength = 600.0f;
		Swirl->SwirlInwardPull = 0.3f;
		Swirl->Falloff = 30.0f;
		Swirl->bDrawShape = true;
		Swirl->RegisterComponent();
	}
	}

	ACameraActor* Camera = World->SpawnActor<ACameraActor>(FVector(380, -420, 330), FRotator::ZeroRotator);
	Camera->SetActorRotation((FVector(0, 0, 90) - Camera->GetActorLocation()).Rotation());
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0))
	{
		PC->SetViewTarget(Camera);
	}

	UE_LOG(LogSloshDemo, Display, TEXT("Slosh demo: grid %s, cell %.2f cm, capacity %d, CFL %.1f, sort every %d, solver %s (RBGS iters %d, PCG iters %d)"),
		*Volume->GridResolution.ToString(), Volume->CellSize, Volume->ParticleCapacity, Volume->CFL, Volume->SortInterval,
		Volume->PressureSolver == ESloshPressureSolverType::MGPCG ? TEXT("MGPCG") : Volume->PressureSolver == ESloshPressureSolverType::RBGS ? TEXT("RBGS") : TEXT("Auto"),
		Volume->PressureIterations, Volume->PCGIterations);
}

void ASloshDemoGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Elapsed += DeltaSeconds;
	if (bPaintTest)
	{
		TickPaintTest();
		return;
	}
	if (bSkelTest)
	{
		TickSkelTest();
		return;
	}
	if (bPlacedScene)
	{
		TickPlacedScene();
		return;
	}

	// Kinematic paddle sweeping through the pool (moving obstacle, one-way).
	if (Paddle)
	{
		Paddle->SetActorLocation(PaddleOrigin + FVector(35.0 * FMath::Sin(Elapsed * 2.0), 0, 0));
	}

	// Drop the light box once the dam-break wave has settled.
	if (!FloatingBox && Elapsed > 4.0)
	{
		// -SloshGentleDrop places the box just above the surface, isolating floating equilibrium from plunge dynamics.
		// -SloshFloorStart places it resting on the floor under the liquid (floor-contact lift test).
		const double DropZ = FParse::Param(FCommandLine::Get(), TEXT("SloshFloorStart")) ? 20.5
			: bCalm ? 140.0
			: FParse::Param(FCommandLine::Get(), TEXT("SloshGentleDrop")) ? 100.0 : 230.0;
		FloatingBox = SpawnBody(TEXT("/Engine/BasicShapes/Cube.Cube"), FVector(-20, -60, DropZ), 0.4f, 35.0f, true, false);
	}

	// Fine-grained trace of the buoyant box for coupling diagnostics.
	if (FloatingBox && FParse::Param(FCommandLine::Get(), TEXT("SloshTraceBox")))
	{
		const USloshColliderComponent* C = FloatingBox->FindComponentByClass<USloshColliderComponent>();
		const UStaticMeshComponent* M = FloatingBox->GetStaticMeshComponent();
		UE_LOG(LogSloshDemo, Display, TEXT("trace t=%.3f x=%.0f y=%.0f z=%.1f vz=%.1f sub=%.2f in=%d age=%d F=%s vf=%s"), Elapsed,
			FloatingBox->GetActorLocation().X, FloatingBox->GetActorLocation().Y, FloatingBox->GetActorLocation().Z, M->GetPhysicsLinearVelocity().Z, C->GetSubmergedFraction(), C->IsInLiquid() ? 1 : 0,
			C->GetFeedbackAgeFrames(), *C->GetLastFluidForce().ToCompactString(), *C->GetLocalFluidVelocity().ToCompactString());
		// Raw (unsmoothed) solver pressure force in N, for jitter statistics.
		const FVector Fp = C->GetLastPressureForceNewtons();
		const FVector Pp = PressureProbe ? PressureProbe->FindComponentByClass<USloshColliderComponent>()->GetLastPressureForceNewtons() : FVector::ZeroVector;
		UE_LOG(LogSloshDemo, Display, TEXT("rawforce t=%.3f box=%.2f,%.2f,%.2f probe=%.2f,%.2f,%.2f"), Elapsed, Fp.X, Fp.Y, Fp.Z, Pp.X, Pp.Y, Pp.Z);
	}

	// Per-frame speed trace (CFL / clamp diagnostics).
	if (Volume && FParse::Param(FCommandLine::Get(), TEXT("SloshTraceSpeed")))
	{
		const FSloshResults& R = Volume->GetLatestResults();
		UE_LOG(LogSloshDemo, Display, TEXT("speed t=%.3f max=%.0fcm/s substeps=%d clamped=%d inSolid=%d at=(%.2f,%.2f,%.2f)m div=%.2f box z=%.1f"), Elapsed,
			R.MaxParticleSpeed * 100.0f, R.Substeps, R.ClampedParticles, R.ClampedInSolid, R.ClampedCentroid.X, R.ClampedCentroid.Y, R.ClampedCentroid.Z,
			R.MaxDivergence, FloatingBox ? FloatingBox->GetActorLocation().Z : 0.0);
	}

	FrameTimeAccum += DeltaSeconds;
	GpuTimeAccum += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());
	++FrameCount;
	if (Elapsed >= NextLog && Volume)
	{
		NextLog += 1.0;
		const double AvgMs = FrameTimeAccum / FMath::Max(FrameCount, 1) * 1000.0;
		const double AvgGpuMs = GpuTimeAccum / FMath::Max(FrameCount, 1);
		const USloshColliderComponent* BoxCollider = FloatingBox ? FloatingBox->FindComponentByClass<USloshColliderComponent>() : nullptr;
		const USloshColliderComponent* SphereCollider = HeavySphere ? HeavySphere->FindComponentByClass<USloshColliderComponent>() : nullptr;
		UE_LOG(LogSloshDemo, Display,
			TEXT("t=%.1fs frame=%.2fms (%.1f fps) gpu=%.2fms particles=%d drained=%lld substeps=%d maxSpeed=%.0fcm/s liquid=%.1fL rho=%.4f pvol=%.1fL active=%.2f pool=%.2f ovf=%d mem=%.0f+%.0fMB clamp=%d latency=%d div(mean/max)=%.3f/%.2f 1/s | box z=%.1f sub=%.2f F=%s | sphere z=%.1f sub=%.2f"),
			Elapsed, AvgMs, 1000.0 / FMath::Max(AvgMs, 0.001), AvgGpuMs, Volume->GetParticleCount(), Volume->GetDrainedParticleCount(), Volume->GetLastSubstepCount(),
			Volume->GetMaxParticleSpeed(), Volume->GetLiquidVolumeLiters(), Volume->GetMeanInteriorDensity(), Volume->GetParticleVolumeLiters(), Volume->GetActiveBrickFraction(),
			Volume->GetPoolUsage(), Volume->GetPoolOverflowBricks(), Volume->GetGridMemoryMB(), Volume->GetParticleMemoryMB(), Volume->GetClampedParticleCount(),
			Volume->GetFeedbackLatencyFrames(), Volume->GetMeanDivergence(), Volume->GetMaxDivergence(),
			FloatingBox ? FloatingBox->GetActorLocation().Z : 0.0, BoxCollider ? BoxCollider->GetSubmergedFraction() : 0.0f,
			BoxCollider ? *BoxCollider->GetLastFluidForce().ToCompactString() : TEXT("-"),
			HeavySphere ? HeavySphere->GetActorLocation().Z : 0.0, SphereCollider ? SphereCollider->GetSubmergedFraction() : 0.0f);
		if (PressureProbe)
		{
			const USloshColliderComponent* Probe = PressureProbe->FindComponentByClass<USloshColliderComponent>();
			const FVector F = Probe->GetLastPressureForceNewtons();
			const double Archimedes = 1000.0 * 9.81 * Probe->GetVoxelVolumeLiters() * 1e-3;
			if (Elapsed > 8.0)
			{
				ProbeForceAccum += F;
				ProbeVolumeAccum += Archimedes;
				++ProbeSamples;
			}
			UE_LOG(LogSloshDemo, Display, TEXT("probe t=%.1f F=(%.0f, %.0f, %.0f) N  archimedes(voxel)=%.0f N  ratio=%.3f  mean(t>8s) Fz/archimedes=%.3f lateral=%.0f N"),
				Elapsed, F.X, F.Y, F.Z, Archimedes, Archimedes > 0.0 ? F.Z / Archimedes : 0.0,
				ProbeSamples > 0 && ProbeVolumeAccum > 0.0 ? ProbeForceAccum.Z / ProbeVolumeAccum : 0.0,
				ProbeSamples > 0 ? FVector(ProbeForceAccum.X, ProbeForceAccum.Y, 0).Size() / ProbeSamples : 0.0);
		}
		if (bCalm)
		{
			// Ground truth for a still pool: surface height from the particle volume at rest density (8 per cell),
			// and the Archimedes equilibrium of the 40 cm, 35 kg box (density ratio 0.547).
			const double H = Volume->CellSize;
			const FIntVector Res = Volume->GridResolution;
			const double InteriorArea = double(Res.X - 2) * (Res.Y - 2) * H * H;
			const double LiquidVolume = double(Volume->GetParticleCount()) / 8.0 * H * H * H;
			const double SurfaceZ = 125.0 - Volume->GetDomainSize().Z * 0.5 + H + LiquidVolume / InteriorArea;
			const double ExpectedBoxZ = SurfaceZ - 0.547 * 40.0 + 20.0;
			UE_LOG(LogSloshDemo, Display, TEXT("calm t=%.1f surface(est)=%.1f expectedBoxZ=%.1f boxZ=%.1f"), Elapsed, SurfaceZ, ExpectedBoxZ,
				FloatingBox ? FloatingBox->GetActorLocation().Z : 0.0);
		}
		FrameTimeAccum = 0.0;
		GpuTimeAccum = 0.0;
		FrameCount = 0;
	}

	if (bAutoTest)
	{
		APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
		if (!bProfiled && Elapsed > AutoTestSeconds * 0.5 && PC)
		{
			bProfiled = true;
			PC->ConsoleCommand(TEXT("ProfileGPU"));
		}
		if (!bScreenshot && Elapsed > AutoTestSeconds * 0.75 && PC)
		{
			bScreenshot = true;
			PC->ConsoleCommand(TEXT("HighResShot 1"));
		}
		if (Elapsed > AutoTestSeconds)
		{
			bAutoTest = false;
			FPlatformMisc::RequestExit(false, TEXT("SloshAutoTest"));
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Slosh Paint test scene (-SloshPaintTest). Uses the FluidNinja sample mannequin when present (local test content).
// ---------------------------------------------------------------------------------------------
namespace
{
	USloshPaintPreset* LoadPaintPreset(const TCHAR* Pack, const TCHAR* Name)
	{
		return LoadObject<USloshPaintPreset>(nullptr, *FString::Printf(TEXT("/Slosh/Paint/Presets/%s/DA_SloshPaint_%s.DA_SloshPaint_%s"), Pack, Name, Name));
	}
}

void ASloshDemoGameMode::StartPaintTest()
{
	bPaintTest = true;
	UWorld* World = GetWorld();

	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/FluidNinjaLive/Meshes/Mannequin/Mesh/SKM_Manny_Simple.SKM_Manny_Simple"));
	UAnimSequence* Anim = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/FluidNinjaLive/Meshes/Mannequin/Animations/MF_Unarmed_Walk_Fwd.MF_Unarmed_Walk_Fwd"));
	if (Mesh)
	{
		// Faces -X (towards the camera).
		ASkeletalMeshActor* Character = World->SpawnActor<ASkeletalMeshActor>(FVector(0, 0, 0), FRotator(0, 90, 0));
		USkeletalMeshComponent* Skel = Character->GetSkeletalMeshComponent();
		Skel->SetSkeletalMesh(Mesh);
		Skel->bEnableUpdateRateOptimizations = false;
		if (UMaterialInterface* Dark = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/SloshTest/M_TestSurface.M_TestSurface")))
		{
			for (int32 I = 0; I < Skel->GetNumMaterials(); ++I)
			{
				Skel->SetMaterial(I, Dark);   // contrast for light presets
			}
		}
		if (Anim)
		{
			Skel->SetAnimationMode(EAnimationMode::AnimationSingleNode);
			Skel->PlayAnimation(Anim, true);
			Skel->SetPlayRate(0.5f);
		}
		PaintCharacter = Character;
	}
	UE_LOG(LogSloshDemo, Display, TEXT("paint test: character %s, animation %s"), Mesh ? TEXT("loaded") : TEXT("MISSING"), Anim ? TEXT("loaded") : TEXT("MISSING"));

	AStaticMeshActor* Sphere = World->SpawnActor<AStaticMeshActor>(FVector(0, 110, 110), FRotator::ZeroRotator);
	Sphere->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	Sphere->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
	Sphere->SetActorScale3D(FVector(0.6));
	Sphere->GetStaticMeshComponent()->SetMaterial(0, LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/SloshTest/M_TestSurface.M_TestSurface")));

	ACameraActor* Camera = World->SpawnActor<ACameraActor>(FVector(-190, 50, 120), FRotator::ZeroRotator);
	Camera->SetActorRotation((FVector(0, 50, 112) - Camera->GetActorLocation()).Rotation());
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0))
	{
		PC->SetViewTarget(Camera);
		if (APawn* Pawn = PC->GetPawn())
		{
			Pawn->SetActorHiddenInGame(true);   // the default pawn spawns at the origin, inside the character
			Pawn->SetActorEnableCollision(false);
		}
	}
	// The nozzle Blueprint (auto-paint along its forward axis) sweeps across the lower half of the sphere.
	if (UClass* NozzleClass = LoadClass<AActor>(nullptr, TEXT("/Slosh/Paint/Blueprints/BP_SloshNozzle.BP_SloshNozzle_C")))
	{
		PaintNozzle = World->SpawnActor<AActor>(NozzleClass, FVector(-60, 90, 95), FRotator::ZeroRotator);
	}
	UE_LOG(LogSloshDemo, Display, TEXT("paint test: nozzle %s"), PaintNozzle ? TEXT("spawned") : TEXT("MISSING"));
	AutoTestSeconds = FMath::Max(AutoTestSeconds, 6.0);
}

void ASloshDemoGameMode::PaintTestStroke(USloshPaintPreset* Preset, const TArray<FVector>& Targets, const FVector& RayDir)
{
	USloshPaintComponent* Paint = nullptr;
	int32 Stroke = 0;
	for (const FVector& Target : Targets)
	{
		const FSloshPaintHit Hit = USloshPaintLibrary::TracePaintSurface(this, Target - RayDir * 150.0, Target + RayDir * 150.0, ECC_Visibility, {}, true);
		if (!Hit.bHit)
		{
			continue;
		}
		if (!Paint)
		{
			Paint = USloshPaintComponent::FindOrAddPaintComponent(Hit.Component);
			Paint->UnitScale = 6.0f;
			Stroke = Paint->BeginStroke(Preset, Hit.Location, Hit.Normal, 1.0f, Hit.Bone);
		}
		else
		{
			Paint->AddStrokePoint(Stroke, Hit.Location, Hit.Normal, 1.0f, Hit.Bone);
		}
	}
	if (!Paint)
	{
		UE_LOG(LogSloshDemo, Warning, TEXT("paint test: stroke missed everything (%d rays)"), Targets.Num());
		return;
	}
	Paint->EndStroke(Stroke);
	TSet<FName> Bones;
	const FSloshPaintStroke* Data = Paint->FindStroke(Stroke);
	for (const FSloshPaintPoint& Point : Data->Points)
	{
		Bones.Add(Point.Bone);
	}
	FString BoneList;
	for (const FName& Bone : Bones)
	{
		BoneList += Bone.ToString() + TEXT(" ");
	}
	UE_LOG(LogSloshDemo, Display, TEXT("paint test: stroke %d on %s, %d/%d points, bones: %s"), Stroke, *GetNameSafe(Paint->GetAttachParent()),
		Data->Points.Num(), Targets.Num(), *BoneList);
}

void ASloshDemoGameMode::TickPaintTest()
{
	if (PaintTestStage == 0 && Elapsed > 1.0)
	{
		PaintTestStage = 1;
		USloshPaintPreset* Cum = LoadPaintPreset(TEXT("NSFW"), TEXT("Cum"));
		USloshPaintPreset* Honey = LoadPaintPreset(TEXT("Regular"), TEXT("Honey"));
		USloshPaintPreset* Ketchup = LoadPaintPreset(TEXT("Regular"), TEXT("Ketchup"));
		const FVector Dir(1, 0, 0);
		// Character: a band across the chest, a line down one arm, a wavy line down the belly.
		TArray<FVector> Chest, Arm, Belly;
		for (int32 K = 0; K <= 40; ++K)
		{
			const float U = K / 40.0f;
			Chest.Add(FVector(0, FMath::Lerp(-16.0f, 16.0f, U), 138 + 5 * FMath::Sin(U * 6.0f)));
			Arm.Add(FVector(0, FMath::Lerp(-24.0f, -40.0f, U), FMath::Lerp(140.0f, 100.0f, U)));
			Belly.Add(FVector(0, 3 + 2 * FMath::Sin(U * 9.0f), FMath::Lerp(125.0f, 100.0f, U)));
		}
		PaintTestStroke(Cum, Chest, Dir);
		PaintTestStroke(Ketchup, Arm, Dir);
		PaintTestStroke(Cum, Belly, Dir);
		// Sphere: a swirl and a drip.
		TArray<FVector> Swirl, Drip;
		for (int32 K = 0; K <= 60; ++K)
		{
			const float U = K / 60.0f;
			Swirl.Add(FVector(0, 110 + 18 * FMath::Cos(U * 9.0f), 115 + 14 * FMath::Sin(U * 9.0f)));
			Drip.Add(FVector(0, 100, FMath::Lerp(135.0f, 90.0f, U)));
		}
		PaintTestStroke(Honey, Swirl, Dir);
		PaintTestStroke(Honey, Drip, Dir);
	}
	if (PaintNozzle)
	{
		// Sweep for 2.5 s, then stop (ends the stroke).
		const double U = FMath::Clamp((Elapsed - 0.5) / 2.5, 0.0, 1.0);
		PaintNozzle->SetActorLocation(FVector(-60, FMath::Lerp(88.0, 132.0, U), 95 + 6 * FMath::Sin(U * 12.0)));
		if (U >= 1.0)
		{
			if (USloshPaintBrushComponent* Brush = PaintNozzle->FindComponentByClass<USloshPaintBrushComponent>(); Brush && Brush->IsPainting())
			{
				Brush->StopPainting();
				UE_LOG(LogSloshDemo, Display, TEXT("paint test: nozzle stroke finished"));
			}
		}
	}
	const FString BenchDir = FPaths::ProjectSavedDir() / TEXT("Bench");
	if (PaintTestStage == 1 && Elapsed > 3.5)
	{
		PaintTestStage = 2;
		FScreenshotRequest::RequestScreenshot(BenchDir / TEXT("PaintTest_A.png"), false, false);
	}
	if (PaintTestStage == 2 && Elapsed > 4.7)
	{
		PaintTestStage = 3;
		FScreenshotRequest::RequestScreenshot(BenchDir / TEXT("PaintTest_B.png"), false, false);
		if (PaintCharacter)
		{
			TArray<USceneComponent*> Components;
			PaintCharacter->GetComponents(Components);
			UE_LOG(LogSloshDemo, Display, TEXT("paint test: character has %d scene components after painting"), Components.Num());
		}
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
		{
			TArray<UPrimitiveComponent*> Prims;
			It->GetComponents(Prims);
			for (UPrimitiveComponent* Prim : Prims)
			{
				if (Prim->IsVisible())
				{
					UE_LOG(LogSloshDemo, Display, TEXT("paint test: %s.%s (%s) parent=%s socket=%s bounds=%s r=%.1f mat=%s"), *It->GetName(), *Prim->GetName(),
						*Prim->GetClass()->GetName(), *GetNameSafe(Prim->GetAttachParent()), *Prim->GetAttachSocketName().ToString(),
						*Prim->Bounds.Origin.ToString(), Prim->Bounds.SphereRadius, *GetNameSafe(Prim->GetMaterial(0)));
				}
			}
		}
	}
	if (Elapsed > AutoTestSeconds)
	{
		FPlatformMisc::RequestExit(false, TEXT("SloshPaintTest"));
	}
}

// ---------------------------------------------------------------------------------------------
// Skeletal mesh collision test (-SloshSkelTest). A cylinder pours viscous, honey-like liquid onto the
// shoulder of a skeletal mesh whose bones carry fitted Slosh capsules; the arm swings and the body sways
// so the colliders move. Optional: -SloshSkelMesh=/Game/Path.Asset  -SloshSkelDebug (draw capsules)
//   -SloshSkelViscosity=m^2/s
// ---------------------------------------------------------------------------------------------
namespace
{
	FName FindBone(const FReferenceSkeleton& Ref, std::initializer_list<const TCHAR*> Names)
	{
		for (const TCHAR* Name : Names)
		{
			if (Ref.FindBoneIndex(Name) != INDEX_NONE)
			{
				return FName(Name);
			}
		}
		return NAME_None;
	}
}

void ASloshDemoGameMode::StartSkelTest()
{
	bSkelTest = true;
	UWorld* World = GetWorld();
	const TCHAR* Cmd = FCommandLine::Get();
	AutoTestSeconds = FMath::Max(AutoTestSeconds, 13.0);

	FString MeshPath = TEXT("/Game/FNIA/Frenni/SKM_FrenniFazclaire.SKM_FrenniFazclaire");
	FParse::Value(Cmd, TEXT("SloshSkelMesh="), MeshPath);
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		UE_LOG(LogSloshDemo, Error, TEXT("skel test: mesh %s not found"), *MeshPath);
		FPlatformMisc::RequestExit(false, TEXT("SloshSkelTest"));
		return;
	}

	// Poseable mesh: bones are driven directly from Tick, no animation asset needed.
	AActor* Character = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	SkelPose = NewObject<UPoseableMeshComponent>(Character, TEXT("Pose"));
	Character->SetRootComponent(SkelPose);
	SkelPose->SetSkinnedAssetAndUpdate(Mesh);
	SkelPose->RegisterComponent();

	SkelColliders = NewObject<USloshSkeletalColliderComponent>(Character, TEXT("SloshSkeletalCollider"));
	SkelColliders->bDrawCapsules = FParse::Param(Cmd, TEXT("SloshSkelDebug"));
	SkelColliders->SetupAttachment(SkelPose);
	SkelColliders->RegisterComponent();   // BeginPlay fits the capsules
	if (SkelColliders->GetColliders().IsEmpty())
	{
		SkelColliders->RebuildColliders();
	}
	for (USloshColliderComponent* Collider : SkelColliders->GetColliders())
	{
		Collider->bReportFeedbackWhenKinematic = true;   // wetness metric
		Collider->EnterThreshold = 0.02f;
	}

	// Character frame from the bind pose (component space == world space at the origin).
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	auto BonePos = [Mesh](FName Bone) { return Bone.IsNone() ? FVector::ZeroVector : FTransform(Mesh->GetComposedRefPoseMatrix(Bone)).GetLocation(); };
	const FBoxSphereBounds MeshBounds = Mesh->GetBounds();
	const FName Hips = FindBone(Ref, { TEXT("Hips"), TEXT("pelvis"), TEXT("Pelvis") });
	const FName Foot = FindBone(Ref, { TEXT("Foot_L"), TEXT("foot_l"), TEXT("LeftFoot") });
	const FName Toe = FindBone(Ref, { TEXT("Toe_L"), TEXT("ball_l"), TEXT("LeftToeBase") });
	SkelArmBone = FindBone(Ref, { TEXT("Upperarm_L"), TEXT("upperarm_l"), TEXT("LeftArm") });
	const FName Forearm = FindBone(Ref, { TEXT("Lowerarm_L"), TEXT("lowerarm_l"), TEXT("LeftForeArm") });

	FVector Forward = (!Foot.IsNone() && !Toe.IsNone()) ? (BonePos(Toe) - BonePos(Foot)) * FVector(1, 1, 0) : FVector(0, -1, 0);
	Forward = Forward.GetSafeNormal(UE_SMALL_NUMBER, FVector(0, -1, 0));
	const FVector Up = FVector::UpVector;
	FVector Target = MeshBounds.Origin + FVector(0, 0, MeshBounds.BoxExtent.Z * 0.55);
	FVector Side = FVector::CrossProduct(Up, Forward);
	if (!SkelArmBone.IsNone())
	{
		const FVector Shoulder = BonePos(SkelArmBone);
		const FVector Elbow = Forearm.IsNone() ? Shoulder : BonePos(Forearm);
		Target = FMath::Lerp(Shoulder, Elbow, 0.25) + Up * 4.0;
		const FVector Out = (Shoulder - (Hips.IsNone() ? MeshBounds.Origin : BonePos(Hips))) * FVector(1, 1, 0);
		Side = (Out - FVector::DotProduct(Out, Forward) * Forward).GetSafeNormal(UE_SMALL_NUMBER, Side);
		SkelArmRefCS = FTransform(Mesh->GetComposedRefPoseMatrix(SkelArmBone)).GetRotation();
		SkelSwingAxis = Forward;
	}

	// Nozzle: a pour from just above the shoulder, tilted slightly so the cylinder reads in front of the head.
	const float Speed = 60.0f;
	const FVector EmitPos = Target + Up * 38.0 + Forward * 4.0;
	const FVector Aim = (-Up + Forward * 0.12 + Side * 0.05).GetSafeNormal();

	AStaticMeshActor* Cylinder = World->SpawnActor<AStaticMeshActor>(EmitPos - Aim * 23.0, FRotationMatrix::MakeFromZ(Aim).Rotator());
	Cylinder->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	Cylinder->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
	Cylinder->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Cylinder->SetActorScale3D(FVector(0.09, 0.09, 0.4));   // 9 cm wide, 40 cm long; the emitter sits just past its tip

	AActor* EmitterActor = World->SpawnActor<AActor>(EmitPos, Aim.Rotation());
	USceneComponent* EmitterRoot = NewObject<USceneComponent>(EmitterActor);
	EmitterActor->SetRootComponent(EmitterRoot);
	EmitterRoot->RegisterComponent();
	EmitterActor->SetActorLocationAndRotation(EmitPos, Aim.Rotation());
	USloshEmitterComponent* Emitter = NewObject<USloshEmitterComponent>(EmitterActor, TEXT("Emitter"));
	Emitter->SetupAttachment(EmitterRoot);
	Emitter->Radius = 3.0f;
	Emitter->ParticlesPerSecond = 2500.0f;   // ~1.2 L/s at 2.5 cm cells (8 particles per cell)
	Emitter->Speed = Speed;
	Emitter->RegisterComponent();

	// Domain: the character plus the nozzle, floor at z = 0 (the grid's wall layer sits just below).
	FBox Domain = MeshBounds.GetBox();
	Domain += EmitPos;
	Domain = Domain.ExpandBy(FVector(30, 30, 15));
	const float Cell = 2.5f;
	Domain.Min.Z = -Cell;
	auto Cells = [Cell](double Size) -> int32 { return FMath::DivideAndRoundUp(int32(FMath::CeilToInt(Size / Cell)), 8) * 8; };
	const FIntVector Res(Cells(Domain.GetSize().X), Cells(Domain.GetSize().Y), Cells(Domain.GetSize().Z));
	const FVector DomainSize = FVector(Res) * Cell;
	const FVector DomainCenter(Domain.GetCenter().X, Domain.GetCenter().Y, Domain.Min.Z + DomainSize.Z * 0.5);

	FActorSpawnParameters Deferred;
	Deferred.bDeferConstruction = true;
	Volume = World->SpawnActor<ASloshVolume>(DomainCenter, FRotator::ZeroRotator, Deferred);
	Volume->GridResolution = Res;
	Volume->CellSize = Cell;
	Volume->ParticleCapacity = 1 << 20;
	Volume->bFillOnBeginPlay = false;
	// Honey-like: dense, very viscous, sticky at solid contact.
	Volume->Physical.Density = 1400.0f;
	Volume->Physical.KinematicViscosity = 0.01f;
	FParse::Value(Cmd, TEXT("SloshSkelViscosity="), Volume->Physical.KinematicViscosity);
	// -SloshSkelPreset=Honey: a Regular Fluid Painter preset as the liquid (physics, look and mesh material).
	FString PresetName;
	if (FParse::Value(Cmd, TEXT("SloshSkelPreset="), PresetName))
	{
		Volume->Material = LoadObject<USloshMaterialAsset>(nullptr, *FString::Printf(TEXT("/Slosh/Paint/Presets/Regular/DA_SloshPaint_%s.DA_SloshPaint_%s"), *PresetName, *PresetName));
		UE_LOG(LogSloshDemo, Display, TEXT("skel test: preset %s %s"), *PresetName, Volume->Material ? TEXT("loaded") : TEXT("MISSING"));
	}
	Volume->Physical.Transfer = ESloshTransfer::APIC;
	Volume->Physical.BoundaryFriction = 0.9f;
	Volume->Visual.AbsorptionPerMeter = FVector(6.0, 18.0, 60.0);
	Volume->Visual.ScatterColor = FLinearColor(0.35f, 0.17f, 0.02f);
	Volume->Visual.FresnelF0 = 0.04f;
	Volume->Visual.SpecularPower = 250.0f;
	ApplySurfaceFlags(Volume);
	UGameplayStatics::FinishSpawningActor(Volume, FTransform(DomainCenter));

	AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(FVector(DomainCenter.X, DomainCenter.Y, 0), FRotator::ZeroRotator);
	Floor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
	Floor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Floor->SetActorScale3D(FVector(DomainSize.X / 100.0 + 2.0, DomainSize.Y / 100.0 + 2.0, 1.0));
	if (UMaterialInterface* Dark = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/SloshTest/M_TestSurface.M_TestSurface")))
	{
		Floor->GetStaticMeshComponent()->SetMaterial(0, Dark);
	}

	const FVector LookAt = Target - Up * 20.0;
	ACameraActor* Camera = World->SpawnActor<ACameraActor>(LookAt + Forward * 150.0 + Side * 95.0 + Up * 10.0, FRotator::ZeroRotator);
	Camera->SetActorRotation((LookAt - Camera->GetActorLocation()).Rotation());
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0))
	{
		PC->SetViewTarget(Camera);
		if (APawn* Pawn = PC->GetPawn())
		{
			Pawn->SetActorHiddenInGame(true);
			Pawn->SetActorEnableCollision(false);
		}
	}

	UE_LOG(LogSloshDemo, Display, TEXT("skel test: mesh %s, %d colliders, forward %s side %s, target %s, nozzle %s aim %s, grid %s (%.1f cm) domain %s"),
		*Mesh->GetName(), SkelColliders->GetColliders().Num(), *Forward.ToCompactString(), *Side.ToCompactString(), *Target.ToCompactString(),
		*EmitPos.ToCompactString(), *Aim.ToCompactString(), *Res.ToString(), Cell, *DomainSize.ToCompactString());
}

void ASloshDemoGameMode::TickSkelTest()
{
	// The arm raises and lowers in the frontal plane; the whole body sways a little.
	if (SkelPose && !SkelArmBone.IsNone())
	{
		const double Angle = FMath::DegreesToRadians(22.0) * (0.5 - 0.5 * FMath::Cos(Elapsed * UE_TWO_PI * 0.2));
		SkelPose->SetBoneRotationByName(SkelArmBone, (FQuat(SkelSwingAxis, -Angle) * SkelArmRefCS).Rotator(), EBoneSpaces::ComponentSpace);
	}
	if (SkelPose)
	{
		SkelPose->GetOwner()->SetActorRotation(FRotator(0, 6.0 * FMath::Sin(Elapsed * 0.7), 0));
	}
	if (Elapsed > 9.0)
	{
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
		{
			if (USloshEmitterComponent* Emitter = It->FindComponentByClass<USloshEmitterComponent>())
			{
				Emitter->bEmitting = false;   // watch it drip and settle
			}
		}
	}

	FrameTimeAccum += GetWorld()->GetDeltaSeconds();
	GpuTimeAccum += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());
	++FrameCount;
	if (Elapsed >= NextLog && Volume)
	{
		NextLog += 1.0;
		int32 Wet = 0;
		FString WetBones;
		if (SkelColliders)
		{
			for (const USloshColliderComponent* Collider : SkelColliders->GetColliders())
			{
				if (Collider && Collider->GetSubmergedFraction() > 0.0f)
				{
					++Wet;
					WetBones += FString::Printf(TEXT(" %s:%.2f"), *Collider->GetAttachSocketName().ToString(), Collider->GetSubmergedFraction());
				}
			}
		}
		const double AvgMs = FrameTimeAccum / FMath::Max(FrameCount, 1) * 1000.0;
		UE_LOG(LogSloshDemo, Display, TEXT("skel t=%.1fs frame=%.2fms gpu=%.2fms particles=%d substeps=%d maxSpeed=%.0fcm/s liquid=%.2fL clamp=%d | wet colliders %d/%d:%s"),
			Elapsed, AvgMs, GpuTimeAccum / FMath::Max(FrameCount, 1), Volume->GetParticleCount(), Volume->GetLastSubstepCount(), Volume->GetMaxParticleSpeed(),
			Volume->GetLiquidVolumeLiters(), Volume->GetClampedParticleCount(), Wet, SkelColliders ? SkelColliders->GetColliders().Num() : 0, *WetBones);
		FrameTimeAccum = 0.0;
		GpuTimeAccum = 0.0;
		FrameCount = 0;
	}

	const FString BenchDir = FPaths::ProjectSavedDir() / TEXT("Bench");
	const double Shots[] = { 3.0, 6.0, 9.0, 12.5 };
	if (SkelTestStage < int32(UE_ARRAY_COUNT(Shots)) && Elapsed > Shots[SkelTestStage])
	{
		FScreenshotRequest::RequestScreenshot(BenchDir / FString::Printf(TEXT("SkelTest_%c.png"), TCHAR('A' + SkelTestStage)), false, false);
		++SkelTestStage;
	}
	if (bAutoTest && Elapsed > AutoTestSeconds)
	{
		FPlatformMisc::RequestExit(false, TEXT("SloshSkelTest"));
	}
}

void ASloshDemoGameMode::TickPlacedScene()
{
	FrameTimeAccum += GetWorld()->GetDeltaSeconds();
	GpuTimeAccum += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());
	++FrameCount;
	if (Elapsed >= NextLog && Volume)
	{
		NextLog += 1.0;
		FString Wet;
		for (TActorIterator<ASloshSkelTestCharacter> It(GetWorld()); It; ++It)
		{
			int32 Count = 0;
			const FString Bones = It->DescribeWetBones(Count);
			Wet += FString::Printf(TEXT(" | %s wet %d/%d:%s"), *It->GetActorNameOrLabel(), Count, It->SloshCollider->GetColliders().Num(), *Bones);
		}
		const double AvgMs = FrameTimeAccum / FMath::Max(FrameCount, 1) * 1000.0;
		UE_LOG(LogSloshDemo, Display, TEXT("placed t=%.1fs frame=%.2fms gpu=%.2fms particles=%d drained=%lld substeps=%d maxSpeed=%.0fcm/s liquid=%.2fL clamp=%d%s"),
			Elapsed, AvgMs, GpuTimeAccum / FMath::Max(FrameCount, 1), Volume->GetParticleCount(), Volume->GetDrainedParticleCount(), Volume->GetLastSubstepCount(),
			Volume->GetMaxParticleSpeed(), Volume->GetLiquidVolumeLiters(), Volume->GetClampedParticleCount(), *Wet);
		FrameTimeAccum = 0.0;
		GpuTimeAccum = 0.0;
		FrameCount = 0;
	}
	if (bAutoTest)
	{
		const double Shots[] = { 3.0, 6.0, 9.0, 12.5 };
		if (SkelTestStage < int32(UE_ARRAY_COUNT(Shots)) && Elapsed > Shots[SkelTestStage])
		{
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Bench") / FString::Printf(TEXT("Placed_%c.png"), TCHAR('A' + SkelTestStage)), false, false);
			++SkelTestStage;
		}
		if (Elapsed > AutoTestSeconds)
		{
			FPlatformMisc::RequestExit(false, TEXT("SloshAutoTest"));
		}
	}
}
