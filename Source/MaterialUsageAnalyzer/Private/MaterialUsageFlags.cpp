// Copyright Epic Games, Inc. All Rights Reserved.

#include "MaterialUsageFlags.h"

namespace MaterialUsageAnalyzer
{

static const FUsageFlagDesc GAllUsageFlags[] =
{
	// Usage                              PropertyName                            Short       Display                       Analyzed  Slow   Detection
	{ MATUSAGE_SkeletalMesh,              "bUsedWithSkeletalMesh",                TEXT("SK"), TEXT("Skeletal Mesh"),           true,  false, EFlagDetection::ReferencerClass },
	{ MATUSAGE_ParticleSprites,           "bUsedWithParticleSprites",             TEXT("PS"), TEXT("Particle Sprites"),        false, true,  EFlagDetection::RequiresAssetLoad },
	{ MATUSAGE_BeamTrails,                "bUsedWithBeamTrails",                  TEXT("BT"), TEXT("Beam Trails"),             false, true,  EFlagDetection::RequiresAssetLoad },
	{ MATUSAGE_MeshParticles,             "bUsedWithMeshParticles",               TEXT("MP"), TEXT("Mesh Particles"),          false, true,  EFlagDetection::RequiresAssetLoad },
	{ MATUSAGE_StaticLighting,            "bUsedWithStaticLighting",              TEXT("SL"), TEXT("Static Lighting"),         false, false, EFlagDetection::ComponentDriven },
	{ MATUSAGE_MorphTargets,              "bUsedWithMorphTargets",                TEXT("MT"), TEXT("Morph Targets"),           true,  false, EFlagDetection::RegistryTagWithFallback },
	{ MATUSAGE_SplineMesh,                "bUsedWithSplineMeshes",                TEXT("SP"), TEXT("Spline Mesh"),             true,  false, EFlagDetection::ComponentDriven },
	{ MATUSAGE_InstancedStaticMeshes,     "bUsedWithInstancedStaticMeshes",       TEXT("IS"), TEXT("Instanced Static Meshes"), true,  false, EFlagDetection::ComponentDriven },
	{ MATUSAGE_GeometryCollections,       "bUsedWithGeometryCollections",         TEXT("GC"), TEXT("Geometry Collections"),    false, false, EFlagDetection::ReferencerClass },
	{ MATUSAGE_Clothing,                  "bUsedWithClothing",                    TEXT("CL"), TEXT("Clothing"),                true,  true,  EFlagDetection::RequiresAssetLoad },
	{ MATUSAGE_NiagaraSprites,            "bUsedWithNiagaraSprites",              TEXT("NS"), TEXT("Niagara Sprites"),         true,  true,  EFlagDetection::RequiresAssetLoad },
	{ MATUSAGE_NiagaraRibbons,            "bUsedWithNiagaraRibbons",              TEXT("NR"), TEXT("Niagara Ribbons"),         true,  true,  EFlagDetection::RequiresAssetLoad },
	{ MATUSAGE_NiagaraMeshParticles,      "bUsedWithNiagaraMeshParticles",        TEXT("NM"), TEXT("Niagara Mesh Particles"),  true,  true,  EFlagDetection::RequiresAssetLoad },
	{ MATUSAGE_GeometryCache,             "bUsedWithGeometryCache",               TEXT("GA"), TEXT("Geometry Cache"),          false, false, EFlagDetection::ReferencerClass },
	{ MATUSAGE_Water,                     "bUsedWithWater",                       TEXT("WA"), TEXT("Water"),                   true,  false, EFlagDetection::ComponentDriven },
	{ MATUSAGE_HairStrands,               "bUsedWithHairStrands",                 TEXT("HA"), TEXT("Hair Strands"),            true,  false, EFlagDetection::ReferencerClass },
	{ MATUSAGE_LidarPointCloud,           "bUsedWithLidarPointCloud",             TEXT("LI"), TEXT("Lidar Point Cloud"),       false, false, EFlagDetection::ReferencerClass },
	{ MATUSAGE_VirtualHeightfieldMesh,    "bUsedWithVirtualHeightfieldMesh",      TEXT("VH"), TEXT("Virtual Heightfield"),     false, false, EFlagDetection::ComponentDriven },
	{ MATUSAGE_Nanite,                    "bUsedWithNanite",                      TEXT("NA"), TEXT("Nanite"),                  true,  false, EFlagDetection::RegistryTagWithFallback },
	{ MATUSAGE_Voxels,                    "bUsedWithVoxels",                      TEXT("VX"), TEXT("Nanite Voxels"),           false, false, EFlagDetection::ComponentDriven },
	{ MATUSAGE_VolumetricCloud,           "bUsedWithVolumetricCloud",             TEXT("VC"), TEXT("Volumetric Cloud"),        false, false, EFlagDetection::ComponentDriven },
	{ MATUSAGE_HeterogeneousVolumes,      "bUsedWithHeterogeneousVolumes",        TEXT("HV"), TEXT("Heterogeneous Volumes"),   false, true,  EFlagDetection::RequiresAssetLoad },
	{ MATUSAGE_StaticMesh,                "bUsedWithStaticMesh",                  TEXT("SM"), TEXT("Static Mesh"),             true,  false, EFlagDetection::ReferencerClass },
};
static_assert(UE_ARRAY_COUNT(GAllUsageFlags) == MATUSAGE_MAX, "Usage flag table out of sync with EMaterialUsage");

TConstArrayView<FUsageFlagDesc> GetAllUsageFlags()
{
	return MakeArrayView(GAllUsageFlags);
}

TConstArrayView<const FUsageFlagDesc*> GetAnalyzedUsageFlags()
{
	// Grid display order: SM SK CL NA NM NR NS MT IS SP WA HA
	static const EMaterialUsage GridOrder[] =
	{
		MATUSAGE_StaticMesh,
		MATUSAGE_SkeletalMesh,
		MATUSAGE_Clothing,
		MATUSAGE_Nanite,
		MATUSAGE_NiagaraMeshParticles,
		MATUSAGE_NiagaraRibbons,
		MATUSAGE_NiagaraSprites,
		MATUSAGE_MorphTargets,
		MATUSAGE_InstancedStaticMeshes,
		MATUSAGE_SplineMesh,
		MATUSAGE_Water,
		MATUSAGE_HairStrands,
	};

	static TArray<const FUsageFlagDesc*> Analyzed = []
	{
		TArray<const FUsageFlagDesc*> Result;
		for (EMaterialUsage Usage : GridOrder)
		{
			const FUsageFlagDesc* Desc = FindDescByUsage(Usage);
			check(Desc && Desc->bAnalyzed);
			Result.Add(Desc);
		}
		return Result;
	}();
	return MakeArrayView(Analyzed);
}

const FUsageFlagDesc* FindDescByUsage(EMaterialUsage Usage)
{
	for (const FUsageFlagDesc& Desc : GAllUsageFlags)
	{
		if (Desc.Usage == Usage)
		{
			return &Desc;
		}
	}
	return nullptr;
}

uint32 GetAnalyzedMask()
{
	static const uint32 Mask = []
	{
		uint32 Result = 0;
		for (const FUsageFlagDesc* Desc : GetAnalyzedUsageFlags())
		{
			Result |= MaskBit(Desc->Usage);
		}
		return Result;
	}();
	return Mask;
}

uint32 GetComponentDrivenMask()
{
	// Needs a level component sighting (or the engine's per-MI auto-record) to prove:
	// which mesh component type used the material, or level-only systems like water.
	return MaskBit(MATUSAGE_StaticMesh)
		| MaskBit(MATUSAGE_SkeletalMesh)
		| MaskBit(MATUSAGE_Nanite)
		| MaskBit(MATUSAGE_InstancedStaticMeshes)
		| MaskBit(MATUSAGE_SplineMesh)
		| MaskBit(MATUSAGE_Water)
		| MaskBit(MATUSAGE_HairStrands);
}

} // namespace MaterialUsageAnalyzer
