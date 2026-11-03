// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/EngineTypes.h"
#include "SloshPaintLibrary.generated.h"

class USkinnedMeshComponent;
class USloshPaintComponent;
class USloshPaintPreset;

/** Where a paint ray landed. */
USTRUCT(BlueprintType)
struct SLOSH_API FSloshPaintHit
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Slosh|Paint")
	bool bHit = false;

	UPROPERTY(BlueprintReadOnly, Category = "Slosh|Paint")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "Slosh|Paint")
	FVector Normal = FVector::UpVector;

	/** The painted component (the primitive the paint attaches to). */
	UPROPERTY(BlueprintReadOnly, Category = "Slosh|Paint")
	TObjectPtr<USceneComponent> Component;

	/** Dominant skin bone at the hit (skeletal meshes), else None. */
	UPROPERTY(BlueprintReadOnly, Category = "Slosh|Paint")
	FName Bone;

	UPROPERTY(BlueprintReadOnly, Category = "Slosh|Paint")
	float Distance = 0.0f;
};

UCLASS()
class SLOSH_API USloshPaintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Traces a paint ray. Skeletal meshes are hit on their rendered (CPU-skinned) surface rather than their physics
	 * bodies, including skeletal meshes without collision when bIncludeSkinnedWithoutCollision is set.
	 * Paint geometry never blocks the ray.
	 */
	UFUNCTION(BlueprintCallable, Category = "Slosh|Paint", meta = (WorldContext = "WorldContextObject", AutoCreateRefTerm = "ActorsToIgnore"))
	static FSloshPaintHit TracePaintSurface(const UObject* WorldContextObject, FVector Start, FVector End,
		TEnumAsByte<ECollisionChannel> TraceChannel, const TArray<AActor*>& ActorsToIgnore, bool bIncludeSkinnedWithoutCollision = true);

	/**
	 * Intersects a ray with the CPU-skinned render surface of a skinned mesh. Needs CPU-accessible render data
	 * (always in the editor; at runtime enable "Allow CPU Access" on the skeletal mesh). Returns false on a miss.
	 */
	UFUNCTION(BlueprintCallable, Category = "Slosh|Paint")
	static bool TraceSkinnedSurface(USkinnedMeshComponent* Component, FVector Start, FVector End, FVector& OutLocation, FVector& OutNormal, FName& OutBone, float& OutDistance);

	/** Paints a single dab (one-point stroke) at a hit: projectile impacts, splats. Returns the stroke id. */
	UFUNCTION(BlueprintCallable, Category = "Slosh|Paint")
	static int32 PaintDab(const FSloshPaintHit& Hit, USloshPaintPreset* Preset, float Pressure = 1.0f);
};
