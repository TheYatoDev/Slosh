// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SloshMaterialAsset.generated.h"

UENUM(BlueprintType)
enum class ESloshTransfer : uint8
{
	/** Affine particle-in-cell: low noise, conserves angular momentum, slightly more damped. */
	APIC,
	/** PIC/FLIP blend controlled by FlipRatio: livelier splashes, noisier. */
	FLIP,
};

/** Physical model consumed by the solver. Changing these never affects rendering. */
USTRUCT(BlueprintType)
struct SLOSH_API FSloshPhysicalProperties
{
	GENERATED_BODY()

	/** Rest density in kg/m^3 (water 1000, blood ~1060). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (ClampMin = "1.0"))
	float Density = 1000.0f;

	/** Kinematic viscosity in m^2/s (water 1e-6, blood ~3e-6..4e-6; gameplay slime 0.01..0.5). Solved implicitly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (ClampMin = "0.0"))
	float KinematicViscosity = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh")
	ESloshTransfer Transfer = ESloshTransfer::APIC;

	/** FLIP mode only: 0 = pure PIC, 1 = pure FLIP. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Transfer == ESloshTransfer::FLIP"))
	float FlipRatio = 0.95f;

	/** Tangential velocity removed at solid contact (0 = free slip, 1 = sticky). Approximates adhesion/wetting. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BoundaryFriction = 0.0f;

	/** Strength of the particle-density correction that counters volume drift. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DensityCorrection = 0.15f;
};

/** Visual model consumed only by the screen-space fluid renderer. */
USTRUCT(BlueprintType)
struct SLOSH_API FSloshVisualProperties
{
	GENERATED_BODY()

	/** Beer-Lambert absorption per meter for R, G, B. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render")
	FVector AbsorptionPerMeter = FVector(4.0, 1.2, 0.8);

	/** Colour the liquid tends to when thick (in-scattering approximation). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render")
	FLinearColor ScatterColor = FLinearColor(0.02f, 0.08f, 0.10f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "0.0"))
	float RefractionStrength = 0.02f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FresnelF0 = 0.02f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "1.0"))
	float SpecularPower = 400.0f;

	/** Splat radius as a multiple of the cell size. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "0.1"))
	float ParticleRadiusScale = 0.6f;

	/** Bilateral smoothing radius in pixels. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "0", ClampMax = "24"))
	int32 SmoothingRadius = 7;
};

/** A liquid type: physical and visual properties are kept in separate structs and modules. */
UCLASS(BlueprintType)
class SLOSH_API USloshMaterialAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh")
	FSloshPhysicalProperties Physical;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh")
	FSloshVisualProperties Visual;
};
