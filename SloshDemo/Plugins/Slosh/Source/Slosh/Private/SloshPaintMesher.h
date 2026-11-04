// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"

/**
 * Stroke -> liquid geometry, after the Blender Fluid Painter geometry-node graph (PAINTER_DESIGN.md §1):
 * resample by thickness, jitter radius, offset along the surface normal, drop profile (static bulb or travelling
 * droplet), ellipsoids flattened against the surface, smooth-union SDF -> marching cubes -> Laplacian smoothing,
 * per-vertex fading/object coordinates for the materials, and scattered layer instances (bubbles, sugar).
 * Pure geometry: runs on a worker thread.
 */
namespace Slosh::Paint
{
	struct FLayerParams
	{
		bool bEnabled = false;
		bool bRods = false;        // thin rods instead of beads (Fluid Painter "Layer Type")
		bool bInside = false;      // pushed below the liquid surface
		float Size = 1.0f;
		float SizeRandom = 0.0f;
		float Amount = 1.0f;
		float Shift = 0.0f;
		int32 Seed = 0;
		int32 MaterialSlot = INDEX_NONE;
	};

	struct FPresetParams
	{
		float UnitCm = 0.1f;       // Fluid Painter unit: Scale x 1 mm
		float Scale = 1.0f;
		float Thickness = 1.0f;
		float Roughness = 0.0f;
		float Flatness = 0.0f;
		float Offset = 0.0f;
		float Fading = 1.0f;
		float MaterialUVScale = 1.0f;
		int32 Quality = 0;
		int32 SmoothIterations = 10;
		uint8 DropMode = 0;        // 0 none, 1 static bulb, 2 travelling droplet
		float DropLength = 0.5f;
		float DropSize = 2.0f;
		float DropExponent = 1.0f;
		float DropMove = 0.0f;
		bool bDropMoveRandom = false;
		int32 DropMoveSeed = 0;
		float DropSpeed = 1.0f;
		bool bDropSpeedRandom = false;
		int32 DropSpeedSeed = 0;
		FLayerParams Layers[2];

		uint32 Hash() const;
	};

	struct FInputPoint
	{
		FVector3d Position = FVector3d::ZeroVector;   // group space (bone or paint component), cm
		FVector3d Normal = FVector3d::UnitZ();
		double Pressure = 1.0;
	};

	struct FStrokeJob
	{
		TArray<FInputPoint> Points;
		FPresetParams Preset;
		int32 Seed = 0;
		int32 MaterialSlot = 0;
	};

	struct FBuildSettings
	{
		double Reveal = 1.0;          // fraction of each stroke's length that is drawn
		double DropTime = 0.0;        // Fluid Painter "Drop Move" (animated drops)
		double QualityScale = 1.0;    // > 1 = finer voxels
		int32 MaxCellsPerStroke = 4 * 1024 * 1024;
		int32 MaxLayerInstancesPerStroke = 4000;
	};

	struct FLayerInstances
	{
		int32 MaterialSlot = INDEX_NONE;
		bool bRods = false;
		TArray<FTransform> Transforms;   // unit sphere (radius 1) / unit rod (radius 1, length 1) -> group space
	};

	struct FStrokeResult
	{
		UE::Geometry::FDynamicMesh3 Mesh;   // group space; UV0 (fading, arc/unit), UV1/UV2 object coords; MaterialID
		TArray<FLayerInstances> Layers;
	};

	/** Builds one stroke. Returns false when the stroke produces no geometry (too short, revealed to zero, ...). */
	bool BuildStroke(const FStrokeJob& Job, const FBuildSettings& Settings, FStrokeResult& Out);

	/** Appends Source into Target (initialising Target's attribute layout on first use). */
	void AppendStrokeMesh(UE::Geometry::FDynamicMesh3& Target, const UE::Geometry::FDynamicMesh3& Source);

	/** UV layers used by the paint materials: 0 = (fading, arc length / unit), 1 = object xy, 2 = (object z, 0). */
	constexpr int32 NumUVLayers = 3;
}
