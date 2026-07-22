// Copyright Epic Games, Inc. All Rights Reserved.

#include "MaterialUsageHierarchyTab.h"

#include "InputCoreTypes.h"
#include "MaterialUsageEditorActions.h"
#include "Misc/ScopedSlowTask.h"
#include "SlateIM.h"

#define LOCTEXT_NAMESPACE "MaterialUsageAnalyzer"

namespace MaterialUsageAnalyzer
{

namespace HierarchyTabPrivate
{
	enum EFilterCondition
	{
		Condition_Needs = 0,
		Condition_DoesNotNeed,
		Condition_Wasteful,
		Condition_Missing,
		Condition_HasOverride,
		Condition_MAX
	};

	static const TCHAR* GetConditionName(int32 Condition)
	{
		switch (Condition)
		{
		case Condition_Needs: return TEXT("needs");
		case Condition_DoesNotNeed: return TEXT("doesn't need");
		case Condition_Wasteful: return TEXT("wasteful");
		case Condition_Missing: return TEXT("missing");
		case Condition_HasOverride: return TEXT("has override");
		default: return TEXT("?");
		}
	}
}

FMaterialUsageHierarchyTab::FMaterialUsageHierarchyTab()
{
	using namespace HierarchyTabPrivate;

	for (const FUsageFlagDesc* Desc : GetAnalyzedUsageFlags())
	{
		FlagComboItems.Add(Desc->DisplayName);
	}
	for (int32 Condition = 0; Condition < Condition_MAX; ++Condition)
	{
		ConditionComboItems.Add(GetConditionName(Condition));
	}
	BatchEditComboItems = { TEXT("-"), TEXT("Override ON"), TEXT("Override OFF") };
	BatchEditStates.Init(0, GetAnalyzedUsageFlags().Num());
}

void FMaterialUsageHierarchyTab::StartAnalysis(FMaterialUsageAnalysisEngine& Engine)
{
	if (!ParentMaterial.IsValid())
	{
		return;
	}

	FAnalysisSettings Settings;
	Settings.bExcludeNonCookedReferencers = bExcludeNonCookedReferencers;
	Settings.bDeepScanLevels = bDeepScanLevels;
	Settings.FlagsToCheckMask = bResolveSlowFlags ? GetAnalyzedMask() : 0;
	// Fast flags are always checked; the toggle only gates load-based resolution.
	if (!bResolveSlowFlags)
	{
		for (const FUsageFlagDesc* Desc : GetAnalyzedUsageFlags())
		{
			if (!Desc->bSlow)
			{
				Settings.FlagsToCheckMask |= MaskBit(Desc->Usage);
			}
		}
	}

	ExcludeDirsText.ParseIntoArray(Settings.ExcludeDirPrefixes, TEXT(","), true);
	for (FString& Prefix : Settings.ExcludeDirPrefixes)
	{
		Prefix.TrimStartAndEndInline();
	}

	Engine.StartHierarchySession(ParentMaterial, Settings);
	bDisplayDirty = true;
	StatusMessage.Reset();
}

void FMaterialUsageHierarchyTab::Draw(FMaterialUsageAnalysisEngine& Engine)
{
	DrawToolbar(Engine);
	DrawSettings();

	const bool bHierarchyResults = Engine.GetSessionKind() == ESessionKind::Hierarchy && Engine.HasResults();
	if (bHierarchyResults)
	{
		DrawParentPanel(Engine);
		DrawFlagEfficiency(Engine);
		DrawParentFlagEditor(Engine);
		DrawFilters(Engine);
		DrawBatchEditRow(Engine);
		if (bShowLegend)
		{
			DrawLegend();
		}
		DrawTable(Engine);
	}
	else if (!StatusMessage.IsEmpty())
	{
		SlateIM::Padding(FMargin(6.0f));
		SlateIM::Text(StatusMessage, FLinearColor(1.0f, 0.7f, 0.3f));
	}
}

void FMaterialUsageHierarchyTab::DrawToolbar(FMaterialUsageAnalysisEngine& Engine)
{
	SlateIM::BeginHorizontalStack();

	SlateIM::Padding(FMargin(4.0f, 4.0f, 2.0f, 2.0f));
	SlateIM::SetToolTip(TEXT("Uses the first material selected in the Content Browser. A selected instance walks up to its root material."));
	if (SlateIM::Button(TEXT("Load Parent from Content Browser")))
	{
		FAssetData NewParent;
		FString Error;
		if (EditorActions::GetSelectedParentMaterial(NewParent, Error))
		{
			ParentMaterial = NewParent;
			StartAnalysis(Engine);
		}
		else
		{
			StatusMessage = Error;
		}
	}

	SlateIM::Padding(FMargin(2.0f, 4.0f, 2.0f, 2.0f));
	if (SlateIM::Button(TEXT("Refresh")) && ParentMaterial.IsValid())
	{
		Engine.InvalidateChildrenIndex();
		StartAnalysis(Engine);
	}

	SlateIM::Padding(FMargin(8.0f, 6.0f, 2.0f, 2.0f));
	SlateIM::VAlign(VAlign_Center);
	if (ParentMaterial.IsValid())
	{
		SlateIM::Text(FString::Printf(TEXT("Selected Material: %s"), *ParentMaterial.GetObjectPathString()));
	}
	else
	{
		SlateIM::Text(TEXT("Selected Material: <none>"), GetStatusColorUnknown());
	}

	SlateIM::EndHorizontalStack();
}

void FMaterialUsageHierarchyTab::DrawSettings()
{
	SlateIM::BeginHorizontalStack();

	SlateIM::Padding(FMargin(4.0f, 2.0f));
	SlateIM::SetToolTip(TEXT("Only referencers that are cooked into the game count. Editor-only referencers are ignored."));
	SlateIM::CheckBox(TEXT("Exclude non-cooked referencers"), bExcludeNonCookedReferencers);

	SlateIM::Padding(FMargin(8.0f, 2.0f, 2.0f, 2.0f));
	SlateIM::SetToolTip(TEXT("Resolve slow flags (Clothing, Niagara renderer types) by loading the referencing assets."));
	SlateIM::CheckBox(TEXT("Resolve slow flags (loads assets)"), bResolveSlowFlags);

	SlateIM::Padding(FMargin(8.0f, 2.0f, 2.0f, 2.0f));
	SlateIM::SetToolTip(TEXT("Load referencing levels (including World Partition actor packages) and inspect components for exact usage. Slow and memory heavy; the currently open level is skipped."));
	SlateIM::CheckBox(TEXT("Deep Scan Levels"), bDeepScanLevels);

	SlateIM::Padding(FMargin(8.0f, 2.0f, 2.0f, 2.0f));
	SlateIM::VAlign(VAlign_Center);
	SlateIM::Text(TEXT("Exclude referencer directories:"));

	SlateIM::Padding(FMargin(2.0f));
	SlateIM::MinWidth(260.0f);
	SlateIM::SetToolTip(TEXT("Comma-separated package path prefixes, e.g. /Game/Developers, /Game/Test"));
	SlateIM::EditableText(ExcludeDirsText, TEXT("/Game/Developers, /Game/Test"));

	SlateIM::EndHorizontalStack();
}

void FMaterialUsageHierarchyTab::DrawParentPanel(FMaterialUsageAnalysisEngine& Engine)
{
	const FParentSummary& Summary = Engine.ParentSummary;

	SlateIM::BeginHorizontalStack();

	SlateIM::Padding(FMargin(4.0f, 2.0f));
	SlateIM::Text(FString::Printf(TEXT("Instances: %d"), Summary.NumInstances));

	SlateIM::Padding(FMargin(8.0f, 2.0f, 2.0f, 2.0f));
	SlateIM::SetToolTip(TEXT("Instances with a static permutation compile their own shader set; per-instance overrides directly cut their permutations."));
	SlateIM::Text(FString::Printf(TEXT("Static permutations: %d"), Summary.NumStaticPermutations));

	SlateIM::Padding(FMargin(8.0f, 2.0f, 2.0f, 2.0f));
	SlateIM::SetToolTip(TEXT("Flag instances that are effectively enabled but not needed by any referencer."));
	SlateIM::Text(FString::Printf(TEXT("Wasteful: %d"), Summary.NumWastefulFlagInstances),
		Summary.NumWastefulFlagInstances > 0 ? GetStatusColorWasteful() : GetStatusColorNeeded());

	SlateIM::Padding(FMargin(8.0f, 2.0f, 2.0f, 2.0f));
	SlateIM::SetToolTip(TEXT("Flag instances that are needed but effectively disabled (would fall back to the default material in game)."));
	SlateIM::Text(FString::Printf(TEXT("Missing: %d"), Summary.NumMissingFlagInstances),
		Summary.NumMissingFlagInstances > 0 ? GetStatusColorMissing() : GetStatusColorNeeded());

	if (Summary.NumUnknownMIs > 0)
	{
		SlateIM::Padding(FMargin(8.0f, 2.0f, 2.0f, 2.0f));
		SlateIM::SetToolTip(TEXT("Instances with flags that could not be decided (level referencers without deep scan / unresolved slow flags)."));
		SlateIM::Text(FString::Printf(TEXT("With unknowns: %d"), Summary.NumUnknownMIs), GetStatusColorUnknown());
	}

	SlateIM::Padding(FMargin(12.0f, 2.0f, 2.0f, 2.0f));
	SlateIM::CheckBox(TEXT("Show Other Referencers"), bShowOtherReferencers);

	SlateIM::Padding(FMargin(8.0f, 2.0f, 2.0f, 2.0f));
	SlateIM::CheckBox(TEXT("Show Color Legend"), bShowLegend);

	SlateIM::EndHorizontalStack();

	if (bShowOtherReferencers)
	{
		TMap<FName, FTopLevelAssetPath> UniqueReferencers;
		for (const FMIRow& Row : Engine.Rows)
		{
			for (const FReferencerRecord& Record : Row.OtherReferencers)
			{
				UniqueReferencers.FindOrAdd(Record.PackageName) = Record.AssetClass;
			}
		}

		SlateIM::MaxHeight(140.0f);
		if (SlateIM::BeginScrollBox())
		{
		}
		if (UniqueReferencers.Num() == 0)
		{
			SlateIM::Text(TEXT("  No other referencers."), GetStatusColorUnknown());
		}
		for (const TPair<FName, FTopLevelAssetPath>& Pair : UniqueReferencers)
		{
			SlateIM::Text(FString::Printf(TEXT("  %s  [%s]"), *Pair.Key.ToString(), *Pair.Value.GetAssetName().ToString()));
		}
		SlateIM::EndScrollBox();
	}
}

void FMaterialUsageHierarchyTab::DrawFlagEfficiency(FMaterialUsageAnalysisEngine& Engine)
{
	const FParentSummary& Summary = Engine.ParentSummary;
	if (!Summary.bFlagsKnown || Summary.NumInstances == 0)
	{
		return;
	}

	SlateIM::BeginHorizontalWrap();
	for (const FUsageFlagDesc* Desc : GetAnalyzedUsageFlags())
	{
		const uint32 Bit = MaskBit(Desc->Usage);
		if ((Summary.EnabledMask & Bit) == 0)
		{
			continue;
		}

		const int32 Needed = Summary.NeededCount[(uint32)Desc->Usage];
		const float Fraction = Summary.NumInstances > 0 ? (float)Needed / (float)Summary.NumInstances : 0.0f;
		const bool bLowUse = Fraction < 0.1f;

		SlateIM::Padding(FMargin(6.0f, 2.0f));
		SlateIM::SetToolTip(FString::Printf(TEXT("%s is enabled on the parent and needed by %d of %d instances."), Desc->DisplayName, Needed, Summary.NumInstances));
		SlateIM::Text(FString::Printf(TEXT("%s: %d/%d (%.1f%%)"), Desc->DisplayName, Needed, Summary.NumInstances, Fraction * 100.0f),
			bLowUse ? GetStatusColorWasteful() : GetStatusColorNeeded());
	}
	SlateIM::EndHorizontalWrap();
}

void FMaterialUsageHierarchyTab::DrawParentFlagEditor(FMaterialUsageAnalysisEngine& Engine)
{
	FParentSummary& Summary = Engine.ParentSummary;

	SlateIM::BeginHorizontalStack();
	SlateIM::Padding(FMargin(4.0f, 2.0f));
	SlateIM::CheckBox(TEXT("Edit Parent Usage Flags"), bEditParentFlags);

	if (bEditParentFlags && Summary.PendingEnabledMask != Summary.EnabledMask)
	{
		SlateIM::Padding(FMargin(8.0f, 2.0f, 2.0f, 2.0f));
		if (SlateIM::Button(TEXT("Save Parent Material")))
		{
			const EAppReturnType::Type Confirm = SlateIM::ModalDialog(EAppMsgType::OkCancel,
				TEXT("Changing parent usage flags recompiles the material (and its instances). Continue?"),
				EAppMsgCategory::Warning, TEXT("Save Parent Material"));
			if (Confirm == EAppReturnType::Ok)
			{
				// Deferred: recompiles + save prompt tick Slate and must not run inside the SlateIM root.
				const FSoftObjectPath MaterialPath = Summary.Path;
				const uint32 NewMask = Summary.PendingEnabledMask;
				const uint32 OldMask = Summary.EnabledMask;
				Engine.Defer([this, &Engine, MaterialPath, NewMask, OldMask]()
				{
					if (UPackage* Package = EditorActions::ApplyParentUsageFlags(MaterialPath, NewMask, OldMask))
					{
						EditorActions::PromptSaveDirtyPackages({ Package });
						if (Engine.ParentSummary.Path == MaterialPath)
						{
							Engine.ParentSummary.EnabledMask = NewMask;
							StartAnalysis(Engine); // effective masks of untagged instances derive from the parent
						}
					}
				});
			}
		}

		SlateIM::Padding(FMargin(2.0f));
		if (SlateIM::Button(TEXT("Revert Parent Flags")))
		{
			Summary.PendingEnabledMask = Summary.EnabledMask;
		}
	}
	SlateIM::EndHorizontalStack();

	if (!bEditParentFlags)
	{
		return;
	}

	SlateIM::BeginHorizontalWrap();
	for (const FUsageFlagDesc& Desc : GetAllUsageFlags())
	{
		const uint32 Bit = MaskBit(Desc.Usage);
		bool bEnabled = (Summary.PendingEnabledMask & Bit) != 0;
		SlateIM::Padding(FMargin(6.0f, 2.0f));
		if (SlateIM::CheckBox(Desc.DisplayName, bEnabled))
		{
			if (bEnabled)
			{
				Summary.PendingEnabledMask |= Bit;
			}
			else
			{
				Summary.PendingEnabledMask &= ~Bit;
			}
		}
	}
	SlateIM::EndHorizontalWrap();
}

void FMaterialUsageHierarchyTab::DrawFilters(FMaterialUsageAnalysisEngine& Engine)
{
	using namespace HierarchyTabPrivate;

	SlateIM::BeginHorizontalStack();

	SlateIM::Padding(FMargin(4.0f, 2.0f));
	SlateIM::VAlign(VAlign_Center);
	SlateIM::Text(TEXT("Add Filter:"));

	SlateIM::Padding(FMargin(2.0f));
	SlateIM::MinWidth(170.0f);
	SlateIM::ComboBox(FlagComboItems, NewFilterFlagIndex);

	SlateIM::Padding(FMargin(2.0f));
	SlateIM::MinWidth(120.0f);
	SlateIM::ComboBox(ConditionComboItems, NewFilterCondition);

	SlateIM::Padding(FMargin(2.0f));
	if (SlateIM::Button(TEXT("+ Add Filter")))
	{
		Filters.Add({ NewFilterFlagIndex, NewFilterCondition });
		bDisplayDirty = true;
	}

	if (Filters.Num() > 0)
	{
		SlateIM::Padding(FMargin(8.0f, 2.0f, 2.0f, 2.0f));
		if (SlateIM::Button(TEXT("Clear All Filters")))
		{
			Filters.Reset();
			bDisplayDirty = true;
		}
	}

	SlateIM::Padding(FMargin(8.0f, 2.0f, 2.0f, 2.0f));
	SlateIM::VAlign(VAlign_Center);
	SlateIM::Text(TEXT("Name:"));
	SlateIM::Padding(FMargin(2.0f));
	SlateIM::MinWidth(180.0f);
	if (SlateIM::EditableText(NameFilter, TEXT("filter by name/path")))
	{
		bDisplayDirty = true;
	}

	SlateIM::EndHorizontalStack();

	if (Filters.Num() > 0)
	{
		SlateIM::BeginHorizontalWrap();
		for (int32 FilterIndex = 0; FilterIndex < Filters.Num(); ++FilterIndex)
		{
			const FFlagFilter& Filter = Filters[FilterIndex];
			const FUsageFlagDesc* Desc = GetAnalyzedUsageFlags()[Filter.AnalyzedFlagIndex];
			SlateIM::Padding(FMargin(4.0f, 2.0f));
			if (SlateIM::Button(FString::Printf(TEXT("%s %s  X"), *FString(Desc->DisplayName), GetConditionName(Filter.Condition))))
			{
				Filters.RemoveAt(FilterIndex);
				bDisplayDirty = true;
				break;
			}
		}
		SlateIM::EndHorizontalWrap();
	}
}

void FMaterialUsageHierarchyTab::DrawBatchEditRow(FMaterialUsageAnalysisEngine& Engine)
{
	SlateIM::BeginHorizontalStack();

	SlateIM::Padding(FMargin(4.0f, 2.0f));
	SlateIM::VAlign(VAlign_Center);
	SlateIM::Text(FString::Printf(TEXT("Batch Edit Filtered MIs (%d):"), DisplayIndices.Num()));

	TConstArrayView<const FUsageFlagDesc*> Analyzed = GetAnalyzedUsageFlags();
	for (int32 FlagIndex = 0; FlagIndex < Analyzed.Num(); ++FlagIndex)
	{
		SlateIM::Padding(FMargin(2.0f));
		SlateIM::VAlign(VAlign_Center);
		SlateIM::Text(Analyzed[FlagIndex]->ShortName);
		SlateIM::Padding(FMargin(0.0f, 2.0f));
		SlateIM::MinWidth(96.0f);
		SlateIM::ComboBox(BatchEditComboItems, BatchEditStates[FlagIndex]);
	}

	SlateIM::Padding(FMargin(6.0f, 2.0f));
	SlateIM::SetToolTip(TEXT("Apply the selected override states to every currently filtered instance."));
	if (SlateIM::Button(TEXT("Apply")))
	{
		for (const int32 RowIndex : DisplayIndices)
		{
			FMIRow& Row = Engine.Rows[RowIndex];
			for (int32 FlagIndex = 0; FlagIndex < Analyzed.Num(); ++FlagIndex)
			{
				const uint32 Bit = MaskBit(Analyzed[FlagIndex]->Usage);
				if (BatchEditStates[FlagIndex] == 1)
				{
					Row.PendingClearOverrideMask &= ~Bit;
					Row.PendingOverrideSetMask |= Bit;
					Row.PendingOverrideValueMask |= Bit;
				}
				else if (BatchEditStates[FlagIndex] == 2)
				{
					Row.PendingClearOverrideMask &= ~Bit;
					Row.PendingOverrideSetMask |= Bit;
					Row.PendingOverrideValueMask &= ~Bit;
				}
			}
		}
	}

	SlateIM::EndHorizontalStack();

	SlateIM::BeginHorizontalStack();

	SlateIM::Padding(FMargin(4.0f, 2.0f));
	SlateIM::SetToolTip(TEXT("For every filtered instance: override OFF for wasteful flags, override ON for missing flags."));
	if (SlateIM::Button(TEXT("Adopt Needed As Overrides")))
	{
		for (const int32 RowIndex : DisplayIndices)
		{
			FMIRow& Row = Engine.Rows[RowIndex];
			const uint32 Wasteful = Row.WastefulMask();
			const uint32 Missing = Row.MissingMask();
			for (const FUsageFlagDesc* Desc : Analyzed)
			{
				const uint32 Bit = MaskBit(Desc->Usage);
				if (Wasteful & Bit)
				{
					Row.PendingClearOverrideMask &= ~Bit;
					Row.PendingOverrideSetMask |= Bit;
					Row.PendingOverrideValueMask &= ~Bit;
				}
				else if (Missing & Bit)
				{
					Row.PendingClearOverrideMask &= ~Bit;
					Row.PendingOverrideSetMask |= Bit;
					Row.PendingOverrideValueMask |= Bit;
				}
			}
		}
	}

	SlateIM::Padding(FMargin(4.0f, 2.0f));
	SlateIM::SetToolTip(TEXT("Queue removal of all explicit overrides on every filtered instance (back to inherit)."));
	if (SlateIM::Button(TEXT("Clear Overrides")))
	{
		for (const int32 RowIndex : DisplayIndices)
		{
			FMIRow& Row = Engine.Rows[RowIndex];
			Row.PendingOverrideSetMask = 0;
			Row.PendingOverrideValueMask = 0;
			Row.PendingClearOverrideMask = Row.OverrideMask & GetAnalyzedMask();
		}
	}

	const int32 NumDirty = CountDirtyRows(Engine);

	SlateIM::Padding(FMargin(12.0f, 2.0f, 2.0f, 2.0f));
	if (SlateIM::Button(TEXT("Revert All")) && NumDirty > 0)
	{
		const EAppReturnType::Type Confirm = SlateIM::ModalDialog(EAppMsgType::OkCancel,
			FString::Printf(TEXT("Discard pending override edits on %d instances?"), NumDirty),
			EAppMsgCategory::Warning, TEXT("Revert All"));
		if (Confirm == EAppReturnType::Ok)
		{
			RevertAll(Engine);
		}
	}

	SlateIM::Padding(FMargin(2.0f));
	SlateIM::SetToolTip(TEXT("Applies pending overrides to ALL edited instances (not just the filtered set), then prompts for checkout and save."));
	if (SlateIM::Button(FString::Printf(TEXT("Save All (%d dirty)"), NumDirty)) && NumDirty > 0)
	{
		// Deferred: instance loads, the slow-task dialog and the save prompt tick Slate.
		Engine.Defer([this, &Engine]()
		{
			SaveAll(Engine);
		});
	}

	SlateIM::EndHorizontalStack();
}

void FMaterialUsageHierarchyTab::DrawLegend()
{
	SlateIM::BeginHorizontalStack();
	SlateIM::Padding(FMargin(4.0f, 2.0f));
	SlateIM::Text(TEXT("[check] needed+on"), GetStatusColorNeeded());
	SlateIM::Padding(FMargin(8.0f, 2.0f));
	SlateIM::Text(TEXT("[o] on but unneeded (wasteful)"), GetStatusColorWasteful());
	SlateIM::Padding(FMargin(8.0f, 2.0f));
	SlateIM::Text(TEXT("[x] needed but off (missing)"), GetStatusColorMissing());
	SlateIM::Padding(FMargin(8.0f, 2.0f));
	SlateIM::Text(TEXT("[.] off, not needed"), GetStatusColorUnknown());
	SlateIM::Padding(FMargin(8.0f, 2.0f));
	SlateIM::Text(TEXT("[?] unknown"), GetStatusColorUnknown());
	SlateIM::Padding(FMargin(8.0f, 2.0f));
	SlateIM::Text(TEXT("* explicit override (click a cell to cycle inherit/on/off)"), GetStatusColorOverride());
	SlateIM::EndHorizontalStack();
}

bool FMaterialUsageHierarchyTab::PassesFilters(const FMIRow& Row) const
{
	using namespace HierarchyTabPrivate;

	if (!NameFilter.IsEmpty())
	{
		if (!Row.Path.ToString().Contains(NameFilter))
		{
			return false;
		}
	}

	TConstArrayView<const FUsageFlagDesc*> Analyzed = GetAnalyzedUsageFlags();
	for (const FFlagFilter& Filter : Filters)
	{
		const uint32 Bit = MaskBit(Analyzed[Filter.AnalyzedFlagIndex]->Usage);
		bool bPass = false;
		switch (Filter.Condition)
		{
		case Condition_Needs: bPass = (Row.NeededMask() & Bit) != 0; break;
		case Condition_DoesNotNeed: bPass = (Row.NeededMask() & Bit) == 0 && (Row.UnknownMask & Bit) == 0; break;
		case Condition_Wasteful: bPass = (Row.WastefulMask() & Bit) != 0; break;
		case Condition_Missing: bPass = (Row.MissingMask() & Bit) != 0; break;
		case Condition_HasOverride: bPass = (Row.OverrideMask & Bit) != 0 || Row.GetPendingState(Bit) != 0; break;
		default: break;
		}
		if (!bPass)
		{
			return false;
		}
	}
	return true;
}

void FMaterialUsageHierarchyTab::BuildDisplayList(FMaterialUsageAnalysisEngine& Engine)
{
	DisplayIndices.Reset();
	for (int32 RowIndex = 0; RowIndex < Engine.Rows.Num(); ++RowIndex)
	{
		if (PassesFilters(Engine.Rows[RowIndex]))
		{
			DisplayIndices.Add(RowIndex);
		}
	}

	// Positional row identity in SlateIM: keep a stable order across refreshes.
	DisplayIndices.StableSort([&Engine](int32 A, int32 B)
	{
		return Engine.Rows[A].Path.ToString() < Engine.Rows[B].Path.ToString();
	});

	bDisplayDirty = false;
	LastRowCount = Engine.Rows.Num();
}

void FMaterialUsageHierarchyTab::CycleCellState(FMIRow& Row, uint32 Bit)
{
	const int32 CurrentState = Row.GetPendingState(Bit);
	const int32 NextState = (CurrentState + 1) % 3;

	Row.PendingOverrideSetMask &= ~Bit;
	Row.PendingOverrideValueMask &= ~Bit;
	Row.PendingClearOverrideMask &= ~Bit;

	if (NextState == 0)
	{
		// Back to inherit: queue a clear only when a saved override exists.
		if (Row.OverrideMask & Bit)
		{
			Row.PendingClearOverrideMask |= Bit;
		}
	}
	else
	{
		Row.PendingOverrideSetMask |= Bit;
		if (NextState == 1)
		{
			Row.PendingOverrideValueMask |= Bit;
		}
	}
}

void FMaterialUsageHierarchyTab::DrawTable(FMaterialUsageAnalysisEngine& Engine)
{
	if (bDisplayDirty || LastRowCount != Engine.Rows.Num())
	{
		BuildDisplayList(Engine);
	}

	TConstArrayView<const FUsageFlagDesc*> Analyzed = GetAnalyzedUsageFlags();

	SlateIM::Fill();
	SlateIM::HAlign(HAlign_Fill);
	SlateIM::VAlign(VAlign_Fill);
	SlateIM::BeginTable();

	SlateIM::InitialTableColumnWidth(240.0f);
	SlateIM::AddTableColumn(TEXT("Name"));
	SlateIM::InitialTableColumnWidth(330.0f);
	SlateIM::AddTableColumn(TEXT("Path"));
	for (const FUsageFlagDesc* Desc : Analyzed)
	{
		SlateIM::FixedTableColumnWidth(38.0f);
		SlateIM::AddTableColumn(Desc->ShortName);
	}
	SlateIM::FixedTableColumnWidth(44.0f);
	SlateIM::AddTableColumn(TEXT("Perm"));
	SlateIM::FixedTableColumnWidth(40.0f);
	SlateIM::AddTableColumn(TEXT("Lvl"));

	for (const int32 RowIndex : DisplayIndices)
	{
		FMIRow& Row = Engine.Rows[RowIndex];
		const uint32 Needed = Row.NeededMask();
		const uint32 Wasteful = Row.WastefulMask();
		const uint32 Missing = Row.MissingMask();

		if (SlateIM::NextTableCell())
		{
			SlateIM::Padding(FMargin(4.0f, 0.0f));
			SlateIM::SetToolTip(TEXT("Click to browse to this instance in the Content Browser."));
			SlateIM::Text(Row.AssetData.AssetName.ToString(), Row.IsDirty() ? GetStatusColorOverride() : FLinearColor::White);
			if (SlateIM::IsHovered() && SlateIM::IsKeyPressed(EKeys::LeftMouseButton))
			{
				EditorActions::BrowseToAsset(Row.Path);
			}
		}
		if (SlateIM::NextTableCell())
		{
			SlateIM::Padding(FMargin(4.0f, 0.0f));
			SlateIM::Text(Row.AssetData.PackagePath.ToString(), FLinearColor(0.7f, 0.7f, 0.7f));
		}

		for (const FUsageFlagDesc* Desc : Analyzed)
		{
			if (!SlateIM::NextTableCell())
			{
				continue;
			}

			const uint32 Bit = MaskBit(Desc->Usage);
			const int32 PendingState = Row.GetPendingState(Bit);
			const bool bEffectiveOn = PendingState == 1 || (PendingState == 0 && (Row.EffectiveMask & Bit) != 0 && (Row.PendingClearOverrideMask & Bit) == 0);

			FLinearColor Color;
			const TCHAR* Glyph;
			if (Row.UnknownMask & Bit)
			{
				Color = GetStatusColorUnknown();
				Glyph = TEXT("?");
			}
			else if (Missing & Bit)
			{
				Color = GetStatusColorMissing();
				Glyph = TEXT("x");
			}
			else if (Wasteful & Bit)
			{
				Color = GetStatusColorWasteful();
				Glyph = TEXT("o");
			}
			else if ((Needed & Bit) && bEffectiveOn)
			{
				Color = GetStatusColorNeeded();
				Glyph = TEXT("+");
			}
			else
			{
				Color = FLinearColor(0.45f, 0.45f, 0.45f);
				Glyph = TEXT(".");
			}

			const bool bHasOverride = PendingState != 0;
			const bool bPendingEdit = (Row.PendingOverrideSetMask & Bit) != 0 || (Row.PendingClearOverrideMask & Bit) != 0;
			TStringBuilder<8> CellText;
			CellText << Glyph;
			if (bHasOverride)
			{
				CellText << TEXT("*");
			}
			if (bPendingEdit)
			{
				CellText << TEXT("!");
			}

			SlateIM::HAlign(HAlign_Center);
			SlateIM::SetToolTip(FString::Printf(TEXT("%s — effective %s%s. Click to cycle: inherit / override ON / override OFF."),
				Desc->DisplayName,
				bEffectiveOn ? TEXT("ON") : TEXT("OFF"),
				bHasOverride ? TEXT(" (explicit override)") : TEXT("")));
			SlateIM::Text(CellText.ToView(), bHasOverride ? GetStatusColorOverride() : Color);
			if (SlateIM::IsHovered() && SlateIM::IsKeyPressed(EKeys::LeftMouseButton))
			{
				CycleCellState(Row, Bit);
			}
		}

		if (SlateIM::NextTableCell())
		{
			SlateIM::HAlign(HAlign_Center);
			SlateIM::SetToolTip(TEXT("Static permutation: this instance compiles its own shader set."));
			SlateIM::Text(Row.bHasStaticPermutation ? TEXT("yes") : TEXT("-"),
				Row.bHasStaticPermutation ? FLinearColor::White : FLinearColor(0.45f, 0.45f, 0.45f));
		}
		if (SlateIM::NextTableCell())
		{
			SlateIM::HAlign(HAlign_Center);
			SlateIM::SetToolTip(TEXT("Number of referencing level packages."));
			SlateIM::Text(Row.NumLevelReferencers > 0 ? *FString::Printf(TEXT("%d"), Row.NumLevelReferencers) : TEXT("-"),
				FLinearColor(0.7f, 0.7f, 0.7f));
		}
	}

	SlateIM::EndTable();
}

int32 FMaterialUsageHierarchyTab::CountDirtyRows(const FMaterialUsageAnalysisEngine& Engine) const
{
	int32 NumDirty = 0;
	for (const FMIRow& Row : Engine.Rows)
	{
		if (Row.IsDirty())
		{
			NumDirty++;
		}
	}
	return NumDirty;
}

void FMaterialUsageHierarchyTab::SaveAll(FMaterialUsageAnalysisEngine& Engine)
{
	TArray<UPackage*> DirtyPackages;
	int32 NumDirty = CountDirtyRows(Engine);

	FScopedSlowTask SlowTask((float)NumDirty, LOCTEXT("ApplyingOverrides", "Applying usage overrides..."));
	SlowTask.MakeDialog(true);

	for (FMIRow& Row : Engine.Rows)
	{
		if (!Row.IsDirty())
		{
			continue;
		}
		SlowTask.EnterProgressFrame(1.0f);
		if (SlowTask.ShouldCancel())
		{
			break;
		}
		if (UPackage* Package = EditorActions::ApplyRowOverrides(Row))
		{
			DirtyPackages.Add(Package);
		}
	}

	EditorActions::PromptSaveDirtyPackages(DirtyPackages);
	Engine.RebuildParentSummary();
	bDisplayDirty = true;
}

void FMaterialUsageHierarchyTab::RevertAll(FMaterialUsageAnalysisEngine& Engine)
{
	for (FMIRow& Row : Engine.Rows)
	{
		Row.ClearPending();
	}
	bDisplayDirty = true;
}

} // namespace MaterialUsageAnalyzer

#undef LOCTEXT_NAMESPACE
