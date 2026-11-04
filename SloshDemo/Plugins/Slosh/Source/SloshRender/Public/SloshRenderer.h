// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "SloshProxy.h"

class FSceneInterface;
class FSloshSurfaceMeshBuffers;

/** How a volume's liquid surface is produced. */
enum class ESloshSurfaceRenderMode : uint8
{
	/** Screen-space particle splats (spheres or anisotropic ellipsoids) with depth smoothing. */
	Splats,
	/** A density field built from the particles, ray-marched per pixel: smooth by construction. */
	Raymarch,
	/** The same field turned into a GPU triangle mesh (surface nets), drawn with a UE material. */
	Mesh,
};

/** Visual parameters for one liquid volume (render thread copy). */
struct FSloshRenderSettings
{
	FVector3f AbsorptionPerMeter = FVector3f(4.0f, 1.2f, 0.8f);
	FVector3f ScatterColor = FVector3f(0.02f, 0.08f, 0.10f);
	FVector3f SkyZenithColor = FVector3f(0.25f, 0.45f, 0.9f);
	FVector3f SkyHorizonColor = FVector3f(0.75f, 0.85f, 1.0f);
	FVector3f LightDirectionWorld = FVector3f(0.3f, 0.2f, -0.9f);   // direction the light travels
	FVector3f LightColor = FVector3f(3.0f, 2.9f, 2.7f);
	float RefractionStrength = 0.02f;
	float FresnelF0 = 0.02f;
	float SpecularPower = 400.0f;
	float ParticleRadiusScale = 0.6f;
	int32 SmoothingRadius = 7;
	float SmoothingScale = 0.0f;         // > 0: adaptive (depth-scaled, tilt-aware) smoothing + thickness blur
	int32 SmoothingIterations = 1;
	float EdgeFadeThickness = 0.0f;
	float ScatterLighting = 0.0f;
	float Anisotropy = 0.0f;             // 0 = sphere splats
	float AnisotropyMaxStretch = 4.0f;
	float AnisotropySmoothing = 0.5f;
	ESloshSurfaceRenderMode SurfaceMode = ESloshSurfaceRenderMode::Splats;
	float FieldResolution = 1.0f;        // field voxels per solver cell
	float SurfaceSmoothing = 2.0f;       // field kernel radius in splat radii
	float IsoLevel = 0.5f;               // surface threshold: 0.5 = particle radius, lower = fatter, higher = thinner
	FVector3f LocalOffsetCm = FVector3f::ZeroVector;   // volume-local cm of the sim-space origin (mesh mode)
	TSharedPtr<FSloshSurfaceMeshBuffers, ESPMode::ThreadSafe> MeshBuffers;   // mesh mode
	bool bEnabled = true;
};

/**
 * Screen-space liquid renderer. Hooks the renderer's public post-opaque delegate (after opaque
 * lighting and fog, before translucency) and composites every registered volume of the view's scene.
 * It only reads simulation buffers.
 */
namespace SloshRender
{
	/** Game thread, after the Renderer module is loaded. Idempotent. */
	SLOSHRENDER_API void Startup();
	SLOSHRENDER_API void Shutdown();

	SLOSHRENDER_API void AddOrUpdateVolume_GameThread(const FSloshProxyPtr& Proxy, FSceneInterface* Scene, const FSloshRenderSettings& Settings);
	SLOSHRENDER_API void RemoveVolume_GameThread(const FSloshProxy* Proxy);
}
