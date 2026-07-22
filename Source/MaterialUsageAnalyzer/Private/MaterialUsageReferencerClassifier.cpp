// Copyright Epic Games, Inc. All Rights Reserved.

#include "MaterialUsageReferencerClassifier.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/AssetRegistryInterface.h"

namespace MaterialUsageAnalyzer
{

namespace ClassifierPrivate
{
	static const FTopLevelAssetPath StaticMeshClass(TEXT("/Script/Engine"), TEXT("StaticMesh"));
	static const FTopLevelAssetPath SkeletalMeshClass(TEXT("/Script/Engine"), TEXT("SkeletalMesh"));
	static const FTopLevelAssetPath WorldClass(TEXT("/Script/Engine"), TEXT("World"));
	static const FTopLevelAssetPath NiagaraSystemClass(TEXT("/Script/Niagara"), TEXT("NiagaraSystem"));
	static const FTopLevelAssetPath ParticleSystemClass(TEXT("/Script/Engine"), TEXT("ParticleSystem"));
	static const FTopLevelAssetPath GroomAssetClass(TEXT("/Script/HairStrandsCore"), TEXT("GroomAsset"));
	static const FTopLevelAssetPath GroomBindingClass(TEXT("/Script/HairStrandsCore"), TEXT("GroomBindingAsset"));
	static const FTopLevelAssetPath GeometryCollectionClass(TEXT("/Script/GeometryCollectionEngine"), TEXT("GeometryCollection"));
	static const FTopLevelAssetPath GeometryCacheClass(TEXT("/Script/GeometryCache"), TEXT("GeometryCache"));
	static const FTopLevelAssetPath FoliageTypeISMClass(TEXT("/Script/Foliage"), TEXT("FoliageType_InstancedStaticMesh"));
	static const FTopLevelAssetPath LandscapeGrassTypeClass(TEXT("/Script/Landscape"), TEXT("LandscapeGrassType"));
	static const FTopLevelAssetPath LidarPointCloudClass(TEXT("/Script/LidarPointCloudRuntime"), TEXT("LidarPointCloud"));
	static const FTopLevelAssetPath BlueprintClass(TEXT("/Script/Engine"), TEXT("Blueprint"));
	static const FName NaniteEnabledTag("NaniteEnabled");
	static const FName MorphTargetsTag("MorphTargets");
	static const FName NativeParentClassTag("NativeParentClass");

	/** True when a Blueprint asset's native parent chain is a Packed Level Actor (bakes meshes into ISM/HISM). */
	static bool IsPackedLevelActorBlueprint(const FAssetData& Asset)
	{
		FString NativeParent;
		return Asset.GetTagValue(NativeParentClassTag, NativeParent) && NativeParent.Contains(TEXT("PackedLevelActor"));
	}

	static bool IsMaterialClass(const FTopLevelAssetPath& ClassPath)
	{
		return ClassPath.GetPackageName() == FName("/Script/Engine")
			&& (ClassPath.GetAssetName() == FName("Material")
				|| ClassPath.GetAssetName() == FName("MaterialInstanceConstant")
				|| ClassPath.GetAssetName() == FName("MaterialFunction")
				|| ClassPath.GetAssetName() == FName("MaterialInstanceDynamic"));
	}

	static bool IsLevelPackage(FName PackageName)
	{
		TStringBuilder<256> Builder;
		PackageName.ToString(Builder);
		const FStringView View = Builder.ToView();
		return View.Contains(TEXT("/__ExternalActors__/")) || View.Contains(TEXT("/__ExternalObjects__/"));
	}

	// Second hop: look at what references this static mesh to decide if it is instanced.
	// Cached per static-mesh package because many MIs share the same meshes.
	static EStaticMeshPlacement ResolveStaticMeshPlacement(IAssetRegistry& AssetRegistry, FName StaticMeshPackage,
		TMap<FName, EStaticMeshPlacement>& Cache)
	{
		if (const EStaticMeshPlacement* Cached = Cache.Find(StaticMeshPackage))
		{
			return *Cached;
		}

		EStaticMeshPlacement Result = EStaticMeshPlacement::PlainOrLevel;

		TArray<FName> Referencers;
		AssetRegistry.GetReferencers(StaticMeshPackage, Referencers, UE::AssetRegistry::EDependencyCategory::Package);

		for (const FName ReferencerPackage : Referencers)
		{
			if (Result == EStaticMeshPlacement::InstancedKnown)
			{
				break; // strongest verdict, no need to keep looking
			}

			TArray<FAssetData> Assets;
			AssetRegistry.GetAssetsByPackageName(ReferencerPackage, Assets, true /*bIncludeOnlyOnDiskAssets*/);
			for (const FAssetData& Asset : Assets)
			{
				const FTopLevelAssetPath ClassPath = Asset.AssetClassPath;
				if (ClassPath == FoliageTypeISMClass || ClassPath == LandscapeGrassTypeClass)
				{
					Result = EStaticMeshPlacement::InstancedKnown;
					break;
				}
				if (ClassPath == BlueprintClass)
				{
					if (IsPackedLevelActorBlueprint(Asset))
					{
						Result = EStaticMeshPlacement::InstancedKnown;
						break;
					}
					// A generic Blueprint may add an ISM/HISM component in its construction
					// script - invisible to the registry, so treat ISM as unknowable, not wasteful.
					Result = EStaticMeshPlacement::InstancedMaybe;
				}
			}
		}

		Cache.Add(StaticMeshPackage, Result);
		return Result;
	}
}

uint32 FReferencerClassifier::UsageMaskFromMeshVerdict(const FTopLevelAssetPath& MeshClass, const FMeshVerdict& Verdict)
{
	using namespace ClassifierPrivate;

	uint32 Mask = 0;
	if (MeshClass == StaticMeshClass)
	{
		Mask |= MaskBit(MATUSAGE_StaticMesh);
		if (Verdict.bNaniteKnown && Verdict.bNanite)
		{
			Mask |= MaskBit(MATUSAGE_Nanite);
		}
	}
	else if (MeshClass == SkeletalMeshClass)
	{
		Mask |= MaskBit(MATUSAGE_SkeletalMesh);
		if (Verdict.MorphTargetCount > 0)
		{
			Mask |= MaskBit(MATUSAGE_MorphTargets);
		}
		if (Verdict.bClothingKnown && Verdict.bHasClothing)
		{
			Mask |= MaskBit(MATUSAGE_Clothing);
		}
		if (Verdict.bNaniteKnown && Verdict.bNanite)
		{
			Mask |= MaskBit(MATUSAGE_Nanite);
		}
	}
	return Mask;
}

void FReferencerClassifier::ClassifyReferencers(IAssetRegistry& AssetRegistry, const FMIRow& Row, const FAnalysisSettings& Settings,
	const TMap<FSoftObjectPath, FMeshVerdict>& MeshVerdicts,
	const TMap<FSoftObjectPath, FNiagaraVerdict>& NiagaraVerdicts,
	TMap<FName, EStaticMeshPlacement>& StaticMeshPlacementCache,
	FOutput& Out)
{
	using namespace ClassifierPrivate;

	UE::AssetRegistry::FDependencyQuery Query;
	if (Settings.bExcludeNonCookedReferencers)
	{
		Query.Required = UE::AssetRegistry::EDependencyProperty::Game;
	}

	TArray<FName> ReferencerPackages;
	AssetRegistry.GetReferencers(Row.PackageName, ReferencerPackages, UE::AssetRegistry::EDependencyCategory::Package, Query);

	const uint32 FlagsToCheck = Settings.FlagsToCheckMask;
	const uint32 NiagaraMask = MaskBit(MATUSAGE_NiagaraSprites) | MaskBit(MATUSAGE_NiagaraRibbons) | MaskBit(MATUSAGE_NiagaraMeshParticles);
	const uint32 CascadeMask = MaskBit(MATUSAGE_ParticleSprites) | MaskBit(MATUSAGE_BeamTrails) | MaskBit(MATUSAGE_MeshParticles);

	for (const FName ReferencerPackage : ReferencerPackages)
	{
		TStringBuilder<256> PackageString;
		ReferencerPackage.ToString(PackageString);

		bool bExcluded = false;
		for (const FString& Prefix : Settings.ExcludeDirPrefixes)
		{
			if (!Prefix.IsEmpty() && FStringView(PackageString).StartsWith(Prefix))
			{
				bExcluded = true;
				break;
			}
		}
		if (bExcluded)
		{
			continue;
		}

		if (IsLevelPackage(ReferencerPackage))
		{
			Out.NumLevelReferencers++;
			Out.LevelPackages.AddUnique(ReferencerPackage);
			continue;
		}

		TArray<FAssetData> ReferencerAssets;
		AssetRegistry.GetAssetsByPackageName(ReferencerPackage, ReferencerAssets, true /*bIncludeOnlyOnDiskAssets*/);

		if (ReferencerAssets.Num() == 0)
		{
			// Package with no registry assets (script package etc.) — record as other.
			Out.OtherReferencers.Add({ ReferencerPackage, FTopLevelAssetPath() });
			continue;
		}

		for (const FAssetData& Referencer : ReferencerAssets)
		{
			const FTopLevelAssetPath ClassPath = Referencer.AssetClassPath;
			const FName ClassPackage = ClassPath.GetPackageName();

			if (ClassPath == StaticMeshClass)
			{
				Out.NeededMask |= MaskBit(MATUSAGE_StaticMesh);

				// The static mesh only tells us "static mesh". Whether it is *instanced*
				// (Packed Level Actor, foliage, ISM Blueprint) lives one hop further out.
				if ((FlagsToCheck & MaskBit(MATUSAGE_InstancedStaticMeshes))
					&& (Out.NeededMask & MaskBit(MATUSAGE_InstancedStaticMeshes)) == 0)
				{
					switch (ResolveStaticMeshPlacement(AssetRegistry, ReferencerPackage, StaticMeshPlacementCache))
					{
					case EStaticMeshPlacement::InstancedKnown:
						Out.NeededMask |= MaskBit(MATUSAGE_InstancedStaticMeshes);
						break;
					case EStaticMeshPlacement::InstancedMaybe:
						Out.UnknownMask |= MaskBit(MATUSAGE_InstancedStaticMeshes);
						break;
					default:
						break;
					}
				}

				FString NaniteTag;
				if (Referencer.GetTagValue(NaniteEnabledTag, NaniteTag))
				{
					if (NaniteTag == TEXT("True"))
					{
						Out.NeededMask |= MaskBit(MATUSAGE_Nanite);
					}
				}
				else if (FlagsToCheck & MaskBit(MATUSAGE_Nanite))
				{
					// Old save without the tag: needs a load to know.
					const FMeshVerdict* Cached = MeshVerdicts.Find(Referencer.GetSoftObjectPath());
					if (Cached && Cached->bNaniteKnown)
					{
						if (Cached->bNanite)
						{
							Out.NeededMask |= MaskBit(MATUSAGE_Nanite);
						}
					}
					else
					{
						Out.SlowDependencies.Emplace(ESlowItemKind::StaticMeshNanite, Referencer.GetSoftObjectPath());
					}
				}
				else
				{
					Out.UnknownMask |= MaskBit(MATUSAGE_Nanite);
				}
			}
			else if (ClassPath == SkeletalMeshClass)
			{
				Out.NeededMask |= MaskBit(MATUSAGE_SkeletalMesh);

				int32 MorphCount = 0;
				if (Referencer.GetTagValue(MorphTargetsTag, MorphCount) && MorphCount > 0)
				{
					Out.NeededMask |= MaskBit(MATUSAGE_MorphTargets);
				}

				FString NaniteTag;
				if (Referencer.GetTagValue(NaniteEnabledTag, NaniteTag) && NaniteTag == TEXT("True"))
				{
					Out.NeededMask |= MaskBit(MATUSAGE_Nanite);
				}

				if (FlagsToCheck & MaskBit(MATUSAGE_Clothing))
				{
					const FMeshVerdict* Cached = MeshVerdicts.Find(Referencer.GetSoftObjectPath());
					if (Cached && Cached->bClothingKnown)
					{
						if (Cached->bHasClothing)
						{
							Out.NeededMask |= MaskBit(MATUSAGE_Clothing);
						}
					}
					else
					{
						Out.SlowDependencies.Emplace(ESlowItemKind::SkeletalMesh, Referencer.GetSoftObjectPath());
					}
				}
				else
				{
					Out.UnknownMask |= MaskBit(MATUSAGE_Clothing);
				}
			}
			else if (ClassPath == NiagaraSystemClass)
			{
				if (FlagsToCheck & NiagaraMask)
				{
					if (const FNiagaraVerdict* Cached = NiagaraVerdicts.Find(Referencer.GetSoftObjectPath()))
					{
						if (const uint32* UsageForMaterial = Cached->MaterialUsage.Find(Row.Path))
						{
							Out.NeededMask |= *UsageForMaterial;
						}
					}
					else
					{
						Out.SlowDependencies.Emplace(ESlowItemKind::NiagaraSystem, Referencer.GetSoftObjectPath());
					}
				}
				else
				{
					// Not resolving renderer types: can't call these flags wasteful.
					Out.UnknownMask |= NiagaraMask;
				}
			}
			else if (ClassPath == ParticleSystemClass)
			{
				// Legacy Cascade: needs a load to know sprite/beam/mesh emitters. Not part of the
				// analyzed grid, but resolve so parent-panel data is correct when requested.
				if (FlagsToCheck & CascadeMask)
				{
					Out.SlowDependencies.Emplace(ESlowItemKind::ParticleSystem, Referencer.GetSoftObjectPath());
				}
				else
				{
					Out.UnknownMask |= CascadeMask;
				}
			}
			else if (ClassPath == WorldClass)
			{
				Out.NumLevelReferencers++;
				Out.LevelPackages.AddUnique(ReferencerPackage);
			}
			else if (ClassPath == GroomAssetClass || ClassPath == GroomBindingClass)
			{
				Out.NeededMask |= MaskBit(MATUSAGE_HairStrands);
			}
			else if (ClassPath == FoliageTypeISMClass || ClassPath == LandscapeGrassTypeClass)
			{
				Out.NeededMask |= MaskBit(MATUSAGE_InstancedStaticMeshes) | MaskBit(MATUSAGE_StaticMesh);
			}
			else if (ClassPath == GeometryCollectionClass)
			{
				Out.NeededMask |= MaskBit(MATUSAGE_GeometryCollections);
			}
			else if (ClassPath == GeometryCacheClass)
			{
				Out.NeededMask |= MaskBit(MATUSAGE_GeometryCache);
			}
			else if (ClassPath == LidarPointCloudClass)
			{
				Out.NeededMask |= MaskBit(MATUSAGE_LidarPointCloud);
			}
			else if (ClassPackage == FName("/Script/Water"))
			{
				Out.NeededMask |= MaskBit(MATUSAGE_Water);
			}
			else if (ClassPath == BlueprintClass)
				{
					// Material directly referenced by a Blueprint (e.g. an override material on a
					// Packed Level Actor's instanced component).
					if (IsPackedLevelActorBlueprint(Referencer))
					{
						Out.NeededMask |= MaskBit(MATUSAGE_InstancedStaticMeshes) | MaskBit(MATUSAGE_StaticMesh);
					}
					else
					{
						// A construction script can place this material on any component type -
						// don't call component-driven flags wasteful when we can't see the components.
						Out.UnknownMask |= GetComponentDrivenMask();
						Out.OtherReferencers.Add({ ReferencerPackage, ClassPath });
					}
				}
				else if (IsMaterialClass(ClassPath))
			{
				// Child instances / sibling materials: not a usage signal.
				// (Blueprint / Packed Level Actor referencers handled above.)
			}
			else
			{
				Out.OtherReferencers.Add({ ReferencerPackage, ClassPath });
			}
		}
	}

	// Level referencers leave component-driven flags unknowable without a deep scan / recorded overrides.
	if (Out.NumLevelReferencers > 0 && !Settings.bDeepScanLevels)
	{
		Out.UnknownMask |= GetComponentDrivenMask();
	}
}

} // namespace MaterialUsageAnalyzer
