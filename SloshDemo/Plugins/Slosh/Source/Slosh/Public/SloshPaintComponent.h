// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "Containers/Ticker.h"
#include "SloshPaintTypes.h"
#include "SloshPaintComponent.generated.h"

class UDynamicMeshComponent;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class USloshPaintPreset;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSloshPaintBuiltSignature, USloshPaintComponent*, PaintComponent);

/**
 * Painted liquid on a surface (Fluid Painter-style strokes, PAINTER_DESIGN.md).
 *
 * Attach it to the painted primitive (FindOrAddPaintComponent does this). Strokes are the saved source of truth;
 * geometry is regenerated from them asynchronously into transient child components. On skeletal meshes every
 * point is stored in the space of its dominant skin bone and the geometry is attached per bone, so the paint
 * follows the animation without per-frame meshing.
 *
 * Animation: Reveal and DropTime are Sequencer-keyable; DropTime also advances with game time (bAnimateDrops).
 */
UCLASS(ClassGroup = (Slosh), meta = (BlueprintSpawnableComponent), HideCategories = (Physics, Collision, Lighting))
class SLOSH_API USloshPaintComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	USloshPaintComponent();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Slosh|Paint")
	TArray<FSloshPaintStroke> Strokes;

	/** Fraction of each stroke's length that is drawn. Key it to pour the paint on over time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, BlueprintSetter = SetReveal, Category = "Slosh|Paint|Animation", meta = (ClampMin = "0", ClampMax = "1"))
	float Reveal = 1.0f;

	/** Time of animated drops (Fluid Painter "Drop Move"). Keyable; advances by itself in game when bAnimateDrops. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Interp, BlueprintSetter = SetDropTime, Category = "Slosh|Paint|Animation")
	float DropTime = 0.0f;

	/** Advance DropTime with game time (only strokes with animated drops are rebuilt). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Paint|Animation")
	bool bAnimateDrops = true;

	/** DropTime units per second when animating. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Paint|Animation")
	float DropTimeScale = 0.25f;

	/** Scales every preset unit on this component (Fluid Painter presets are millimetre-scale: 1 = as authored). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Paint|Quality", meta = (ClampMin = "0.01", ClampMax = "100"))
	float UnitScale = 1.0f;

	/** Meshing resolution multiplier (> 1 = finer voxels, slower builds). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Paint|Quality", meta = (ClampMin = "0.25", ClampMax = "4"))
	float QualityScale = 1.0f;

	UPROPERTY(BlueprintAssignable, Category = "Slosh|Paint")
	FSloshPaintBuiltSignature OnPaintBuilt;

	/** Finds the paint component attached to Target, or creates one (runtime: transient; editor: saved instance component). */
	UFUNCTION(BlueprintCallable, Category = "Slosh|Paint", meta = (DefaultToSelf = "Target"))
	static USloshPaintComponent* FindOrAddPaintComponent(USceneComponent* Target);

	/** Starts a stroke at a world-space point. Bone: skin bone the point belongs to (None for non-skeletal targets). Returns the stroke id. */
	UFUNCTION(BlueprintCallable, Category = "Slosh|Paint")
	int32 BeginStroke(USloshPaintPreset* Preset, FVector WorldLocation, FVector WorldNormal, float Pressure = 1.0f, FName Bone = NAME_None);

	/** Appends a world-space point to a stroke started with BeginStroke. */
	UFUNCTION(BlueprintCallable, Category = "Slosh|Paint")
	bool AddStrokePoint(int32 StrokeId, FVector WorldLocation, FVector WorldNormal, float Pressure = 1.0f, FName Bone = NAME_None);

	/** Finishes a stroke (rebuilds it at full quality). */
	UFUNCTION(BlueprintCallable, Category = "Slosh|Paint")
	void EndStroke(int32 StrokeId);

	/** Adds a complete stroke from world-space points (normals: one per point, or a single normal for all). */
	UFUNCTION(BlueprintCallable, Category = "Slosh|Paint")
	int32 AddStroke(USloshPaintPreset* Preset, const TArray<FVector>& WorldLocations, const TArray<FVector>& WorldNormals, float Pressure = 1.0f, FName Bone = NAME_None);

	UFUNCTION(BlueprintCallable, Category = "Slosh|Paint")
	bool RemoveStroke(int32 StrokeId);

	UFUNCTION(BlueprintCallable, Category = "Slosh|Paint")
	void UndoLastStroke();

	UFUNCTION(BlueprintCallable, Category = "Slosh|Paint")
	void ClearStrokes();

	UFUNCTION(BlueprintCallable, BlueprintSetter, Category = "Slosh|Paint|Animation")
	void SetReveal(float NewReveal);

	UFUNCTION(BlueprintCallable, BlueprintSetter, Category = "Slosh|Paint|Animation")
	void SetDropTime(float NewDropTime);

	/** Regenerates all geometry from the strokes. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Slosh|Paint")
	void Rebuild();

	UFUNCTION(BlueprintPure, Category = "Slosh|Paint")
	int32 GetNumStrokes() const { return Strokes.Num(); }

	UFUNCTION(BlueprintPure, Category = "Slosh|Paint")
	bool IsBuilding() const { return Pending.IsValid(); }

	/** World transform of the space strokes points are stored in (a skin bone, or this component). */
	FTransform GetStrokeSpaceTransform(FName Bone) const;

	/** The skinned mesh this paint sits on, if any. */
	class USkinnedMeshComponent* GetSkinnedTarget() const;

	/** Stroke by id (nullptr if absent). */
	FSloshPaintStroke* FindStroke(int32 StrokeId);

	/** Marks paint dirty after external edits to Strokes (editor tools, Blueprint struct edits). */
	void NotifyStrokesChanged(bool bClearCache = false);

	struct FBuildState;

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
	virtual void PostEditUndo() override;
#endif

private:
	void MarkDirty();
	void StartBuild();
	bool PollBuild(float DeltaTime);
	void ApplyBuild(FBuildState& State);
	void DestroyGeneratedComponents();
	void HandlePresetChanged(USloshPaintPreset* Preset);
	bool HasAnimatedDrops() const;
	FSloshPaintPoint MakePoint(const FVector& WorldLocation, const FVector& WorldNormal, float Pressure, FName Bone) const;

	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UDynamicMeshComponent>> GroupMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> LayerComponents;

	UPROPERTY()
	int32 NextStrokeId = 1;

	TSharedPtr<FBuildState> Pending;
	TMap<int32, TSharedPtr<const struct FSloshPaintStrokeCache>> Cache;
	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle PresetChangedHandle;
	bool bDirty = false;
};
