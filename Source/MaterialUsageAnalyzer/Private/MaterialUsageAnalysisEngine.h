// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AssetRegistry/AssetData.h"
#include "CoreMinimal.h"
#include "MaterialUsageFlags.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

namespace MaterialUsageAnalyzer
{

struct FAnalysisSettings
{
	/** Only consider referencers with the Game dependency property (skips editor-only referencers). */
	bool bExcludeNonCookedReferencers = true;
	/** Referencers whose package path starts with any of these prefixes are ignored. */
	TArray<FString> ExcludeDirPrefixes;
	/** Load referencing .umap / external-actor packages and walk components for exact needs. */
	bool bDeepScanLevels = false;
	/** Which analyzed flags to resolve. Slow flags outside this mask stay Unknown instead of triggering loads. */
	uint32 FlagsToCheckMask = 0;
};

struct FReferencerRecord
{
	FName PackageName;
	FTopLevelAssetPath AssetClass;
};

/** One material instance row of the hierarchy analysis. */
struct FMIRow
{
	FAssetData AssetData;
	FSoftObjectPath Path;
	FName PackageName;
	FSoftObjectPath DirectParentPath;
	int32 Depth = 0;

	bool bHasStaticPermutation = false;
	/** Effective usage flags (parent ± overrides), from the custom registry tag or load fallback. */
	uint32 EffectiveMask = 0;
	/** Explicit per-MI override mask (bOverride_UsageFlags), from the custom registry tag. */
	uint32 OverrideMask = 0;
	/** True when the usage registry tags were present (asset saved since the backport). */
	bool bFlagsFromRegistry = false;

	/** Needs proven by referencer inference (no loads or via slow resolvers). */
	uint32 InferredNeededMask = 0;
	/** Flags whose state can't be decided (level referencers without deep scan, slow flags not checked). */
	uint32 UnknownMask = 0;
	int32 NumLevelReferencers = 0;
	TArray<FName> LevelPackages;
	TArray<FReferencerRecord> OtherReferencers;
	bool bClassified = false;

	// Pending grid edits (applied on Save All via SetMaterialUsageOverride)
	uint32 PendingOverrideSetMask = 0;
	uint32 PendingOverrideValueMask = 0;
	uint32 PendingClearOverrideMask = 0;

	bool IsDirty() const { return PendingOverrideSetMask != 0 || PendingClearOverrideMask != 0; }
	void ClearPending() { PendingOverrideSetMask = PendingOverrideValueMask = PendingClearOverrideMask = 0; }

	/** Overrides that are ON: engine auto-recorded (or user-set) proof of need. */
	uint32 RecordedMask() const { return OverrideMask & EffectiveMask; }
	uint32 NeededMask() const { return InferredNeededMask | RecordedMask(); }
	uint32 WastefulMask() const { return EffectiveMask & ~NeededMask() & ~UnknownMask & GetAnalyzedMask(); }
	uint32 MissingMask() const { return NeededMask() & ~EffectiveMask & GetAnalyzedMask(); }

	/** Pending-edit-aware view of a single flag: 0=inherit, 1=override on, 2=override off. */
	int32 GetPendingState(uint32 Bit) const
	{
		if (PendingClearOverrideMask & Bit)
		{
			return 0;
		}
		if (PendingOverrideSetMask & Bit)
		{
			return (PendingOverrideValueMask & Bit) ? 1 : 2;
		}
		if (OverrideMask & Bit)
		{
			return (EffectiveMask & Bit) ? 1 : 2;
		}
		return 0;
	}
};

struct FParentSummary
{
	FSoftObjectPath Path;
	FString Name;
	uint32 EnabledMask = 0;
	uint32 PendingEnabledMask = 0;
	bool bFlagsKnown = false;
	int32 NumInstances = 0;
	int32 NumStaticPermutations = 0;
	int32 NumWastefulFlagInstances = 0;
	int32 NumMissingFlagInstances = 0;
	int32 NumUnknownMIs = 0;
	int32 NeededCount[MATUSAGE_MAX] = {};
};

enum class EBatchStatus : uint8 { Pending, Analyzing, Complete, Failed };

struct FBatchRow
{
	FSoftObjectPath MaterialPath;
	FString Name;
	EBatchStatus Status = EBatchStatus::Pending;
	uint32 EnabledMask = 0;
	bool bFlagsKnown = false;
	int32 NumMIs = 0;
	int32 NumStaticPermutations = 0;
	int32 TotalUnnecessary = 0;
	int32 UnnecessaryPerFlag[MATUSAGE_MAX] = {};
	int32 MissingPerFlag[MATUSAGE_MAX] = {};
};

enum class EAnalysisPhase : uint8
{
	Idle,
	EnumerateHierarchy,
	ClassifyReferencers,
	ResolveSlow,
	DeepScanLevels,
	Finalize,
	Ready,
	Cancelled,
};

enum class ESessionKind : uint8 { None, Hierarchy, Batch };

/**
 * Second-hop verdict for how a static mesh is placed, cached per static-mesh package.
 * A material on a static mesh can't tell from its direct referencers whether the mesh is
 * instanced (Packed Level Actor / foliage / ISM Blueprint) - that lives one hop further out.
 */
enum class EStaticMeshPlacement : uint8
{
	Unresolved = 0,
	PlainOrLevel = 1,   // placed as a plain static mesh / directly in a level - ISM stays optimizable
	InstancedKnown = 2, // Packed Level Actor / foliage / grass - ISM is genuinely needed
	InstancedMaybe = 3, // a generic Blueprint references it - ISM unknowable, never call it wasteful
};

// Shared load-verdict caches (persist across runs)
struct FMeshVerdict
{
	bool bNaniteKnown = false;
	bool bNanite = false;
	int32 MorphTargetCount = INDEX_NONE;
	bool bClothingKnown = false;
	bool bHasClothing = false;
};

struct FNiagaraVerdict
{
	/** Per referenced material object path: NS/NR/NM (+HeterogeneousVolumes) usage mask. */
	TMap<FSoftObjectPath, uint32> MaterialUsage;
};

struct FLevelScanResult
{
	/** Per material object path: component-driven usage mask observed in the level. */
	TMap<FSoftObjectPath, uint32> MaterialUsage;
	bool bScanned = false;
};

enum class ESlowItemKind : uint8 { SkeletalMesh, StaticMeshNanite, NiagaraSystem, MaterialFlags, ParticleSystem };

struct FSlowItem
{
	ESlowItemKind Kind;
	FSoftObjectPath AssetPath;
	/** Rows whose verdict depends on this asset (indices into Rows). */
	TArray<int32> DependentRows;
};

/**
 * Incremental analysis engine shared by both tabs. One session at a time,
 * pumped from the SlateIM draw tick with a per-frame time budget.
 */
class FMaterialUsageAnalysisEngine
{
public:
	void StartHierarchySession(const FAssetData& InParentMaterial, const FAnalysisSettings& InSettings);
	void StartBatchSession(const TArray<FAssetData>& InMaterials, const FAnalysisSettings& InSettings);
	void Cancel();

	/**
	 * Process queued work until the budget elapses.
	 * Must be called OUTSIDE any SlateIM root (e.g. from a core ticker): asset loads can pop
	 * modal slow-task dialogs which tick Slate re-entrantly.
	 */
	void Pump(double BudgetSeconds);

	/**
	 * Queue an action (saves, modal prompts, registry scans) to run outside the SlateIM draw.
	 * Executed by the window ticker before the next Pump.
	 */
	void Defer(TFunction<void()> Action) { DeferredActions.Add(MoveTemp(Action)); }
	void RunDeferredActions();

	bool IsBusy() const { return Phase != EAnalysisPhase::Idle && Phase != EAnalysisPhase::Ready && Phase != EAnalysisPhase::Cancelled; }
	bool HasResults() const { return Phase == EAnalysisPhase::Ready || Phase == EAnalysisPhase::Cancelled; }
	EAnalysisPhase GetPhase() const { return Phase; }
	ESessionKind GetSessionKind() const { return SessionKind; }
	float GetProgress() const;
	FString GetPhaseLabel() const;

	/** Recompute the hierarchy aggregate counters from Rows (after edits/saves). */
	void RebuildParentSummary();

	/** Drop the cached MIC children index (call after saves that reparent or on explicit refresh). */
	void InvalidateChildrenIndex() { bChildrenIndexBuilt = false; }

	// Hierarchy session results
	FParentSummary ParentSummary;
	TArray<FMIRow> Rows;

	// Batch session results
	TArray<FBatchRow> BatchRows;

	FAnalysisSettings Settings;

private:
	struct FDescendant
	{
		FAssetData AssetData;
		int32 Depth = 0;
		FSoftObjectPath ParentPath;
	};

	void EnsureChildrenIndex();
	void CollectDescendants(const FSoftObjectPath& RootMaterial, TArray<FDescendant>& OutInstances) const;
	void InitRowFromAssetData(FMIRow& Row, const FDescendant& Descendant) const;
	void BuildRowsForRoot(const FSoftObjectPath& RootMaterial);
	/** Untagged MIs were saved before the backport, so they carry no overrides: effective = direct parent's effective. */
	void FillUntaggedEffectiveFlags(uint32 RootEnabledMask);
	bool StepEnumerate();
	bool StepClassify();
	bool StepResolveSlow();
	bool SlowQueueDrained() const;
	bool IsSlowItemCached(const FSlowItem& Item) const;
	void IssueSlowLoadRequests();
	/** @return true when an item was processed, false while waiting on the async loader. */
	bool StepResolveSlowWork();
	void ProcessSlowItem(const FSlowItem& Item);
	bool StepBatch();
	bool StepDeepScan();
	bool ShouldSkipLevelPackage(FName LevelPackageName) const;
	void IssueLevelLoadRequests();
	void ProcessScannedLevelPackage(FName LevelPackageName, UPackage* Package);
	void ApplyLevelResultToRows(FName LevelPackageName);
	void FinalizeSession();
	void QueueSlowItem(ESlowItemKind Kind, const FSoftObjectPath& AssetPath, int32 RowIndex);
	void ApplySlowVerdictToRow(const FSlowItem& Item);
	void MaybeCollectGarbage(bool bForce = false);

	EAnalysisPhase Phase = EAnalysisPhase::Idle;
	ESessionKind SessionKind = ESessionKind::None;
	bool bCancelRequested = false;
	bool bPumping = false;
	TArray<TFunction<void()>> DeferredActions;

	// Children index: direct parent object path -> MIC asset data
	TMultiMap<FSoftObjectPath, FAssetData> ChildrenIndex;
	bool bChildrenIndexBuilt = false;

	// Hierarchy session cursors
	int32 ClassifyCursor = 0;

	// Batch session state
	TArray<FAssetData> BatchMaterials;
	int32 BatchCursor = 0;
	int32 BatchSubPhase = 0; // 0 = enumerate, 1 = classify rows, 2 = resolve slow + rollup

	// Slow queue. Asset packages load through LoadPackageAsync with a bounded in-flight
	// window; verdict extraction happens on the game thread once a package is resident.
	TArray<FSlowItem> SlowQueue;
	int32 SlowCursor = 0;
	TMap<FSoftObjectPath, int32> SlowQueueIndexByPath;

	struct FCompletedSlowLoad
	{
		int32 SlowItemIndex = INDEX_NONE;
		TStrongObjectPtr<UPackage> Package;
	};
	TArray<FCompletedSlowLoad> CompletedSlowLoads;
	int32 NextSlowRequestIndex = 0;
	int32 SlowLoadsInFlight = 0;
	static constexpr int32 MaxSlowLoadsInFlight = 16;

	// Deep scan queue. Level packages load through LoadPackageAsync with a bounded in-flight
	// window so serialization overlaps across the async loading workers; component walks stay
	// on the game thread.
	TArray<FName> LevelScanQueue;
	int32 LevelScanCursor = 0;
	TMap<FName, TArray<int32>> LevelDependentRows;

	struct FCompletedLevelLoad
	{
		FName PackageName;
		TStrongObjectPtr<UPackage> Package;
	};
	TArray<FCompletedLevelLoad> CompletedLevelLoads;
	int32 NextLevelRequestIndex = 0;
	int32 LevelLoadsInFlight = 0;
	/** Bumped on session start/cancel so stale async completions are ignored. */
	int32 SessionSerial = 0;
	static constexpr int32 MaxLevelLoadsInFlight = 8;

	// Caches (persist across sessions)
	TMap<FSoftObjectPath, FMeshVerdict> MeshVerdicts;
	TMap<FSoftObjectPath, FNiagaraVerdict> NiagaraVerdicts;
	TMap<FName, FLevelScanResult> LevelScanResults;
	TMap<FSoftObjectPath, uint32> MaterialFlagsCache;
	/** Second-hop static-mesh placement verdicts (Packed Level Actor / foliage / ISM Blueprint detection). */
	TMap<FName, EStaticMeshPlacement> StaticMeshPlacementCache;

	int32 LoadsSinceGC = 0;
	int32 TotalWorkItems = 0;
	int32 CompletedWorkItems = 0;

	friend class FReferencerClassifier;
};

} // namespace MaterialUsageAnalyzer
