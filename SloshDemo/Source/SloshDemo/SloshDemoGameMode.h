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
	/** -SloshPaintTest: an animated character and a sphere painted through traces, two screenshots, then exit. */
	void StartPaintTest();
	void TickPaintTest();
	void PaintTestStroke(class USloshPaintPreset* Preset, const TArray<FVector>& Targets, const FVector& RayDir);

	bool bPaintTest = false;
	int32 PaintTestStage = 0;

	/** -SloshSkelTest: a cylinder pours viscous liquid onto an animated skeletal mesh fitted with bone capsules. */
	void StartSkelTest();
	void TickSkelTest();

	bool bSkelTest = false;
	int32 SkelTestStage = 0;

	UPROPERTY()
	TObjectPtr<class UPoseableMeshComponent> SkelPose;

	UPROPERTY()
	TObjectPtr<class USloshSkeletalColliderComponent> SkelColliders;

	/** A level that already contains a SloshVolume is used as-is: no demo scene, just stats (and -SloshAutoTest captures). */
	void TickPlacedScene();
	bool bPlacedScene = false;

	FName SkelArmBone;
	FQuat SkelArmRefCS = FQuat::Identity;
	FVector SkelSwingAxis = FVector::ForwardVector;

	UPROPERTY()
	TObjectPtr<AActor> PaintCharacter;

	UPROPERTY()
	TObjectPtr<AActor> PaintNozzle;

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
