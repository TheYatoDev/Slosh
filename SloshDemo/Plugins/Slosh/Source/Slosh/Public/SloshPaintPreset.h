// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SloshPaintPreset.generated.h"

class UMaterialInterface;

UENUM(BlueprintType)
enum class ESloshPaintDropMode : uint8
{
	/** No drop. */
	None,
	/** The stroke swells into a bulb toward its end (Fluid Painter Drop Mode 1). */
	Static,
	/** Only a droplet is drawn, travelling along the stroke with DropTime (Fluid Painter Drop Mode 2). */
	Animated,
};

/** A layer of small instances scattered over the liquid: air bubbles, soap bubbles, sugar, sprinkles. */
USTRUCT(BlueprintType)
struct SLOSH_API FSloshPaintLayer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	bool bEnabled = false;

	/** Thin rods (random orientation) instead of beads. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer", meta = (EditCondition = "bEnabled"))
	bool bRods = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer", meta = (EditCondition = "bEnabled"))
	TObjectPtr<UMaterialInterface> Material;

	/** Sink the instances below the liquid surface (bubbles inside honey). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer", meta = (EditCondition = "bEnabled"))
	bool bInside = false;

	/** Instance radius in preset units (Scale x 1 mm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer", meta = (EditCondition = "bEnabled", ClampMin = "0.1", ClampMax = "50"))
	float Size = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer", meta = (EditCondition = "bEnabled", ClampMin = "0", ClampMax = "1"))
	float SizeRandom = 0.0f;

	/** Density: Amount / Scale^2 x 10^5 instances per m^2 of liquid surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer", meta = (EditCondition = "bEnabled", ClampMin = "0.1", ClampMax = "10"))
	float Amount = 1.0f;

	/** Extra offset along the surface normal, preset units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer", meta = (EditCondition = "bEnabled", ClampMin = "-10", ClampMax = "10"))
	float Shift = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer", meta = (EditCondition = "bEnabled"))
	int32 Seed = 0;
};

/**
 * A paintable liquid: how strokes become geometry (thickness, flatness, drops, layers) and how they are shaded.
 * Parameters mirror the Blender Fluid Painter add-on's geometry-node inputs one-to-one so its presets port
 * directly (PAINTER_DESIGN.md). Lengths are in preset units: Scale x 1 mm.
 */
UCLASS(BlueprintType)
class SLOSH_API USloshPaintPreset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Preset")
	FName Category;

	/** Adult-content preset (kept in a separate content folder so it can be excluded). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Preset")
	bool bAdultContent = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid")
	TObjectPtr<UMaterialInterface> Material;

	/** Scales the procedural material features (noise, Voronoi) relative to the liquid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid", meta = (ClampMin = "0.001"))
	float MaterialUVScale = 1.0f;

	/** Preset unit = Scale x 1 mm. Every length below is in these units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid", meta = (ClampMin = "0.1", ClampMax = "100000"))
	float Scale = 1.0f;

	/** Bead radius (and resampling spacing) in preset units, multiplied by brush pressure. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid", meta = (ClampMin = "0.1", ClampMax = "5"))
	float Thickness = 1.0f;

	/** Meshing resolution: 0 = Fluid Painter default, 2 = finest. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid", meta = (ClampMin = "0", ClampMax = "2"))
	int32 Quality = 0;

	/** Random bead-radius variation along the stroke. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid", meta = (ClampMin = "0", ClampMax = "1"))
	float Roughness = 0.0f;

	/** Squash beads against the surface (0 = round, 1 = flat film). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid", meta = (ClampMin = "0", ClampMax = "1"))
	float Flatness = 0.0f;

	/** Move the stroke along the surface normal, preset units (negative sinks it into the surface). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid", meta = (ClampMin = "-100", ClampMax = "100"))
	float Offset = 0.0f;

	/** Height over the surface (preset units) across which the material's fading ramp runs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid", meta = (ClampMin = "0.001", ClampMax = "10"))
	float Fading = 1.0f;

	/** Laplacian smoothing passes on the merged surface (the Fluid Painter Smooth modifier). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid", meta = (ClampMin = "0", ClampMax = "64"))
	int32 SmoothIterations = 10;

	/** Fluid Painter liquids cast no shadows (their materials are transparent to shadow rays). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fluid")
	bool bCastShadows = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drops")
	ESloshPaintDropMode DropMode = ESloshPaintDropMode::None;

	/** Fraction of the stroke the drop covers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drops", meta = (ClampMin = "0.1", ClampMax = "1"))
	float DropLength = 0.5f;

	/** Drop radius relative to the bead radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drops", meta = (ClampMin = "1", ClampMax = "5"))
	float DropSize = 2.0f;

	/** Profile exponent: 1 = cone, higher = rounder bulb at the very end. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drops", meta = (ClampMin = "1", ClampMax = "10"))
	float DropExponent = 1.0f;

	/** Phase offset of animated drops (added to the paint component's DropTime). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drops")
	float DropMove = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drops")
	bool bDropMoveRandom = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drops")
	int32 DropMoveSeed = 0;

	/** Animated drop speed (stroke lengths per unit of DropTime). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drops", meta = (ClampMin = "-100", ClampMax = "100"))
	float DropSpeed = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drops")
	bool bDropSpeedRandom = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Drops")
	int32 DropSpeedSeed = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers")
	FSloshPaintLayer LayerA;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers")
	FSloshPaintLayer LayerB;

	/** Preset unit in centimetres. */
	float GetUnitCm() const { return FMath::Max(Scale, 0.1f) * 0.1f; }

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

	/** Broadcast when a preset is edited so paint components using it rebuild. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnPresetChanged, USloshPaintPreset*);
	static FOnPresetChanged OnPresetChanged;
};
