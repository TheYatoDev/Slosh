// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Engine/EngineTypes.h"
#include "SloshPaintLibrary.h"
#include "SloshPaintBrushComponent.generated.h"

class APlayerController;
class USloshPaintComponent;
class USloshPaintPreset;

UENUM(BlueprintType)
enum class ESloshPaintAim : uint8
{
	/** Ray along this component's forward axis (hoses, nozzles, sockets on characters). */
	ComponentForward,
	/** Ray through the mouse cursor of AimController (point-and-paint gameplay). */
	MouseCursor,
	/** Ray from the camera of AimController through the screen centre. */
	ScreenCenter,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSloshPaintStrokeSignature, USloshPaintComponent*, PaintComponent, int32, StrokeId);

/**
 * Runtime paint brush. While painting, it traces every tick and lays down a stroke on whatever surface the ray
 * hits (static meshes, skeletal meshes on their skin, anything with collision), starting a new stroke when the
 * ray moves to another object or leaves the surface.
 */
UCLASS(ClassGroup = (Slosh), meta = (BlueprintSpawnableComponent))
class SLOSH_API USloshPaintBrushComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	USloshPaintBrushComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Brush")
	TObjectPtr<USloshPaintPreset> Preset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Brush")
	ESloshPaintAim Aim = ESloshPaintAim::ComponentForward;

	/** Controller for MouseCursor / ScreenCenter aiming (defaults to the first local player). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Brush")
	TObjectPtr<APlayerController> AimController;

	/** Bead-radius multiplier (pen pressure). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Brush", meta = (ClampMin = "0.1", ClampMax = "5"))
	float Pressure = 1.0f;

	/** Minimum distance between stroke points, cm (0 = automatic from the preset's thickness). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Brush", meta = (ClampMin = "0"))
	float Spacing = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Brush", meta = (ClampMin = "1"))
	float MaxDistance = 2000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Brush")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_Visibility;

	/** Also paint skeletal meshes that have no collision (CPU-skinned surface test). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Brush")
	bool bPaintSkinnedWithoutCollision = true;

	UPROPERTY(BlueprintAssignable, Category = "Slosh|Brush")
	FSloshPaintStrokeSignature OnStrokeStarted;

	UPROPERTY(BlueprintAssignable, Category = "Slosh|Brush")
	FSloshPaintStrokeSignature OnStrokeFinished;

	UFUNCTION(BlueprintCallable, Category = "Slosh|Brush")
	void StartPainting();

	UFUNCTION(BlueprintCallable, Category = "Slosh|Brush")
	void StopPainting();

	UFUNCTION(BlueprintPure, Category = "Slosh|Brush")
	bool IsPainting() const { return bPainting; }

	/** One paint step along an explicit ray (for custom aiming); returns where it landed. */
	UFUNCTION(BlueprintCallable, Category = "Slosh|Brush")
	FSloshPaintHit PaintAlongRay(FVector Start, FVector End);

	/** The ray the brush would trace this frame. */
	UFUNCTION(BlueprintCallable, Category = "Slosh|Brush")
	bool GetAimRay(FVector& OutStart, FVector& OutEnd) const;

protected:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void FinishStroke();
	float EffectiveSpacing() const;

	bool bPainting = false;
	TWeakObjectPtr<USloshPaintComponent> CurrentPaint;
	TWeakObjectPtr<USceneComponent> CurrentTarget;
	int32 CurrentStroke = 0;
	FVector LastPoint = FVector::ZeroVector;
};
