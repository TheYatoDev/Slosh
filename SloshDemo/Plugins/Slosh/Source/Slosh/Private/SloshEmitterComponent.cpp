// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshEmitterComponent.h"
#include "SloshSubsystem.h"
#include "Engine/World.h"

USloshEmitterComponent::USloshEmitterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USloshEmitterComponent::OnRegister()
{
	Super::OnRegister();
	if (USloshSubsystem* Subsystem = USloshSubsystem::Get(GetWorld()))
	{
		Subsystem->RegisterEmitter(this);
	}
}

void USloshEmitterComponent::OnUnregister()
{
	if (USloshSubsystem* Subsystem = USloshSubsystem::Get(GetWorld()))
	{
		Subsystem->UnregisterEmitter(this);
	}
	Super::OnUnregister();
}
