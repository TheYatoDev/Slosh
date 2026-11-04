#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SloshColliderComponent.h"
#include "SloshShowcaseActors.generated.h"

class UStaticMeshComponent;
class UMaterialInterface;

UENUM(BlueprintType)
enum class ESloshShowcaseMotion : uint8
{
	/** Back and forth along Axis by Amplitude. */
	Oscillate,
	/** Spin at RotationRate. */
	Rotate,
	/** Circle around this mover's location in its XY plane, radius Amplitude. */
	Orbit,
};

/**
 * Showcase helper: moves another actor (kinematically) so liquid has something moving to react to:
 * sweeping nozzles, paddles, stirring rods. The target's colliders pick up the motion as velocity.
 */
UCLASS()
class ASloshShowcaseMover : public AActor
{
	GENERATED_BODY()

public:
	ASloshShowcaseMover();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase")
	TObjectPtr<AActor> Target;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase")
	ESloshShowcaseMotion Mode = ESloshShowcaseMotion::Oscillate;

	/** Oscillate: world direction of travel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase")
	FVector Axis = FVector(1, 0, 0);

	/** Oscillate: half the travel; Orbit: radius (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase", meta = (Units = "Centimeters"))
	float Amplitude = 50.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase", meta = (ClampMin = "0.1", Units = "Seconds"))
	float Period = 4.0f;

	/** Rotate: degrees per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase")
	FRotator RotationRate = FRotator(0, 45, 0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase", meta = (ClampMin = "0.0", Units = "Seconds"))
	float StartDelay = 0.0f;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

private:
	FTransform TargetStart;
	double Time = 0.0;
};

UENUM(BlueprintType)
enum class ESloshShowcaseShape : uint8
{
	Box,
	Sphere,
};

/**
 * Showcase helper: a basic-shape body the liquid sees (a USloshColliderComponent sized to the mesh). Static
 * obstacle, kinematic paddle (with a mover) or a physics body that floats or sinks (bSimulatePhysics).
 */
UCLASS()
class ASloshShowcaseBody : public AActor
{
	GENERATED_BODY()

public:
	ASloshShowcaseBody();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Slosh Showcase")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Slosh Showcase")
	TObjectPtr<USloshColliderComponent> Collider;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase")
	ESloshShowcaseShape Shape = ESloshShowcaseShape::Box;

	/** Full size in cm (sphere: X is the diameter). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase")
	FVector Size = FVector(40.0);

	/** Let physics move it so the liquid's pressure can float or push it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase")
	bool bSimulatePhysics = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase", meta = (ClampMin = "0.1", Units = "Kilograms", EditCondition = "bSimulatePhysics"))
	float MassKg = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase")
	ESloshCouplingMode CouplingMode = ESloshCouplingMode::PressureAndDrag;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Showcase")
	TObjectPtr<UMaterialInterface> SurfaceMaterial;

	virtual void OnConstruction(const FTransform& Transform) override;

protected:
	virtual void BeginPlay() override;
};
