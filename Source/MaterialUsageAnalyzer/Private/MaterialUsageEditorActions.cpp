// Copyright Epic Games, Inc. All Rights Reserved.

#include "MaterialUsageEditorActions.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "IContentBrowserSingleton.h"
#include "IDesktopPlatform.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace MaterialUsageAnalyzer::EditorActions
{

static IAssetRegistry& GetAssetRegistry()
{
	return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
}

bool GetSelectedParentMaterial(FAssetData& OutMaterial, FString& OutError)
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	const FTopLevelAssetPath MaterialClass = UMaterial::StaticClass()->GetClassPathName();
	IAssetRegistry& AssetRegistry = GetAssetRegistry();

	for (const FAssetData& Selected : SelectedAssets)
	{
		if (Selected.AssetClassPath == MaterialClass)
		{
			OutMaterial = Selected;
			return true;
		}

		// Walk a selected material instance up its Parent chain to the root material.
		FAssetData Current = Selected;
		for (int32 Depth = 0; Depth < 32; ++Depth)
		{
			FString ParentExportPath;
			if (!Current.GetTagValue(FName("Parent"), ParentExportPath) || ParentExportPath.IsEmpty())
			{
				break;
			}
			const FSoftObjectPath ParentPath(FPackageName::ExportTextPathToObjectPath(ParentExportPath));
			Current = AssetRegistry.GetAssetByObjectPath(ParentPath);
			if (!Current.IsValid())
			{
				break;
			}
			if (Current.AssetClassPath == MaterialClass)
			{
				OutMaterial = Current;
				return true;
			}
		}
	}

	OutError = SelectedAssets.Num() == 0
		? TEXT("Nothing selected in the Content Browser.")
		: TEXT("Selection contains no material (or the parent chain could not be resolved).");
	return false;
}

void GetSelectedMaterials(TArray<FAssetData>& OutMaterials)
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	const FTopLevelAssetPath MaterialClass = UMaterial::StaticClass()->GetClassPathName();
	for (const FAssetData& Selected : SelectedAssets)
	{
		if (Selected.AssetClassPath == MaterialClass)
		{
			OutMaterials.Add(Selected);
		}
	}
}

bool GetSelectedMaterialInterface(FAssetData& OutAsset)
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FAssetData> SelectedAssets;
	ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);

	const FTopLevelAssetPath MaterialClass = UMaterial::StaticClass()->GetClassPathName();
	const FTopLevelAssetPath MICClass = UMaterialInstanceConstant::StaticClass()->GetClassPathName();
	const FTopLevelAssetPath MIDClass(TEXT("/Script/Engine"), TEXT("MaterialInstanceDynamic"));

	for (const FAssetData& Selected : SelectedAssets)
	{
		if (Selected.AssetClassPath == MaterialClass
			|| Selected.AssetClassPath == MICClass
			|| Selected.AssetClassPath == MIDClass)
		{
			OutAsset = Selected;
			return true;
		}
	}
	return false;
}

bool PickContentDirectory(FString& OutLongPackagePath)
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform == nullptr)
	{
		return false;
	}

	const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	FString PickedDirectory;
	if (!DesktopPlatform->OpenDirectoryDialog(ParentWindowHandle, TEXT("Pick a content directory"), FPaths::ProjectContentDir(), PickedDirectory))
	{
		return false;
	}

	FString LongPackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(PickedDirectory / TEXT(""), LongPackageName))
	{
		return false;
	}

	OutLongPackagePath = LongPackageName;
	OutLongPackagePath.RemoveFromEnd(TEXT("/"));
	return !OutLongPackagePath.IsEmpty();
}

void GatherMaterialsInPath(const FString& LongPackagePath, bool bRecursive, TArray<FAssetData>& OutMaterials)
{
	IAssetRegistry& AssetRegistry = GetAssetRegistry();
	AssetRegistry.ScanPathsSynchronous({ LongPackagePath });

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssetsByPath(FName(*LongPackagePath), Assets, bRecursive);

	const FTopLevelAssetPath MaterialClass = UMaterial::StaticClass()->GetClassPathName();
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetClassPath == MaterialClass)
		{
			OutMaterials.Add(Asset);
		}
	}
}

UPackage* ApplyParentUsageFlags(const FSoftObjectPath& MaterialPath, uint32 NewMask, uint32 OldMask)
{
	UMaterial* Material = Cast<UMaterial>(MaterialPath.TryLoad());
	if (Material == nullptr || NewMask == OldMask)
	{
		return nullptr;
	}

	Material->Modify();
	Material->PreEditChange(nullptr);

	for (const FUsageFlagDesc& Desc : GetAllUsageFlags())
	{
		const uint32 Bit = MaskBit(Desc.Usage);
		if ((NewMask & Bit) != (OldMask & Bit))
		{
			Material->SetUsageByFlag(Desc.Usage, (NewMask & Bit) != 0);
		}
	}

	// One recompile for all changed flags (PostEditChangePropertyInternal path).
	Material->PostEditChange();
	Material->MarkPackageDirty();
	return Material->GetPackage();
}

UPackage* ApplyRowOverrides(FMIRow& Row)
{
	if (!Row.IsDirty())
	{
		return nullptr;
	}

	UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Row.Path.TryLoad());
	if (Instance == nullptr)
	{
		return nullptr;
	}

	for (const FUsageFlagDesc* Desc : GetAnalyzedUsageFlags())
	{
		const uint32 Bit = MaskBit(Desc->Usage);
		if (Row.PendingClearOverrideMask & Bit)
		{
			UMaterialEditingLibrary::SetMaterialUsageOverride(Instance, Desc->Usage, false, false);
		}
		else if (Row.PendingOverrideSetMask & Bit)
		{
			UMaterialEditingLibrary::SetMaterialUsageOverride(Instance, Desc->Usage, true, (Row.PendingOverrideValueMask & Bit) != 0);
		}
	}

	// Refresh the row's stored state from the live instance.
	Row.OverrideMask = Instance->BasePropertyOverrides.bOverride_UsageFlags;
	Row.EffectiveMask = Instance->GetUsageFlags();
	Row.bFlagsFromRegistry = true;
	Row.ClearPending();

	return Instance->GetPackage();
}

bool PromptSaveDirtyPackages(const TArray<UPackage*>& Packages)
{
	if (Packages.Num() == 0)
	{
		return true;
	}

	TArray<UPackage*> ToSave;
	for (UPackage* Package : Packages)
	{
		if (Package != nullptr)
		{
			ToSave.AddUnique(Package);
		}
	}

	const FEditorFileUtils::EPromptReturnCode Result = FEditorFileUtils::PromptForCheckoutAndSave(ToSave, true /*bCheckDirty*/, true /*bPromptToSave*/);
	return Result != FEditorFileUtils::PR_Cancelled;
}

bool ExportCsv(const FString& SuggestedFileName, const FString& CsvContent)
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform == nullptr)
	{
		return false;
	}

	const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	TArray<FString> OutFilenames;
	if (!DesktopPlatform->SaveFileDialog(ParentWindowHandle, TEXT("Export CSV"), FPaths::ProjectSavedDir(), SuggestedFileName,
		TEXT("CSV Files (*.csv)|*.csv"), EFileDialogFlags::None, OutFilenames) || OutFilenames.Num() == 0)
	{
		return false;
	}

	return FFileHelper::SaveStringToFile(CsvContent, *OutFilenames[0]);
}

void BrowseToAsset(const FSoftObjectPath& AssetPath)
{
	const FAssetData Asset = GetAssetRegistry().GetAssetByObjectPath(AssetPath);
	if (Asset.IsValid())
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		ContentBrowserModule.Get().SyncBrowserToAssets({ Asset });
	}
}

} // namespace MaterialUsageAnalyzer::EditorActions
