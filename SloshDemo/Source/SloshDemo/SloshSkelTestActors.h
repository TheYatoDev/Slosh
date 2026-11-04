#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SloshSkelTestActors.generated.h"

class UPoseableMeshComponent;
class USkeletalMesh;
class UStaticMeshComponent;
class USloshEmitterComponent;
class USloshSkeletalColliderComponent;

/**
 * Placeable skeletal-collision test subject: a poseable mesh made solid to Slosh liquid by fitted bone
 * capsules, with an optional procedural bone swing and body sway so the colliders move.
 * Capsule fitting settings live on the SloshCollider component.
 */
UCLASS()
class ASloshSkelTestCharacter : public AActor
{
	GENERATED_BODY()

public:
	ASloshSkelTestCharacter();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Slosh Test")
	TObjectPtr<USkeletalMesh> SkeletalMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Slosh Test")
	TObjectPtr<UPoseableMeshComponent> Mesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Slosh Test")
	TObjectPtr<USloshSkeletalColliderComponent> SloshCollider;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Test|Motion")
	bool bAnimate = true;

	/** Bone rotated back and forth (its children follow). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Test|Motion")
	FName SwingBone = TEXT("Upperarm_L");

	/** Swing axis in mesh component space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Test|Motion")
	FVector SwingAxis = FVector(0, 1, 0);

	/** Peak swing angle; the bone goes from its bind pose to this and back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Test|Motion", meta = (Units = "Degrees"))
	float SwingDegrees = -22.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Test|Motion", meta = (ClampMin = "0.1", Units = "Seconds"))
	float SwingPeriod = 5.0f;

	/** Whole-body yaw sway amplitude around the placed rotation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Test|Motion", meta = (Units = "Degrees"))
	float SwayDegrees = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Test|Motion", meta = (ClampMin = "0.1", Units = "Seconds"))
	float SwayPeriod = 9.0f;

	/** Per-bone wetness summary ("Bone:fraction ..."), from the delayed GPU feedback. */
	UFUNCTION(BlueprintPure, Category = "Slosh Test")
	FString DescribeWetBones(int32& OutWetCount) const;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

private:
	FQuat SwingRefCS = FQuat::Identity;
	FRotator PlacedRotation = FRotator::ZeroRotator;
	double Time = 0.0;
};

/** Placeable pouring nozzle: a cylinder with a Slosh emitter at its tip, emitting along the actor's +X. */
UCLASS()
class ASloshPourNozzle : public AActor
{
	GENERATED_BODY()

public:
	ASloshPourNozzle();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Slosh Test")
	TObjectPtr<UStaticMeshComponent> Body;

	/** Rate, radius and speed live here. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Slosh Test")
	TObjectPtr<USloshEmitterComponent> Emitter;

	/** Wait this long after play starts before pouring. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Test", meta = (ClampMin = "0.0", Units = "Seconds"))
	float StartDelay = 0.0f;

	/** Stop pouring after this many seconds of play (0 = pour forever). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Test", meta = (ClampMin = "0.0", Units = "Seconds"))
	float StopAfter = 9.0f;

	/** Disable continuous pouring (bursts only). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Test")
	bool bContinuous = true;

	/** Fire Emitter->Burst(BurstParticles) every this many seconds (0 = no bursts). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Test", meta = (ClampMin = "0.0", Units = "Seconds"))
	float BurstInterval = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh Test", meta = (ClampMin = "0"))
	int32 BurstParticles = 4000;

	virtual void Tick(float DeltaSeconds) override;

private:
	double Time = 0.0;
	double NextBurst = 0.0;
};
