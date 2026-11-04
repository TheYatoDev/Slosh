// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshManipulatorActors.h"
#include "SloshManipulatorComponent.h"

ASloshDrainActor::ASloshDrainActor()
{
	Drain = CreateDefaultSubobject<USloshDrainComponent>(TEXT("Drain"));
	Drain->bDrawShape = true;
	RootComponent = Drain;
}

ASloshForceFieldActor::ASloshForceFieldActor()
{
	ForceField = CreateDefaultSubobject<USloshForceFieldComponent>(TEXT("ForceField"));
	ForceField->bDrawShape = true;
	RootComponent = ForceField;
}
