// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MaterialUsageAnalysisEngine.h"

class IAssetRegistry;

namespace MaterialUsageAnalyzer
{

/**
 * Registry-only classification of one MI's referencers into needed usage flags.
 * Expensive verdicts (clothing, Niagara renderer types, missing Nanite tags) are
 * queued as slow items instead of loading inline.
 */
class FReferencerClassifier
{
public:
	struct FOutput
	{
		uint32 NeededMask = 0;
		uint32 UnknownMask = 0;
		int32 NumLevelReferencers = 0;
		TArray<FReferencerRecord> OtherReferencers;
		/** Slow assets this row depends on (kind + path). */
		TArray<TPair<ESlowItemKind, FSoftObjectPath>> SlowDependencies;
		/** Level packages to deep-scan when enabled. */
		TArray<FName> LevelPackages;
	};

	static void ClassifyReferencers(IAssetRegistry& AssetRegistry, const FMIRow& Row, const FAnalysisSettings& Settings,
		const TMap<FSoftObjectPath, FMeshVerdict>& MeshVerdicts,
		const TMap<FSoftObjectPath, FNiagaraVerdict>& NiagaraVerdicts,
		TMap<FName, EStaticMeshPlacement>& StaticMeshPlacementCache,
		FOutput& Out);

	/** Applies a resolved mesh verdict to a needed mask (morph/clothing/nanite bits for a skeletal or static mesh referencer). */
	static uint32 UsageMaskFromMeshVerdict(const FTopLevelAssetPath& MeshClass, const FMeshVerdict& Verdict);
};

} // namespace MaterialUsageAnalyzer
