// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "SloshProxy.h"

class FSceneInterface;

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
