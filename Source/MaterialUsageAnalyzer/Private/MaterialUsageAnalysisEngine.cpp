// Copyright Epic Games, Inc. All Rights Reserved.

#include "MaterialUsageAnalysisEngine.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialUsageReferencerClassifier.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModuleRequired.h"
#include "Particles/ParticleSystem.h"
#include "Particles/TypeData/ParticleModuleTypeDataBase.h"
#include "Particles/TypeData/ParticleModuleTypeDataBeam2.h"
#include "Particles/TypeData/ParticleModuleTypeDataMesh.h"
#include "Particles/TypeData/ParticleModuleTypeDataRibbon.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "WorldPartition/WorldPartition.h"

namespace MaterialUsageAnalyzer
{

namespace EnginePrivate
{
	static const FName ParentTag("Parent");
	static const FName UsageFlagsTag("UsageFlags");
	static const FName UsageFlagsOverrideMaskTag("UsageFlagsOverrideMask");
	static const FName HasStaticPermutationTag("HasStaticPermutationResource");
	constexpr int32 MaxHierarchyDepth = 32;
	constexpr int32 LoadsPerGC = 32;
	// Collecting garbage flushes in-flight async loads, so GC is deferred until the async window is
	// idle. This hard cap forces a GC anyway (accepting one flush) to keep memory bounded.
	constexpr int32 HardGCCap = 192;

	static IAssetRegistry& GetAssetRegistry()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	static int32 CountBits(uint32 Mask)
	{
		return FMath::CountBits(Mask);
	}
}

void FMaterialUsageAnalysisEngine::StartHierarchySession(const FAssetData& InParentMaterial, const FAnalysisSettings& InSettings)
{
	using namespace EnginePrivate;

	Settings = InSettings;
	SessionKind = ESessionKind::Hierarchy;
	Phase = EAnalysisPhase::EnumerateHierarchy;
	bCancelRequested = false;

	Rows.Reset();
	BatchRows.Reset();
	SlowQueue.Reset();
	SlowQueueIndexByPath.Reset();
	SlowCursor = 0;
	NextSlowRequestIndex = 0;
	SlowLoadsInFlight = 0;
	CompletedSlowLoads.Reset();
	ClassifyCursor = 0;
	LevelScanQueue.Reset();
	LevelScanCursor = 0;
	LevelDependentRows.Reset();
	CompletedLevelLoads.Reset();
	NextLevelRequestIndex = 0;
	LevelLoadsInFlight = 0;
	SessionSerial++;
	TotalWorkItems = 0;
	CompletedWorkItems = 0;

	ParentSummary = FParentSummary();
	ParentSummary.Path = InParentMaterial.GetSoftObjectPath();
	ParentSummary.Name = InParentMaterial.AssetName.ToString();

	uint32 ParentFlags = 0;
	if (InParentMaterial.GetTagValue(UsageFlagsTag, ParentFlags))
	{
		ParentSummary.EnabledMask = ParentFlags;
		ParentSummary.PendingEnabledMask = ParentFlags;
		ParentSummary.bFlagsKnown = true;
	}
	else if (const uint32* Cached = MaterialFlagsCache.Find(ParentSummary.Path))
	{
		ParentSummary.EnabledMask = *Cached;
		ParentSummary.PendingEnabledMask = *Cached;
		ParentSummary.bFlagsKnown = true;
	}
	else
	{
		QueueSlowItem(ESlowItemKind::MaterialFlags, ParentSummary.Path, INDEX_NONE);
	}
}

void FMaterialUsageAnalysisEngine::StartBatchSession(const TArray<FAssetData>& InMaterials, const FAnalysisSettings& InSettings)
{
	Settings = InSettings;
	SessionKind = ESessionKind::Batch;
	Phase = EAnalysisPhase::EnumerateHierarchy;
	bCancelRequested = false;

	Rows.Reset();
	SlowQueue.Reset();
	SlowQueueIndexByPath.Reset();
	SlowCursor = 0;
	NextSlowRequestIndex = 0;
	SlowLoadsInFlight = 0;
	CompletedSlowLoads.Reset();
	ClassifyCursor = 0;
	LevelScanQueue.Reset();
	LevelScanCursor = 0;
	LevelDependentRows.Reset();
	CompletedLevelLoads.Reset();
	NextLevelRequestIndex = 0;
	LevelLoadsInFlight = 0;
	SessionSerial++;

	BatchMaterials = InMaterials;
	BatchCursor = 0;
	BatchSubPhase = 0;

	BatchRows.Reset();
	BatchRows.Reserve(InMaterials.Num());
	for (const FAssetData& Material : InMaterials)
	{
		FBatchRow& Row = BatchRows.AddDefaulted_GetRef();
		Row.MaterialPath = Material.GetSoftObjectPath();
		Row.Name = Material.AssetName.ToString();

		uint32 Flags = 0;
		if (Material.GetTagValue(EnginePrivate::UsageFlagsTag, Flags))
		{
			Row.EnabledMask = Flags;
			Row.bFlagsKnown = true;
		}
	}

	TotalWorkItems = InMaterials.Num();
	CompletedWorkItems = 0;
}

void FMaterialUsageAnalysisEngine::Cancel()
{
	if (IsBusy())
	{
		bCancelRequested = true;
		// Ignore async load completions still in flight for this session.
		SessionSerial++;
		CompletedLevelLoads.Reset();
		LevelLoadsInFlight = 0;
		CompletedSlowLoads.Reset();
		SlowLoadsInFlight = 0;
	}
}

float FMaterialUsageAnalysisEngine::GetProgress() const
{
	return TotalWorkItems > 0 ? FMath::Clamp((float)CompletedWorkItems / (float)TotalWorkItems, 0.0f, 1.0f) : 0.0f;
}

FString FMaterialUsageAnalysisEngine::GetPhaseLabel() const
{
	if (SessionKind == ESessionKind::Batch && IsBusy())
	{
		return FString::Printf(TEXT("Analyzing materials (%d/%d)..."), FMath::Min(BatchCursor + 1, BatchMaterials.Num()), BatchMaterials.Num());
	}

	switch (Phase)
	{
	case EAnalysisPhase::EnumerateHierarchy: return TEXT("Enumerating material instances...");
	case EAnalysisPhase::ClassifyReferencers: return FString::Printf(TEXT("Classifying referencers (%d/%d)..."), ClassifyCursor, Rows.Num());
	case EAnalysisPhase::ResolveSlow: return FString::Printf(TEXT("Loading assets for slow flags (%d/%d)..."), SlowCursor, SlowQueue.Num());
	case EAnalysisPhase::DeepScanLevels: return FString::Printf(TEXT("Deep scanning levels (%d/%d)..."), LevelScanCursor, LevelScanQueue.Num());
	case EAnalysisPhase::Finalize: return TEXT("Finalizing...");
	default: return FString();
	}
}

void FMaterialUsageAnalysisEngine::EnsureChildrenIndex()
{
	using namespace EnginePrivate;

	if (bChildrenIndexBuilt)
	{
		return;
	}

	ChildrenIndex.Reset();

	FARFilter Filter;
	Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> AllInstances;
	GetAssetRegistry().GetAssets(Filter, AllInstances);

	for (const FAssetData& Instance : AllInstances)
	{
		FString ParentExportPath;
		if (Instance.GetTagValue(ParentTag, ParentExportPath) && !ParentExportPath.IsEmpty())
		{
			const FString ParentObjectPath = FPackageName::ExportTextPathToObjectPath(ParentExportPath);
			ChildrenIndex.Add(FSoftObjectPath(ParentObjectPath), Instance);
		}
	}

	bChildrenIndexBuilt = true;
}

void FMaterialUsageAnalysisEngine::CollectDescendants(const FSoftObjectPath& RootMaterial, TArray<FDescendant>& OutInstances) const
{
	struct FVisit { FSoftObjectPath Path; int32 Depth; };
	TArray<FVisit> Stack;
	TSet<FSoftObjectPath> Visited;
	Stack.Push({ RootMaterial, 0 });
	Visited.Add(RootMaterial);

	while (Stack.Num() > 0)
	{
		const FVisit Visit = Stack.Pop();
		if (Visit.Depth >= EnginePrivate::MaxHierarchyDepth)
		{
			continue;
		}

		TArray<FAssetData> Children;
		ChildrenIndex.MultiFind(Visit.Path, Children);
		for (const FAssetData& Child : Children)
		{
			const FSoftObjectPath ChildPath = Child.GetSoftObjectPath();
			if (!Visited.Contains(ChildPath))
			{
				Visited.Add(ChildPath);
				OutInstances.Add({ Child, Visit.Depth + 1, Visit.Path });
				Stack.Push({ ChildPath, Visit.Depth + 1 });
			}
		}
	}
}

void FMaterialUsageAnalysisEngine::InitRowFromAssetData(FMIRow& Row, const FDescendant& Descendant) const
{
	using namespace EnginePrivate;

	const FAssetData& AssetData = Descendant.AssetData;
	Row.AssetData = AssetData;
	Row.Path = AssetData.GetSoftObjectPath();
	Row.PackageName = AssetData.PackageName;
	Row.DirectParentPath = Descendant.ParentPath;
	Row.Depth = Descendant.Depth;

	FString StaticPermTag;
	if (AssetData.GetTagValue(HasStaticPermutationTag, StaticPermTag))
	{
		Row.bHasStaticPermutation = (StaticPermTag == TEXT("True"));
	}

	uint32 Effective = 0;
	uint32 OverrideMask = 0;
	const bool bHasEffective = AssetData.GetTagValue(UsageFlagsTag, Effective);
	const bool bHasOverride = AssetData.GetTagValue(UsageFlagsOverrideMaskTag, OverrideMask);
	if (bHasEffective)
	{
		Row.EffectiveMask = Effective;
		Row.OverrideMask = bHasOverride ? OverrideMask : 0;
		Row.bFlagsFromRegistry = true;
	}
}

void FMaterialUsageAnalysisEngine::BuildRowsForRoot(const FSoftObjectPath& RootMaterial)
{
	TArray<FDescendant> Instances;
	CollectDescendants(RootMaterial, Instances);

	// Depth-ascending so FillUntaggedEffectiveFlags can resolve chains parent-first.
	Instances.StableSort([](const FDescendant& A, const FDescendant& B) { return A.Depth < B.Depth; });

	Rows.Reset();
	Rows.Reserve(Instances.Num());
	for (const FDescendant& Instance : Instances)
	{
		FMIRow& Row = Rows.AddDefaulted_GetRef();
		InitRowFromAssetData(Row, Instance);
	}
}

void FMaterialUsageAnalysisEngine::FillUntaggedEffectiveFlags(uint32 RootEnabledMask)
{
	TMap<FSoftObjectPath, uint32> EffectiveByPath;
	EffectiveByPath.Reserve(Rows.Num());

	for (FMIRow& Row : Rows)
	{
		if (!Row.bFlagsFromRegistry)
		{
			// Saved before the backport: no overrides possible, inherit the direct parent's effective flags.
			const uint32* ParentEffective = EffectiveByPath.Find(Row.DirectParentPath);
			Row.EffectiveMask = ParentEffective ? *ParentEffective : RootEnabledMask;
			Row.OverrideMask = 0;
		}
		EffectiveByPath.Add(Row.Path, Row.EffectiveMask);
	}
}

bool FMaterialUsageAnalysisEngine::StepEnumerate()
{
	EnsureChildrenIndex();

	BuildRowsForRoot(ParentSummary.Path);
	ParentSummary.NumInstances = Rows.Num();
	TotalWorkItems = FMath::Max(1, Rows.Num());
	CompletedWorkItems = 0;
	ClassifyCursor = 0;
	Phase = EAnalysisPhase::ClassifyReferencers;
	return true;
}

bool FMaterialUsageAnalysisEngine::StepClassify()
{
	using namespace EnginePrivate;

	if (ClassifyCursor >= Rows.Num())
	{
		Phase = EAnalysisPhase::ResolveSlow;
		TotalWorkItems = FMath::Max(1, SlowQueue.Num() - SlowCursor);
		CompletedWorkItems = 0;
		return true;
	}

	const int32 RowIndex = ClassifyCursor++;
	FMIRow& Row = Rows[RowIndex];

	FReferencerClassifier::FOutput Output;
	FReferencerClassifier::ClassifyReferencers(GetAssetRegistry(), Row, Settings, MeshVerdicts, NiagaraVerdicts, StaticMeshPlacementCache, Output);

	Row.InferredNeededMask |= Output.NeededMask;
	Row.UnknownMask |= Output.UnknownMask;
	Row.NumLevelReferencers = Output.NumLevelReferencers;
	Row.LevelPackages = Output.LevelPackages;
	Row.OtherReferencers = MoveTemp(Output.OtherReferencers);
	Row.bClassified = true;

	for (const TPair<ESlowItemKind, FSoftObjectPath>& SlowDep : Output.SlowDependencies)
	{
		QueueSlowItem(SlowDep.Key, SlowDep.Value, RowIndex);
	}

	if (Settings.bDeepScanLevels)
	{
		for (const FName LevelPackage : Output.LevelPackages)
		{
			LevelDependentRows.FindOrAdd(LevelPackage).Add(RowIndex);
			if (!LevelScanQueue.Contains(LevelPackage))
			{
				LevelScanQueue.Add(LevelPackage);
			}
		}
	}

	CompletedWorkItems++;
	return true;
}

void FMaterialUsageAnalysisEngine::QueueSlowItem(ESlowItemKind Kind, const FSoftObjectPath& AssetPath, int32 RowIndex)
{
	if (int32* ExistingIndex = SlowQueueIndexByPath.Find(AssetPath))
	{
		if (RowIndex != INDEX_NONE)
		{
			SlowQueue[*ExistingIndex].DependentRows.AddUnique(RowIndex);
		}
		return;
	}

	FSlowItem& Item = SlowQueue.AddDefaulted_GetRef();
	Item.Kind = Kind;
	Item.AssetPath = AssetPath;
	if (RowIndex != INDEX_NONE)
	{
		Item.DependentRows.Add(RowIndex);
	}
	SlowQueueIndexByPath.Add(AssetPath, SlowQueue.Num() - 1);
}

bool FMaterialUsageAnalysisEngine::SlowQueueDrained() const
{
	return NextSlowRequestIndex >= SlowQueue.Num() && SlowLoadsInFlight == 0 && CompletedSlowLoads.Num() == 0;
}

bool FMaterialUsageAnalysisEngine::IsSlowItemCached(const FSlowItem& Item) const
{
	switch (Item.Kind)
	{
	case ESlowItemKind::SkeletalMesh:
	{
		const FMeshVerdict* Verdict = MeshVerdicts.Find(Item.AssetPath);
		return Verdict != nullptr && Verdict->bClothingKnown;
	}
	case ESlowItemKind::StaticMeshNanite:
	{
		const FMeshVerdict* Verdict = MeshVerdicts.Find(Item.AssetPath);
		return Verdict != nullptr && Verdict->bNaniteKnown;
	}
	case ESlowItemKind::NiagaraSystem:
		return NiagaraVerdicts.Contains(Item.AssetPath);
	case ESlowItemKind::MaterialFlags:
		return MaterialFlagsCache.Contains(Item.AssetPath);
	default:
		return false;
	}
}

void FMaterialUsageAnalysisEngine::IssueSlowLoadRequests()
{
	while (NextSlowRequestIndex < SlowQueue.Num() && SlowLoadsInFlight < MaxSlowLoadsInFlight)
	{
		const int32 ItemIndex = NextSlowRequestIndex++;
		const FSlowItem& Item = SlowQueue[ItemIndex];

		// Cached verdicts skip the load and only re-apply to their dependent rows.
		if (IsSlowItemCached(Item))
		{
			ProcessSlowItem(Item);
			SlowCursor++;
			CompletedWorkItems++;
			continue;
		}

		SlowLoadsInFlight++;
		const int32 RequestSerial = SessionSerial;
		LoadPackageAsync(Item.AssetPath.GetLongPackageName(),
			FLoadPackageAsyncDelegate::CreateLambda([this, RequestSerial, ItemIndex](const FName&, UPackage* Package, EAsyncLoadingResult::Type)
			{
				// Completion runs on the game thread; ignore stragglers from cancelled sessions.
				if (RequestSerial != SessionSerial)
				{
					return;
				}
				SlowLoadsInFlight--;
				// Queue even on failure: ProcessSlowItem handles unresolvable assets.
				CompletedSlowLoads.Add({ ItemIndex, TStrongObjectPtr<UPackage>(Package) });
			}));
	}
}

bool FMaterialUsageAnalysisEngine::StepResolveSlowWork()
{
	IssueSlowLoadRequests();

	if (CompletedSlowLoads.Num() == 0)
	{
		return false;
	}

	FCompletedSlowLoad Completed = CompletedSlowLoads.Pop();
	const FSlowItem Item = SlowQueue[Completed.SlowItemIndex];
	ProcessSlowItem(Item);
	Completed.Package.Reset();

	SlowCursor++;
	CompletedWorkItems++;
	MaybeCollectGarbage();
	return true;
}

bool FMaterialUsageAnalysisEngine::StepResolveSlow()
{
	if (SlowQueueDrained())
	{
		if (Settings.bDeepScanLevels && LevelScanCursor < LevelScanQueue.Num())
		{
			Phase = EAnalysisPhase::DeepScanLevels;
			TotalWorkItems = FMath::Max(1, LevelScanQueue.Num());
			CompletedWorkItems = LevelScanCursor;
		}
		else
		{
			Phase = EAnalysisPhase::Finalize;
		}
		return true;
	}

	return StepResolveSlowWork();
}

void FMaterialUsageAnalysisEngine::ProcessSlowItem(const FSlowItem& Item)
{
	using namespace EnginePrivate;

	switch (Item.Kind)
	{
	case ESlowItemKind::SkeletalMesh:
	{
		FMeshVerdict& Verdict = MeshVerdicts.FindOrAdd(Item.AssetPath);
		if (!Verdict.bClothingKnown)
		{
			if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Item.AssetPath.TryLoad()))
			{
				Verdict.bClothingKnown = true;
				Verdict.bHasClothing = Mesh->GetMeshClothingAssets().Num() > 0;
				Verdict.MorphTargetCount = Mesh->GetMorphTargets().Num();
				LoadsSinceGC++;
			}
			else
			{
				Verdict.bClothingKnown = true; // unloadable: treat as no clothing
			}
		}
		break;
	}
	case ESlowItemKind::StaticMeshNanite:
	{
		FMeshVerdict& Verdict = MeshVerdicts.FindOrAdd(Item.AssetPath);
		if (!Verdict.bNaniteKnown)
		{
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(Item.AssetPath.TryLoad()))
			{
				Verdict.bNaniteKnown = true;
				Verdict.bNanite = Mesh->IsNaniteEnabled();
				LoadsSinceGC++;
			}
			else
			{
				Verdict.bNaniteKnown = true;
			}
		}
		break;
	}
	case ESlowItemKind::NiagaraSystem:
	{
		if (!NiagaraVerdicts.Contains(Item.AssetPath))
		{
			FNiagaraVerdict& Verdict = NiagaraVerdicts.Add(Item.AssetPath);
			if (UNiagaraSystem* System = Cast<UNiagaraSystem>(Item.AssetPath.TryLoad()))
			{
				LoadsSinceGC++;
				for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
				{
					if (!Handle.GetIsEnabled())
					{
						continue;
					}
					Handle.ForEachEnabledRendererWithIndex([&Verdict](const UNiagaraRendererProperties* Renderer, int32)
					{
						if (Renderer == nullptr)
						{
							return;
						}
						uint32 RendererMask = 0;
						if (Renderer->IsA<UNiagaraSpriteRendererProperties>())
						{
							RendererMask = MaskBit(MATUSAGE_NiagaraSprites);
						}
						else if (Renderer->IsA<UNiagaraRibbonRendererProperties>())
						{
							RendererMask = MaskBit(MATUSAGE_NiagaraRibbons);
						}
						else if (Renderer->IsA<UNiagaraMeshRendererProperties>())
						{
							RendererMask = MaskBit(MATUSAGE_NiagaraMeshParticles);
						}
						if (RendererMask == 0)
						{
							return;
						}

						TArray<UMaterialInterface*> UsedMaterials;
						Renderer->GetUsedMaterials(nullptr, UsedMaterials);
						for (UMaterialInterface* Material : UsedMaterials)
						{
							if (Material)
							{
								Verdict.MaterialUsage.FindOrAdd(FSoftObjectPath(Material)) |= RendererMask;
							}
						}
					});
				}
			}
		}
		break;
	}
	case ESlowItemKind::ParticleSystem:
	{
		// Legacy Cascade: conservative union of emitter type-data flags applied to all dependent rows.
		if (UParticleSystem* System = Cast<UParticleSystem>(Item.AssetPath.TryLoad()))
		{
			LoadsSinceGC++;
			uint32 SystemMask = 0;
			for (UParticleEmitter* Emitter : System->Emitters)
			{
				if (Emitter == nullptr || Emitter->LODLevels.Num() == 0 || Emitter->LODLevels[0] == nullptr)
				{
					continue;
				}
				UParticleLODLevel* LOD = Emitter->LODLevels[0];
				if (LOD->TypeDataModule == nullptr)
				{
					SystemMask |= MaskBit(MATUSAGE_ParticleSprites);
				}
				else if (LOD->TypeDataModule->IsA<UParticleModuleTypeDataMesh>())
				{
					SystemMask |= MaskBit(MATUSAGE_MeshParticles);
				}
				else if (LOD->TypeDataModule->IsA<UParticleModuleTypeDataBeam2>() || LOD->TypeDataModule->IsA<UParticleModuleTypeDataRibbon>())
				{
					SystemMask |= MaskBit(MATUSAGE_BeamTrails);
				}
			}
			for (const int32 RowIndex : Item.DependentRows)
			{
				if (Rows.IsValidIndex(RowIndex))
				{
					Rows[RowIndex].InferredNeededMask |= SystemMask;
				}
			}
		}
		break;
	}
	case ESlowItemKind::MaterialFlags:
	{
		if (UMaterialInterface* Material = Cast<UMaterialInterface>(Item.AssetPath.TryLoad()))
		{
			LoadsSinceGC++;
			const uint32 Flags = Material->GetUsageFlags();
			MaterialFlagsCache.Add(Item.AssetPath, Flags);

			if (SessionKind == ESessionKind::Hierarchy && Item.AssetPath == ParentSummary.Path)
			{
				ParentSummary.EnabledMask = Flags;
				ParentSummary.PendingEnabledMask = Flags;
				ParentSummary.bFlagsKnown = true;
			}
			for (const int32 RowIndex : Item.DependentRows)
			{
				if (Rows.IsValidIndex(RowIndex))
				{
					Rows[RowIndex].EffectiveMask = Flags;
					if (UMaterialInstance* Instance = Cast<UMaterialInstance>(Material))
					{
						Rows[RowIndex].OverrideMask = Instance->BasePropertyOverrides.bOverride_UsageFlags;
					}
					Rows[RowIndex].bFlagsFromRegistry = true;
				}
			}
		}
		break;
	}
	}

	// Re-apply mesh/Niagara verdicts to dependent rows now that they are resolved.
	if (Item.Kind == ESlowItemKind::SkeletalMesh || Item.Kind == ESlowItemKind::StaticMeshNanite)
	{
		const FMeshVerdict& Verdict = MeshVerdicts.FindChecked(Item.AssetPath);
		const FTopLevelAssetPath MeshClass = Item.Kind == ESlowItemKind::SkeletalMesh
			? FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("SkeletalMesh"))
			: FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("StaticMesh"));
		const uint32 Mask = FReferencerClassifier::UsageMaskFromMeshVerdict(MeshClass, Verdict);
		for (const int32 RowIndex : Item.DependentRows)
		{
			if (Rows.IsValidIndex(RowIndex))
			{
				Rows[RowIndex].InferredNeededMask |= Mask;
			}
		}
	}
	else if (Item.Kind == ESlowItemKind::NiagaraSystem)
	{
		const FNiagaraVerdict& Verdict = NiagaraVerdicts.FindChecked(Item.AssetPath);
		for (const int32 RowIndex : Item.DependentRows)
		{
			if (Rows.IsValidIndex(RowIndex))
			{
				if (const uint32* UsageForMaterial = Verdict.MaterialUsage.Find(Rows[RowIndex].Path))
				{
					Rows[RowIndex].InferredNeededMask |= *UsageForMaterial;
				}
			}
		}
	}
}

bool FMaterialUsageAnalysisEngine::StepBatch()
{
	using namespace EnginePrivate;

	if (BatchCursor >= BatchMaterials.Num())
	{
		Phase = EAnalysisPhase::Finalize;
		return true;
	}

	FBatchRow& BatchRow = BatchRows[BatchCursor];

	if (BatchSubPhase == 0)
	{
		EnsureChildrenIndex();
		BatchRow.Status = EBatchStatus::Analyzing;

		if (!BatchRow.bFlagsKnown)
		{
			if (const uint32* Cached = MaterialFlagsCache.Find(BatchRow.MaterialPath))
			{
				BatchRow.EnabledMask = *Cached;
				BatchRow.bFlagsKnown = true;
			}
			else
			{
				QueueSlowItem(ESlowItemKind::MaterialFlags, BatchRow.MaterialPath, INDEX_NONE);
			}
		}

		BuildRowsForRoot(BatchRow.MaterialPath);

		// Include the material itself so its own referencers count (0-MI materials still get a verdict).
		FMIRow SelfRow;
		InitRowFromAssetData(SelfRow, { BatchMaterials[BatchCursor], 0, FSoftObjectPath() });
		Rows.Insert(MoveTemp(SelfRow), 0);

		ClassifyCursor = 0;
		BatchSubPhase = 1;
		return true;
	}

	if (BatchSubPhase == 1)
	{
		if (ClassifyCursor < Rows.Num())
		{
			const int32 RowIndex = ClassifyCursor++;
			FMIRow& Row = Rows[RowIndex];

			FReferencerClassifier::FOutput Output;
			FReferencerClassifier::ClassifyReferencers(GetAssetRegistry(), Row, Settings, MeshVerdicts, NiagaraVerdicts, StaticMeshPlacementCache, Output);

			Row.InferredNeededMask |= Output.NeededMask;
			Row.UnknownMask |= Output.UnknownMask;
			Row.NumLevelReferencers = Output.NumLevelReferencers;
			Row.LevelPackages = Output.LevelPackages;
			Row.OtherReferencers = MoveTemp(Output.OtherReferencers);
			Row.bClassified = true;

			for (const TPair<ESlowItemKind, FSoftObjectPath>& SlowDep : Output.SlowDependencies)
			{
				QueueSlowItem(SlowDep.Key, SlowDep.Value, RowIndex);
			}
			return true;
		}
		BatchSubPhase = 2;
		return true;
	}

	if (!SlowQueueDrained())
	{
		return StepResolveSlowWork();
	}

	// Rollup for this material
	if (!BatchRow.bFlagsKnown)
	{
		if (const uint32* Cached = MaterialFlagsCache.Find(BatchRow.MaterialPath))
		{
			BatchRow.EnabledMask = *Cached;
			BatchRow.bFlagsKnown = true;
		}
	}
	FillUntaggedEffectiveFlags(BatchRow.EnabledMask);

	BatchRow.NumMIs = Rows.Num() - 1; // exclude the material's own pseudo-row
	BatchRow.NumStaticPermutations = 0;
	BatchRow.TotalUnnecessary = 0;
	FMemory::Memzero(BatchRow.UnnecessaryPerFlag);
	FMemory::Memzero(BatchRow.MissingPerFlag);

	const uint32 CheckMask = Settings.FlagsToCheckMask & GetAnalyzedMask();
	for (const FMIRow& Row : Rows)
	{
		if (Row.bHasStaticPermutation)
		{
			BatchRow.NumStaticPermutations++;
		}
		const uint32 Wasteful = Row.WastefulMask() & CheckMask;
		const uint32 Missing = Row.MissingMask() & CheckMask;
		for (uint32 UsageIndex = 0; UsageIndex < MATUSAGE_MAX; ++UsageIndex)
		{
			const uint32 Bit = 1u << UsageIndex;
			if (Wasteful & Bit)
			{
				BatchRow.UnnecessaryPerFlag[UsageIndex]++;
				BatchRow.TotalUnnecessary++;
			}
			if (Missing & Bit)
			{
				BatchRow.MissingPerFlag[UsageIndex]++;
			}
		}
	}
	BatchRow.Status = BatchRow.bFlagsKnown ? EBatchStatus::Complete : EBatchStatus::Failed;

	CompletedWorkItems++;
	BatchCursor++;
	BatchSubPhase = 0;
	Rows.Reset();
	SlowQueue.Reset();
	SlowQueueIndexByPath.Reset();
	SlowCursor = 0;
	NextSlowRequestIndex = 0;
	return true;
}

bool FMaterialUsageAnalysisEngine::ShouldSkipLevelPackage(FName LevelPackageName) const
{
	using namespace EnginePrivate;

	// World Partition main map packages initialize their UWorldPartition on load, and a
	// partition that was never uninitialized asserts in BeginDestroy when GC purges it.
	// Their actors live in external actor packages (scanned individually), so the main
	// package can be skipped safely.
	TArray<FAssetData> LevelAssets;
	GetAssetRegistry().GetAssetsByPackageName(LevelPackageName, LevelAssets, true /*bIncludeOnlyOnDiskAssets*/);
	for (const FAssetData& LevelAsset : LevelAssets)
	{
		FString PartitionedTag;
		static const FName NAME_LevelIsPartitioned(TEXT("LevelIsPartitioned"));
		if (LevelAsset.GetTagValue(NAME_LevelIsPartitioned, PartitionedTag))
		{
			return true;
		}
	}

	// Never touch the currently open editor map.
	if (GEditor)
	{
		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			TStringBuilder<256> OpenPackage;
			EditorWorld->GetPackage()->GetFName().ToString(OpenPackage);
			TStringBuilder<256> ScanPackage;
			LevelPackageName.ToString(ScanPackage);
			if (OpenPackage.ToView() == ScanPackage.ToView() || FStringView(ScanPackage).Contains(OpenPackage.ToView()))
			{
				return true;
			}
		}
	}

	return false;
}

void FMaterialUsageAnalysisEngine::IssueLevelLoadRequests()
{
	while (NextLevelRequestIndex < LevelScanQueue.Num() && LevelLoadsInFlight < MaxLevelLoadsInFlight)
	{
		const FName LevelPackageName = LevelScanQueue[NextLevelRequestIndex++];

		FLevelScanResult& Result = LevelScanResults.FindOrAdd(LevelPackageName);
		if (Result.bScanned || ShouldSkipLevelPackage(LevelPackageName))
		{
			Result.bScanned = true;
			ApplyLevelResultToRows(LevelPackageName);
			LevelScanCursor++;
			CompletedWorkItems++;
			continue;
		}

		LevelLoadsInFlight++;
		const int32 RequestSerial = SessionSerial;
		LoadPackageAsync(LevelPackageName.ToString(),
			FLoadPackageAsyncDelegate::CreateLambda([this, RequestSerial](const FName& PackageName, UPackage* Package, EAsyncLoadingResult::Type LoadResult)
			{
				// Completion runs on the game thread; ignore stragglers from cancelled sessions.
				if (RequestSerial != SessionSerial)
				{
					return;
				}
				LevelLoadsInFlight--;
				if (LoadResult == EAsyncLoadingResult::Succeeded && Package != nullptr)
				{
					CompletedLevelLoads.Add({ PackageName, TStrongObjectPtr<UPackage>(Package) });
				}
				else
				{
					LevelScanResults.FindOrAdd(PackageName).bScanned = true;
					ApplyLevelResultToRows(PackageName);
					LevelScanCursor++;
					CompletedWorkItems++;
				}
			}));
	}
}

void FMaterialUsageAnalysisEngine::ProcessScannedLevelPackage(FName LevelPackageName, UPackage* Package)
{
	FLevelScanResult& Result = LevelScanResults.FindOrAdd(LevelPackageName);

	TArray<UObject*> Objects;
	GetObjectsWithPackage(Package, Objects, true /*bIncludeNestedObjects*/);
	for (UObject* Object : Objects)
	{
		UPrimitiveComponent* Component = Cast<UPrimitiveComponent>(Object);
		if (Component == nullptr)
		{
			continue;
		}

		uint32 ComponentMask = 0;
		if (Component->IsA<USplineMeshComponent>())
		{
			ComponentMask = MaskBit(MATUSAGE_SplineMesh) | MaskBit(MATUSAGE_StaticMesh);
		}
		else if (Component->IsA<UInstancedStaticMeshComponent>())
		{
			ComponentMask = MaskBit(MATUSAGE_InstancedStaticMeshes) | MaskBit(MATUSAGE_StaticMesh);
		}
		else if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Component))
		{
			ComponentMask = MaskBit(MATUSAGE_StaticMesh);
			if (UStaticMesh* Mesh = StaticMeshComponent->GetStaticMesh())
			{
				if (Mesh->IsNaniteEnabled())
				{
					ComponentMask |= MaskBit(MATUSAGE_Nanite);
				}
			}
		}
		else if (USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(Component))
		{
			ComponentMask = MaskBit(MATUSAGE_SkeletalMesh);
			if (USkeletalMesh* Mesh = SkeletalMeshComponent->GetSkeletalMeshAsset())
			{
				if (Mesh->GetMorphTargets().Num() > 0)
				{
					ComponentMask |= MaskBit(MATUSAGE_MorphTargets);
				}
				if (Mesh->GetMeshClothingAssets().Num() > 0)
				{
					ComponentMask |= MaskBit(MATUSAGE_Clothing);
				}
			}
		}
		else
		{
			const FString ClassPath = Component->GetClass()->GetPathName();
			if (ClassPath.StartsWith(TEXT("/Script/Water.")))
			{
				ComponentMask = MaskBit(MATUSAGE_Water);
			}
			else if (ClassPath == TEXT("/Script/HairStrandsCore.GroomComponent"))
			{
				ComponentMask = MaskBit(MATUSAGE_HairStrands);
			}
		}

		if (ComponentMask == 0)
		{
			continue;
		}

		TArray<UMaterialInterface*> UsedMaterials;
		Component->GetUsedMaterials(UsedMaterials, false);
		for (UMaterialInterface* Material : UsedMaterials)
		{
			if (Material)
			{
				Result.MaterialUsage.FindOrAdd(FSoftObjectPath(Material)) |= ComponentMask;
			}
		}
	}

	Result.bScanned = true;
}

void FMaterialUsageAnalysisEngine::ApplyLevelResultToRows(FName LevelPackageName)
{
	const FLevelScanResult& Result = LevelScanResults.FindOrAdd(LevelPackageName);

	if (const TArray<int32>* DependentRows = LevelDependentRows.Find(LevelPackageName))
	{
		for (const int32 RowIndex : *DependentRows)
		{
			if (!Rows.IsValidIndex(RowIndex))
			{
				continue;
			}
			FMIRow& Row = Rows[RowIndex];
			if (const uint32* Mask = Result.MaterialUsage.Find(Row.Path))
			{
				Row.InferredNeededMask |= *Mask;
			}

			// Once every referencing level has been scanned, component-driven flags are decidable.
			bool bAllScanned = true;
			for (const FName RowLevel : Row.LevelPackages)
			{
				const FLevelScanResult* RowLevelResult = LevelScanResults.Find(RowLevel);
				if (RowLevelResult == nullptr || !RowLevelResult->bScanned)
				{
					bAllScanned = false;
					break;
				}
			}
			if (bAllScanned)
			{
				Row.UnknownMask &= ~GetComponentDrivenMask();
			}
		}
	}
}

bool FMaterialUsageAnalysisEngine::StepDeepScan()
{
	using namespace EnginePrivate;

	IssueLevelLoadRequests();

	if (CompletedLevelLoads.Num() == 0)
	{
		if (NextLevelRequestIndex >= LevelScanQueue.Num() && LevelLoadsInFlight == 0)
		{
			Phase = EAnalysisPhase::Finalize;
			return true;
		}
		// Waiting on the async loader; yield the rest of this tick's budget.
		return false;
	}

	FCompletedLevelLoad Completed = CompletedLevelLoads.Pop();
	ProcessScannedLevelPackage(Completed.PackageName, Completed.Package.Get());
	Completed.Package.Reset();
	ApplyLevelResultToRows(Completed.PackageName);

	LevelScanCursor++;
	CompletedWorkItems++;
	LoadsSinceGC += LoadsPerGC; // levels are heavy: force GC after each
	MaybeCollectGarbage();
	return true;
}

void FMaterialUsageAnalysisEngine::RebuildParentSummary()
{
	ParentSummary.NumInstances = Rows.Num();
	ParentSummary.NumStaticPermutations = 0;
	ParentSummary.NumWastefulFlagInstances = 0;
	ParentSummary.NumMissingFlagInstances = 0;
	ParentSummary.NumUnknownMIs = 0;
	FMemory::Memzero(ParentSummary.NeededCount);

	for (const FMIRow& Row : Rows)
	{
		if (Row.bHasStaticPermutation)
		{
			ParentSummary.NumStaticPermutations++;
		}
		ParentSummary.NumWastefulFlagInstances += EnginePrivate::CountBits(Row.WastefulMask());
		ParentSummary.NumMissingFlagInstances += EnginePrivate::CountBits(Row.MissingMask());
		if ((Row.UnknownMask & GetAnalyzedMask()) != 0)
		{
			ParentSummary.NumUnknownMIs++;
		}

		const uint32 Needed = Row.NeededMask();
		for (uint32 UsageIndex = 0; UsageIndex < MATUSAGE_MAX; ++UsageIndex)
		{
			if (Needed & (1u << UsageIndex))
			{
				ParentSummary.NeededCount[UsageIndex]++;
			}
		}
	}
}

void FMaterialUsageAnalysisEngine::FinalizeSession()
{
	if (SessionKind == ESessionKind::Hierarchy)
	{
		if (!ParentSummary.bFlagsKnown)
		{
			if (const uint32* Cached = MaterialFlagsCache.Find(ParentSummary.Path))
			{
				ParentSummary.EnabledMask = *Cached;
				ParentSummary.PendingEnabledMask = *Cached;
				ParentSummary.bFlagsKnown = true;
			}
		}
		FillUntaggedEffectiveFlags(ParentSummary.EnabledMask);
		RebuildParentSummary();
	}
	else if (SessionKind == ESessionKind::Batch)
	{
		BatchRows.StableSort([](const FBatchRow& A, const FBatchRow& B)
		{
			if (A.TotalUnnecessary != B.TotalUnnecessary)
			{
				return A.TotalUnnecessary > B.TotalUnnecessary;
			}
			return A.MaterialPath.ToString() < B.MaterialPath.ToString();
		});
	}

	MaybeCollectGarbage(true);
	Phase = EAnalysisPhase::Ready;
}

void FMaterialUsageAnalysisEngine::MaybeCollectGarbage(bool bForce)
{
	// GC flushes in-flight async loads (it serializes the pipeline - it "looks single threaded").
	// Only collect while the async window is idle; force past a hard cap so memory stays bounded,
	// and honour explicit bForce at the end of a phase.
	const bool bAsyncIdle = (SlowLoadsInFlight == 0 && LevelLoadsInFlight == 0);
	const bool bSoftLimit = LoadsSinceGC >= EnginePrivate::LoadsPerGC;
	const bool bHardLimit = LoadsSinceGC >= EnginePrivate::HardGCCap;
	if ((bSoftLimit && bAsyncIdle) || bHardLimit || (bForce && LoadsSinceGC > 0))
	{
		LoadsSinceGC = 0;

		// Any world partition dragged in by our loads (including transitively, e.g. level
		// instance actors hard-referencing other maps) initializes on load and asserts in
		// BeginDestroy if GC purges it while still initialized. Uninitialize orphaned
		// partitions first — same pattern as WorldPartitionHLODsBuilder. Live worlds
		// (editor map, PIE) are initialized via InitWorld and are left alone.
		for (TObjectIterator<UWorldPartition> It; It; ++It)
		{
			UWorldPartition* WorldPartition = *It;
			UWorld* OuterWorld = WorldPartition ? WorldPartition->GetTypedOuter<UWorld>() : nullptr;
			if (WorldPartition && WorldPartition->IsInitialized() && (OuterWorld == nullptr || !OuterWorld->IsInitialized()))
			{
				WorldPartition->Uninitialize();
			}
		}

		TryCollectGarbage(RF_NoFlags, false /*bPerformFullPurge*/);
	}
}

void FMaterialUsageAnalysisEngine::RunDeferredActions()
{
	while (DeferredActions.Num() > 0)
	{
		TArray<TFunction<void()>> Actions = MoveTemp(DeferredActions);
		DeferredActions.Reset();
		for (TFunction<void()>& Action : Actions)
		{
			Action();
		}
	}
}

void FMaterialUsageAnalysisEngine::Pump(double BudgetSeconds)
{
	if (!IsBusy() || bPumping)
	{
		return;
	}
	TGuardValue<bool> PumpGuard(bPumping, true);

	const double EndTime = FPlatformTime::Seconds() + BudgetSeconds;
	do
	{
		if (bCancelRequested)
		{
			// Keep partial results; rows never classified stay fully unknown.
			for (FMIRow& Row : Rows)
			{
				if (!Row.bClassified)
				{
					Row.UnknownMask |= GetAnalyzedMask();
				}
			}
			if (SessionKind == ESessionKind::Hierarchy)
			{
				RebuildParentSummary();
			}
			Phase = EAnalysisPhase::Cancelled;
			return;
		}

		bool bDidWork = true;
		switch (Phase)
		{
		case EAnalysisPhase::EnumerateHierarchy:
			bDidWork = SessionKind == ESessionKind::Batch ? StepBatch() : StepEnumerate();
			break;
		case EAnalysisPhase::ClassifyReferencers:
			bDidWork = SessionKind == ESessionKind::Batch ? StepBatch() : StepClassify();
			break;
		case EAnalysisPhase::ResolveSlow:
			bDidWork = SessionKind == ESessionKind::Batch ? StepBatch() : StepResolveSlow();
			break;
		case EAnalysisPhase::DeepScanLevels:
			bDidWork = StepDeepScan();
			break;
		case EAnalysisPhase::Finalize:
			FinalizeSession();
			return;
		default:
			return;
		}

		if (!bDidWork)
		{
			// Waiting on async work (level loads); yield the rest of this tick's budget.
			return;
		}
	}
	while (IsBusy() && FPlatformTime::Seconds() < EndTime);
}

} // namespace MaterialUsageAnalyzer
