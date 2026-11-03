#include "SloshDemoGameMode.h"
#include "SloshVolume.h"
#include "SloshColliderComponent.h"
#include "SloshEmitterComponent.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogSloshDemo, Log, All);

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, SloshDemo, "SloshDemo");

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
	Emitter->RegisterComponent();
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
			TEXT("t=%.1fs frame=%.2fms (%.1f fps) gpu=%.2fms particles=%d substeps=%d maxSpeed=%.0fcm/s liquid=%.1fL rho=%.4f pvol=%.1fL active=%.2f pool=%.2f ovf=%d mem=%.0f+%.0fMB clamp=%d latency=%d div(mean/max)=%.3f/%.2f 1/s | box z=%.1f sub=%.2f F=%s | sphere z=%.1f sub=%.2f"),
			Elapsed, AvgMs, 1000.0 / FMath::Max(AvgMs, 0.001), AvgGpuMs, Volume->GetParticleCount(), Volume->GetLastSubstepCount(),
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
