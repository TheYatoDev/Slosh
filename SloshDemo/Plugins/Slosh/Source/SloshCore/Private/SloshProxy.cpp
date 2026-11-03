// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshProxy.h"
#include "SloshShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "GlobalShader.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"

DECLARE_GPU_STAT(Slosh);

namespace
{
	constexpr int32 ThreadGroupSize = 128;
	constexpr uint32 NumStats = 14;
	constexpr uint32 BrickCells = 512;   // 8^3 cells per brick / pool slot
	constexpr uint32 MinParticleSlots = 16384;
	constexpr uint32 ParticleSlotAlignment = 16384;

	template <typename TShader>
	void AddSimPass(FRDGBuilder& GraphBuilder, FRDGEventName&& Name, const FSloshPassParameters& Base, uint32 NumThreads,
		TFunctionRef<void(FSloshPassParameters&)> Customize)
	{
		if (NumThreads == 0)
		{
			return;
		}
		TShaderMapRef<TShader> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSloshPassParameters* Parameters = GraphBuilder.AllocParameters<FSloshPassParameters>();
		*Parameters = Base;
		Customize(*Parameters);
		FComputeShaderUtils::AddPass(GraphBuilder, MoveTemp(Name), Shader, Parameters,
			Slosh::GroupCount1D(NumThreads, ThreadGroupSize));
	}

	void NoCustomization(FSloshPassParameters&) {}

	template <typename TShader>
	void AddIndirectPass(FRDGBuilder& GraphBuilder, FRDGEventName&& Name, const FSloshPassParameters& Base, FRDGBufferRef Args,
		uint32 ArgsOffset, TFunctionRef<void(FSloshPassParameters&)> Customize)
	{
		TShaderMapRef<TShader> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSloshPassParameters* Parameters = GraphBuilder.AllocParameters<FSloshPassParameters>();
		*Parameters = Base;
		Customize(*Parameters);
		Parameters->IndirectArgs = Args;
		FComputeShaderUtils::AddPass(GraphBuilder, MoveTemp(Name), Shader, Parameters, Args, ArgsOffset);
	}

	// Grid pass over the allocated bricks: indirect over the active-brick list (sparse), or a direct scan of
	// every pool slot (ScanCells threads; free slots exit).
	template <typename TShader>
	void AddGridPass(FRDGBuilder& GraphBuilder, FRDGEventName&& Name, const FSloshPassParameters& Base, uint32 ScanCells,
		FRDGBufferRef SparseArgs, TFunctionRef<void(FSloshPassParameters&)> Customize)
	{
		if (SparseArgs)
		{
			AddIndirectPass<TShader>(GraphBuilder, MoveTemp(Name), Base, SparseArgs, 0,
				[&](FSloshPassParameters& P) { Customize(P); P.bSparse = 1u; });
			return;
		}
		TShaderMapRef<TShader> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSloshPassParameters* Parameters = GraphBuilder.AllocParameters<FSloshPassParameters>();
		*Parameters = Base;
		Customize(*Parameters);
		Parameters->bSparse = 0u;
		FComputeShaderUtils::AddPass(GraphBuilder, MoveTemp(Name), Shader, Parameters,
			Slosh::GroupCount1D(ScanCells, ThreadGroupSize));
	}

	constexpr uint32 MGCoarsestMaxCells = 512;
	constexpr uint32 SortScanBlock = 1024;   // keys per scan group (SloshSort.usf SCAN_BLOCK)
	constexpr int32 MGMaxLevels = 8;

	template <typename TShader>
	void AddMGPass(FRDGBuilder& GraphBuilder, FRDGEventName&& Name, const FSloshMGParameters& Parameters, FIntVector GroupCount)
	{
		TShaderMapRef<TShader> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSloshMGParameters* P = GraphBuilder.AllocParameters<FSloshMGParameters>();
		*P = Parameters;
		FComputeShaderUtils::AddPass(GraphBuilder, MoveTemp(Name), Shader, P, GroupCount);
	}

	// Level-0 MG pass: indirect over the active bricks when SparseArgs is set (ArgsOffset selects the fine-cell or
	// level-1-cell entry), otherwise dense.
	template <typename TShader>
	void AddMGPassL0(FRDGBuilder& GraphBuilder, FRDGEventName&& Name, const FSloshMGParameters& Parameters, FIntVector DenseGroups,
		FRDGBufferRef SparseArgs, uint32 ArgsOffset)
	{
		TShaderMapRef<TShader> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSloshMGParameters* P = GraphBuilder.AllocParameters<FSloshMGParameters>();
		*P = Parameters;
		if (SparseArgs)
		{
			P->bSparse = 1u;
			P->IndirectArgs = SparseArgs;
			FComputeShaderUtils::AddPass(GraphBuilder, MoveTemp(Name), Shader, P, SparseArgs, ArgsOffset);
		}
		else
		{
			P->bSparse = 0u;
			FComputeShaderUtils::AddPass(GraphBuilder, MoveTemp(Name), Shader, P, DenseGroups);
		}
	}

	FIntVector Groups(uint32 Threads)
	{
		return Slosh::GroupCount1D(Threads, ThreadGroupSize);
	}

	FRDGBufferRef CreatePersistent(FRDGBuilder& GraphBuilder, TRefCountPtr<FRDGPooledBuffer>& Out, const TCHAR* Name, uint32 Stride, uint32 Count)
	{
		FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(Stride, FMath::Max(Count, 1u)), Name);
		Out = GraphBuilder.ConvertToExternalBuffer(Buffer);
		return Buffer;
	}
}

FSloshProxy::FSloshProxy()
{
	for (int32 Index = 0; Index < NumReadbackSlots; ++Index)
	{
		Readbacks[Index].Coupling = new FRHIGPUBufferReadback(TEXT("Slosh.CouplingReadback"));
		Readbacks[Index].Stats = new FRHIGPUBufferReadback(TEXT("Slosh.StatsReadback"));
	}
}

FSloshProxy::~FSloshProxy()
{
	ReleaseReadbacks();
}

void FSloshProxy::ReleaseReadbacks()
{
	for (FReadbackSlot& Slot : Readbacks)
	{
		delete Slot.Coupling;
		delete Slot.Stats;
		Slot.Coupling = nullptr;
		Slot.Stats = nullptr;
	}
}

void FSloshProxy::SubmitFrame_GameThread(FSloshFrameInput&& Input)
{
	check(IsInGameThread());
	{
		FScopeLock Lock(&ResultsLock);
		++SubmittedFrames;
	}
	ENQUEUE_RENDER_COMMAND(SloshStep)(
		[Self = AsShared(), Input = MoveTemp(Input)](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("Slosh"));
			Self->Step_RenderThread(GraphBuilder, Input);
			GraphBuilder.Execute();
			Self->PollReadbacks_RenderThread();
		});
}

void FSloshProxy::ReleaseOnRenderThread(FSloshProxyPtr& InOutProxy)
{
	if (!InOutProxy.IsValid())
	{
		return;
	}
	ENQUEUE_RENDER_COMMAND(SloshRelease)(
		[Proxy = MoveTemp(InOutProxy)](FRHICommandListImmediate&) mutable
		{
			Proxy.Reset();
		});
	InOutProxy.Reset();
}

FSloshResults FSloshProxy::GetResults() const
{
	FScopeLock Lock(&ResultsLock);
	FSloshResults Copy = Results;
	Copy.LatestSubmittedFrame = SubmittedFrames;
	return Copy;
}

void FSloshProxy::Allocate(FRDGBuilder& GraphBuilder, const FSloshSolverSettings& Settings)
{
	Layout = Settings;
	const FIntVector D = Settings.GridDim;
	NumCells = uint32(D.X) * uint32(D.Y) * uint32(D.Z);

	// Sparse brick storage: 8^3 bricks; a pool of slots sized by the budget plus the null slot 0. Every
	// per-cell buffer holds PoolCells cells and every per-face buffer three planes of PoolCells faces.
	BrickDim = FIntVector((D.X + 7) / 8, (D.Y + 7) / 8, (D.Z + 7) / 8);
	NumBricks = uint32(BrickDim.X) * uint32(BrickDim.Y) * uint32(BrickDim.Z);
	const uint32 PoolSlots = FMath::Clamp<uint32>(uint32(FMath::CeilToDouble(double(FMath::Clamp(Settings.SparseStorageBudget, 0.0f, 1.0f)) * NumBricks)), 1u, NumBricks);
	NumPoolBricks = PoolSlots + 1;
	PoolCells = NumPoolBricks * BrickCells;
	PoolScanCells = PoolSlots * BrickCells;
	TotalFaces = 3 * PoolCells;
	LastPoolUsage = 1.0f;

	GridMemoryBytes = 0;
	auto Persistent = [&](TRefCountPtr<FRDGPooledBuffer>& Out, const TCHAR* Name, uint32 Stride, uint32 Count)
	{
		GridMemoryBytes += uint64(Stride) * FMath::Max(Count, 1u);
		return CreatePersistent(GraphBuilder, Out, Name, Stride, Count);
	};

	// Particle buffers start empty and grow with the live count (EnsureParticleCapacity).
	ParticleBuffer.SafeRelease();
	ParticleSortBuffer.SafeRelease();
	SortRankBuffer.SafeRelease();
	AllocatedParticleCapacity = 0;
	ParticleMemoryBytes = 0;
	NumParticles = 0;
	EnsureParticleCapacity(GraphBuilder, MinParticleSlots);
	SortBrickDim = FIntVector((D.X + 3) / 4, (D.Y + 3) / 4, (D.Z + 3) / 4);
	NumSortKeys = uint32(SortBrickDim.X) * uint32(SortBrickDim.Y) * uint32(SortBrickDim.Z);
	NumScanBlocks = FMath::DivideAndRoundUp(NumSortKeys, SortScanBlock);
	Persistent(SortCountsBuffer, TEXT("Slosh.SortCounts"), sizeof(uint32), NumSortKeys);
	Persistent(SortOffsetsBuffer, TEXT("Slosh.SortOffsets"), sizeof(uint32), NumSortKeys);
	Persistent(ScanBlockSumsBuffer, TEXT("Slosh.ScanBlockSums"), sizeof(uint32), NumScanBlocks);
	FRDGBufferRef Cleared[] =
	{
		Persistent(VelBuffers[0], TEXT("Slosh.VelA"), sizeof(float), TotalFaces),
		Persistent(VelBuffers[1], TEXT("Slosh.VelB"), sizeof(float), TotalFaces),
		Persistent(VelBuffers[2], TEXT("Slosh.VelC"), sizeof(float), TotalFaces),
		Persistent(VelOldBuffer, TEXT("Slosh.VelOld"), sizeof(float), TotalFaces),
		Persistent(AccumBuffer, TEXT("Slosh.Accum"), sizeof(int32), TotalFaces * 2),
		Persistent(FaceFlagsBuffer, TEXT("Slosh.FaceFlags"), sizeof(uint32), TotalFaces),
		Persistent(FaceWeightBuffer, TEXT("Slosh.FaceWeight"), sizeof(float), TotalFaces),
		Persistent(CellTypeBuffer, TEXT("Slosh.CellType"), sizeof(uint32), PoolCells),
		Persistent(CellCountBuffer, TEXT("Slosh.CellCount"), sizeof(int32), PoolCells),
		Persistent(PressureBuffer, TEXT("Slosh.Pressure"), sizeof(float), PoolCells),
		Persistent(DivergenceBuffer, TEXT("Slosh.Divergence"), sizeof(float), PoolCells),
		Persistent(CellDiagBuffer, TEXT("Slosh.CellDiag"), sizeof(float), PoolCells),
		Persistent(DensityQBuffer, TEXT("Slosh.DensityQ"), sizeof(float), PoolCells),
		Persistent(DensityRhsBuffer, TEXT("Slosh.DensityRhs"), sizeof(float), PoolCells),
		Persistent(CouplingBuffer, TEXT("Slosh.Coupling"), sizeof(int32), Slosh::MaxColliders * Slosh::CouplingStride),
		Persistent(StatsBuffer, TEXT("Slosh.Stats"), sizeof(uint32), NumStats),
		Persistent(ActiveBricksBuffer, TEXT("Slosh.ActiveBricks"), sizeof(uint32), NumBricks + 1),
		Persistent(BrickTableBuffer, TEXT("Slosh.BrickTable"), sizeof(uint32), NumBricks),
		Persistent(BrickTouchBuffer, TEXT("Slosh.BrickTouch"), sizeof(uint32), NumBricks),
		Persistent(BrickWantBuffer, TEXT("Slosh.BrickWant"), sizeof(uint32), NumBricks),
		Persistent(NewSlotsBuffer, TEXT("Slosh.NewSlots"), sizeof(uint32), NumPoolBricks + 1),
	};
	for (FRDGBufferRef Buffer : Cleared)
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Buffer), 0u);
	}
	// Every slot starts free: SlotBrick = none, free stack = [PoolSlots, 1, 2, ..., PoolSlots].
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Persistent(SlotBrickBuffer, TEXT("Slosh.SlotBrick"), sizeof(uint32), NumPoolBricks)), 0xFFFFFFFFu);
	{
		TArray<uint32> FreeInit;
		FreeInit.SetNumUninitialized(NumPoolBricks);
		FreeInit[0] = PoolSlots;
		for (uint32 Slot = 1; Slot <= PoolSlots; ++Slot)
		{
			FreeInit[Slot] = Slot;
		}
		FRDGBufferRef FreeSlots = CreateStructuredBuffer(GraphBuilder, TEXT("Slosh.FreeSlots"), sizeof(uint32), FreeInit.Num(),
			FreeInit.GetData(), FreeInit.Num() * sizeof(uint32));
		FreeSlotsBuffer = GraphBuilder.ConvertToExternalBuffer(FreeSlots);
		GridMemoryBytes += FreeInit.Num() * sizeof(uint32);
	}

	// MGPCG hierarchy: halve until the level fits the single-group coarse solver.
	MGLevels.Reset();
	FMGLevel& Fine = MGLevels.AddDefaulted_GetRef();
	Fine.Dim = D;
	Fine.Cells = NumCells;
	while (MGLevels.Last().Cells > MGCoarsestMaxCells && MGLevels.Num() < MGMaxLevels)
	{
		const FIntVector Prev = MGLevels.Last().Dim;
		FMGLevel& Level = MGLevels.AddDefaulted_GetRef();
		Level.Dim = FIntVector((Prev.X + 1) / 2, (Prev.Y + 1) / 2, (Prev.Z + 1) / 2);
		Level.Cells = uint32(Level.Dim.X) * uint32(Level.Dim.Y) * uint32(Level.Dim.Z);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Persistent(Level.Type, TEXT("Slosh.MG.Type"), sizeof(uint32), Level.Cells)), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Persistent(Level.X, TEXT("Slosh.MG.X"), sizeof(float), Level.Cells)), 0u);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Persistent(Level.B, TEXT("Slosh.MG.B"), sizeof(float), Level.Cells)), 0u);
	}
	// Dot products are only reduced on level 0, whose dense dispatch scans the pool.
	NumPartials = FMath::DivideAndRoundUp(PoolScanCells, uint32(ThreadGroupSize));

	// Level 0 of MGPCG lives in the pool; coarse levels (1/8 the cells and less) stay dense.
	FRDGBufferRef PCGBuffers[] =
	{
		Persistent(PCGResidual, TEXT("Slosh.PCG.R"), sizeof(float), PoolCells),
		Persistent(PCGPrecond, TEXT("Slosh.PCG.Z"), sizeof(float), PoolCells),
		Persistent(PCGSearch, TEXT("Slosh.PCG.D"), sizeof(float), PoolCells),
		Persistent(PCGProduct, TEXT("Slosh.PCG.Q"), sizeof(float), PoolCells),
		Persistent(PCGPartials, TEXT("Slosh.PCG.Partials"), sizeof(float), NumPartials),
		Persistent(PCGScalars, TEXT("Slosh.PCG.Scalars"), sizeof(float), 4),
	};
	for (FRDGBufferRef Buffer : PCGBuffers)
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Buffer), 0u);
	}

	TotalEmitted = 0;
	PrevDt = 0.0f;
	LastMaxSpeed = 0.0f;
	bAllocated = true;
}

void FSloshProxy::EnsureParticleCapacity(FRDGBuilder& GraphBuilder, uint32 Needed)
{
	const uint32 Limit = FMath::Max(Layout.ParticleCapacity, MinParticleSlots);
	Needed = FMath::Min(FMath::Max(Needed, MinParticleSlots), Limit);
	if (Needed <= AllocatedParticleCapacity)
	{
		return;
	}
	// Grow by at least 1.5x so steady emission reallocates O(log n) times; the first allocation after a reset is
	// sized exactly to the initial fill.
	uint32 NewCapacity = Needed;
	if (AllocatedParticleCapacity > MinParticleSlots)
	{
		NewCapacity = uint32(FMath::Min<uint64>(FMath::Max<uint64>(Needed, uint64(AllocatedParticleCapacity) * 3 / 2), Limit));
	}
	NewCapacity = FMath::Min(Align(NewCapacity, ParticleSlotAlignment), Limit);

	RDG_EVENT_SCOPE(GraphBuilder, "GrowParticles(%u -> %u)", AllocatedParticleCapacity, NewCapacity);
	FRDGBufferRef Old = ParticleBuffer.IsValid() ? GraphBuilder.RegisterExternalBuffer(ParticleBuffer) : nullptr;
	FRDGBufferRef New = CreatePersistent(GraphBuilder, ParticleBuffer, TEXT("Slosh.Particles"), sizeof(FSloshParticleGPU), NewCapacity);
	if (Old && NumParticles > 0)
	{
		AddCopyBufferPass(GraphBuilder, New, 0, Old, 0, uint64(NumParticles) * sizeof(FSloshParticleGPU));
	}
	// Sort scratch holds nothing between frames.
	CreatePersistent(GraphBuilder, ParticleSortBuffer, TEXT("Slosh.ParticlesSorted"), sizeof(FSloshParticleGPU), NewCapacity);
	CreatePersistent(GraphBuilder, SortRankBuffer, TEXT("Slosh.SortRank"), sizeof(uint32), NewCapacity);
	AllocatedParticleCapacity = NewCapacity;
	ParticleMemoryBytes = uint64(NewCapacity) * (2 * sizeof(FSloshParticleGPU) + sizeof(uint32));
}

void FSloshProxy::Step_RenderThread(FRDGBuilder& GraphBuilder, const FSloshFrameInput& Input)
{
	check(IsInRenderingThread());
	RDG_EVENT_SCOPE_STAT(GraphBuilder, Slosh, "Slosh");

	FSloshSolverSettings S = Input.Settings;
	S.GridDim = FIntVector(FMath::Clamp(S.GridDim.X, 4, 256), FMath::Clamp(S.GridDim.Y, 4, 256), FMath::Clamp(S.GridDim.Z, 4, 256));
	S.ParticleCapacity = FMath::Clamp<uint32>(S.ParticleCapacity, 1024u, Slosh::MaxParticleCapacity);
	S.CellSize = FMath::Max(S.CellSize, 1e-3f);

	if (Input.bReset || !bAllocated || !S.LayoutEquals(Layout))
	{
		Allocate(GraphBuilder, S);
	}
	Layout = S;
	SimToWorld = Input.SimToWorld;
	++SimFrame;

	const float H = S.CellSize;
	const uint32 NumColliders = FMath::Min<uint32>(Input.Colliders.Num(), Slosh::MaxColliders);

	FRDGBufferRef Particles = GraphBuilder.RegisterExternalBuffer(ParticleBuffer);
	FRDGBufferRef Vel[3] =
	{
		GraphBuilder.RegisterExternalBuffer(VelBuffers[0]),
		GraphBuilder.RegisterExternalBuffer(VelBuffers[1]),
		GraphBuilder.RegisterExternalBuffer(VelBuffers[2]),
	};
	FRDGBufferRef VelOld = GraphBuilder.RegisterExternalBuffer(VelOldBuffer);
	FRDGBufferRef Coupling = GraphBuilder.RegisterExternalBuffer(CouplingBuffer);
	FRDGBufferRef Stats = GraphBuilder.RegisterExternalBuffer(StatsBuffer);

	static const FSloshColliderGPU DummyCollider;
	FRDGBufferRef Colliders = NumColliders > 0
		? CreateStructuredBuffer(GraphBuilder, TEXT("Slosh.Colliders"), sizeof(FSloshColliderGPU), NumColliders,
			Input.Colliders.GetData(), NumColliders * sizeof(FSloshColliderGPU))
		: CreateStructuredBuffer(GraphBuilder, TEXT("Slosh.Colliders"), sizeof(FSloshColliderGPU), 1,
			&DummyCollider, sizeof(FSloshColliderGPU));

	FRDGBufferUAVRef VelUAV[3] = { GraphBuilder.CreateUAV(Vel[0]), GraphBuilder.CreateUAV(Vel[1]), GraphBuilder.CreateUAV(Vel[2]) };
	FRDGBufferUAVRef CouplingUAV = GraphBuilder.CreateUAV(Coupling);
	FRDGBufferUAVRef StatsUAV = GraphBuilder.CreateUAV(Stats);

	AddClearUAVPass(GraphBuilder, CouplingUAV, 0u);
	AddClearUAVPass(GraphBuilder, StatsUAV, 0u);

	FSloshPassParameters Base;
	FMemory::Memzero(Base);
	Base.GridDim = S.GridDim;
	Base.CellSize = H;
	Base.InvCellSize = 1.0f / H;
	Base.TotalFaces = TotalFaces;
	Base.PoolCells = PoolCells;
	Base.NumPoolBricks = NumPoolBricks;
	Base.ParticleCapacity = S.ParticleCapacity;
	Base.NumColliders = NumColliders;
	Base.Gravity = S.Gravity;
	Base.FlipRatio = FMath::Clamp(S.FlipRatio, 0.0f, 1.0f);
	Base.bUseAPIC = S.TransferMode == ESloshTransferMode::APIC ? 1u : 0u;
	Base.MaxSpeed = FMath::Max(S.MaxSpeed, 0.1f);
	Base.Friction = FMath::Clamp(S.BoundaryFriction, 0.0f, 1.0f);
	Base.ParticleRadius = S.ParticleRadiusCells * H;
	Base.SORWeight = FMath::Clamp(S.SORWeight, 0.5f, 1.95f);
	// The two-sided position projection replaces the one-sided velocity correction.
	Base.DensityCorrection = S.bTwoSidedDensityProjection ? 0.0f : FMath::Max(S.DensityCorrection, 0.0f);
	Base.TargetParticlesPerCell = 8.0f;
	Base.DensityDeadband = FMath::Max(S.DensityDeadband, 0.0f);
	Base.CouplingScale = Slosh::CouplingFixedScale;
	Base.WarmStartScale = 1.0f;
	Base.bSmoothCoupling = S.bSmoothCouplingForce ? 1u : 0u;
	Base.bGhostFluid = S.bGhostFluid ? 1u : 0u;
	Base.Particles = GraphBuilder.CreateUAV(Particles);
	Base.Vel = VelUAV[0];
	Base.VelSrc = VelUAV[1];
	Base.VelDst = VelUAV[2];
	Base.VelOld = GraphBuilder.CreateUAV(VelOld);
	Base.Accum = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(AccumBuffer));
	Base.FaceFlags = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(FaceFlagsBuffer));
	Base.FaceWeight = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(FaceWeightBuffer));
	Base.CellType = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(CellTypeBuffer));
	Base.CellCount = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(CellCountBuffer));
	Base.Pressure = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(PressureBuffer));
	Base.Divergence = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(DivergenceBuffer));
	Base.CellDiag = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(CellDiagBuffer));
	FRDGBufferRef DensityQ = GraphBuilder.RegisterExternalBuffer(DensityQBuffer);
	FRDGBufferRef DensityRhs = GraphBuilder.RegisterExternalBuffer(DensityRhsBuffer);
	FRDGBufferUAVRef DensityQUAV = GraphBuilder.CreateUAV(DensityQ);
	FRDGBufferUAVRef DensityRhsUAV = GraphBuilder.CreateUAV(DensityRhs);
	Base.DensityQ = DensityQUAV;
	FRDGBufferRef ActiveBricks = GraphBuilder.RegisterExternalBuffer(ActiveBricksBuffer);
	FRDGBufferRef BrickTable = GraphBuilder.RegisterExternalBuffer(BrickTableBuffer);
	FRDGBufferRef SlotBrick = GraphBuilder.RegisterExternalBuffer(SlotBrickBuffer);
	Base.ActiveBricks = GraphBuilder.CreateSRV(ActiveBricks);
	Base.BrickTable = GraphBuilder.CreateSRV(BrickTable);
	Base.SlotBrick = GraphBuilder.CreateSRV(SlotBrick);
	// Allocator kernels swap the pool SRVs for UAVs (a pass must not bind both views of a buffer).
	FRDGBufferUAVRef ActiveBricksUAV = GraphBuilder.CreateUAV(ActiveBricks);
	FRDGBufferUAVRef BrickTableUAV = GraphBuilder.CreateUAV(BrickTable);
	FRDGBufferUAVRef SlotBrickUAV = GraphBuilder.CreateUAV(SlotBrick);
	FRDGBufferUAVRef FreeSlotsUAV = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(FreeSlotsBuffer));
	FRDGBufferUAVRef BrickTouchUAV = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(BrickTouchBuffer));
	FRDGBufferUAVRef BrickWantUAV = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(BrickWantBuffer));
	FRDGBufferUAVRef NewSlotsUAV = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(NewSlotsBuffer));
	auto BindAllocator = [&](FSloshPassParameters& P)
	{
		P.ActiveBricks = nullptr;
		P.BrickTable = nullptr;
		P.SlotBrick = nullptr;
		P.RWActiveBricks = ActiveBricksUAV;
		P.RWBrickTable = BrickTableUAV;
		P.RWSlotBrick = SlotBrickUAV;
		P.FreeSlots = FreeSlotsUAV;
		P.BrickTouch = BrickTouchUAV;
		P.BrickWant = BrickWantUAV;
		P.NewSlots = NewSlotsUAV;
	};
	Base.BrickDim = BrickDim;
	Base.NumBricks = NumBricks;
	// Projecting every N frames removes the same fraction of the density error per unit time with r' = 1 - (1 - r)^N.
	const int32 DensityInterval = FMath::Clamp(S.DensityProjectionInterval, 1, 8);
	Base.DensityRelaxation = 1.0f - FMath::Pow(1.0f - FMath::Clamp(S.DensityRelaxation, 0.0f, 1.0f), float(DensityInterval));
	Base.Coupling = CouplingUAV;
	Base.Stats = StatsUAV;
	Base.Colliders = GraphBuilder.CreateSRV(Colliders);

	// Emission appends after the live particles and stops at capacity. (Recycling slots is incompatible with
	// the spatial sort: slot order is spatial, so overwriting a range would erase a contiguous region.)
	// Particle buffers grow first if this frame's emission needs more slots.
	{
		uint64 Requested = NumParticles;
		for (const FSloshEmitRequest& Emit : Input.Emits)
		{
			Requested += Emit.Shape == ESloshEmitShape::FillCells
				? uint64(FMath::Max(Emit.CellDims.X, 0)) * uint64(FMath::Max(Emit.CellDims.Y, 0)) * uint64(FMath::Max(Emit.CellDims.Z, 0)) * 8ull
				: uint64(Emit.Count);
		}
		EnsureParticleCapacity(GraphBuilder, uint32(FMath::Min<uint64>(Requested, S.ParticleCapacity)));
		Particles = GraphBuilder.RegisterExternalBuffer(ParticleBuffer);
		Base.Particles = GraphBuilder.CreateUAV(Particles);
		Base.ParticleCapacity = AllocatedParticleCapacity;
	}
	{
		RDG_EVENT_SCOPE(GraphBuilder, "Emit");
		for (const FSloshEmitRequest& Emit : Input.Emits)
		{
			uint64 Count = Emit.Count;
			if (Emit.Shape == ESloshEmitShape::FillCells)
			{
				Count = uint64(FMath::Max(Emit.CellDims.X, 0)) * uint64(FMath::Max(Emit.CellDims.Y, 0)) * uint64(FMath::Max(Emit.CellDims.Z, 0)) * 8ull;
			}
			Count = FMath::Min<uint64>(Count, FMath::Min(S.ParticleCapacity, AllocatedParticleCapacity) - FMath::Min(NumParticles, AllocatedParticleCapacity));
			if (Count == 0)
			{
				continue;
			}
			const uint32 Cursor = NumParticles;
			AddSimPass<FSloshEmitParticlesCS>(GraphBuilder, RDG_EVENT_NAME("EmitParticles(%u)", uint32(Count)), Base, uint32(Count),
				[&](FSloshPassParameters& P)
				{
					P.EmitCount = uint32(Count);
					P.EmitCursor = Cursor;
					P.EmitMode = uint32(Emit.Shape);
					P.EmitSeed = EmitSeed++;
					P.EmitCenter = Emit.Center;
					P.EmitExtent = Emit.Extent;
					P.EmitVelocity = Emit.Velocity;
					P.EmitCellMin = Emit.CellMin;
					P.EmitCellDims = Emit.CellDims;
					P.BrickTouch = BrickTouchUAV;
				});
			TotalEmitted += Count;
			NumParticles += uint32(Count);
		}
	}
	Base.NumParticles = NumParticles;

	if (S.SortInterval > 0 && NumParticles > 0 && (SimFrame % uint64(S.SortInterval)) == 0)
	{
		Particles = AddSpatialSort(GraphBuilder, Particles);
		Base.Particles = GraphBuilder.CreateUAV(Particles);
	}

	int32 Substeps = 0;
	FRDGBufferRef LastSparseArgs = nullptr;
	if (!Input.bPaused && NumParticles > 0)
	{
		// CFL-limited substepping from the (lagged) max particle speed readback.
		const float FrameDt = FMath::Clamp(Input.DeltaTime, 1e-4f, FMath::Clamp(S.MaxFrameDt, 1.0f / 240.0f, 1.0f / 10.0f));
		const float SpeedEstimate = FMath::Max(LastMaxSpeed, 1.0f) + S.Gravity.Size() * FrameDt;
		Substeps = FMath::Clamp(FMath::CeilToInt(SpeedEstimate * FrameDt / (FMath::Max(S.CFL, 0.1f) * H)),
			FMath::Max(S.MinSubsteps, 1), FMath::Max(S.MaxSubsteps, 1));
		const float Dt = FrameDt / Substeps;
		Base.Dt = Dt;
		Base.ViscosityAlpha = S.KinematicViscosity * Dt / (H * H);
		Base.PressureToPascal = S.Density * H / Dt;

		for (int32 Step = 0; Step < Substeps; ++Step)
		{
			RDG_EVENT_SCOPE(GraphBuilder, "Substep %d", Step);
			int32 Cur = 0;
			auto BindVel = [&](FSloshPassParameters& P, int32 Main, int32 Src, int32 Dst)
			{
				P.Vel = VelUAV[Main];
				P.VelSrc = VelUAV[Src];
				P.VelDst = VelUAV[Dst];
			};

			Base.WarmStartScale = PrevDt > 0.0f ? Dt / PrevDt : 1.0f;
			PrevDt = Dt;

			// Brick pool: storage for the bricks near particles (Slosh.usf BrickFreeCS .. ClearNewSlotsCS). Grid
			// passes then visit only allocated bricks, through the active list (indirect) or a pool scan.
			// Indirect dispatch costs a few microseconds per pass, which outweighs the savings for many small passes
			// (RBGS) once most of the pool is in use (measured), so the list is used only below SparseMaxActiveFraction.
			FRDGBufferRef PoolArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(3), TEXT("Slosh.PoolArgs"));
			{
				RDG_EVENT_SCOPE(GraphBuilder, "BrickPool");
				FRDGBufferUAVRef PoolArgsUAV = GraphBuilder.CreateUAV(PoolArgs, PF_R32_UINT);
				AddSimPass<FSloshBrickFreeCS>(GraphBuilder, RDG_EVENT_NAME("BrickFree"), Base, NumBricks, BindAllocator);
				AddSimPass<FSloshBrickAllocCS>(GraphBuilder, RDG_EVENT_NAME("BrickAlloc"), Base, NumBricks, BindAllocator);
				AddSimPass<FSloshPoolArgsCS>(GraphBuilder, RDG_EVENT_NAME("PoolArgs"), Base, 1,
					[&](FSloshPassParameters& P) { BindAllocator(P); P.SparseArgsOut = PoolArgsUAV; });
				AddIndirectPass<FSloshClearNewSlotsCS>(GraphBuilder, RDG_EVENT_NAME("ClearNewSlots"), Base, PoolArgs,
					2 * sizeof(FRHIDispatchIndirectParameters), [&](FSloshPassParameters& P) { P.NewSlots = NewSlotsUAV; });
			}
			const bool bUseSparse = S.bSparseBricks && LastPoolUsage < S.SparseMaxActiveFraction;
			FRDGBufferRef SparseArgs = bUseSparse ? PoolArgs : nullptr;
			LastSparseArgs = SparseArgs;

			AddGridPass<FSloshBuildSolidCS>(GraphBuilder, RDG_EVENT_NAME("BuildSolid"), Base, PoolScanCells, SparseArgs, NoCustomization);
			AddSimPass<FSloshP2GCS>(GraphBuilder, RDG_EVENT_NAME("P2G"), Base, NumParticles, NoCustomization);
			AddGridPass<FSloshMarkCellsCS>(GraphBuilder, RDG_EVENT_NAME("MarkCells"), Base, PoolScanCells, SparseArgs, NoCustomization);
			AddGridPass<FSloshNormalizeFacesCS>(GraphBuilder, RDG_EVENT_NAME("NormalizeFaces+Forces"), Base, PoolScanCells, SparseArgs,
				[&](FSloshPassParameters& P) { BindVel(P, 0, 1, 2); });

			if (Base.ViscosityAlpha > 0.0f && S.ViscosityIterations > 0)
			{
				RDG_EVENT_SCOPE(GraphBuilder, "Viscosity");
				// Vel[0] keeps u*, iterate between Vel[1] and Vel[2].
				AddCopyBufferPass(GraphBuilder, Vel[1], Vel[0]);
				int32 Src = 1;
				for (int32 It = 0; It < S.ViscosityIterations; ++It)
				{
					const int32 Dst = Src == 1 ? 2 : 1;
					AddGridPass<FSloshViscosityJacobiCS>(GraphBuilder, RDG_EVENT_NAME("ViscosityJacobi"), Base, PoolScanCells, SparseArgs,
						[&](FSloshPassParameters& P) { BindVel(P, 0, Src, Dst); });
					Src = Dst;
				}
				Cur = Src;
			}

			const int32 OtherA = (Cur + 1) % 3;
			const int32 OtherB = (Cur + 2) % 3;
			AddGridPass<FSloshDivergenceCS>(GraphBuilder, RDG_EVENT_NAME("Divergence"), Base, PoolScanCells, SparseArgs,
				[&](FSloshPassParameters& P) { BindVel(P, Cur, OtherA, OtherB); });

			if (S.PressureSolver == ESloshPressureSolver::MGPCG)
			{
				AddPressureSolveMGPCG(GraphBuilder, S, GraphBuilder.RegisterExternalBuffer(CellTypeBuffer),
					GraphBuilder.RegisterExternalBuffer(PressureBuffer), GraphBuilder.RegisterExternalBuffer(DivergenceBuffer),
					GraphBuilder.RegisterExternalBuffer(CellDiagBuffer), SparseArgs);
			}
			else
			{
				RDG_EVENT_SCOPE(GraphBuilder, "Pressure RBGS x%d", S.PressureIterations);
				for (int32 It = 0; It < S.PressureIterations; ++It)
				{
					for (uint32 Color = 0; Color < 2; ++Color)
					{
						AddGridPass<FSloshPressureRBGSCS>(GraphBuilder, RDG_EVENT_NAME("RBGS"), Base, PoolScanCells, SparseArgs,
							[&](FSloshPassParameters& P) { BindVel(P, Cur, OtherA, OtherB); P.Parity = Color; });
					}
				}
			}

			AddGridPass<FSloshProjectCS>(GraphBuilder, RDG_EVENT_NAME("Project"), Base, PoolScanCells, SparseArgs,
				[&](FSloshPassParameters& P) { BindVel(P, Cur, OtherA, OtherB); });
			if (NumColliders > 0 && S.bSmoothCouplingForce)
			{
				AddGridPass<FSloshCouplingForceCS>(GraphBuilder, RDG_EVENT_NAME("CouplingForce"), Base, PoolScanCells, SparseArgs,
					[&](FSloshPassParameters& P) { BindVel(P, Cur, OtherA, OtherB); });
			}
			if (Step == Substeps - 1)
			{
				AddGridPass<FSloshResidualCS>(GraphBuilder, RDG_EVENT_NAME("Residual"), Base, PoolScanCells, SparseArgs,
					[&](FSloshPassParameters& P) { BindVel(P, Cur, OtherA, OtherB); });
			}

			const bool bDensityProjection = S.bTwoSidedDensityProjection && Step == Substeps - 1 && Base.DensityRelaxation > 0.0f
				&& (SimFrame % uint64(DensityInterval)) == 0;
			if (bDensityProjection)
			{
				RDG_EVENT_SCOPE(GraphBuilder, "DensityProjection");
				// Reuse the pressure solvers: Divergence := rhs, Pressure := q (same ghost-fluid operator).
				AddGridPass<FSloshDensityProjectionRhsCS>(GraphBuilder, RDG_EVENT_NAME("Rhs"), Base, PoolScanCells, SparseArgs,
					[&](FSloshPassParameters& P) { BindVel(P, Cur, OtherA, OtherB); P.Divergence = DensityRhsUAV; P.Pressure = DensityQUAV; P.DensityQ = nullptr; });
				if (S.PressureSolver == ESloshPressureSolver::MGPCG)
				{
					FSloshSolverSettings ProjectionSettings = S;
					ProjectionSettings.PCGIterations = 2;
					AddPressureSolveMGPCG(GraphBuilder, ProjectionSettings, GraphBuilder.RegisterExternalBuffer(CellTypeBuffer), DensityQ, DensityRhs,
						GraphBuilder.RegisterExternalBuffer(CellDiagBuffer), SparseArgs);
				}
				else
				{
					for (int32 It = 0; It < FMath::Max(S.DensityProjectionIterations, 1); ++It)
					{
						for (uint32 Color = 0; Color < 2; ++Color)
						{
							AddGridPass<FSloshPressureRBGSCS>(GraphBuilder, RDG_EVENT_NAME("RBGS"), Base, PoolScanCells, SparseArgs,
								[&](FSloshPassParameters& P) { BindVel(P, Cur, OtherA, OtherB); P.Parity = Color; P.SORWeight = 1.7f;
									P.Divergence = DensityRhsUAV; P.Pressure = DensityQUAV; P.DensityQ = nullptr; });
						}
					}
				}
			}

			{
				RDG_EVENT_SCOPE(GraphBuilder, "Extrapolate");
				for (int32 It = 0; It < S.ExtrapolationLayers; ++It)
				{
					const int32 Dst = (Cur + 1) % 3;
					const int32 Spare = (Cur + 2) % 3;
					AddGridPass<FSloshExtrapolateCS>(GraphBuilder, RDG_EVENT_NAME("Extrapolate"), Base, PoolScanCells, SparseArgs,
						[&](FSloshPassParameters& P) { BindVel(P, Spare, Cur, Dst); P.ExtrapIteration = uint32(It); });
					Cur = Dst;
				}
			}

			if (bDensityProjection)
			{
				// Face displacements into the buffer G2P binds as VelDst (spare once extrapolation is done).
				AddGridPass<FSloshDensityDisplacementCS>(GraphBuilder, RDG_EVENT_NAME("DensityDisplacement"), Base, PoolScanCells, SparseArgs,
					[&](FSloshPassParameters& P) { BindVel(P, Cur, (Cur + 1) % 3, (Cur + 2) % 3); });
			}
			AddSimPass<FSloshG2PAdvectCS>(GraphBuilder, RDG_EVENT_NAME("G2P+Advect"), Base, NumParticles,
				[&](FSloshPassParameters& P) { BindVel(P, Cur, (Cur + 1) % 3, (Cur + 2) % 3); P.bApplyDensityProjection = bDensityProjection ? 1u : 0u;
					P.BrickTouch = BrickTouchUAV; });

			// Keep the post-step field in Vel[0] for the next substep / external consumers.
			if (Cur != 0)
			{
				AddCopyBufferPass(GraphBuilder, Vel[0], Vel[Cur]);
			}
		}

		if (NumColliders > 0)
		{
			AddGridPass<FSloshSubmergedCS>(GraphBuilder, RDG_EVENT_NAME("Submerged"), Base, PoolScanCells, LastSparseArgs, NoCustomization);
		}
	}

	// Asynchronous feedback: copy the small coupling/stat buffers into a free readback slot.
	FReadbackSlot& Slot = Readbacks[NextReadbackSlot];
	if (!Slot.bPending && Substeps > 0)
	{
		AddEnqueueCopyPass(GraphBuilder, Slot.Coupling, Coupling, Slosh::MaxColliders * Slosh::CouplingStride * sizeof(int32));
		AddEnqueueCopyPass(GraphBuilder, Slot.Stats, Stats, NumStats * sizeof(uint32));
		Slot.bPending = true;
		Slot.Frame = SimFrame;
		Slot.Substeps = Substeps;
		Slot.NumColliders = int32(NumColliders);
		NextReadbackSlot = (NextReadbackSlot + 1) % NumReadbackSlots;
	}
}

void FSloshProxy::PollReadbacks_RenderThread()
{
	check(IsInRenderingThread());
	// Slots complete in submission order; process the oldest first.
	for (int32 Offset = 0; Offset < NumReadbackSlots; ++Offset)
	{
		FReadbackSlot& Slot = Readbacks[(NextReadbackSlot + Offset) % NumReadbackSlots];
		if (!Slot.bPending || !Slot.Coupling->IsReady() || !Slot.Stats->IsReady())
		{
			continue;
		}

		FSloshResults NewResults;
		NewResults.SimFrame = Slot.Frame;
		NewResults.Substeps = Slot.Substeps;
		const float InvSubsteps = 1.0f / float(FMath::Max(Slot.Substeps, 1));
		const float InvScale = 1.0f / Slosh::CouplingFixedScale;

		const int32* CouplingData = static_cast<const int32*>(Slot.Coupling->Lock(Slosh::MaxColliders * Slosh::CouplingStride * sizeof(int32)));
		NewResults.Colliders.SetNum(Slot.NumColliders);
		for (int32 Index = 0; Index < Slot.NumColliders; ++Index)
		{
			const int32* C = CouplingData + Index * Slosh::CouplingStride;
			FSloshColliderFeedback& Out = NewResults.Colliders[Index];
			Out.PressureForce = FVector3f(C[0], C[1], C[2]) * (InvScale * InvSubsteps);
			Out.PressureTorque = FVector3f(C[3], C[4], C[5]) * (InvScale * InvSubsteps);
			Out.WettedFaces = FMath::RoundToInt(C[6] * InvSubsteps);
			Out.SubmergedCells = C[7];
			Out.SolidCells = C[11];
			Out.SubmergedCentroidOffset = C[7] > 0 ? FVector3f(C[12], C[13], C[14]) * (InvScale / float(C[7])) : FVector3f::ZeroVector;
			Out.MeanFluidVelocity = C[6] > 0 ? FVector3f(C[8], C[9], C[10]) * (InvScale / float(C[6])) : FVector3f::ZeroVector;
		}
		Slot.Coupling->Unlock();

		const uint32* StatsData = static_cast<const uint32*>(Slot.Stats->Lock(NumStats * sizeof(uint32)));
		NewResults.MaxParticleSpeed = FMath::IsFinite(*reinterpret_cast<const float*>(&StatsData[0])) ? *reinterpret_cast<const float*>(&StatsData[0]) : 0.0f;
		NewResults.FluidCells = FMath::RoundToInt(StatsData[1] * InvSubsteps);
		NewResults.MaxDivergence = *reinterpret_cast<const float*>(&StatsData[2]);
		NewResults.MeanDivergence = StatsData[4] > 0 ? float(double(StatsData[3]) * 1e-3 / double(StatsData[4])) : 0.0f;
		NewResults.MeanInteriorDensity = StatsData[6] > 0 ? float(1.0 + double(int32(StatsData[5])) * 1e-4 / double(StatsData[6])) : 0.0f;
		NewResults.InteriorCells = int32(StatsData[6]);
		NewResults.ActiveBrickFraction = NumBricks > 0 ? float(StatsData[7]) / float(NumBricks) : -1.0f;
		NewResults.PoolUsage = NumPoolBricks > 1 ? float(StatsData[7]) / float(NumPoolBricks - 1) : -1.0f;
		NewResults.PoolOverflowBricks = int32(StatsData[8]);
		NewResults.ClampedParticles = int32(StatsData[9]);
		NewResults.ClampedInSolid = int32(StatsData[13]);
		NewResults.ClampedCentroid = StatsData[9] > 0 ? FVector3f(StatsData[10], StatsData[11], StatsData[12]) * (0.01f / float(StatsData[9])) : FVector3f::ZeroVector;
		NewResults.GridMemoryMB = float(double(GridMemoryBytes) / (1024.0 * 1024.0));
		NewResults.ParticleMemoryMB = float(double(ParticleMemoryBytes) / (1024.0 * 1024.0));
		NewResults.AllocatedParticleCapacity = int32(AllocatedParticleCapacity);
		LastPoolUsage = NewResults.PoolUsage;
		Slot.Stats->Unlock();

		Slot.bPending = false;
		LastMaxSpeed = NewResults.MaxParticleSpeed;

		FScopeLock Lock(&ResultsLock);
		if (NewResults.SimFrame >= Results.SimFrame)
		{
			Results = MoveTemp(NewResults);
		}
	}
}

void FSloshProxy::AddPressureSolveMGPCG(FRDGBuilder& GraphBuilder, const FSloshSolverSettings& Settings,
	FRDGBufferRef CellType, FRDGBufferRef Pressure, FRDGBufferRef Divergence, FRDGBufferRef CellDiag, FRDGBufferRef SparseArgs)
{
	const int32 NumLevels = MGLevels.Num();
	RDG_EVENT_SCOPE(GraphBuilder, "Pressure MGPCG x%d (%d levels)", Settings.PCGIterations, NumLevels);

	FRDGBufferUAVRef CellTypeUAV = GraphBuilder.CreateUAV(CellType);
	FRDGBufferUAVRef PressureUAV = GraphBuilder.CreateUAV(Pressure);
	FRDGBufferUAVRef DivergenceUAV = GraphBuilder.CreateUAV(Divergence);
	FRDGBufferUAVRef R = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(PCGResidual));
	FRDGBufferUAVRef Z = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(PCGPrecond));
	FRDGBufferUAVRef Dir = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(PCGSearch));
	FRDGBufferUAVRef Q = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(PCGProduct));
	FRDGBufferUAVRef Partials = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(PCGPartials));
	FRDGBufferUAVRef Scalars = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(PCGScalars));

	// Per-level views: level 0 uses the simulation's cell types, z as x and r as b.
	TArray<FRDGBufferUAVRef, TInlineAllocator<MGMaxLevels>> LevelType, LevelX, LevelB;
	for (int32 L = 0; L < NumLevels; ++L)
	{
		if (L == 0)
		{
			LevelType.Add(CellTypeUAV);
			LevelX.Add(Z);
			LevelB.Add(R);
		}
		else
		{
			LevelType.Add(GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(MGLevels[L].Type)));
			LevelX.Add(GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(MGLevels[L].X)));
			LevelB.Add(GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(MGLevels[L].B)));
		}
	}

	FSloshMGParameters Common;
	FMemory::Memzero(Common);
	Common.NumPartials = NumPartials;
	Common.CoarseIterations = uint32(FMath::Clamp(Settings.MGCoarseIterations, 1, 200));
	Common.DiagEpsilon = 1e-4f;
	// Every MG kernel references DiagF statically, so it is always bound; only level 0 reads it.
	Common.DiagF = GraphBuilder.CreateSRV(CellDiag);
	Common.ActiveBricks = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ActiveBricksBuffer));
	Common.BrickTable = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(BrickTableBuffer));
	Common.SlotBrick = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(SlotBrickBuffer));
	Common.NumPoolBricks = NumPoolBricks;
	Common.BrickDim = BrickDim;
	const uint32 CoarseArgsOffset = sizeof(FRHIDispatchIndirectParameters);

	auto FineLevel = [&](int32 L)
	{
		FSloshMGParameters P = Common;
		P.FineDim = MGLevels[L].Dim;
		P.FineCells = MGLevels[L].Cells;
		P.TypeF = LevelType[L];
		// Level 0 uses the exact (ghost-fluid) operator; coarse levels keep the counted stencil, which is
		// only a preconditioner approximation.
		P.bGhostDiag = (L == 0) ? 1u : 0u;
		P.bPooledFine = (L == 0) ? 1u : 0u;   // level 0 is the simulation's brick pool
		return P;
	};
	auto TwoLevel = [&](int32 L)
	{
		FSloshMGParameters P = FineLevel(L);
		P.CoarseDim = MGLevels[L + 1].Dim;
		P.CoarseCells = MGLevels[L + 1].Cells;
		P.TypeC = LevelType[L + 1];
		return P;
	};
	auto Smooth = [&](int32 L, uint32 Parity, bool bDot)
	{
		FSloshMGParameters P = FineLevel(L);
		P.XF = LevelX[L];
		P.BF = LevelB[L];
		P.Parity = Parity;
		P.bAccumulateDot = bDot ? 1u : 0u;
		P.Partials = Partials;   // referenced statically by the shader even when the dot is skipped
		if (L == 0)
		{
			AddMGPassL0<FSloshMGSmoothCS>(GraphBuilder, RDG_EVENT_NAME("Smooth L0 %s", Parity == 0 ? TEXT("red") : TEXT("black")), P, Groups(PoolScanCells), SparseArgs, 0);
		}
		else
		{
			AddMGPass<FSloshMGSmoothCS>(GraphBuilder, RDG_EVENT_NAME("Smooth L%d %s", L, Parity == 0 ? TEXT("red") : TEXT("black")), P, Groups(P.FineCells));
		}
	};
	auto FinalizeDot = [&](uint32 Slot, bool bShift)
	{
		FSloshMGParameters P = Common;
		P.Partials = Partials;
		P.Scalars = Scalars;
		P.DotSlot = Slot;
		P.bShiftRho = bShift ? 1u : 0u;
		P.bSparse = SparseArgs ? 1u : 0u;   // only reads the partials a sparse dispatch wrote
		AddMGPass<FSloshPCGFinalizeDotCS>(GraphBuilder, RDG_EVENT_NAME("FinalizeDot[%u]", Slot), P, FIntVector(1, 1, 1));
	};
	// Applies the V-cycle preconditioner to r (level-0 B) into z (level-0 X), and reduces r.z.
	// The level-0 red pre-smoothing sweep is fused into the kernel that produced r.
	auto VCycle = [&]()
	{
		RDG_EVENT_SCOPE(GraphBuilder, "VCycle");
		if (NumLevels == 1)
		{
			FSloshMGParameters P = FineLevel(0);
			P.XF = LevelX[0];
			P.BF = LevelB[0];
			AddMGPass<FSloshMGCoarseSolveCS>(GraphBuilder, RDG_EVENT_NAME("CoarseSolve L0"), P, FIntVector(1, 1, 1));
			Smooth(0, 0, true);
			return;
		}
		Smooth(0, 1, false);
		for (int32 L = 0; L < NumLevels - 1; ++L)
		{
			FSloshMGParameters P = TwoLevel(L);
			P.XF = LevelX[L];
			P.BF = LevelB[L];
			P.XC = LevelX[L + 1];
			P.BC = LevelB[L + 1];
			if (L == 0)
			{
				AddMGPassL0<FSloshMGRestrictCS>(GraphBuilder, RDG_EVENT_NAME("Restrict L0"), P, Groups(P.CoarseCells), SparseArgs, CoarseArgsOffset);
			}
			else
			{
				AddMGPass<FSloshMGRestrictCS>(GraphBuilder, RDG_EVENT_NAME("Restrict L%d", L), P, Groups(P.CoarseCells));
			}
			if (L + 1 < NumLevels - 1)
			{
				Smooth(L + 1, 1, false);
			}
		}
		{
			FSloshMGParameters P = FineLevel(NumLevels - 1);
			P.XF = LevelX[NumLevels - 1];
			P.BF = LevelB[NumLevels - 1];
			AddMGPass<FSloshMGCoarseSolveCS>(GraphBuilder, RDG_EVENT_NAME("CoarseSolve L%d", NumLevels - 1), P, FIntVector(1, 1, 1));
		}
		for (int32 L = NumLevels - 2; L >= 0; --L)
		{
			FSloshMGParameters P = TwoLevel(L);
			P.XF = LevelX[L];
			P.XC = LevelX[L + 1];
			if (L == 0)
			{
				AddMGPassL0<FSloshMGProlongCS>(GraphBuilder, RDG_EVENT_NAME("Prolong L1"), P, Groups(PoolScanCells), SparseArgs, 0);
			}
			else
			{
				AddMGPass<FSloshMGProlongCS>(GraphBuilder, RDG_EVENT_NAME("Prolong L%d", L + 1), P, Groups(P.FineCells));
			}
			Smooth(L, 1, false);
			Smooth(L, 0, L == 0);
		}
	};
	auto UpdateDirection = [&](bool bFirst)
	{
		FSloshMGParameters P = FineLevel(0);
		P.Z = Z;
		P.D = Dir;
		P.Scalars = Scalars;
		P.bFirstIteration = bFirst ? 1u : 0u;
		AddMGPassL0<FSloshPCGUpdateDCS>(GraphBuilder, RDG_EVENT_NAME("UpdateD"), P, Groups(PoolScanCells), SparseArgs, 0);
	};

	// Coarse cell types for this substep.
	for (int32 L = 0; L < NumLevels - 1; ++L)
	{
		AddMGPass<FSloshMGBuildTypeCS>(GraphBuilder, RDG_EVENT_NAME("BuildType L%d", L + 1), TwoLevel(L), Groups(MGLevels[L + 1].Cells));
	}

	// r = b - A p (warm start), z = M^-1 r, rho = r.z, d = z
	{
		FSloshMGParameters P = FineLevel(0);
		P.P = PressureUAV;
		P.R = R;
		P.Z = Z;
		P.D = Dir;
		P.Div = DivergenceUAV;
		AddMGPassL0<FSloshPCGInitCS>(GraphBuilder, RDG_EVENT_NAME("PCGInit"), P, Groups(PoolScanCells), SparseArgs, 0);
	}
	VCycle();
	FinalizeDot(0, true);
	UpdateDirection(true);

	const int32 Iterations = FMath::Clamp(Settings.PCGIterations, 1, 100);
	for (int32 It = 0; It < Iterations; ++It)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "PCG %d", It);
		{
			FSloshMGParameters P = FineLevel(0);
			P.D = Dir;
			P.Q = Q;
			P.Partials = Partials;
			AddMGPassL0<FSloshPCGApplyACS>(GraphBuilder, RDG_EVENT_NAME("ApplyA"), P, Groups(PoolScanCells), SparseArgs, 0);
		}
		FinalizeDot(2, false);
		{
			FSloshMGParameters P = FineLevel(0);
			P.P = PressureUAV;
			P.R = R;
			P.Z = Z;
			P.D = Dir;
			P.Q = Q;
			P.Scalars = Scalars;
			AddMGPassL0<FSloshPCGUpdatePRCS>(GraphBuilder, RDG_EVENT_NAME("UpdatePR"), P, Groups(PoolScanCells), SparseArgs, 0);
		}
		// The last iteration only needs the pressure update.
		if (It + 1 < Iterations)
		{
			VCycle();
			FinalizeDot(0, true);
			UpdateDirection(false);
		}
	}
}

FRDGBufferRef FSloshProxy::AddSpatialSort(FRDGBuilder& GraphBuilder, FRDGBufferRef Particles)
{
	RDG_EVENT_SCOPE(GraphBuilder, "SortParticles(%u)", NumParticles);
	FRDGBufferRef Sorted = GraphBuilder.RegisterExternalBuffer(ParticleSortBuffer);
	FRDGBufferUAVRef Counts = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(SortCountsBuffer));
	AddClearUAVPass(GraphBuilder, Counts, 0u);

	FSloshSortParameters Common;
	FMemory::Memzero(Common);
	Common.GridDim = Layout.GridDim;
	Common.BrickDim = SortBrickDim;
	Common.InvCellSize = 1.0f / Layout.CellSize;
	Common.NumParticles = NumParticles;
	Common.NumSortKeys = NumSortKeys;
	Common.NumScanBlocks = NumScanBlocks;
	FRDGBufferUAVRef ParticlesUAV = GraphBuilder.CreateUAV(Particles);
	FRDGBufferUAVRef Rank = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(SortRankBuffer));
	FRDGBufferUAVRef Offsets = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(SortOffsetsBuffer));
	FRDGBufferUAVRef BlockSums = GraphBuilder.CreateUAV(GraphBuilder.RegisterExternalBuffer(ScanBlockSumsBuffer));
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);

	{
		FSloshSortParameters* P = GraphBuilder.AllocParameters<FSloshSortParameters>();
		*P = Common;
		P->Particles = ParticlesUAV;
		P->SortRank = Rank;
		P->SortCounts = Counts;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Count"), TShaderMapRef<FSloshSortCountCS>(ShaderMap), P, Groups(NumParticles));
	}
	{
		FSloshSortParameters* P = GraphBuilder.AllocParameters<FSloshSortParameters>();
		*P = Common;
		P->SortCounts = Counts;
		P->SortOffsets = Offsets;
		P->ScanBlockSums = BlockSums;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("ScanBlocks"), TShaderMapRef<FSloshSortScanBlocksCS>(ShaderMap), P, FIntVector(int32(NumScanBlocks), 1, 1));
	}
	{
		FSloshSortParameters* P = GraphBuilder.AllocParameters<FSloshSortParameters>();
		*P = Common;
		P->ScanBlockSums = BlockSums;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("ScanBlockSums"), TShaderMapRef<FSloshSortScanBlockSumsCS>(ShaderMap), P, FIntVector(1, 1, 1));
	}
	{
		FSloshSortParameters* P = GraphBuilder.AllocParameters<FSloshSortParameters>();
		*P = Common;
		P->Particles = ParticlesUAV;
		P->ParticlesOut = GraphBuilder.CreateUAV(Sorted);
		P->SortRank = Rank;
		P->SortOffsets = Offsets;
		P->ScanBlockSums = BlockSums;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Scatter"), TShaderMapRef<FSloshSortScatterCS>(ShaderMap), P, Groups(NumParticles));
	}

	// The sorted copy becomes the live particle buffer (renderer reads it through GetParticleBuffer_RenderThread).
	Swap(ParticleBuffer, ParticleSortBuffer);
	return Sorted;
}
