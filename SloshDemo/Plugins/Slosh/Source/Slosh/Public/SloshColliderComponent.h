// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "SloshTypes.h"
#include "SloshColliderComponent.generated.h"

class UPrimitiveComponent;

UENUM(BlueprintType)
enum class ESloshColliderShapeType : uint8
{
	Sphere,
	Box,
	Capsule,
};

UENUM(BlueprintType)
enum class ESloshCouplingMode : uint8
{
	/** Obstacle only: the body moves the liquid, the liquid never pushes back. */
	OneWay,
	/** Archimedes buoyancy from the GPU submerged-volume estimate plus quadratic drag. Never sticks to the
	 *  floor, but bobs (delayed sensor); prefer for bodies that may rest on the bottom. */
	BuoyancyAndDrag,
	/** Integrated solver pressure on the body surface plus drag. Settles floating bodies stably (measured).
	 *  Faces in contact with the floor, walls or other bodies receive the surrounding liquid's pressure
	 *  ("seepage"), so bodies resting on the bottom still feel buoyancy. */
	PressureAndDrag,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSloshContactSignature, USloshColliderComponent*, Collider);

/**
 * Analytic collision shape that the liquid sees, attached to (and usually sized to) a primitive.
 * When the parent primitive simulates physics, delayed GPU feedback is turned into forces on it.
 *
 * Feedback latency: values are 2-4 frames old (asynchronous GPU readback); never treat them as
 * authoritative for the current frame.
 */
UCLASS(ClassGroup = (Slosh), meta = (BlueprintSpawnableComponent))
class SLOSH_API USloshColliderComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	USloshColliderComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Shape")
	ESloshColliderShapeType Shape = ESloshColliderShapeType::Sphere;

	/** Sphere/capsule radius, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Shape", meta = (ClampMin = "0.1"))
	float Radius = 50.0f;

	/** Box half extents, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Shape")
	FVector BoxExtent = FVector(50.0);

	/** Capsule half height of the cylindrical part along local Z, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Shape", meta = (ClampMin = "0.0"))
	float CapsuleHalfHeight = 50.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Coupling")
	ESloshCouplingMode CouplingMode = ESloshCouplingMode::PressureAndDrag;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Coupling", meta = (ClampMin = "0.0"))
	float BuoyancyScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Coupling", meta = (ClampMin = "0.0"))
	float DragCoefficient = 0.8f;

	/** Linear (heave/sway) damping relative to the local liquid, per unit submerged fraction (1/s).
	 *  Approximates wave-radiation damping and counters the bobbing caused by delayed feedback. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Coupling", meta = (ClampMin = "0.0"))
	float LinearDrag = 3.0f;

	/** Angular damping torque per unit submerged fraction (1/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Coupling", meta = (ClampMin = "0.0"))
	float AngularDrag = 1.5f;

	/** Added-mass coefficient used to stabilise lagged explicit coupling (0.5 = sphere). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Coupling", meta = (ClampMin = "0.0"))
	float AddedMassCoefficient = 0.5f;

	/** Clamp on fluid-induced acceleration, cm/s^2. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Coupling", meta = (ClampMin = "0.0"))
	float MaxFluidAcceleration = 2500.0f;

	/** Accumulate and report solver pressure forces even when the body is kinematic (sensors, probes). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Coupling")
	bool bReportFeedbackWhenKinematic = false;

	/** Submerged fraction above which the body is considered "in" the liquid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Events", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float EnterThreshold = 0.05f;

	UPROPERTY(BlueprintAssignable, Category = "Slosh|Events")
	FSloshContactSignature OnEnterLiquid;

	UPROPERTY(BlueprintAssignable, Category = "Slosh|Events")
	FSloshContactSignature OnExitLiquid;

	/** Fraction of the collider volume estimated to be under liquid (delayed). */
	UFUNCTION(BlueprintPure, Category = "Slosh")
	float GetSubmergedFraction() const { return SubmergedFraction; }

	UFUNCTION(BlueprintPure, Category = "Slosh")
	bool IsInLiquid() const { return bInLiquid; }

	/** Last fluid force applied to the body, world space, Unreal units (kg*cm/s^2). */
	UFUNCTION(BlueprintPure, Category = "Slosh")
	FVector GetLastFluidForce() const { return LastForce; }

	/** Integrated solver pressure force on the collider surface, world space, Newtons (delayed). */
	UFUNCTION(BlueprintPure, Category = "Slosh")
	FVector GetLastPressureForceNewtons() const { return LastPressureForceN; }

	/** Volume of the collider as voxelised by the solver (solid cells x cell volume), liters (delayed). */
	UFUNCTION(BlueprintPure, Category = "Slosh")
	float GetVoxelVolumeLiters() const { return VoxelVolumeLiters; }

	/** Mean liquid velocity next to the body, world space cm/s (delayed). */
	UFUNCTION(BlueprintPure, Category = "Slosh")
	FVector GetLocalFluidVelocity() const { return LocalFluidVelocity; }

	/** Age of the feedback in game frames. */
	UFUNCTION(BlueprintPure, Category = "Slosh")
	int32 GetFeedbackAgeFrames() const { return FeedbackAgeFrames; }

	// Called by ASloshVolume.
	FSloshColliderGPU BuildGPUCollider(const FTransform& WorldToSim, float DeltaTime);
	void ReceiveFeedback(const FSloshColliderFeedback& Feedback, const FTransform& SimToWorldNoScale, float CellSizeMeters, float FluidDensity, int32 AgeFrames);
	UPrimitiveComponent* GetSimulatedBody() const;
	bool WantsTwoWayCoupling() const;

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	float ShapeVolumeCm3() const;

	FTransform PrevTransform;
	bool bHasPrevTransform = false;

	FVector PendingForce = FVector::ZeroVector;
	FVector PendingTorque = FVector::ZeroVector;
	FVector LastForce = FVector::ZeroVector;
	FVector LocalFluidVelocity = FVector::ZeroVector;
	FVector LastPressureForceN = FVector::ZeroVector;
	float VoxelVolumeLiters = 0.0f;
	float SubmergedFraction = 0.0f;
	int32 FeedbackAgeFrames = -1;
	bool bInLiquid = false;
};
