// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;

namespace MaterialUsageAnalyzer
{

enum class EGraphSeverity : uint8 { Info, Warning, High };

/** One optimization suggestion emitted by the rule engine. */
struct FGraphSuggestion
{
	EGraphSeverity Severity = EGraphSeverity::Info;
	FString Text;
};

/** A name/value pair for the metric tables. */
struct FGraphMetric
{
	FString Name;
	FString Value;
};

/** Per-static-switch analysis: which path this instance takes and whether it earns its permutation. */
struct FStaticSwitchInfo
{
	FString Name;
	bool bIsParameter = true;   // false = inline UMaterialExpressionStaticSwitch (not per-instance)
	bool bGraphDriven = false;  // inline switch whose Value input is wired (compile-time, not a parameter)
	bool bValue = false;        // resolved value on the analyzed instance -> active branch
	bool bValueOverridden = false; // instance overrides the parent/default value
	int32 TrueBranchCost = 0;   // texture-sample-weighted cost of the True (A) branch
	int32 FalseBranchCost = 0;  // ... of the False (B) branch
	bool bRedundant = false;    // both branches resolve to the same expression -> switch does nothing
	EGraphSeverity Severity = EGraphSeverity::Info;
	FString Verdict;

	// --- Cross-instance variance (populated by the optional instance scan) ---
	bool bVarianceScanned = false;
	int32 InstancesTrue = 0;    // instances resolving this switch to True
	int32 InstancesFalse = 0;   // ... to False
	bool bConstantAcrossInstances = false; // every instance takes the same branch
};

/**
 * Result of analyzing one loaded material (or instance): compiled shader stats,
 * render relevance flags, a structural node census of the effective graph
 * (base material + its functions + the instance's resolved layer stack), and
 * a ranked list of optimization suggestions.
 */
struct FGraphAnalysisResult
{
	bool bValid = false;
	bool bStatsPending = false; // shader map still compiling - stats read as "Compiling..."

	FString AssetName;
	FString AssetPath;
	FString BaseMaterialName;
	bool bIsInstance = false;

	// --- Compiled shader stats (from FMaterialStatsUtils::ExtractMatertialStatsInfo) ---
	int32 SamplerUsage = 0;
	FString SamplersText;
	FString TextureSamplesText;
	FString VTLookupsText;
	FString VTStacksText;
	FString ShaderCountText;
	FString ShaderErrors;
	TArray<FGraphMetric> InstructionCounts; // per representative shader

	// --- Render relevance / material flags ---
	FString BlendMode;
	FString ShadingModels;
	bool bTwoSided = false;
	bool bUsesWPO = false;
	bool bUsesPDO = false;
	bool bUsesDisplacement = false;
	bool bTessellationEnabled = false;
	bool bMasked = false;
	bool bHasPerInstanceCustomData = false;
	bool bHasRVTOutput = false;
	int32 NumCustomizedUVs = 0;

	// --- Structural node census (effective graph for this instance) ---
	int32 TotalExpressions = 0;
	int32 TextureSampleNodes = 0;
	int32 StaticSwitchNodes = 0;
	int32 ScalarParamNodes = 0;
	int32 VectorParamNodes = 0;
	int32 CustomHLSLNodes = 0;
	int32 FunctionCallNodes = 0;
	int32 PannerRotatorNodes = 0;
	int32 WorldAlignedNodes = 0;
	int32 DuplicateTextureSamples = 0;

	// --- Material layer stack (resolved for the analyzed instance) ---
	int32 LayerCount = 0;
	int32 ActiveLayerCount = 0;
	int32 BlendCount = 0;
	TArray<FString> LayerNames;
	TArray<FString> BlendNames;

	// --- Per static switch analysis ---
	TArray<FStaticSwitchInfo> StaticSwitches;
	bool bVarianceScanned = false;
	int32 InstancesScanned = 0;

	TArray<FGraphSuggestion> Suggestions;
	FString StatusMessage;
};

/** Analyzes a loaded material/instance graph. Must run on the game thread, outside the SlateIM draw (loads assets, reads shader maps). */
class FGraphAnalyzer
{
public:
	static void Analyze(UMaterialInterface* Material, FGraphAnalysisResult& Out);

	/**
	 * Loads every material instance in the analyzed material's hierarchy and tallies each static
	 * switch parameter's value across them, so constant ("never varies") switches can be flagged
	 * for deletion. Heavy (loads all instances) - run deferred with a slow task. Updates InOut.
	 */
	static void ScanSwitchVariance(UMaterialInterface* Material, FGraphAnalysisResult& InOut);
};

} // namespace MaterialUsageAnalyzer
