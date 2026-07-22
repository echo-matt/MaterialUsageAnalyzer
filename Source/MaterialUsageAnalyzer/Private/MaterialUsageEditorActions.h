// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AssetRegistry/AssetData.h"
#include "CoreMinimal.h"
#include "MaterialUsageAnalysisEngine.h"

class UPackage;

namespace MaterialUsageAnalyzer
{

/** Editor-side actions: content browser pickers, flag/override writes, batch save, CSV export. */
namespace EditorActions
{
	/**
	 * Resolve the current content browser selection to a root UMaterial.
	 * A selected material instance walks its Parent chain up to the root material.
	 * @return true when a root material was found.
	 */
	bool GetSelectedParentMaterial(FAssetData& OutMaterial, FString& OutError);

	/** All selected UMaterial assets (instances are ignored). */
	void GetSelectedMaterials(TArray<FAssetData>& OutMaterials);

	/** First selected material or material instance, for single-asset graph analysis. @return true if found. */
	bool GetSelectedMaterialInterface(FAssetData& OutAsset);

	/** Directory picker constrained to project/engine content; returns a /Game style package path. */
	bool PickContentDirectory(FString& OutLongPackagePath);

	/** All UMaterial assets under the package path. */
	void GatherMaterialsInPath(const FString& LongPackagePath, bool bRecursive, TArray<FAssetData>& OutMaterials);

	/**
	 * Write the parent material's bUsedWith* flags to match NewMask (PostEditChange path: recompiles shaders).
	 * @return the dirtied package, or nullptr on failure.
	 */
	UPackage* ApplyParentUsageFlags(const FSoftObjectPath& MaterialPath, uint32 NewMask, uint32 OldMask);

	/**
	 * Apply a row's pending per-MI usage overrides via UMaterialEditingLibrary::SetMaterialUsageOverride.
	 * Updates the row's stored masks on success and clears its pending state.
	 * @return the dirtied package, or nullptr on failure.
	 */
	UPackage* ApplyRowOverrides(FMIRow& Row);

	/** Checkout + save prompt for the given packages. @return true unless the user cancelled. */
	bool PromptSaveDirtyPackages(const TArray<UPackage*>& Packages);

	/** Save-file dialog + write. @return true when written. */
	bool ExportCsv(const FString& SuggestedFileName, const FString& CsvContent);

	/** Focus the content browser on an asset. */
	void BrowseToAsset(const FSoftObjectPath& AssetPath);
}

} // namespace MaterialUsageAnalyzer
