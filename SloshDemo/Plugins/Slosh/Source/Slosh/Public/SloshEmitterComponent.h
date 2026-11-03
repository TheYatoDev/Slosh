// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "SloshEmitterComponent.generated.h"

/** Continuously emits liquid particles from a sphere into whichever SloshVolume contains it. */
UCLASS(ClassGroup = (Slosh), meta = (BlueprintSpawnableComponent))
class SLOSH_API USloshEmitterComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	USloshEmitterComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh")
	bool bEmitting = true;

	/** Emission sphere radius in cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (ClampMin = "1.0"))
	float Radius = 10.0f;

	/** Particles per second (8 particles fill one solver cell). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (ClampMin = "0.0"))
	float ParticlesPerSecond = 20000.0f;

	/** Initial speed along the component's forward (X) axis, cm/s. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh")
	float Speed = 300.0f;

	/** Internal: fractional particle carry-over between frames. */
	float Carry = 0.0f;

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
};
