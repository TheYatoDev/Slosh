// Copyright notice: see Plugins/Slosh/NOTICE.md.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

// One parameter layout shared by every solver kernel. All grid/particle state is bound as UAVs so
// any kernel can be dispatched with the same struct; RDG inserts the UAV barriers between passes.
BEGIN_SHADER_PARAMETER_STRUCT(FSloshPassParameters, )
	SHADER_PARAMETER(FIntVector, GridDim)
	SHADER_PARAMETER(float, CellSize)
	SHADER_PARAMETER(float, InvCellSize)
	SHADER_PARAMETER(uint32, TotalFaces)
	SHADER_PARAMETER(uint32, PoolCells)
	SHADER_PARAMETER(uint32, NumPoolBricks)
	SHADER_PARAMETER(uint32, NumParticles)
	SHADER_PARAMETER(uint32, ParticleCapacity)
	SHADER_PARAMETER(uint32, NumColliders)
	SHADER_PARAMETER(float, Dt)
	SHADER_PARAMETER(FVector3f, Gravity)
	SHADER_PARAMETER(float, FlipRatio)
	SHADER_PARAMETER(uint32, bUseAPIC)
	SHADER_PARAMETER(float, MaxSpeed)
	SHADER_PARAMETER(float, Friction)
	SHADER_PARAMETER(float, ParticleRadius)
	SHADER_PARAMETER(float, ViscosityAlpha)
	SHADER_PARAMETER(float, SORWeight)
	SHADER_PARAMETER(uint32, Parity)
	SHADER_PARAMETER(float, WarmStartScale)
	SHADER_PARAMETER(float, PressureToPascal)
	SHADER_PARAMETER(float, DensityCorrection)
	SHADER_PARAMETER(float, TargetParticlesPerCell)
	SHADER_PARAMETER(float, DensityDeadband)
	SHADER_PARAMETER(float, CouplingScale)
	SHADER_PARAMETER(uint32, ExtrapIteration)
	SHADER_PARAMETER(uint32, bSmoothCoupling)
	SHADER_PARAMETER(uint32, bGhostFluid)
	SHADER_PARAMETER(uint32, bApplyDensityProjection)
	SHADER_PARAMETER(uint32, bSparse)
	SHADER_PARAMETER(FIntVector, BrickDim)
	SHADER_PARAMETER(uint32, NumBricks)
	SHADER_PARAMETER(float, DensityRelaxation)

	SHADER_PARAMETER(uint32, EmitCount)
	SHADER_PARAMETER(uint32, EmitCursor)
	SHADER_PARAMETER(uint32, EmitMode)
	SHADER_PARAMETER(uint32, EmitSeed)
	SHADER_PARAMETER(FVector3f, EmitCenter)
	SHADER_PARAMETER(FVector3f, EmitExtent)
	SHADER_PARAMETER(FVector3f, EmitVelocity)
	SHADER_PARAMETER(FIntVector, EmitCellMin)
	SHADER_PARAMETER(FIntVector, EmitCellDims)

	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSloshParticlePacked>, Particles)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, Vel)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, VelOld)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, VelSrc)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, VelDst)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, Accum)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, FaceFlags)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, FaceWeight)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CellType)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, CellCount)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, Pressure)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, Divergence)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, CellDiag)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, DensityQ)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, Coupling)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, Stats)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSloshCollider>, Colliders)
	// Brick pool: SRVs for the solver kernels, UAVs only for the allocator kernels (never both in one pass).
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BrickTable)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SlotBrick)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ActiveBricks)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWBrickTable)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWSlotBrick)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWActiveBricks)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, FreeSlots)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BrickTouch)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, BrickWant)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, NewSlots)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, SparseArgsOut)
	RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
END_SHADER_PARAMETER_STRUCT()

class FSloshShaderBase : public FGlobalShader
{
public:
	FSloshShaderBase() = default;
	FSloshShaderBase(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

#define SLOSH_DECLARE_SIM_SHADER(ShaderClass) \
	class ShaderClass : public FSloshShaderBase \
	{ \
	public: \
		DECLARE_GLOBAL_SHADER(ShaderClass); \
		SHADER_USE_PARAMETER_STRUCT(ShaderClass, FSloshShaderBase); \
		using FParameters = FSloshPassParameters; \
	};

SLOSH_DECLARE_SIM_SHADER(FSloshEmitParticlesCS)
SLOSH_DECLARE_SIM_SHADER(FSloshBuildSolidCS)
SLOSH_DECLARE_SIM_SHADER(FSloshP2GCS)
SLOSH_DECLARE_SIM_SHADER(FSloshMarkCellsCS)
SLOSH_DECLARE_SIM_SHADER(FSloshNormalizeFacesCS)
SLOSH_DECLARE_SIM_SHADER(FSloshViscosityJacobiCS)
SLOSH_DECLARE_SIM_SHADER(FSloshDivergenceCS)
SLOSH_DECLARE_SIM_SHADER(FSloshPressureRBGSCS)
SLOSH_DECLARE_SIM_SHADER(FSloshProjectCS)
SLOSH_DECLARE_SIM_SHADER(FSloshExtrapolateCS)
SLOSH_DECLARE_SIM_SHADER(FSloshResidualCS)
SLOSH_DECLARE_SIM_SHADER(FSloshCouplingForceCS)
SLOSH_DECLARE_SIM_SHADER(FSloshDensityProjectionRhsCS)
SLOSH_DECLARE_SIM_SHADER(FSloshBrickFreeCS)
SLOSH_DECLARE_SIM_SHADER(FSloshBrickAllocCS)
SLOSH_DECLARE_SIM_SHADER(FSloshPoolArgsCS)
SLOSH_DECLARE_SIM_SHADER(FSloshClearNewSlotsCS)
SLOSH_DECLARE_SIM_SHADER(FSloshSubmergedCS)
SLOSH_DECLARE_SIM_SHADER(FSloshG2PAdvectCS)
SLOSH_DECLARE_SIM_SHADER(FSloshDensityDisplacementCS)

// ---------------------------------------------------------------------------------------------
// MGPCG pressure solver (SloshPressureMG.usf). Every kernel binds only the roles it uses.
// ---------------------------------------------------------------------------------------------
BEGIN_SHADER_PARAMETER_STRUCT(FSloshMGParameters, )
	SHADER_PARAMETER(FIntVector, FineDim)
	SHADER_PARAMETER(FIntVector, CoarseDim)
	SHADER_PARAMETER(uint32, FineCells)
	SHADER_PARAMETER(uint32, CoarseCells)
	SHADER_PARAMETER(uint32, Parity)
	SHADER_PARAMETER(uint32, bAccumulateDot)
	SHADER_PARAMETER(uint32, bFirstIteration)
	SHADER_PARAMETER(uint32, DotSlot)
	SHADER_PARAMETER(uint32, bShiftRho)
	SHADER_PARAMETER(uint32, NumPartials)
	SHADER_PARAMETER(uint32, CoarseIterations)
	SHADER_PARAMETER(float, DiagEpsilon)
	SHADER_PARAMETER(uint32, bGhostDiag)
	SHADER_PARAMETER(uint32, bSparse)
	SHADER_PARAMETER(uint32, bPooledFine)
	SHADER_PARAMETER(uint32, NumPoolBricks)
	SHADER_PARAMETER(FIntVector, BrickDim)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, TypeF)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, TypeC)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, XF)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, BF)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, XC)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, BC)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, P)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, R)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, Z)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, D)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, Q)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, Div)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, Partials)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, Scalars)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, DiagF)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ActiveBricks)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BrickTable)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SlotBrick)
	RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
END_SHADER_PARAMETER_STRUCT()

#define SLOSH_DECLARE_MG_SHADER(ShaderClass) 	class ShaderClass : public FSloshShaderBase 	{ 	public: 		DECLARE_GLOBAL_SHADER(ShaderClass); 		SHADER_USE_PARAMETER_STRUCT(ShaderClass, FSloshShaderBase); 		using FParameters = FSloshMGParameters; 	};

SLOSH_DECLARE_MG_SHADER(FSloshMGBuildTypeCS)
SLOSH_DECLARE_MG_SHADER(FSloshPCGInitCS)
SLOSH_DECLARE_MG_SHADER(FSloshMGSmoothCS)
SLOSH_DECLARE_MG_SHADER(FSloshMGRestrictCS)
SLOSH_DECLARE_MG_SHADER(FSloshMGCoarseSolveCS)
SLOSH_DECLARE_MG_SHADER(FSloshMGProlongCS)
SLOSH_DECLARE_MG_SHADER(FSloshPCGApplyACS)
SLOSH_DECLARE_MG_SHADER(FSloshPCGFinalizeDotCS)
SLOSH_DECLARE_MG_SHADER(FSloshPCGUpdatePRCS)
SLOSH_DECLARE_MG_SHADER(FSloshPCGUpdateDCS)

// ---------------------------------------------------------------------------------------------
// Spatial particle sort (SloshSort.usf)
// ---------------------------------------------------------------------------------------------
BEGIN_SHADER_PARAMETER_STRUCT(FSloshSortParameters, )
	SHADER_PARAMETER(FIntVector, GridDim)
	SHADER_PARAMETER(FIntVector, BrickDim)
	SHADER_PARAMETER(float, InvCellSize)
	SHADER_PARAMETER(uint32, NumParticles)
	SHADER_PARAMETER(uint32, NumSortKeys)
	SHADER_PARAMETER(uint32, NumScanBlocks)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSloshParticlePacked>, Particles)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FSloshParticlePacked>, ParticlesOut)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SortRank)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SortCounts)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SortOffsets)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ScanBlockSums)
END_SHADER_PARAMETER_STRUCT()

#define SLOSH_DECLARE_SORT_SHADER(ShaderClass) \
	class ShaderClass : public FSloshShaderBase \
	{ \
	public: \
		DECLARE_GLOBAL_SHADER(ShaderClass); \
		SHADER_USE_PARAMETER_STRUCT(ShaderClass, FSloshShaderBase); \
		using FParameters = FSloshSortParameters; \
	};

SLOSH_DECLARE_SORT_SHADER(FSloshSortCountCS)
SLOSH_DECLARE_SORT_SHADER(FSloshSortScanBlocksCS)
SLOSH_DECLARE_SORT_SHADER(FSloshSortScanBlockSumsCS)
SLOSH_DECLARE_SORT_SHADER(FSloshSortScatterCS)
