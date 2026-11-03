// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshColliderComponent.h"
#include "SloshSubsystem.h"
#include "Components/PrimitiveComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Engine/World.h"

namespace
{
	// Unreal units: cm, kg. 1 N = 100 kg*cm/s^2, 1 N*m = 1e4 kg*cm^2/s^2.
	constexpr double NewtonToUnreal = 100.0;
	constexpr double NewtonMeterToUnreal = 10000.0;
}

USloshColliderComponent::USloshColliderComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void USloshColliderComponent::OnRegister()
{
	Super::OnRegister();
	if (USloshSubsystem* Subsystem = USloshSubsystem::Get(GetWorld()))
	{
		Subsystem->RegisterCollider(this);
	}
}

void USloshColliderComponent::OnUnregister()
{
	if (USloshSubsystem* Subsystem = USloshSubsystem::Get(GetWorld()))
	{
		Subsystem->UnregisterCollider(this);
	}
	Super::OnUnregister();
}

UPrimitiveComponent* USloshColliderComponent::GetSimulatedBody() const
{
	UPrimitiveComponent* Body = Cast<UPrimitiveComponent>(GetAttachParent());
	if (!Body && GetOwner())
	{
		Body = Cast<UPrimitiveComponent>(GetOwner()->GetRootComponent());
	}
	return Body && Body->IsSimulatingPhysics() ? Body : nullptr;
}

bool USloshColliderComponent::WantsTwoWayCoupling() const
{
	return CouplingMode != ESloshCouplingMode::OneWay && GetSimulatedBody() != nullptr;
}

float USloshColliderComponent::ShapeVolumeCm3() const
{
	const float S = GetComponentScale().GetAbsMax();
	switch (Shape)
	{
	case ESloshColliderShapeType::Box:
		return 8.0f * BoxExtent.X * BoxExtent.Y * BoxExtent.Z * S * S * S;
	case ESloshColliderShapeType::Capsule:
	{
		const float R = Radius * S;
		return UE_PI * R * R * (2.0f * CapsuleHalfHeight * S) + 4.0f / 3.0f * UE_PI * R * R * R;
	}
	default:
	{
		const float R = Radius * S;
		return 4.0f / 3.0f * UE_PI * R * R * R;
	}
	}
}

FSloshColliderGPU USloshColliderComponent::BuildGPUCollider(const FTransform& WorldToSim, float DeltaTime)
{
	const FTransform World = GetComponentTransform();
	const float Scale = World.GetScale3D().GetAbsMax();
	const float ToMeters = WorldToSim.GetScale3D().X;

	FVector LinearVel = FVector::ZeroVector;
	FVector AngularVel = FVector::ZeroVector;
	if (UPrimitiveComponent* Body = GetSimulatedBody())
	{
		LinearVel = Body->GetPhysicsLinearVelocity();
		AngularVel = Body->GetPhysicsAngularVelocityInRadians();
		// Velocity of this component's origin, which may be offset from the body's COM.
		LinearVel += FVector::CrossProduct(AngularVel, World.GetLocation() - Body->GetCenterOfMass());
	}
	else if (bHasPrevTransform && DeltaTime > UE_KINDA_SMALL_NUMBER)
	{
		// Kinematic / animated: finite-difference the transform.
		LinearVel = (World.GetLocation() - PrevTransform.GetLocation()) / DeltaTime;
		const FQuat Delta = World.GetRotation() * PrevTransform.GetRotation().Inverse();
		FVector Axis;
		float Angle;
		Delta.ToAxisAndAngle(Axis, Angle);
		Angle = FMath::UnwindRadians(Angle);
		AngularVel = Axis * (Angle / DeltaTime);
	}
	PrevTransform = World;
	bHasPrevTransform = true;

	FSloshColliderGPU Out;
	Out.Center = FVector3f(WorldToSim.TransformPosition(World.GetLocation()));
	const FQuat SimRot = WorldToSim.GetRotation() * World.GetRotation();
	Out.Rotation = FVector4f(SimRot.X, SimRot.Y, SimRot.Z, SimRot.W);
	switch (Shape)
	{
	case ESloshColliderShapeType::Box:
		Out.Type = uint32(ESloshColliderShape::Box);
		Out.Extent = FVector3f(BoxExtent * Scale * ToMeters);
		break;
	case ESloshColliderShapeType::Capsule:
		Out.Type = uint32(ESloshColliderShape::Capsule);
		Out.Extent = FVector3f(Radius * Scale * ToMeters, CapsuleHalfHeight * Scale * ToMeters, 0.0f);
		break;
	default:
		Out.Type = uint32(ESloshColliderShape::Sphere);
		Out.Extent = FVector3f(Radius * Scale * ToMeters, 0.0f, 0.0f);
		break;
	}
	Out.LinearVelocity = FVector3f(WorldToSim.TransformVector(LinearVel));
	Out.AngularVelocity = FVector3f(WorldToSim.GetRotation().RotateVector(AngularVel));
	Out.Flags = (WantsTwoWayCoupling() || bReportFeedbackWhenKinematic) ? Slosh::ColliderFlagTwoWay : 0u;
	return Out;
}

void USloshColliderComponent::ReceiveFeedback(const FSloshColliderFeedback& Feedback, const FTransform& SimToWorldNoScale,
	float CellSizeMeters, float FluidDensity, int32 AgeFrames)
{
	FeedbackAgeFrames = AgeFrames;
	const double CellVolumeM3 = double(CellSizeMeters) * CellSizeMeters * CellSizeMeters;
	const double SubmergedM3 = Feedback.SubmergedCells * CellVolumeM3;
	const double ShapeM3 = ShapeVolumeCm3() * 1e-6;
	SubmergedFraction = ShapeM3 > 0.0 ? float(FMath::Clamp(SubmergedM3 / ShapeM3, 0.0, 1.0)) : 0.0f;
	LocalFluidVelocity = SimToWorldNoScale.TransformVectorNoScale(FVector(Feedback.MeanFluidVelocity)) * 100.0;
	LastPressureForceN = SimToWorldNoScale.TransformVectorNoScale(FVector(Feedback.PressureForce));
	VoxelVolumeLiters = float(Feedback.SolidCells * CellVolumeM3 * 1000.0);

	const bool bWasInLiquid = bInLiquid;
	bInLiquid = SubmergedFraction > EnterThreshold || (bWasInLiquid && SubmergedFraction > EnterThreshold * 0.5f);
	if (bInLiquid != bWasInLiquid)
	{
		(bInLiquid ? OnEnterLiquid : OnExitLiquid).Broadcast(this);
	}

	UPrimitiveComponent* Body = GetSimulatedBody();
	if (!Body || CouplingMode == ESloshCouplingMode::OneWay)
	{
		PendingForce = PendingTorque = LastForce = FVector::ZeroVector;
		return;
	}

	const FVector GravityCm = FVector(0, 0, GetWorld() ? GetWorld()->GetGravityZ() : -980.0);
	const double BodyMass = FMath::Max(double(Body->GetMass()), 1e-3);
	const FVector BodyVelMs = Body->GetPhysicsLinearVelocity() * 0.01;
	const FVector FluidVelMs = LocalFluidVelocity * 0.01;
	const FVector RelVel = FluidVelMs - BodyVelMs;

	// Quadratic drag on an approximate frontal area (a quarter of the wetted surface).
	const double WettedAreaM2 = Feedback.WettedFaces * double(CellSizeMeters) * CellSizeMeters;
	const FVector DragN = 0.5 * FluidDensity * DragCoefficient * (0.25 * WettedAreaM2) * RelVel.Size() * RelVel;

	FVector DynamicForce;   // Unreal units
	FVector Torque;         // Unreal units
	FVector ApplicationPoint = GetComponentLocation();
	if (CouplingMode == ESloshCouplingMode::PressureAndDrag)
	{
		DynamicForce = (SimToWorldNoScale.TransformVectorNoScale(FVector(Feedback.PressureForce)) + DragN) * NewtonToUnreal;
		Torque = SimToWorldNoScale.TransformVectorNoScale(FVector(Feedback.PressureTorque)) * NewtonMeterToUnreal;
	}
	else
	{
		const FVector BuoyancyN = -FluidDensity * SubmergedM3 * (GravityCm * 0.01) * BuoyancyScale;
		DynamicForce = (BuoyancyN + DragN) * NewtonToUnreal;
		// Apply buoyancy at the submerged centroid for a righting moment.
		const FVector CentroidOffset = SimToWorldNoScale.TransformVectorNoScale(FVector(Feedback.SubmergedCentroidOffset)) * 100.0;
		ApplicationPoint += CentroidOffset;
		Torque = FVector::CrossProduct(ApplicationPoint - Body->GetCenterOfMass(), BuoyancyN * NewtonToUnreal);
	}

	// Linear damping relative to the liquid, proportional to submersion (Unreal units: kg*cm/s^2).
	DynamicForce += LinearDrag * SubmergedFraction * BodyMass * (LocalFluidVelocity - Body->GetPhysicsLinearVelocity());

	// Angular damping proportional to submersion.
	const FVector Inertia = Body->GetInertiaTensor();
	Torque -= AngularDrag * SubmergedFraction * (Inertia.GetAbsMax() * Body->GetPhysicsAngularVelocityInRadians());

	// Implicit added-mass treatment keeps light bodies stable under lagged explicit forcing:
	// F_applied = (m_b * F_dyn - m_a * m_b * g) / (m_b + m_a).
	const double AddedMass = AddedMassCoefficient * FluidDensity * SubmergedM3;
	FVector Applied = (BodyMass * DynamicForce - AddedMass * BodyMass * GravityCm) / (BodyMass + AddedMass);
	const double MaxForce = BodyMass * MaxFluidAcceleration;
	Applied = Applied.GetClampedToMaxSize(MaxForce);
	Torque *= BodyMass / (BodyMass + AddedMass);

	// Light temporal smoothing hides readback jitter.
	PendingForce = FMath::Lerp(PendingForce, Applied, 0.5);
	PendingTorque = FMath::Lerp(PendingTorque, Torque, 0.5);
	LastForce = PendingForce;
}

void USloshColliderComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UPrimitiveComponent* Body = GetSimulatedBody();
	if (!Body || CouplingMode == ESloshCouplingMode::OneWay || !bInLiquid)
	{
		return;
	}
	// The off-centre buoyancy moment is already folded into PendingTorque, so force goes through the COM.
	if (!PendingForce.IsNearlyZero())
	{
		Body->AddForce(PendingForce);
	}
	if (!PendingTorque.IsNearlyZero())
	{
		Body->AddTorqueInRadians(PendingTorque);
	}
}
