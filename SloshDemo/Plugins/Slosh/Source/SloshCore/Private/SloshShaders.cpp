// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshShaders.h"

#define SLOSH_SIM_USF "/Plugin/Slosh/Private/Slosh.usf"

IMPLEMENT_GLOBAL_SHADER(FSloshEmitParticlesCS, SLOSH_SIM_USF, "EmitParticlesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshBuildSolidCS, SLOSH_SIM_USF, "BuildSolidCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshP2GCS, SLOSH_SIM_USF, "P2GCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshMarkCellsCS, SLOSH_SIM_USF, "MarkCellsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshNormalizeFacesCS, SLOSH_SIM_USF, "NormalizeFacesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshViscosityJacobiCS, SLOSH_SIM_USF, "ViscosityJacobiCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshDivergenceCS, SLOSH_SIM_USF, "DivergenceCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshPressureRBGSCS, SLOSH_SIM_USF, "PressureRBGSCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshProjectCS, SLOSH_SIM_USF, "ProjectCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshExtrapolateCS, SLOSH_SIM_USF, "ExtrapolateCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshResidualCS, SLOSH_SIM_USF, "ResidualCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshCouplingForceCS, SLOSH_SIM_USF, "CouplingForceCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshDensityProjectionRhsCS, SLOSH_SIM_USF, "DensityProjectionRhsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshBrickFreeCS, SLOSH_SIM_USF, "BrickFreeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshBrickAllocCS, SLOSH_SIM_USF, "BrickAllocCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshPoolArgsCS, SLOSH_SIM_USF, "PoolArgsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshClearNewSlotsCS, SLOSH_SIM_USF, "ClearNewSlotsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshSubmergedCS, SLOSH_SIM_USF, "SubmergedCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshG2PAdvectCS, SLOSH_SIM_USF, "G2PAdvectCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshDensityDisplacementCS, SLOSH_SIM_USF, "DensityDisplacementCS", SF_Compute);

#define SLOSH_MG_USF "/Plugin/Slosh/Private/SloshPressureMG.usf"

IMPLEMENT_GLOBAL_SHADER(FSloshMGBuildTypeCS, SLOSH_MG_USF, "MGBuildTypeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshPCGInitCS, SLOSH_MG_USF, "PCGInitCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshMGSmoothCS, SLOSH_MG_USF, "MGSmoothCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshMGRestrictCS, SLOSH_MG_USF, "MGRestrictCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshMGCoarseSolveCS, SLOSH_MG_USF, "MGCoarseSolveCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshMGProlongCS, SLOSH_MG_USF, "MGProlongCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshPCGApplyACS, SLOSH_MG_USF, "PCGApplyACS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshPCGFinalizeDotCS, SLOSH_MG_USF, "PCGFinalizeDotCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshPCGUpdatePRCS, SLOSH_MG_USF, "PCGUpdatePRCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshPCGUpdateDCS, SLOSH_MG_USF, "PCGUpdateDCS", SF_Compute);

#define SLOSH_SORT_USF "/Plugin/Slosh/Private/SloshSort.usf"

IMPLEMENT_GLOBAL_SHADER(FSloshSortCountCS, SLOSH_SORT_USF, "SortCountCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshSortScanBlocksCS, SLOSH_SORT_USF, "SortScanBlocksCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshSortScanBlockSumsCS, SLOSH_SORT_USF, "SortScanBlockSumsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSloshSortScatterCS, SLOSH_SORT_USF, "SortScatterCS", SF_Compute);
