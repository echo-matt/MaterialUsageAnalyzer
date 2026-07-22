// Copyright Epic Games, Inc. All Rights Reserved.

#include "MaterialUsageBatchTab.h"

#include "InputCoreTypes.h"
#include "MaterialUsageEditorActions.h"
#include "Misc/PackageName.h"
#include "SlateIM.h"

namespace MaterialUsageAnalyzer
{

FMaterialUsageBatchTab::FMaterialUsageBatchTab()
{
	FlagChecked.Init(true, GetAnalyzedUsageFlags().Num());
}

void FMaterialUsageBatchTab::AddMaterials(const TArray<FAssetData>& NewMaterials)
{
	int32 NumAdded = 0;
	for (const FAssetData& Material : NewMaterials)
	{
		const bool bAlreadyPresent = Materials.ContainsByPredicate([&Material](const FAssetData& Existing)
		{
			return Existing.GetSoftObjectPath() == Material.GetSoftObjectPath();
		});
		if (!bAlreadyPresent)
		{
			Materials.Add(Material);
			NumAdded++;
		}
	}
	StatusMessage = FString::Printf(TEXT("Added %d materials (%d total)."), NumAdded, Materials.Num());
}

void FMaterialUsageBatchTab::Draw(FMaterialUsageAnalysisEngine& Engine)
{
	DrawInputToolbar(Engine);
	DrawFlagsToCheck();
	DrawRunRow(Engine);

	if (!StatusMessage.IsEmpty())
	{
		SlateIM::Padding(FMargin(6.0f, 2.0f));
		SlateIM::Text(StatusMessage, FLinearColor(0.7f, 0.7f, 0.7f));
	}

	DrawTable(Engine);
}

void FMaterialUsageBatchTab::DrawInputToolbar(FMaterialUsageAnalysisEngine& Engine)
{
	SlateIM::BeginHorizontalStack();

	SlateIM::Padding(FMargin(4.0f, 4.0f, 2.0f, 2.0f));
	SlateIM::SetToolTip(TEXT("Adds every UMaterial selected in the Content Browser (instances are ignored)."));
	if (SlateIM::Button(TEXT("Add Selected Materials from Content Browser")))
	{
		TArray<FAssetData> Selected;
		EditorActions::GetSelectedMaterials(Selected);
		if (Selected.Num() > 0)
		{
			AddMaterials(Selected);
		}
		else
		{
			StatusMessage = TEXT("No materials selected in the Content Browser (instances don't count here).");
		}
	}

	SlateIM::Padding(FMargin(2.0f, 4.0f, 2.0f, 2.0f));
	if (SlateIM::Button(TEXT("Add Materials from Directory...")))
	{
		// Deferred: the native directory dialog pumps Slate (ExternalModalStart) and a
		// synchronous registry scan can pop a slow-task dialog - both re-enter the SlateIM root.
		Engine.Defer([this, bRecursive = bRecursiveDirectoryScan]()
		{
			FString LongPackagePath;
			if (EditorActions::PickContentDirectory(LongPackagePath))
			{
				TArray<FAssetData> Found;
				EditorActions::GatherMaterialsInPath(LongPackagePath, bRecursive, Found);
				AddMaterials(Found);
			}
		});
	}

	SlateIM::Padding(FMargin(2.0f, 6.0f, 2.0f, 2.0f));
	SlateIM::CheckBox(TEXT("Recursive"), bRecursiveDirectoryScan);

	SlateIM::Padding(FMargin(12.0f, 4.0f, 2.0f, 2.0f));
	if (SlateIM::Button(TEXT("Remove Selected")) && SelectedForRemoval.Num() > 0)
	{
		Materials.RemoveAll([this](const FAssetData& Material)
		{
			return SelectedForRemoval.Contains(Material.GetSoftObjectPath());
		});
		SelectedForRemoval.Reset();
	}

	SlateIM::Padding(FMargin(2.0f, 4.0f, 2.0f, 2.0f));
	if (SlateIM::Button(TEXT("Clear All")))
	{
		Materials.Reset();
		SelectedForRemoval.Reset();
		Engine.BatchRows.Reset();
	}

	SlateIM::Padding(FMargin(12.0f, 6.0f, 2.0f, 2.0f));
	SlateIM::VAlign(VAlign_Center);
	SlateIM::Text(FString::Printf(TEXT("Materials: %d"), Materials.Num()));

	SlateIM::EndHorizontalStack();
}

void FMaterialUsageBatchTab::DrawFlagsToCheck()
{
	SlateIM::BeginHorizontalStack();
	SlateIM::Padding(FMargin(4.0f, 2.0f));
	SlateIM::Text(TEXT("Flags to Check:"));
	SlateIM::Padding(FMargin(6.0f, 2.0f, 2.0f, 2.0f));
	if (SlateIM::Button(TEXT("Select All")))
	{
		for (bool& bChecked : FlagChecked)
		{
			bChecked = true;
		}
	}
	SlateIM::Padding(FMargin(2.0f));
	if (SlateIM::Button(TEXT("Select None")))
	{
		for (bool& bChecked : FlagChecked)
		{
			bChecked = false;
		}
	}
	SlateIM::Padding(FMargin(12.0f, 2.0f, 2.0f, 2.0f));
	SlateIM::VAlign(VAlign_Center);
	SlateIM::Text(TEXT("Normal = fast."));
	SlateIM::Padding(FMargin(4.0f, 2.0f, 2.0f, 2.0f));
	SlateIM::VAlign(VAlign_Center);
	SlateIM::Text(TEXT("Orange = slow (loads assets)."), GetStatusColorWasteful());
	SlateIM::EndHorizontalStack();

	SlateIM::BeginHorizontalWrap();
	TConstArrayView<const FUsageFlagDesc*> Analyzed = GetAnalyzedUsageFlags();
	for (int32 FlagIndex = 0; FlagIndex < Analyzed.Num(); ++FlagIndex)
	{
		const FUsageFlagDesc* Desc = Analyzed[FlagIndex];
		SlateIM::Padding(FMargin(6.0f, 2.0f));
		if (Desc->bSlow)
		{
			SlateIM::SetToolTip(FString::Printf(TEXT("%s requires loading referencing assets to resolve exactly."), Desc->DisplayName));
		}
		bool bChecked = FlagChecked[FlagIndex];
		if (SlateIM::CheckBox(Desc->DisplayName, bChecked))
		{
			FlagChecked[FlagIndex] = bChecked;
		}
		if (Desc->bSlow)
		{
			SlateIM::Padding(FMargin(0.0f, 2.0f, 6.0f, 2.0f));
			SlateIM::VAlign(VAlign_Center);
			SlateIM::Text(TEXT("(slow)"), GetStatusColorWasteful());
		}
	}
	SlateIM::EndHorizontalWrap();
}

void FMaterialUsageBatchTab::StartAnalysis(FMaterialUsageAnalysisEngine& Engine)
{
	FAnalysisSettings Settings;
	Settings.bExcludeNonCookedReferencers = bExcludeNonCookedReferencers;
	Settings.bDeepScanLevels = false;

	TConstArrayView<const FUsageFlagDesc*> Analyzed = GetAnalyzedUsageFlags();
	Settings.FlagsToCheckMask = 0;
	for (int32 FlagIndex = 0; FlagIndex < Analyzed.Num(); ++FlagIndex)
	{
		if (FlagChecked[FlagIndex])
		{
			Settings.FlagsToCheckMask |= MaskBit(Analyzed[FlagIndex]->Usage);
		}
	}

	ExcludeDirsText.ParseIntoArray(Settings.ExcludeDirPrefixes, TEXT(","), true);
	for (FString& Prefix : Settings.ExcludeDirPrefixes)
	{
		Prefix.TrimStartAndEndInline();
	}

	Engine.StartBatchSession(Materials, Settings);
	StatusMessage.Reset();
}

void FMaterialUsageBatchTab::DrawRunRow(FMaterialUsageAnalysisEngine& Engine)
{
	SlateIM::BeginHorizontalStack();

	SlateIM::Padding(FMargin(4.0f, 2.0f));
	SlateIM::CheckBox(TEXT("Exclude non-cooked referencers"), bExcludeNonCookedReferencers);

	SlateIM::Padding(FMargin(8.0f, 2.0f, 2.0f, 2.0f));
	SlateIM::VAlign(VAlign_Center);
	SlateIM::Text(TEXT("Exclude dirs:"));
	SlateIM::Padding(FMargin(2.0f));
	SlateIM::MinWidth(220.0f);
	SlateIM::EditableText(ExcludeDirsText, TEXT("/Game/Developers"));

	SlateIM::Padding(FMargin(12.0f, 2.0f, 2.0f, 2.0f));
	if (SlateIM::Button(TEXT("Analyze Materials")) && Materials.Num() > 0 && !Engine.IsBusy())
	{
		StartAnalysis(Engine);
	}

	const bool bBatchReady = Engine.GetSessionKind() == ESessionKind::Batch && Engine.HasResults() && Engine.BatchRows.Num() > 0;
	if (bBatchReady)
	{
		SlateIM::Padding(FMargin(2.0f));
		if (SlateIM::Button(TEXT("Export to CSV")))
		{
			// Build the CSV now (pure), but defer the save dialog: the native file dialog
			// pumps Slate (ExternalModalStart) and re-enters the SlateIM root if run in-draw.
			const FString Csv = BuildCsv(Engine);
			Engine.Defer([this, Csv]()
			{
				StatusMessage = EditorActions::ExportCsv(TEXT("MaterialUsageAnalysis.csv"), Csv)
					? TEXT("CSV exported.")
					: TEXT("CSV export cancelled.");
			});
		}
	}

	SlateIM::EndHorizontalStack();
}

void FMaterialUsageBatchTab::DrawTable(FMaterialUsageAnalysisEngine& Engine)
{
	TConstArrayView<const FUsageFlagDesc*> Analyzed = GetAnalyzedUsageFlags();
	const bool bShowResults = Engine.GetSessionKind() == ESessionKind::Batch && Engine.BatchRows.Num() > 0;

	SlateIM::Fill();
	SlateIM::HAlign(HAlign_Fill);
	SlateIM::VAlign(VAlign_Fill);
	SlateIM::BeginTable();

	SlateIM::FixedTableColumnWidth(36.0f);
	SlateIM::AddTableColumn(TEXT("Sel"));
	SlateIM::FixedTableColumnWidth(76.0f);
	SlateIM::AddTableColumn(TEXT("Status"));
	SlateIM::InitialTableColumnWidth(220.0f);
	SlateIM::AddTableColumn(TEXT("Material Name"));
	SlateIM::InitialTableColumnWidth(280.0f);
	SlateIM::AddTableColumn(TEXT("Material Path"));
	SlateIM::FixedTableColumnWidth(56.0f);
	SlateIM::AddTableColumn(TEXT("# MIs"));
	SlateIM::FixedTableColumnWidth(56.0f);
	SlateIM::AddTableColumn(TEXT("Perms"));
	SlateIM::FixedTableColumnWidth(100.0f);
	SlateIM::AddTableColumn(TEXT("Unnecessary"));
	for (const FUsageFlagDesc* Desc : Analyzed)
	{
		SlateIM::FixedTableColumnWidth(38.0f);
		SlateIM::AddTableColumn(Desc->ShortName);
	}

	if (bShowResults)
	{
		for (const FBatchRow& Row : Engine.BatchRows)
		{
			if (SlateIM::NextTableCell())
			{
				// Selection isn't meaningful on result rows.
				SlateIM::Text(TEXT(""));
			}
			if (SlateIM::NextTableCell())
			{
				const TCHAR* StatusText = TEXT("Pending");
				FLinearColor StatusColor = GetStatusColorUnknown();
				switch (Row.Status)
				{
				case EBatchStatus::Analyzing: StatusText = TEXT("Analyzing"); StatusColor = FLinearColor(0.5f, 0.7f, 1.0f); break;
				case EBatchStatus::Complete: StatusText = TEXT("Complete"); StatusColor = GetStatusColorNeeded(); break;
				case EBatchStatus::Failed: StatusText = TEXT("Failed"); StatusColor = GetStatusColorMissing(); break;
				default: break;
				}
				SlateIM::Text(StatusText, StatusColor);
			}
			if (SlateIM::NextTableCell())
			{
				SlateIM::Padding(FMargin(4.0f, 0.0f));
				SlateIM::SetToolTip(TEXT("Click to browse to this material."));
				SlateIM::Text(Row.Name);
				if (SlateIM::IsHovered() && SlateIM::IsKeyPressed(EKeys::LeftMouseButton))
				{
					EditorActions::BrowseToAsset(Row.MaterialPath);
				}
			}
			if (SlateIM::NextTableCell())
			{
				SlateIM::Padding(FMargin(4.0f, 0.0f));
				SlateIM::Text(FPackageName::GetLongPackagePath(Row.MaterialPath.GetLongPackageName()), FLinearColor(0.7f, 0.7f, 0.7f));
			}
			if (SlateIM::NextTableCell())
			{
				SlateIM::HAlign(HAlign_Center);
				SlateIM::Text(FString::Printf(TEXT("%d"), Row.NumMIs));
			}
			if (SlateIM::NextTableCell())
			{
				SlateIM::HAlign(HAlign_Center);
				SlateIM::Text(FString::Printf(TEXT("%d"), Row.NumStaticPermutations));
			}
			if (SlateIM::NextTableCell())
			{
				SlateIM::HAlign(HAlign_Center);
				SlateIM::Text(FString::Printf(TEXT("%d"), Row.TotalUnnecessary),
					Row.TotalUnnecessary > 0 ? GetStatusColorWasteful() : GetStatusColorNeeded());
			}
			for (const FUsageFlagDesc* Desc : Analyzed)
			{
				if (SlateIM::NextTableCell())
				{
					const int32 Count = Row.UnnecessaryPerFlag[(uint32)Desc->Usage];
					SlateIM::HAlign(HAlign_Center);
					if ((Row.EnabledMask & MaskBit(Desc->Usage)) == 0 && Count == 0)
					{
						SlateIM::Text(TEXT("-"), FLinearColor(0.4f, 0.4f, 0.4f));
					}
					else
					{
						SlateIM::Text(FString::Printf(TEXT("%d"), Count), Count > 0 ? GetStatusColorWasteful() : FLinearColor(0.7f, 0.7f, 0.7f));
					}
				}
			}
		}
	}
	else
	{
		// Pending input list with removal selection.
		TArray<FAssetData> Sorted = Materials;
		Sorted.StableSort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetObjectPathString() < B.GetObjectPathString();
		});

		for (const FAssetData& Material : Sorted)
		{
			const FSoftObjectPath Path = Material.GetSoftObjectPath();
			if (SlateIM::NextTableCell())
			{
				bool bSelected = SelectedForRemoval.Contains(Path);
				SlateIM::HAlign(HAlign_Center);
				if (SlateIM::CheckBox(TEXT(""), bSelected))
				{
					if (bSelected)
					{
						SelectedForRemoval.Add(Path);
					}
					else
					{
						SelectedForRemoval.Remove(Path);
					}
				}
			}
			if (SlateIM::NextTableCell())
			{
				SlateIM::Text(TEXT("Pending"), GetStatusColorUnknown());
			}
			if (SlateIM::NextTableCell())
			{
				SlateIM::Padding(FMargin(4.0f, 0.0f));
				SlateIM::Text(Material.AssetName.ToString());
			}
			if (SlateIM::NextTableCell())
			{
				SlateIM::Padding(FMargin(4.0f, 0.0f));
				SlateIM::Text(Material.PackagePath.ToString(), FLinearColor(0.7f, 0.7f, 0.7f));
			}
			if (SlateIM::NextTableCell())
			{
				SlateIM::Text(TEXT(""));
			}
			if (SlateIM::NextTableCell())
			{
				SlateIM::Text(TEXT(""));
			}
			if (SlateIM::NextTableCell())
			{
				SlateIM::Text(TEXT(""));
			}
			for (int32 FlagIndex = 0; FlagIndex < Analyzed.Num(); ++FlagIndex)
			{
				if (SlateIM::NextTableCell())
				{
					SlateIM::Text(TEXT(""));
				}
			}
		}
	}

	SlateIM::EndTable();
}

FString FMaterialUsageBatchTab::BuildCsv(const FMaterialUsageAnalysisEngine& Engine) const
{
	TConstArrayView<const FUsageFlagDesc*> Analyzed = GetAnalyzedUsageFlags();

	TStringBuilder<4096> Csv;
	Csv << TEXT("Status,MaterialName,MaterialPath,NumMIs,StaticPermutations,TotalUnnecessary");
	for (const FUsageFlagDesc* Desc : Analyzed)
	{
		Csv << TEXT(",Unnecessary_") << Desc->DisplayName;
	}
	for (const FUsageFlagDesc* Desc : Analyzed)
	{
		Csv << TEXT(",Missing_") << Desc->DisplayName;
	}
	Csv << TEXT("\n");

	for (const FBatchRow& Row : Engine.BatchRows)
	{
		const TCHAR* StatusText = TEXT("Pending");
		switch (Row.Status)
		{
		case EBatchStatus::Analyzing: StatusText = TEXT("Analyzing"); break;
		case EBatchStatus::Complete: StatusText = TEXT("Complete"); break;
		case EBatchStatus::Failed: StatusText = TEXT("Failed"); break;
		default: break;
		}

		Csv << StatusText << TEXT(",");
		Csv << Row.Name << TEXT(",");
		Csv << Row.MaterialPath.ToString() << TEXT(",");
		Csv << Row.NumMIs << TEXT(",");
		Csv << Row.NumStaticPermutations << TEXT(",");
		Csv << Row.TotalUnnecessary;
		for (const FUsageFlagDesc* Desc : Analyzed)
		{
			Csv << TEXT(",") << Row.UnnecessaryPerFlag[(uint32)Desc->Usage];
		}
		for (const FUsageFlagDesc* Desc : Analyzed)
		{
			Csv << TEXT(",") << Row.MissingPerFlag[(uint32)Desc->Usage];
		}
		Csv << TEXT("\n");
	}

	return Csv.ToString();
}

} // namespace MaterialUsageAnalyzer
