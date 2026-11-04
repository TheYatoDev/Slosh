// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "SloshTypes.h"
#include "SloshManipulatorComponent.generated.h"

UENUM(BlueprintType)
enum class ESloshManipulatorShapeType : uint8
{
	Sphere,
	Box,
	/** Along local Z: Radius plus CapsuleHalfHeight of straight section. */
	Capsule,
	/** The whole liquid domain (force fields only make sense with this; a drain would empty the volume). */
	Everywhere,
};

/**
 * Base for particle manipulators acting on whichever SloshVolume they overlap (after ZibraLiquids' manipulators):
 * a shape, an enabled switch, and a fade-out distance. See USloshDrainComponent and USloshForceFieldComponent.
 */
UCLASS(Abstract, ClassGroup = (Slosh))
class SLOSH_API USloshManipulatorComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	USloshManipulatorComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Shape")
	ESloshManipulatorShapeType Shape = ESloshManipulatorShapeType::Sphere;

	/** Sphere / capsule radius, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Shape", meta = (ClampMin = "0.1"))
	float Radius = 25.0f;

	/** Box half extents, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Shape")
	FVector BoxExtent = FVector(25.0);

	/** Capsule half height of the straight part along local Z, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Shape", meta = (ClampMin = "0.0"))
	float CapsuleHalfHeight = 25.0f;

	/** Draw the shape while playing (debug). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Debug")
	bool bDrawShape = false;

	/** Called by ASloshVolume: the GPU description in sim space, or false when it does not apply this frame. */
	virtual bool BuildGPU(const FTransform& WorldToSim, FSloshManipulatorGPU& Out) const;

	/** Rough reach in cm from the component location (for skipping manipulators far outside a domain). */
	float GetReach() const;

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Extra fade-out distance beyond the shape, cm (force fields). */
	virtual float GetFalloffCm() const { return 0.0f; }
	virtual FColor GetDebugColor() const { return FColor::White; }
};

/** Removes liquid that enters its shape (ZibraLiquids "Void"): a sink, an overflow, a drain in a floor. */
UCLASS(ClassGroup = (Slosh), meta = (BlueprintSpawnableComponent))
class SLOSH_API USloshDrainComponent : public USloshManipulatorComponent
{
	GENERATED_BODY()

public:
	/** Particles removed per second as a rate (each particle inside leaves with probability 1 - exp(-rate dt)).
	 *  0 removes everything inside at once. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (ClampMin = "0.0"))
	float RemovalRate = 0.0f;

	virtual bool BuildGPU(const FTransform& WorldToSim, FSloshManipulatorGPU& Out) const override;

protected:
	virtual FColor GetDebugColor() const override { return FColor::Red; }
};

UENUM(BlueprintType)
enum class ESloshForceType : uint8
{
	/** Constant acceleration along the component's forward (X) axis: wind, a push, a conveyor. */
	Directional,
	/** Away from the centre (positive Strength) or toward it (negative): explosions, magnets, gathering. */
	Radial,
	/** Around the component's up (Z) axis, with an optional pull toward the axis: whirlpools, stirring. */
	Swirl,
	/** Slows the liquid down (Strength in 1/s): thickening zones, calm areas. */
	Drag,
};

/** Pushes, pulls, swirls or slows the liquid inside its shape (ZibraLiquids "Force Field"). */
UCLASS(ClassGroup = (Slosh), meta = (BlueprintSpawnableComponent))
class SLOSH_API USloshForceFieldComponent : public USloshManipulatorComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh")
	ESloshForceType ForceType = ESloshForceType::Directional;

	/** Acceleration in cm/s^2 (gravity is 980). Drag: damping rate in 1/s. Negative Radial attracts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh")
	float Strength = 500.0f;

	/** Swirl only: pull toward the axis as a fraction of the swirl strength (keeps a whirlpool together). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (ClampMin = "0.0", ClampMax = "4.0", EditCondition = "ForceType == ESloshForceType::Swirl"))
	float SwirlInwardPull = 0.3f;

	/** Distance beyond the shape over which the force fades to zero, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (ClampMin = "0.0"))
	float Falloff = 10.0f;

	virtual bool BuildGPU(const FTransform& WorldToSim, FSloshManipulatorGPU& Out) const override;

protected:
	virtual float GetFalloffCm() const override { return Falloff; }
	virtual FColor GetDebugColor() const override { return FColor::Cyan; }
};
