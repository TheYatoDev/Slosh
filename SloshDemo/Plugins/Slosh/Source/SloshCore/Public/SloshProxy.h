// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "SloshTypes.h"
#include "RenderGraphResources.h"
#include "HAL/CriticalSection.h"

class FRDGBuilder;
class FRHIGPUBufferReadback;

/**
 * Owns the persistent GPU state of one liquid domain and runs its solver on the render thread.
 *
 * Threading contract:
 *  - Created and submitted from the game thread (SubmitFrame_GameThread enqueues a render command
 *    that builds and executes a standalone RDG graph, so simulation is independent of view count).
 *  - All *_RenderThread accessors are only valid on the rendering thread.
 *  - GetResults() is thread safe and returns GPU feedback that is several frames old.
 *  - The last reference must be released on the render thread (see ReleaseOnRenderThread).
 */
class SLOSHCORE_API FSloshProxy : public TSharedFromThis<FSloshProxy, ESPMode::ThreadSafe>
{
public:
	FSloshProxy();
	~FSloshProxy();

	/** Game thread: queue one frame of simulation. */
	void SubmitFrame_GameThread(FSloshFrameInput&& Input);

	/** Game thread: hand the proxy to the render thread for destruction. Resets InOutProxy. */
	static void ReleaseOnRenderThread(TSharedPtr<FSloshProxy, ESPMode::ThreadSafe>& InOutProxy);

	/** Any thread: latest completed GPU readback. */
	FSloshResults GetResults() const;

	// Render thread
	void Step_RenderThread(FRDGBuilder& GraphBuilder, const FSloshFrameInput& Input);
	void PollReadbacks_RenderThread();
	const TRefCountPtr<FRDGPooledBuffer>& GetParticleBuffer_RenderThread() const { return ParticleBuffer; }
	uint32 GetNumParticles_RenderThread() const { return NumParticles; }
	const FTransform& GetSimToWorld_RenderThread() const { return SimToWorld; }
	float GetCellSize_RenderThread() const { return Layout.CellSize; }

private:
	void Allocate(FRDGBuilder& GraphBuilder, const FSloshSolverSettings& Settings);
	void AddPressureSolveMGPCG(FRDGBuilder& GraphBuilder, const FSloshSolverSettings& Settings,
		FRDGBufferRef CellType, FRDGBufferRef Pressure, FRDGBufferRef Divergence, FRDGBufferRef CellDiag, FRDGBufferRef SparseArgs);
	void ReleaseReadbacks();
	void EnsureParticleCapacity(FRDGBuilder& GraphBuilder, uint32 Needed);
	FRDGBufferRef AddSpatialSort(FRDGBuilder& GraphBuilder, FRDGBufferRef Particles);

	struct FReadbackSlot
	{
		FRHIGPUBufferReadback* Coupling = nullptr;
		FRHIGPUBufferReadback* Stats = nullptr;
		uint64 Frame = 0;
		int32 Substeps = 0;
		int32 NumColliders = 0;
		bool bPending = false;
	};

	// Render-thread state
	FSloshSolverSettings Layout;
	bool bAllocated = false;
	uint32 NumCells = 0;           // dense cell count of the grid (MG level 0 dimensions)
	uint32 TotalFaces = 0;         // 3 * PoolCells
	uint32 NumPoolBricks = 0;      // pool slots including the null slot 0
	uint32 PoolCells = 0;          // NumPoolBricks * 512 (size of every per-cell buffer)
	uint32 PoolScanCells = 0;      // (NumPoolBricks - 1) * 512 (threads of a pool-scan grid pass)
	uint64 GridMemoryBytes = 0;
	uint64 ParticleMemoryBytes = 0;
	uint32 AllocatedParticleCapacity = 0;   // slots in ParticleBuffer / ParticleSortBuffer / SortRankBuffer
	uint32 NumParticles = 0;
	uint64 TotalEmitted = 0;
	uint64 SimFrame = 0;
	uint32 EmitSeed = 1;
	float PrevDt = 0.0f;
	float LastMaxSpeed = 0.0f;
	FTransform SimToWorld = FTransform::Identity;

	TRefCountPtr<FRDGPooledBuffer> ParticleBuffer;
	TRefCountPtr<FRDGPooledBuffer> ParticleSortBuffer;   // ping-pong target of the spatial sort
	TRefCountPtr<FRDGPooledBuffer> SortRankBuffer;
	TRefCountPtr<FRDGPooledBuffer> SortCountsBuffer;
	TRefCountPtr<FRDGPooledBuffer> SortOffsetsBuffer;
	TRefCountPtr<FRDGPooledBuffer> ScanBlockSumsBuffer;
	FIntVector SortBrickDim = FIntVector::ZeroValue;
	uint32 NumSortKeys = 0;
	uint32 NumScanBlocks = 0;
	TRefCountPtr<FRDGPooledBuffer> VelBuffers[3];
	TRefCountPtr<FRDGPooledBuffer> VelOldBuffer;
	TRefCountPtr<FRDGPooledBuffer> AccumBuffer;
	TRefCountPtr<FRDGPooledBuffer> FaceFlagsBuffer;
	TRefCountPtr<FRDGPooledBuffer> FaceWeightBuffer;
	TRefCountPtr<FRDGPooledBuffer> CellTypeBuffer;
	TRefCountPtr<FRDGPooledBuffer> CellCountBuffer;
	TRefCountPtr<FRDGPooledBuffer> PressureBuffer;
	TRefCountPtr<FRDGPooledBuffer> DivergenceBuffer;
	TRefCountPtr<FRDGPooledBuffer> CellDiagBuffer;
	TRefCountPtr<FRDGPooledBuffer> DensityQBuffer;
	TRefCountPtr<FRDGPooledBuffer> DensityRhsBuffer;
	// Sparse brick storage (Slosh.usf): per-brick slot table, per-slot brick, free stack, allocator scratch.
	TRefCountPtr<FRDGPooledBuffer> ActiveBricksBuffer;
	TRefCountPtr<FRDGPooledBuffer> BrickTableBuffer;
	TRefCountPtr<FRDGPooledBuffer> SlotBrickBuffer;
	TRefCountPtr<FRDGPooledBuffer> FreeSlotsBuffer;
	TRefCountPtr<FRDGPooledBuffer> BrickTouchBuffer;
	TRefCountPtr<FRDGPooledBuffer> BrickWantBuffer;
	TRefCountPtr<FRDGPooledBuffer> NewSlotsBuffer;
	FIntVector BrickDim = FIntVector::ZeroValue;
	uint32 NumBricks = 0;
	float LastPoolUsage = 1.0f;   // lagged readback; drives the list/scan dispatch choice
	TRefCountPtr<FRDGPooledBuffer> CouplingBuffer;
	TRefCountPtr<FRDGPooledBuffer> StatsBuffer;

	// MGPCG state. Level 0 is the simulation grid (CellType/Z/R); levels 1.. are coarsened 2x.
	struct FMGLevel
	{
		FIntVector Dim = FIntVector::ZeroValue;
		uint32 Cells = 0;
		TRefCountPtr<FRDGPooledBuffer> Type;
		TRefCountPtr<FRDGPooledBuffer> X;
		TRefCountPtr<FRDGPooledBuffer> B;
	};
	TArray<FMGLevel> MGLevels;
	TRefCountPtr<FRDGPooledBuffer> PCGResidual;
	TRefCountPtr<FRDGPooledBuffer> PCGPrecond;
	TRefCountPtr<FRDGPooledBuffer> PCGSearch;
	TRefCountPtr<FRDGPooledBuffer> PCGProduct;
	TRefCountPtr<FRDGPooledBuffer> PCGPartials;
	TRefCountPtr<FRDGPooledBuffer> PCGScalars;
	uint32 NumPartials = 0;

	static constexpr int32 NumReadbackSlots = 4;
	FReadbackSlot Readbacks[NumReadbackSlots];
	int32 NextReadbackSlot = 0;

	// Shared with the game thread
	mutable FCriticalSection ResultsLock;
	FSloshResults Results;
	uint64 SubmittedFrames = 0;
};

using FSloshProxyPtr = TSharedPtr<FSloshProxy, ESPMode::ThreadSafe>;
using FSloshProxyWeakPtr = TWeakPtr<FSloshProxy, ESPMode::ThreadSafe>;
