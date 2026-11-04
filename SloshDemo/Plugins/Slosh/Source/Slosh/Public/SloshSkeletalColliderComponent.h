// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "SloshColliderComponent.h"
#include "SloshSkeletalColliderComponent.generated.h"

class USkinnedMeshComponent;

/**
 * Makes a skinned mesh solid to the liquid. At BeginPlay it fits one capsule per significant bone to the
 * bind-pose vertices that bone dominates (PCA axis, percentile length and radius), then attaches a
 * USloshColliderComponent to each bone so the capsules follow animation. Bones with too few vertices
 * (fingers, hair strands, small helpers) hand their vertices to the parent bone.
 *
 * Attach it to (or put it on the same actor as) a SkeletalMeshComponent / PoseableMeshComponent.
 * The mesh's LOD0 render data must be CPU-readable (true in editor and -game from the editor).
 * Capsules count against Slosh::MaxColliders (64) shared by every collider in the domain.
 */
UCLASS(ClassGroup = (Slosh), meta = (BlueprintSpawnableComponent))
class SLOSH_API USloshSkeletalColliderComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	USloshSkeletalColliderComponent();

	/** Upper bound on generated capsules; the bones with the most vertices win. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Fit", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxCapsules = 40;

	/** A bone needs at least this fraction of all vertices to get its own capsule (else it merges into its parent). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Fit", meta = (ClampMin = "0.0", ClampMax = "0.2"))
	float MinVertexFraction = 0.006f;

	/** Percentile of the vertices' distance from the bone axis used as the capsule radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Fit", meta = (ClampMin = "0.3", ClampMax = "1.0"))
	float RadiusPercentile = 0.8f;

	/** Fraction of vertices trimmed at each end of the axis before measuring capsule length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Fit", meta = (ClampMin = "0.0", ClampMax = "0.25"))
	float LengthTrim = 0.03f;

	/** Multiplier on fitted radii (inflate slightly so thin limbs still block a coarse grid). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Fit", meta = (ClampMin = "0.1"))
	float RadiusScale = 1.0f;

	/** Radii never go below this, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Fit", meta = (ClampMin = "0.1"))
	float MinRadius = 2.0f;

	/** Bones (and their whole subtree) that never get a capsule; their vertices are ignored. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Fit")
	TArray<FName> ExcludeBones;

	/** Coupling of the generated colliders. Animated characters should stay OneWay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Coupling")
	ESloshCouplingMode CouplingMode = ESloshCouplingMode::OneWay;

	/** Read back per-bone wetness (each collider's submerged fraction) even though the mesh is kinematic. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Coupling")
	bool bReportWetness = false;

	/** Draw the fitted capsules every frame (debug). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Debug")
	bool bDrawCapsules = false;

	/** Rebuilds the capsules from the current mesh; returns how many were made. */
	UFUNCTION(BlueprintCallable, Category = "Slosh")
	int32 RebuildColliders();

	UFUNCTION(BlueprintCallable, Category = "Slosh")
	void ClearColliders();

	UFUNCTION(BlueprintPure, Category = "Slosh")
	const TArray<USloshColliderComponent*>& GetColliders() const { return ObjectPtrDecay(Colliders); }

	/** The skinned mesh the capsules are fitted to (attach parent, else the owner's first skinned mesh). */
	UFUNCTION(BlueprintPure, Category = "Slosh")
	USkinnedMeshComponent* GetTargetMesh() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	UPROPERTY(Transient)
	TArray<TObjectPtr<USloshColliderComponent>> Colliders;
};
