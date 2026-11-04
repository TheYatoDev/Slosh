// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "SloshEmitterComponent.generated.h"

UENUM(BlueprintType)
enum class ESloshEmitterShape : uint8
{
	Sphere,
	/** Oriented with the component. */
	Box,
};

UENUM(BlueprintType)
enum class ESloshEmitRateMode : uint8
{
	ParticlesPerSecond,
	/** Liquid volume per second, independent of the volume's cell size (8 particles fill one cell). */
	LitersPerSecond,
};

/**
 * Emits liquid into whichever SloshVolume contains it, along the component's forward (X) axis.
 * Continuous emission (rate), one-shot bursts, a lifetime particle limit, velocity spread and inherited motion.
 */
UCLASS(ClassGroup = (Slosh), meta = (BlueprintSpawnableComponent))
class SLOSH_API USloshEmitterComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	USloshEmitterComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh")
	bool bEmitting = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Shape")
	ESloshEmitterShape Shape = ESloshEmitterShape::Sphere;

	/** Emission sphere radius in cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Shape", meta = (ClampMin = "1.0", EditCondition = "Shape == ESloshEmitterShape::Sphere"))
	float Radius = 10.0f;

	/** Emission box half extents in cm (oriented with the component). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Shape", meta = (EditCondition = "Shape == ESloshEmitterShape::Box"))
	FVector BoxExtent = FVector(10.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Rate")
	ESloshEmitRateMode RateMode = ESloshEmitRateMode::ParticlesPerSecond;

	/** Particles per second (8 particles fill one solver cell). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Rate", meta = (ClampMin = "0.0", EditCondition = "RateMode == ESloshEmitRateMode::ParticlesPerSecond"))
	float ParticlesPerSecond = 20000.0f;

	/** Litres of liquid per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Rate", meta = (ClampMin = "0.0", EditCondition = "RateMode == ESloshEmitRateMode::LitersPerSecond"))
	float LitersPerSecond = 1.0f;

	/** Stop after emitting this many particles in total (0 = no limit). Bursts count too. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Rate", meta = (ClampMin = "0"))
	int32 MaxParticles = 0;

	/** Initial speed along the component's forward (X) axis, cm/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Velocity")
	float Speed = 300.0f;

	/** Half angle of the cone the initial direction is randomised within, degrees (0 = a straight jet). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Velocity", meta = (ClampMin = "0.0", ClampMax = "180.0", Units = "Degrees"))
	float VelocitySpread = 0.0f;

	/** Random +- variation of the speed as a fraction (0.2 = speeds between 80% and 120%). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Velocity", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SpeedVariation = 0.0f;

	/** Add the emitter's own motion to the emitted liquid (a moving hose, a swinging bucket). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Velocity")
	bool bInheritVelocity = false;

	UFUNCTION(BlueprintCallable, Category = "Slosh")
	void StartEmitting() { bEmitting = true; }

	UFUNCTION(BlueprintCallable, Category = "Slosh")
	void StopEmitting() { bEmitting = false; }

	/** Emits Count particles at once on the next frame (independent of bEmitting; respects MaxParticles). */
	UFUNCTION(BlueprintCallable, Category = "Slosh")
	void Burst(int32 Count) { PendingBurst += FMath::Max(Count, 0); }

	/** Emits a burst of the given liquid volume in litres (needs the volume's cell size, so applied on emission). */
	UFUNCTION(BlueprintCallable, Category = "Slosh")
	void BurstLiters(float Liters) { PendingBurstLiters += FMath::Max(Liters, 0.0f); }

	/** Particles this emitter has emitted since play started or the last reset. */
	UFUNCTION(BlueprintPure, Category = "Slosh")
	int64 GetEmittedParticles() const { return EmittedParticles; }

	/** Restarts the MaxParticles budget. */
	UFUNCTION(BlueprintCallable, Category = "Slosh")
	void ResetEmittedCount() { EmittedParticles = 0; }

	// Called by ASloshVolume.
	float Carry = 0.0f;
	int32 PendingBurst = 0;
	float PendingBurstLiters = 0.0f;
	int64 EmittedParticles = 0;
	FVector PreviousLocation = FVector::ZeroVector;
	bool bHasPreviousLocation = false;

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
};
