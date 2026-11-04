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

	/** Pull toward nearby collider surfaces (0 = none, 1 = holds a film against gravity on overhangs). Makes thick
	 *  liquids cling to and coat bodies instead of falling off; combine with BoundaryFriction for sticky liquids. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float Adhesion = 0.0f;

	/** How far from a surface adhesion reaches, cm (about one solver cell works well). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (ClampMin = "0.1", ClampMax = "20.0", Units = "Centimeters"))
	float AdhesionDistance = 2.5f;

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

	/** Upper bound on the bilateral smoothing radius in pixels (close-ups hit this cap). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "0", ClampMax = "32"))
	int32 SmoothingRadius = 7;

	/** Adaptive smoothing: radius as a multiple of the splat radius, so the surface looks equally smooth at any
	 *  distance, plus tilt-aware depth smoothing and a thickness blur. Suits thick, viscous liquids (honey, slime).
	 *  0 = the classic fixed SmoothingRadius-pixel filter, which keeps water's fine ripples. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float SmoothingScale = 0.0f;

	/** Horizontal + vertical smoothing passes; each repeat flattens bumps further. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "1", ClampMax = "4"))
	int32 SmoothingIterations = 1;

	/** Stretch each particle along its neighbours (Yu & Turk 2013): films become discs and streams rods, so thin
	 *  liquid renders as a sheet instead of beads. 0 = round splats. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Anisotropy = 0.0f;

	/** Largest allowed ratio between a splat's longest and shortest axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "1.0", ClampMax = "8.0", EditCondition = "Anisotropy > 0"))
	float AnisotropyMaxStretch = 4.0f;

	/** Pull of each splat toward its neighbourhood centre: evens out the surface, slightly shrinks edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Anisotropy > 0"))
	float AnisotropySmoothing = 0.5f;

	/** Liquid thinner than this (cm, along the view ray) fades out, which hides stray single particles and
	 *  softens the edges of films. 0 = hard edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "0.0", ClampMax = "10.0", Units = "Centimeters"))
	float EdgeFadeThickness = 0.0f;

	/** Light the in-scattered colour with a soft diffuse term (0 = flat, the classic look). Helps thick, opaque
	 *  liquids keep their shape; water usually looks better at 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ScatterLighting = 0.0f;
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
