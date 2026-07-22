// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AssetRegistry/AssetData.h"
#include "CoreMinimal.h"
#include "MaterialUsageAnalysisEngine.h"

namespace MaterialUsageAnalyzer
{

/** Tab 2: bulk analysis of many parent materials, worst-offender sorted, CSV export. */
class FMaterialUsageBatchTab
{
public:
	FMaterialUsageBatchTab();

	void Draw(FMaterialUsageAnalysisEngine& Engine);

private:
	void DrawInputToolbar(FMaterialUsageAnalysisEngine& Engine);
	void DrawFlagsToCheck();
	void DrawRunRow(FMaterialUsageAnalysisEngine& Engine);
	void DrawTable(FMaterialUsageAnalysisEngine& Engine);
	void StartAnalysis(FMaterialUsageAnalysisEngine& Engine);
	FString BuildCsv(const FMaterialUsageAnalysisEngine& Engine) const;
	void AddMaterials(const TArray<FAssetData>& NewMaterials);

	// Input list
	TArray<FAssetData> Materials;
	TSet<FSoftObjectPath> SelectedForRemoval;
	bool bRecursiveDirectoryScan = true;

	// Settings
	bool bExcludeNonCookedReferencers = true;
	FString ExcludeDirsText;
	TArray<bool> FlagChecked; // parallel to GetAnalyzedUsageFlags()

	FString StatusMessage;
};

} // namespace MaterialUsageAnalyzer
