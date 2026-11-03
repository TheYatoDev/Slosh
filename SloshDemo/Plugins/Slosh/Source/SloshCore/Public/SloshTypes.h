// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"

// Plain-data types shared between the game thread and the render-thread GPU solver.
// Sim space: meters, origin at the domain's min corner, axes aligned with the owning volume, Z up.

enum class ESloshTransferMode : uint8
{
	FLIP,
	APIC,
};

enum class ESloshPressureSolver : uint8
{
	/** Red-black SOR, fixed iteration count (cheap per iteration, slow convergence at depth). */
	RBGS,
	/** Multigrid-preconditioned conjugate gradient (McAdams et al. 2010). */
	MGPCG,
};

enum class ESloshColliderShape : uint32
{
	Sphere = 0,
	Box = 1,
	Capsule = 2,
};

/** Must match FSloshCollider in SloshCommon.ush (80 bytes). */
struct FSloshColliderGPU
{
	FVector3f Center = FVector3f::ZeroVector;
	uint32 Type = 0;
	FVector4f Rotation = FVector4f(0, 0, 0, 1);
	FVector3f Extent = FVector3f::ZeroVector;
	uint32 Flags = 0;
	FVector3f LinearVelocity = FVector3f::ZeroVector;
	float Pad0 = 0.0f;
	FVector3f AngularVelocity = FVector3f::ZeroVector;
	float Pad1 = 0.0f;
};
static_assert(sizeof(FSloshColliderGPU) == 80, "FSloshColliderGPU must match the HLSL layout");

/** Must match FSloshParticle in SloshCommon.ush (64 bytes). */
// Stored particle (HLSL FSloshParticlePacked): the APIC affine matrix is 9 halves in C.
struct FSloshParticleGPU
{
	FVector3f Pos;
	float Age;
	FVector3f Vel;
	uint32 C[5];
};
static_assert(sizeof(FSloshParticleGPU) == 48, "FSloshParticleGPU must match the HLSL layout");

namespace Slosh
{
	constexpr uint32 MaxColliders = 64;
	constexpr uint32 CouplingStride = 16;
	constexpr float CouplingFixedScale = 1024.0f;
	constexpr uint32 ColliderFlagTwoWay = 1u;

	// Upper bound on particles; 1D dispatches of more than 65535 groups wrap into rows (SloshCommon.ush LinearThreadId).
	constexpr uint32 MaxParticleCapacity = 16u * 1024u * 1024u;
	constexpr uint32 DispatchRowGroups = 32768;

	inline FIntVector GroupCount1D(uint32 NumThreads, uint32 GroupSize)
	{
		const uint32 Groups = FMath::Max(FMath::DivideAndRoundUp(NumThreads, GroupSize), 1u);
		return FIntVector(int32(FMath::Min(Groups, DispatchRowGroups)), int32(FMath::DivideAndRoundUp(Groups, DispatchRowGroups)), 1);
	}
}

/** Physical solver configuration. Grid layout changes reallocate GPU state. */
struct FSloshSolverSettings
{
	FIntVector GridDim = FIntVector(64, 64, 48);
	float CellSize = 0.05f;               // meters
	uint32 ParticleCapacity = 262144;     // maximum live particles; buffers grow with the live count up to this

	ESloshTransferMode TransferMode = ESloshTransferMode::APIC;
	float FlipRatio = 0.95f;              // FLIP mode only
	float Density = 1000.0f;              // kg/m^3
	float KinematicViscosity = 0.0f;      // m^2/s
	float BoundaryFriction = 0.0f;        // 0 = free slip, 1 = no slip (tangential)
	float DensityCorrection = 0.15f;
	float DensityDeadband = 0.05f;        // velocity correction only: correct above (1 + deadband) x rest density
	bool bTwoSidedDensityProjection = true; // position-based two-sided correction (replaces the velocity correction)
	float DensityRelaxation = 0.5f;       // fraction of the density error removed per frame
	int32 DensityProjectionIterations = 30;  // RBGS sweeps (MGPCG uses 2 PCG iterations)
	int32 DensityProjectionInterval = 1;  // project every N frames; relaxation is compounded to 1 - (1 - r)^N
	bool bSparseBricks = true;            // allow indirect dispatch over the active-brick list (else scan the pool)
	float SparseMaxActiveFraction = 0.5f; // use the list only while the active fraction of the pool is below this
	float SparseStorageBudget = 1.0f;     // brick pool capacity as a fraction of all 8^3 bricks (grid memory); bricks
	                                      // beyond it get no storage (PoolOverflowBricks > 0)
	float ParticleRadiusCells = 0.25f;

	FVector3f Gravity = FVector3f(0, 0, -9.81f);  // sim space, m/s^2
	ESloshPressureSolver PressureSolver = ESloshPressureSolver::MGPCG;
	int32 PressureIterations = 60;       // RBGS
	float SORWeight = 1.7f;              // RBGS
	int32 PCGIterations = 8;             // MGPCG outer iterations
	int32 MGCoarseIterations = 16;       // red-black sweeps on the coarsest level
	int32 SortInterval = 4;              // spatially sort particles every N frames (0 = never); 4 measured best in every scene
	bool bSmoothCouplingForce = true;    // SDF-weighted pressure force (false = binary cell faces)
	bool bGhostFluid = true;             // free-surface p = 0 at the interface instead of the air cell centre
	int32 ViscosityIterations = 12;
	int32 ExtrapolationLayers = 2;
	int32 MinSubsteps = 1;
	int32 MaxSubsteps = 4;
	float MaxFrameDt = 1.0f / 30.0f;      // simulated seconds per frame at most: slower frames run in slow motion instead
	                                      // of simulating more time (more substeps, slower frames, a feedback spiral)
	float CFL = 2.0f;
	float MaxSpeed = 20.0f;               // m/s particle speed clamp

	bool LayoutEquals(const FSloshSolverSettings& Other) const
	{
		return GridDim == Other.GridDim && SparseStorageBudget == Other.SparseStorageBudget;
	}
};

enum class ESloshEmitShape : uint32
{
	FillCells = 0,
	Sphere = 1,
	Box = 2,
};

struct FSloshEmitRequest
{
	ESloshEmitShape Shape = ESloshEmitShape::Sphere;
	FVector3f Center = FVector3f::ZeroVector;   // sim space
	FVector3f Extent = FVector3f::ZeroVector;   // sphere: x = radius; box: half extents
	FVector3f Velocity = FVector3f::ZeroVector;
	FIntVector CellMin = FIntVector::ZeroValue; // FillCells only
	FIntVector CellDims = FIntVector::ZeroValue;
	uint32 Count = 0;
};

/** Everything the GPU needs for one game frame. */
struct FSloshFrameInput
{
	FSloshSolverSettings Settings;
	TArray<FSloshColliderGPU> Colliders;
	TArray<FSloshEmitRequest> Emits;
	FTransform SimToWorld = FTransform::Identity;   // includes meters -> cm scale
	float DeltaTime = 1.0f / 60.0f;
	bool bReset = false;
	bool bPaused = false;
};

/** Per-collider fluid feedback, in sim space and SI units. Delayed by the readback latency. */
struct FSloshColliderFeedback
{
	FVector3f PressureForce = FVector3f::ZeroVector;    // N
	FVector3f PressureTorque = FVector3f::ZeroVector;   // N*m about the collider center
	FVector3f MeanFluidVelocity = FVector3f::ZeroVector;// m/s over wetted faces
	FVector3f SubmergedCentroidOffset = FVector3f::ZeroVector; // m, sim axes, relative to collider center
	int32 WettedFaces = 0;
	int32 SubmergedCells = 0;
	int32 SolidCells = 0;
};

struct FSloshResults
{
	TArray<FSloshColliderFeedback> Colliders;
	float MaxParticleSpeed = 0.0f;
	int32 FluidCells = 0;
	float MaxDivergence = 0.0f;   // 1/s, post-projection, last substep
	float MeanDivergence = 0.0f;  // 1/s
	float MeanInteriorDensity = 0.0f;  // mean relative density of liquid cells not touching air (1 = rest)
	int32 InteriorCells = 0;
	float ActiveBrickFraction = -1.0f; // fraction of all 8^3 bricks holding storage (-1 = not measured)
	float PoolUsage = -1.0f;           // allocated bricks / pool capacity
	int32 PoolOverflowBricks = 0;      // bricks that needed storage but found the pool full (summed over substeps)
	int32 ClampedParticles = 0;        // particle updates hitting MaxSpeed (summed over substeps)
	int32 ClampedInSolid = 0;          // ... of which started the substep inside a solid cell
	FVector3f ClampedCentroid = FVector3f::ZeroVector; // sim space, m
	float GridMemoryMB = 0.0f;         // GPU memory of grid, pool, solver and sort buffers
	float ParticleMemoryMB = 0.0f;
	int32 AllocatedParticleCapacity = 0; // particle slots currently allocated (grows on demand up to ParticleCapacity)
	int32 Substeps = 0;
	uint64 SimFrame = 0;         // simulation frame these results were produced on
	uint64 LatestSubmittedFrame = 0;
};
