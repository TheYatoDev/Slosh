#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "SloshDemoGameMode.generated.h"

class ASloshVolume;
class AStaticMeshActor;
class USloshColliderComponent;

/**
 * Builds a self-contained test scene: dam break, a buoyant box, a heavy sphere, a kinematic paddle
 * and a continuous emitter. With -SloshAutoTest it logs metrics, captures a GPU profile and a
 * screenshot, then exits. Optional: -SloshGrid=X,Y,Z  -SloshIters=N  -SloshAutoTestSeconds=S
 */
UCLASS()
class ASloshDemoGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ASloshDemoGameMode();
	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	AStaticMeshActor* SpawnBody(const TCHAR* MeshPath, const FVector& Location, float Scale, float MassKg, bool bSimulate, bool bSphere);

	UPROPERTY()
	TObjectPtr<ASloshVolume> Volume;

	UPROPERTY()
	TObjectPtr<AStaticMeshActor> FloatingBox;

	UPROPERTY()
	TObjectPtr<AStaticMeshActor> HeavySphere;

	UPROPERTY()
	TObjectPtr<AStaticMeshActor> Paddle;

	UPROPERTY()
	TObjectPtr<AStaticMeshActor> PressureProbe;

	FVector ProbeForceAccum = FVector::ZeroVector;
	double ProbeVolumeAccum = 0.0;
	int32 ProbeSamples = 0;

	FVector PaddleOrigin = FVector::ZeroVector;
	double Elapsed = 0.0;
	double NextLog = 1.0;
	double FrameTimeAccum = 0.0;
	double GpuTimeAccum = 0.0;
	int32 FrameCount = 0;
	bool bAutoTest = false;
	bool bCalm = false;   // -SloshCalm: still, deep pool with no paddle/emitter/sphere (ground-truth tests)
	bool bProfiled = false;
	bool bScreenshot = false;
	double AutoTestSeconds = 14.0;
};
