// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MaterialUsageGraphAnalysis.h"
#include "UObject/SoftObjectPath.h"

namespace MaterialUsageAnalyzer
{

class FMaterialUsageAnalysisEngine;

/** Tab 3: analyzes the graph of one loaded material (+ its layers/instance stack) and suggests optimizations. */
class FMaterialUsageGraphTab
{
public:
	void Draw(FMaterialUsageAnalysisEngine& Engine);

private:
	void DrawToolbar(FMaterialUsageAnalysisEngine& Engine);
	void DrawSummary();
	void DrawSuggestions();
	void DrawSwitches();

	FGraphAnalysisResult Result;
	FString StatusMessage;
	FSoftObjectPath AnalyzedPath; // reloaded for the cross-instance variance scan
};

} // namespace MaterialUsageAnalyzer
