// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshManipulatorComponent.h"
#include "SloshSubsystem.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"

USloshManipulatorComponent::USloshManipulatorComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void USloshManipulatorComponent::OnRegister()
{
	Super::OnRegister();
	if (USloshSubsystem* Subsystem = USloshSubsystem::Get(GetWorld()))
	{
		Subsystem->RegisterManipulator(this);
	}
}

void USloshManipulatorComponent::OnUnregister()
{
	if (USloshSubsystem* Subsystem = USloshSubsystem::Get(GetWorld()))
	{
		Subsystem->UnregisterManipulator(this);
	}
	Super::OnUnregister();
}

float USloshManipulatorComponent::GetReach() const
{
	const float S = GetComponentScale().GetAbsMax();
	float R = Radius;
	switch (Shape)
	{
	case ESloshManipulatorShapeType::Box: R = float(BoxExtent.Size()); break;
	case ESloshManipulatorShapeType::Capsule: R = Radius + CapsuleHalfHeight; break;
	case ESloshManipulatorShapeType::Everywhere: return 1e9f;
	default: break;
	}
	return (R + GetFalloffCm()) * S;
}

bool USloshManipulatorComponent::BuildGPU(const FTransform& WorldToSim, FSloshManipulatorGPU& Out) const
{
	if (!bEnabled)
	{
		return false;
	}
	const FTransform World = GetComponentTransform();
	const float Scale = World.GetScale3D().GetAbsMax();
	const float ToMeters = WorldToSim.GetScale3D().X;
	const FQuat SimRot = WorldToSim.GetRotation() * World.GetRotation();

	Out = FSloshManipulatorGPU();
	Out.Center = FVector3f(WorldToSim.TransformPosition(World.GetLocation()));
	Out.Rotation = FVector4f(SimRot.X, SimRot.Y, SimRot.Z, SimRot.W);
	switch (Shape)
	{
	case ESloshManipulatorShapeType::Box:
		Out.Shape = uint32(ESloshManipulatorShape::Box);
		Out.Extent = FVector3f(BoxExtent * Scale * ToMeters);
		break;
	case ESloshManipulatorShapeType::Capsule:
		Out.Shape = uint32(ESloshManipulatorShape::Capsule);
		Out.Extent = FVector3f(Radius * Scale * ToMeters, CapsuleHalfHeight * Scale * ToMeters, 0.0f);
		break;
	case ESloshManipulatorShapeType::Everywhere:
		Out.Shape = uint32(ESloshManipulatorShape::Everywhere);
		break;
	default:
		Out.Shape = uint32(ESloshManipulatorShape::Sphere);
		Out.Extent = FVector3f(Radius * Scale * ToMeters, 0.0f, 0.0f);
		break;
	}
	Out.Falloff = GetFalloffCm() * Scale * ToMeters;
	return true;
}

void USloshManipulatorComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bDrawShape || !GetWorld() || Shape == ESloshManipulatorShapeType::Everywhere)
	{
		return;
	}
	const FTransform T = GetComponentTransform();
	const float S = T.GetScale3D().GetAbsMax();
	const FColor Color = bEnabled ? GetDebugColor() : FColor(80, 80, 80);
	switch (Shape)
	{
	case ESloshManipulatorShapeType::Box:
		DrawDebugBox(GetWorld(), T.GetLocation(), BoxExtent * S, T.GetRotation(), Color, false, -1.0f, 0, 0.5f);
		break;
	case ESloshManipulatorShapeType::Capsule:
		DrawDebugCapsule(GetWorld(), T.GetLocation(), (CapsuleHalfHeight + Radius) * S, Radius * S, T.GetRotation(), Color, false, -1.0f, 0, 0.5f);
		break;
	default:
		DrawDebugSphere(GetWorld(), T.GetLocation(), Radius * S, 16, Color, false, -1.0f, 0, 0.5f);
		break;
	}
}

bool USloshDrainComponent::BuildGPU(const FTransform& WorldToSim, FSloshManipulatorGPU& Out) const
{
	if (!Super::BuildGPU(WorldToSim, Out))
	{
		return false;
	}
	Out.Kind = uint32(ESloshManipulatorKind::Drain);
	Out.Param0 = FMath::Max(RemovalRate, 0.0f);
	Out.Falloff = 0.0f;
	return true;
}

bool USloshForceFieldComponent::BuildGPU(const FTransform& WorldToSim, FSloshManipulatorGPU& Out) const
{
	if (!Super::BuildGPU(WorldToSim, Out) || FMath::IsNearlyZero(Strength))
	{
		return false;
	}
	const FQuat ToSim = WorldToSim.GetRotation();
	switch (ForceType)
	{
	case ESloshForceType::Radial:
		Out.Kind = uint32(ESloshManipulatorKind::Radial);
		Out.Strength = Strength * 0.01f;
		break;
	case ESloshForceType::Swirl:
		Out.Kind = uint32(ESloshManipulatorKind::Swirl);
		Out.Axis = FVector3f(ToSim.RotateVector(GetUpVector()).GetSafeNormal());
		Out.Strength = Strength * 0.01f;
		Out.Param0 = SwirlInwardPull;
		break;
	case ESloshForceType::Drag:
		Out.Kind = uint32(ESloshManipulatorKind::Drag);
		Out.Strength = FMath::Max(Strength, 0.0f);   // 1/s
		break;
	default:
		Out.Kind = uint32(ESloshManipulatorKind::Directional);
		Out.Axis = FVector3f(ToSim.RotateVector(GetForwardVector()).GetSafeNormal());
		Out.Strength = Strength * 0.01f;
		break;
	}
	return true;
}
