// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "InteractiveTool.h"
#include "InteractiveToolBuilder.h"
#include "BaseBehaviors/BehaviorTargetInterfaces.h"
#include "SloshPaintLibrary.h"
#include "SloshPaintTool.generated.h"

class USloshPaintComponent;
class USloshPaintPreset;

UCLASS(Transient)
class USloshPaintToolBuilder : public UInteractiveToolBuilder
{
	GENERATED_BODY()

public:
	virtual bool CanBuildTool(const FToolBuilderState& SceneState) const override { return SceneState.World != nullptr; }
	virtual UInteractiveTool* BuildTool(const FToolBuilderState& SceneState) const override;
};

UCLASS(Transient)
class USloshPaintToolProperties : public UInteractiveToolPropertySet
{
	GENERATED_BODY()

public:
	/** Liquid to paint (Slosh/Content/Paint/Presets). */
	UPROPERTY(EditAnywhere, Category = "Brush")
	TObjectPtr<USloshPaintPreset> Preset;

	/** Bead-radius multiplier (pen pressure). */
	UPROPERTY(EditAnywhere, Category = "Brush", meta = (ClampMin = "0.1", ClampMax = "5"))
	float Pressure = 1.0f;

	/** Minimum distance between stroke points, cm (0 = automatic from the preset). */
	UPROPERTY(EditAnywhere, Category = "Brush", meta = (ClampMin = "0"))
	float Spacing = 0.0f;

	/** Paint skeletal meshes that have no collision (hit test on the CPU-skinned surface). */
	UPROPERTY(EditAnywhere, Category = "Brush")
	bool bPaintSkinnedWithoutCollision = true;
};

/**
 * Drag on any surface to lay down a liquid stroke. Each drag is one undo transaction; the stroke is stored on a
 * USloshPaintComponent added to the painted actor (saved with the level).
 */
UCLASS(Transient)
class USloshPaintTool : public UInteractiveTool, public IClickDragBehaviorTarget, public IHoverBehaviorTarget
{
	GENERATED_BODY()

public:
	void SetWorld(UWorld* InWorld) { World = InWorld; }

	virtual void Setup() override;
	virtual void Shutdown(EToolShutdownType ShutdownType) override;
	virtual void Render(IToolsContextRenderAPI* RenderAPI) override;

	// IClickDragBehaviorTarget
	virtual FInputRayHit CanBeginClickDragSequence(const FInputDeviceRay& PressPos) override;
	virtual void OnClickPress(const FInputDeviceRay& PressPos) override;
	virtual void OnClickDrag(const FInputDeviceRay& DragPos) override;
	virtual void OnClickRelease(const FInputDeviceRay& ReleasePos) override;
	virtual void OnTerminateDragSequence() override;

	// IHoverBehaviorTarget
	virtual FInputRayHit BeginHoverSequenceHitTest(const FInputDeviceRay& PressPos) override;
	virtual void OnBeginHover(const FInputDeviceRay& DevicePos) override {}
	virtual bool OnUpdateHover(const FInputDeviceRay& DevicePos) override;
	virtual void OnEndHover() override { bHoverValid = false; }

private:
	FSloshPaintHit Trace(const FInputDeviceRay& Ray) const;
	void PaintAt(const FSloshPaintHit& Hit);
	void FinishStroke();
	float BrushSpacing() const;
	float BrushRadius() const;

	UPROPERTY()
	TObjectPtr<USloshPaintToolProperties> Properties;

	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<USloshPaintComponent> ActivePaint;
	TWeakObjectPtr<USceneComponent> ActiveTarget;
	int32 ActiveStroke = 0;
	FVector LastPoint = FVector::ZeroVector;
	bool bInTransaction = false;

	FSloshPaintHit Hover;
	bool bHoverValid = false;
};
