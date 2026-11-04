// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "SloshSurfaceMeshComponent.generated.h"

class FSloshSurfaceMeshBuffers;
class UMaterialInterface;

/**
 * Draws a SloshVolume's liquid as a real triangle mesh. The renderer rebuilds the mesh on the GPU every frame
 * (surface nets over the particle density field) and this component draws it indirectly with an ordinary UE
 * material, so it gets engine lighting, shadows and any material, including Fluid Painter preset materials.
 * Created and owned by ASloshVolume when its Surface Mode is Mesh; vertices are in the volume's local space.
 */
UCLASS(ClassGroup = (Slosh), NotBlueprintable)
class SLOSH_API USloshSurfaceMeshComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	USloshSurfaceMeshComponent();

	/** Called by the volume: shared GPU buffers, local half extent of the domain (for bounds) and material. */
	void Configure(const TSharedPtr<FSloshSurfaceMeshBuffers, ESPMode::ThreadSafe>& InBuffers, const FVector& InHalfExtent, UMaterialInterface* InMaterial);

	const TSharedPtr<FSloshSurfaceMeshBuffers, ESPMode::ThreadSafe>& GetBuffers() const { return Buffers; }

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
	virtual int32 GetNumMaterials() const override { return 1; }
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* InMaterial) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SurfaceMaterial;

	TSharedPtr<FSloshSurfaceMeshBuffers, ESPMode::ThreadSafe> Buffers;
	FVector HalfExtent = FVector(100.0);
};
