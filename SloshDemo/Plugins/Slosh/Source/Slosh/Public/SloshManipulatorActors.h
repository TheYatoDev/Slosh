// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SloshManipulatorActors.generated.h"

class USloshDrainComponent;
class USloshForceFieldComponent;

/** Placeable drain: removes liquid that enters its shape. Settings live on the Drain component. */
UCLASS(ClassGroup = (Slosh), meta = (DisplayName = "Slosh Drain"))
class SLOSH_API ASloshDrainActor : public AActor
{
	GENERATED_BODY()

public:
	ASloshDrainActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Slosh")
	TObjectPtr<USloshDrainComponent> Drain;
};

/** Placeable force field: pushes, pulls, swirls or slows liquid. Settings live on the ForceField component. */
UCLASS(ClassGroup = (Slosh), meta = (DisplayName = "Slosh Force Field"))
class SLOSH_API ASloshForceFieldActor : public AActor
{
	GENERATED_BODY()

public:
	ASloshForceFieldActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Slosh")
	TObjectPtr<USloshForceFieldComponent> ForceField;
};
