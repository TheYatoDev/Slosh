#include "SloshSkelTestActors.h"
#include "SloshColliderComponent.h"
#include "SloshEmitterComponent.h"
#include "SloshSkeletalColliderComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ASloshSkelTestCharacter::ASloshSkelTestCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	Mesh = CreateDefaultSubobject<UPoseableMeshComponent>(TEXT("Mesh"));
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RootComponent = Mesh;

	SloshCollider = CreateDefaultSubobject<USloshSkeletalColliderComponent>(TEXT("SloshCollider"));
	SloshCollider->SetupAttachment(Mesh);
	SloshCollider->bReportWetness = true;
}

void ASloshSkelTestCharacter::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (Mesh->GetSkinnedAsset() != SkeletalMesh)
	{
		Mesh->SetSkinnedAssetAndUpdate(SkeletalMesh);
	}
}

void ASloshSkelTestCharacter::BeginPlay()
{
	Super::BeginPlay();
	PlacedRotation = GetActorRotation();
	if (SkeletalMesh && !SwingBone.IsNone() && SkeletalMesh->GetRefSkeleton().FindBoneIndex(SwingBone) != INDEX_NONE)
	{
		SwingRefCS = FTransform(SkeletalMesh->GetComposedRefPoseMatrix(SwingBone)).GetRotation();
	}
}

void ASloshSkelTestCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bAnimate)
	{
		return;
	}
	Time += DeltaSeconds;
	if (SkeletalMesh && !SwingBone.IsNone() && SkeletalMesh->GetRefSkeleton().FindBoneIndex(SwingBone) != INDEX_NONE)
	{
		const double Angle = FMath::DegreesToRadians(SwingDegrees) * (0.5 - 0.5 * FMath::Cos(Time * UE_TWO_PI / SwingPeriod));
		const FVector Axis = SwingAxis.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
		Mesh->SetBoneRotationByName(SwingBone, (FQuat(Axis, Angle) * SwingRefCS).Rotator(), EBoneSpaces::ComponentSpace);
	}
	SetActorRotation(PlacedRotation + FRotator(0, SwayDegrees * FMath::Sin(Time * UE_TWO_PI / SwayPeriod), 0));
}

FString ASloshSkelTestCharacter::DescribeWetBones(int32& OutWetCount) const
{
	OutWetCount = 0;
	FString Out;
	for (const USloshColliderComponent* Collider : SloshCollider->GetColliders())
	{
		if (Collider && Collider->GetSubmergedFraction() > 0.0f)
		{
			++OutWetCount;
			Out += FString::Printf(TEXT(" %s:%.2f"), *Collider->GetAttachSocketName().ToString(), Collider->GetSubmergedFraction());
		}
	}
	return Out;
}

ASloshPourNozzle::ASloshPourNozzle()
{
	PrimaryActorTick.bCanEverTick = true;

	Emitter = CreateDefaultSubobject<USloshEmitterComponent>(TEXT("Emitter"));
	Emitter->Radius = 3.0f;
	Emitter->ParticlesPerSecond = 2500.0f;   // ~1.2 L/s at 2.5 cm cells (8 particles per cell)
	Emitter->Speed = 60.0f;
	RootComponent = Emitter;

	// 9 cm wide, 40 cm long, lying along -X behind the emitter.
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(Emitter);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	Body->SetStaticMesh(Cylinder.Object);
	Body->SetRelativeLocation(FVector(-23.0, 0.0, 0.0));
	Body->SetRelativeRotation(FRotator(90.0, 0.0, 0.0));
	Body->SetRelativeScale3D(FVector(0.09, 0.09, 0.4));
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void ASloshPourNozzle::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Time += DeltaSeconds;
	Emitter->bEmitting = Time >= StartDelay && (StopAfter <= 0.0f || Time < StopAfter);
}
