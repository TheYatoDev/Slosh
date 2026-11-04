// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "SloshPaintTypes.generated.h"

class USloshPaintPreset;

/** One painted point. Position/Normal are in the space of Bone on the painted skeletal mesh, or in the paint
 *  component's space when Bone is None (static meshes and anything else). */
USTRUCT(BlueprintType)
struct SLOSH_API FSloshPaintPoint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paint")
	FVector Position = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paint")
	FVector Normal = FVector::UpVector;

	/** Bead-radius multiplier (pen pressure). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paint", meta = (ClampMin = "0.1", ClampMax = "5"))
	float Pressure = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paint")
	FName Bone;
};

/** A painted stroke: the saved source of truth. Geometry is regenerated from it. */
USTRUCT(BlueprintType)
struct SLOSH_API FSloshPaintStroke
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paint")
	TObjectPtr<USloshPaintPreset> Preset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paint")
	TArray<FSloshPaintPoint> Points;

	/** Random seed (roughness jitter, drop phase, layer scatter). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paint")
	int32 Seed = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paint")
	bool bVisible = true;

	/** Unique within the owning component; identifies cached geometry. */
	UPROPERTY(VisibleAnywhere, Category = "Paint")
	int32 Id = 0;

	/** Bumped on every edit of this stroke. */
	UPROPERTY(Transient)
	int32 Revision = 0;
};
