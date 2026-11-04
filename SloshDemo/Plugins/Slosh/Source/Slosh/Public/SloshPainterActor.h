// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SloshPaintBrushComponent.h"
#include "SloshPainterActor.generated.h"

class USloshPaintPreset;

/**
 * Drop-in runtime painter (base of BP_SloshPainter / BP_SloshNozzle).
 *
 * Mouse mode (default): hold the left mouse button to paint where the cursor points; mouse wheel = pressure;
 * 1-9 / Tab = preset; Z = undo the last stroke. Nozzle mode (Aim = ComponentForward, bAutoPaint): paints along the
 * actor's forward axis while active, e.g. attached to a character socket or a squeeze bottle.
 */
UCLASS(Blueprintable)
class SLOSH_API ASloshPainterActor : public AActor
{
	GENERATED_BODY()

public:
	ASloshPainterActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Slosh|Painter")
	TObjectPtr<USloshPaintBrushComponent> Brush;

	/** Presets to cycle through (the brush starts with the first). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Painter")
	TArray<TObjectPtr<USloshPaintPreset>> Presets;

	/** Bind mouse/keyboard on the first local player (cursor painting, wheel pressure, preset keys, undo). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Painter")
	bool bMouseInput = true;

	/** Paint continuously from BeginPlay (nozzles). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Painter")
	bool bAutoPaint = false;

	/** Pressure change per mouse-wheel notch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Painter")
	float PressureStep = 0.1f;

	UFUNCTION(BlueprintCallable, Category = "Slosh|Painter")
	void SelectPreset(int32 Index);

	UFUNCTION(BlueprintCallable, Category = "Slosh|Painter")
	void NextPreset();

	/** Removes the last stroke this painter made. */
	UFUNCTION(BlueprintCallable, Category = "Slosh|Painter")
	void UndoLastStroke();

	UFUNCTION(BlueprintPure, Category = "Slosh|Painter")
	int32 GetPresetIndex() const { return PresetIndex; }

protected:
	virtual void BeginPlay() override;

private:
	UFUNCTION()
	void HandleStrokeFinished(USloshPaintComponent* PaintComponent, int32 StrokeId);

	void PaintPressed() { Brush->StartPainting(); }
	void PaintReleased() { Brush->StopPainting(); }
	void WheelUp() { Brush->Pressure = FMath::Clamp(Brush->Pressure + PressureStep, 0.1f, 5.0f); }
	void WheelDown() { Brush->Pressure = FMath::Clamp(Brush->Pressure - PressureStep, 0.1f, 5.0f); }

	int32 PresetIndex = 0;
	TArray<TPair<TWeakObjectPtr<USloshPaintComponent>, int32>> History;
};
