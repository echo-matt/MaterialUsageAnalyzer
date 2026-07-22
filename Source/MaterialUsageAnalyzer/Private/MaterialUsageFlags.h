// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialInterface.h"

namespace MaterialUsageAnalyzer
{

/** How a flag's "needed" verdict can be derived for a material instance. */
enum class EFlagDetection : uint8
{
	/** Decidable from the class (and registry tags) of referencing assets, no loads. */
	ReferencerClass,
	/** Registry tag when present, referencing-asset load when the tag is missing (old saves). */
	RegistryTagWithFallback,
	/** Requires loading the referencing asset (skeletal mesh clothing, Niagara renderer types). */
	RequiresAssetLoad,
	/** Only provable from level components (or the engine's per-MI auto-record). */
	ComponentDriven,
};

struct FUsageFlagDesc
{
	EMaterialUsage Usage;
	FName PropertyName;
	const TCHAR* ShortName;
	const TCHAR* DisplayName;
	bool bAnalyzed;
	bool bSlow;
	EFlagDetection Detection;
};

inline uint32 MaskBit(EMaterialUsage Usage) { return 1u << (uint32)Usage; }

/** All 23 usage flags of this engine version. Parent panel edits all of these. */
TConstArrayView<FUsageFlagDesc> GetAllUsageFlags();

/** The 12 analyzed grid flags in display order: SM SK CL NA NM NR NS MT IS SP WA HA. */
TConstArrayView<const FUsageFlagDesc*> GetAnalyzedUsageFlags();

const FUsageFlagDesc* FindDescByUsage(EMaterialUsage Usage);

/** Union mask of the analyzed grid flags. */
uint32 GetAnalyzedMask();

/** Flags whose need is only provable via level components when the referencer is a map package. */
uint32 GetComponentDrivenMask();

// Status colors for the per-MI grid.
inline FLinearColor GetStatusColorNeeded()   { return FLinearColor(0.30f, 0.90f, 0.30f); }
inline FLinearColor GetStatusColorWasteful() { return FLinearColor(1.00f, 0.60f, 0.10f); }
inline FLinearColor GetStatusColorMissing()  { return FLinearColor(1.00f, 0.25f, 0.25f); }
inline FLinearColor GetStatusColorUnknown()  { return FLinearColor(0.50f, 0.50f, 0.50f); }
inline FLinearColor GetStatusColorOverride() { return FLinearColor(0.35f, 0.65f, 1.00f); }

} // namespace MaterialUsageAnalyzer
