#include "SloshShowcaseActors.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

ASloshShowcaseMover::ASloshShowcaseMover()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ASloshShowcaseMover::BeginPlay()
{
	Super::BeginPlay();
	if (Target)
	{
		TargetStart = Target->GetActorTransform();
	}
}

void ASloshShowcaseMover::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Target)
	{
		return;
	}
	Time += DeltaSeconds;
	const double T = FMath::Max(Time - StartDelay, 0.0);
	const double Phase = T * UE_TWO_PI / FMath::Max(Period, 0.1f);
	switch (Mode)
	{
	case ESloshShowcaseMotion::Rotate:
		Target->SetActorRotation(TargetStart.Rotator() + RotationRate * float(T));
		break;
	case ESloshShowcaseMotion::Orbit:
	{
		const FVector Center = GetActorLocation();
		const FVector Offset(FMath::Cos(Phase) * Amplitude, FMath::Sin(Phase) * Amplitude, 0.0);
		Target->SetActorLocation(Center + GetActorRotation().RotateVector(Offset) + FVector(0, 0, TargetStart.GetLocation().Z - Center.Z));
		break;
	}
	default:
		Target->SetActorLocation(TargetStart.GetLocation() + Axis.GetSafeNormal() * (Amplitude * FMath::Sin(Phase)));
		break;
	}
}

ASloshShowcaseBody::ASloshShowcaseBody()
{
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetMobility(EComponentMobility::Movable);
	RootComponent = Mesh;
	Collider = CreateDefaultSubobject<USloshColliderComponent>(TEXT("Collider"));
	Collider->SetupAttachment(Mesh);
	// Basic shapes are 100 cm; the collider inherits the mesh scale.
	Collider->Radius = 50.0f;
	Collider->BoxExtent = FVector(50.0);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	Mesh->SetStaticMesh(Cube.Object);
}

void ASloshShowcaseBody::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UStaticMesh* ShapeMesh = LoadObject<UStaticMesh>(nullptr, Shape == ESloshShowcaseShape::Sphere
		? TEXT("/Engine/BasicShapes/Sphere.Sphere") : TEXT("/Engine/BasicShapes/Cube.Cube"));
	Mesh->SetStaticMesh(ShapeMesh);
	const FVector Scale = Shape == ESloshShowcaseShape::Sphere ? FVector(Size.X / 100.0) : Size / 100.0;
	Mesh->SetRelativeScale3D(Scale);
	Collider->Shape = Shape == ESloshShowcaseShape::Sphere ? ESloshColliderShapeType::Sphere : ESloshColliderShapeType::Box;
	Collider->CouplingMode = CouplingMode;
	if (SurfaceMaterial)
	{
		Mesh->SetMaterial(0, SurfaceMaterial);
	}
}

void ASloshShowcaseBody::BeginPlay()
{
	Super::BeginPlay();
	if (bSimulatePhysics)
	{
		Mesh->SetSimulatePhysics(true);
		Mesh->SetMassOverrideInKg(NAME_None, MassKg, true);
		Mesh->SetLinearDamping(0.05f);
	}
}
