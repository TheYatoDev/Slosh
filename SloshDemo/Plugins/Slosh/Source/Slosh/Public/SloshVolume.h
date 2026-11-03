// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SloshMaterialAsset.h"
#include "SloshProxy.h"
#include "SloshVolume.generated.h"

class UBoxComponent;
class USloshColliderComponent;
class ADirectionalLight;

/**
 * A box-shaped liquid simulation domain (dense MAC grid, GPU FLIP/APIC).
 * The actor location is the domain center; rotation orients the grid; actor scale is ignored.
 */
UENUM(BlueprintType)
enum class ESloshPressureSolverType : uint8
{
	/** RBGS below AutoSolverCellThreshold cells, MGPCG above (measured crossover, see Research/PERFORMANCE_PLAN.md). */
	Auto,
	/** Multigrid-preconditioned conjugate gradient: best cost/accuracy on large grids. */
	MGPCG,
	/** Red-black SOR with a fixed iteration count: cheapest per iteration, weak at depth. */
	RBGS,
};

UCLASS(BlueprintType, Blueprintable)
class SLOSH_API ASloshVolume : public AActor
{
	GENERATED_BODY()

public:
	ASloshVolume();

	// ---- Domain ----
	/** Cells per axis. Particle capacity and cost scale with this. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Slosh|Domain")
	FIntVector GridResolution = FIntVector(64, 64, 48);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Slosh|Domain", meta = (ClampMin = "0.5", Units = "cm"))
	float CellSize = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Slosh|Domain", meta = (ClampMin = "1024"))
	int32 ParticleCapacity = 262144;

	// ---- Liquid ----
	/** Optional liquid type. When unset, the inline properties below are used. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh")
	TObjectPtr<USloshMaterialAsset> Material;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (EditCondition = "Material == nullptr"))
	FSloshPhysicalProperties Physical;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh", meta = (EditCondition = "Material == nullptr"))
	FSloshVisualProperties Visual;

	// ---- Initial state ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Initial")
	bool bFillOnBeginPlay = true;

	/** Region filled at BeginPlay, in cm relative to the domain center (local axes). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Initial")
	FBox InitialFillBox = FBox(FVector(-155, -155, -115), FVector(-75, 155, 15));

	// ---- Solver quality ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver")
	ESloshPressureSolverType PressureSolver = ESloshPressureSolverType::Auto;

	/** Auto mode switches to MGPCG above this many grid cells. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "0", EditCondition = "PressureSolver == ESloshPressureSolverType::Auto"))
	int32 AutoSolverCellThreshold = 400000;

	/** MGPCG outer iterations (each applies one multigrid V-cycle). 3 matched 4 on every accuracy test (PERFORMANCE_PLAN §10h). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "1", ClampMax = "100", EditCondition = "PressureSolver != ESloshPressureSolverType::RBGS"))
	int32 PCGIterations = 3;

	/** Red-black SOR iterations. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "1", ClampMax = "400", EditCondition = "PressureSolver != ESloshPressureSolverType::MGPCG"))
	int32 PressureIterations = 60;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "1.0", ClampMax = "1.95"))
	float SORWeight = 1.7f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "0", ClampMax = "100"))
	int32 ViscosityIterations = 12;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxSubsteps = 4;

	/**
	 * Most simulated time per frame, in seconds. When the game runs slower than this, the liquid runs in slow motion
	 * rather than simulating the full frame time: more simulated time needs more substeps, which slows the frame
	 * further (a feedback spiral). Raise it (up to 0.1) to trade that stability for real-time accuracy.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "0.004", ClampMax = "0.1"))
	float MaxFrameDt = 1.0f / 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "0.25", ClampMax = "10.0"))
	float CFL = 2.0f;

	/**
	 * Spatially sort particles every N frames (0 = never). Sorting keeps P2G/G2P cache-coherent; particles drift out of
	 * order slowly, so sorting every 4 frames costs P2G/G2P little and saves most of the sort (measured in every test scene).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "0", ClampMax = "60"))
	int32 SortInterval = 4;

	/** Rigid-body pressure force from SDF-weighted cell faces (smooth as bodies move) instead of binary faces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver")
	bool bSmoothCouplingForce = true;

	/** Ghost-fluid free surface: pressure is zero at the reconstructed interface instead of the air cell centre. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver")
	bool bGhostFluid = true;

	/** Density correction acts only above (1 + DensityDeadband) x rest density (avoids slow inflation). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float DensityDeadband = 0.05f;

	/** Two-sided, position-based density projection (expands over-dense and contracts under-dense liquid,
	 *  no momentum injected). Replaces the velocity correction when enabled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver")
	bool bTwoSidedDensityProjection = true;

	/** Iterate grid passes through the active-brick list (indirect dispatch) while few pool bricks are in use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver")
	bool bSparseBricks = true;

	/** The active list is used only while less than this fraction of the brick pool is in use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "0.0", ClampMax = "1.01", EditCondition = "bSparseBricks"))
	float SparseMaxActiveFraction = 0.5f;

	/**
	 * Grid storage capacity as a fraction of all 8^3 bricks. Only bricks within one brick of a particle hold
	 * storage, so a shallow or compact body of liquid needs far less than 1.0. Bricks beyond the budget get no
	 * storage (their particles stop seeing the grid): watch GetPoolOverflowBricks().
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float SparseStorageBudget = 1.0f;

	/** Fraction of all 8^3 bricks holding storage (delayed). */
	UFUNCTION(BlueprintPure, Category = "Slosh|Diagnostics")
	float GetActiveBrickFraction() const { return LatestResults.ActiveBrickFraction; }

	/** Allocated bricks / pool capacity (delayed). */
	UFUNCTION(BlueprintPure, Category = "Slosh|Diagnostics")
	float GetPoolUsage() const { return LatestResults.PoolUsage; }

	/** Bricks that needed storage but found the pool full, last frame (delayed). Non-zero: raise SparseStorageBudget. */
	UFUNCTION(BlueprintPure, Category = "Slosh|Diagnostics")
	int32 GetPoolOverflowBricks() const { return LatestResults.PoolOverflowBricks; }

	/** Particle updates that hit MaxParticleSpeed last frame (delayed). Persistent non-zero values mean the step is unresolved. */
	UFUNCTION(BlueprintPure, Category = "Slosh|Diagnostics")
	int32 GetClampedParticleCount() const { return LatestResults.ClampedParticles; }

	const FSloshResults& GetLatestResults() const { return LatestResults; }

	/** GPU memory of the particle buffers (allocated slots, not ParticleCapacity), in MiB. */
	UFUNCTION(BlueprintPure, Category = "Slosh|Diagnostics")
	float GetParticleMemoryMB() const { return LatestResults.ParticleMemoryMB; }

	/** GPU memory of the grid, brick pool and solver buffers, in MiB. */
	UFUNCTION(BlueprintPure, Category = "Slosh|Diagnostics")
	float GetGridMemoryMB() const { return LatestResults.GridMemoryMB; }

	/** Fraction of the density error removed per frame by the projection. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bTwoSidedDensityProjection"))
	float DensityRelaxation = 0.5f;

	/**
	 * Run the density projection every N frames (relaxation compounded so the correction rate is unchanged). 2 kept
	 * volume and buoyancy within the run-to-run spread and saves half the projection cost; useful for large scenes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "1", ClampMax = "8", EditCondition = "bTwoSidedDensityProjection"))
	int32 DensityProjectionInterval = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Solver", meta = (ClampMin = "0.1", Units = "CentimetersPerSecond"))
	float MaxParticleSpeed = 2000.0f;

	// ---- Runtime ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Runtime")
	bool bSimulate = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Runtime")
	bool bRenderLiquid = true;

	/** Directional light used for liquid specular. Auto-detected when unset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render")
	TObjectPtr<ADirectionalLight> SunLight;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render")
	FLinearColor SkyZenithColor = FLinearColor(0.25f, 0.45f, 0.9f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slosh|Render")
	FLinearColor SkyHorizonColor = FLinearColor(0.75f, 0.85f, 1.0f);

	// ---- Blueprint API ----
	/** Fill an axis-aligned (domain-local) box with liquid at rest density. World-space center, extent in cm. */
	UFUNCTION(BlueprintCallable, Category = "Slosh")
	void FillBox(FVector WorldCenter, FVector Extent);

	/** Emit Count particles in a sphere with an initial world velocity (cm/s). */
	UFUNCTION(BlueprintCallable, Category = "Slosh")
	void EmitSphere(FVector WorldCenter, float Radius, FVector WorldVelocity, int32 Count);

	/** Remove all liquid and reallocate GPU state. */
	UFUNCTION(BlueprintCallable, Category = "Slosh")
	void ResetSimulation();

	UFUNCTION(BlueprintCallable, Category = "Slosh")
	void SetPaused(bool bPaused) { bSimulate = !bPaused; }

	UFUNCTION(BlueprintPure, Category = "Slosh")
	bool ContainsPoint(FVector WorldLocation) const;

	/** Domain size in cm. */
	UFUNCTION(BlueprintPure, Category = "Slosh")
	FVector GetDomainSize() const;

	/** Live particle count (exact; tracked on the game thread). */
	UFUNCTION(BlueprintPure, Category = "Slosh")
	int32 GetParticleCount() const { return int32(FMath::Min<int64>(TotalEmittedGameThread, ParticleCapacity)); }

	/** Liquid volume estimated from fluid cells, liters (delayed GPU readback). */
	UFUNCTION(BlueprintPure, Category = "Slosh")
	float GetLiquidVolumeLiters() const;

	UFUNCTION(BlueprintPure, Category = "Slosh")
	float GetMaxParticleSpeed() const { return LatestResults.MaxParticleSpeed * 100.0f; }

	UFUNCTION(BlueprintPure, Category = "Slosh")
	int32 GetLastSubstepCount() const { return LatestResults.Substeps; }

	/** Post-projection velocity divergence, 1/s (solver convergence diagnostic, delayed). */
	UFUNCTION(BlueprintPure, Category = "Slosh|Diagnostics")
	float GetMeanDivergence() const { return LatestResults.MeanDivergence; }

	/** Mean relative particle density of interior liquid (1 = rest). Values above 1 mean compression (volume loss). */
	UFUNCTION(BlueprintPure, Category = "Slosh|Diagnostics")
	float GetMeanInteriorDensity() const { return LatestResults.MeanInteriorDensity; }

	/** Liquid volume implied by the particle count and the mean interior density, liters (delayed). */
	UFUNCTION(BlueprintPure, Category = "Slosh|Diagnostics")
	float GetParticleVolumeLiters() const;

	UFUNCTION(BlueprintPure, Category = "Slosh|Diagnostics")
	float GetMaxDivergence() const { return LatestResults.MaxDivergence; }

	/** Frames between submission and the latest feedback (GPU readback latency). */
	UFUNCTION(BlueprintPure, Category = "Slosh")
	int32 GetFeedbackLatencyFrames() const;

	FTransform GetSimToWorld() const;
	const FSloshPhysicalProperties& GetPhysical() const { return Material ? Material->Physical : Physical; }
	const FSloshVisualProperties& GetVisual() const { return Material ? Material->Visual : Visual; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void OnConstruction(const FTransform& Transform) override;

private:
	FSloshSolverSettings BuildSolverSettings() const;
	void QueueFillLocalBox(const FBox& LocalBoxCm);
	void UpdateRenderRegistration();
	void DistributeFeedback();

	UPROPERTY(VisibleAnywhere, Category = "Slosh")
	TObjectPtr<UBoxComponent> Bounds;

	FSloshProxyPtr Proxy;
	TArray<FSloshEmitRequest> PendingEmits;
	bool bPendingReset = false;
	int64 TotalEmittedGameThread = 0;
	uint64 SubmittedFrames = 0;
	uint64 LastDistributedFrame = 0;
	FSloshResults LatestResults;

	/** Collider order per submitted frame, so delayed feedback maps back to the right component. */
	TMap<uint64, TArray<TWeakObjectPtr<USloshColliderComponent>>> ColliderHistory;
};
