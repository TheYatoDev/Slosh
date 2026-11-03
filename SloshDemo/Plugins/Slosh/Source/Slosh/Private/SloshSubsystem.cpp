// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshSubsystem.h"
#include "SloshVolume.h"
#include "SloshRenderer.h"
#include "SloshColliderComponent.h"
#include "SloshEmitterComponent.h"
#include "Engine/World.h"

USloshSubsystem* USloshSubsystem::Get(const UWorld* World)
{
	return World ? World->GetSubsystem<USloshSubsystem>() : nullptr;
}

void USloshSubsystem::RegisterVolume(ASloshVolume* Volume) { Volumes.AddUnique(Volume); }
void USloshSubsystem::UnregisterVolume(ASloshVolume* Volume) { Volumes.Remove(Volume); }
void USloshSubsystem::RegisterCollider(USloshColliderComponent* Collider) { Colliders.AddUnique(Collider); }
void USloshSubsystem::UnregisterCollider(USloshColliderComponent* Collider) { Colliders.Remove(Collider); }
void USloshSubsystem::RegisterEmitter(USloshEmitterComponent* Emitter) { Emitters.AddUnique(Emitter); }
void USloshSubsystem::UnregisterEmitter(USloshEmitterComponent* Emitter) { Emitters.Remove(Emitter); }

ASloshVolume* USloshSubsystem::FindVolumeAt(FVector WorldLocation) const
{
	for (const TWeakObjectPtr<ASloshVolume>& Volume : Volumes)
	{
		if (Volume.IsValid() && Volume->ContainsPoint(WorldLocation))
		{
			return Volume.Get();
		}
	}
	return nullptr;
}

void USloshRenderEngineSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	SloshRender::Startup();
}

void USloshRenderEngineSubsystem::Deinitialize()
{
	SloshRender::Shutdown();
	Super::Deinitialize();
}
