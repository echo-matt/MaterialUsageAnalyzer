// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AssetRegistry/AssetData.h"
#include "CoreMinimal.h"
#include "MaterialUsageAnalysisEngine.h"

namespace MaterialUsageAnalyzer
{

/** Tab 1: per-parent material instance hierarchy analysis with per-MI usage override editing. */
class FMaterialUsageHierarchyTab
{
public:
	FMaterialUsageHierarchyTab();

	void Draw(FMaterialUsageAnalysisEngine& Engine);

private:
	void StartAnalysis(FMaterialUsageAnalysisEngine& Engine);
	void DrawToolbar(FMaterialUsageAnalysisEngine& Engine);
	void DrawSettings();
	void DrawParentPanel(FMaterialUsageAnalysisEngine& Engine);
	void DrawFlagEfficiency(FMaterialUsageAnalysisEngine& Engine);
	void DrawParentFlagEditor(FMaterialUsageAnalysisEngine& Engine);
	void DrawFilters(FMaterialUsageAnalysisEngine& Engine);
	void DrawBatchEditRow(FMaterialUsageAnalysisEngine& Engine);
	void DrawTable(FMaterialUsageAnalysisEngine& Engine);
	void DrawLegend();

	void BuildDisplayList(FMaterialUsageAnalysisEngine& Engine);
	bool PassesFilters(const FMIRow& Row) const;
	void CycleCellState(FMIRow& Row, uint32 Bit);
	void SaveAll(FMaterialUsageAnalysisEngine& Engine);
	void RevertAll(FMaterialUsageAnalysisEngine& Engine);
	int32 CountDirtyRows(const FMaterialUsageAnalysisEngine& Engine) const;

	// Selection
	FAssetData ParentMaterial;
	FString StatusMessage;

	// Settings (feeds FAnalysisSettings)
	bool bExcludeNonCookedReferencers = true;
	bool bResolveSlowFlags = true;
	bool bDeepScanLevels = false;
	FString ExcludeDirsText;

	// Filters
	struct FFlagFilter
	{
		int32 AnalyzedFlagIndex = 0;
		int32 Condition = 0; // matches GetFilterConditionNames() order
	};
	TArray<FFlagFilter> Filters;
	int32 NewFilterFlagIndex = 0;
	int32 NewFilterCondition = 0;
	FString NameFilter;
	bool bShowOtherReferencers = false;
	bool bShowLegend = false;
	bool bEditParentFlags = false;

	// Batch edit combo state per analyzed flag: 0 = leave, 1 = override ON, 2 = override OFF
	TArray<int32> BatchEditStates;

	// Cached combo item lists
	TArray<FString> FlagComboItems;
	TArray<FString> ConditionComboItems;
	TArray<FString> BatchEditComboItems;

	// Display
	TArray<int32> DisplayIndices;
	bool bDisplayDirty = true;
	int32 LastRowCount = 0;
};

} // namespace MaterialUsageAnalyzer
