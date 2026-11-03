// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Subsystems/EngineSubsystem.h"
#include "SloshSubsystem.generated.h"

class ASloshVolume;
class USloshColliderComponent;
class USloshEmitterComponent;

/** Per-world registry of liquid volumes, colliders and emitters. */
UCLASS()
class SLOSH_API USloshSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static USloshSubsystem* Get(const UWorld* World);

	void RegisterVolume(ASloshVolume* Volume);
	void UnregisterVolume(ASloshVolume* Volume);
	void RegisterCollider(USloshColliderComponent* Collider);
	void UnregisterCollider(USloshColliderComponent* Collider);
	void RegisterEmitter(USloshEmitterComponent* Emitter);
	void UnregisterEmitter(USloshEmitterComponent* Emitter);

	const TArray<TWeakObjectPtr<USloshColliderComponent>>& GetColliders() const { return Colliders; }
	const TArray<TWeakObjectPtr<USloshEmitterComponent>>& GetEmitters() const { return Emitters; }

	/** Returns the first volume whose domain contains WorldLocation, or null. */
	UFUNCTION(BlueprintCallable, Category = "Slosh")
	ASloshVolume* FindVolumeAt(FVector WorldLocation) const;

private:
	TArray<TWeakObjectPtr<ASloshVolume>> Volumes;
	TArray<TWeakObjectPtr<USloshColliderComponent>> Colliders;
	TArray<TWeakObjectPtr<USloshEmitterComponent>> Emitters;
};

/** Hooks the screen-space liquid renderer into the engine renderer for the engine's lifetime. */
UCLASS()
class SLOSH_API USloshRenderEngineSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
};
