// Copyright Epic Games, Inc. All Rights Reserved.

#include "MaterialUsageGraphAnalysis.h"

#include "MaterialStatsCommon.h"
#include "MaterialShared.h"
#include "RHI.h"

#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialLayersFunctions.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionStaticSwitch.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Engine/Texture.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/StrongObjectPtr.h"

// The stats extraction fills the engine-private FShaderStatsInfo (MaterialStats.h). We can't
// include that private header, so we mirror its data layout below and reinterpret_cast into it.
struct FShaderStatsInfo;

namespace MaterialUsageAnalyzer
{

namespace GraphPrivate
{
	// Byte-compatible mirror of the engine-private FShaderStatsInfo. Field order/types MUST match
	// Engine/Source/Editor/MaterialEditor/Private/MaterialStats.h so ExtractMatertialStatsInfo can fill it.
	struct FShaderStatsInfoMirror
	{
		struct FContent
		{
			FString StrDescription;
			FString StrDescriptionLong;
		};

		TMap<ERepresentativeShader, FContent> ShaderInstructionCount;
		TMap<ERepresentativeShader, FContent> GenericShaderStatistics;
		FContent SamplersCount;
		FContent InterpolatorsCount;
		FContent TextureSampleCount;
		FContent VirtualTextureLookupCount;
		FContent VirtualTextureStackCount;
		FContent ShaderCount;
		FContent PreShaderCount;
		FContent LWCUsage;
		FString StrShaderErrors;
	};

	static const TCHAR* RepresentativeShaderName(ERepresentativeShader Shader)
	{
		switch (Shader)
		{
		case ERepresentativeShader::StationarySurface:          return TEXT("Base pass (stationary)");
		case ERepresentativeShader::StationarySurfaceCSM:       return TEXT("Base pass + CSM");
		case ERepresentativeShader::StationarySurfaceNPointLights: return TEXT("Base pass + point lights");
		case ERepresentativeShader::DynamicallyLitObject:       return TEXT("Base pass (dynamic)");
		case ERepresentativeShader::RuntimeVirtualTextureOutput: return TEXT("Runtime virtual texture");
		case ERepresentativeShader::UIDefaultFragmentShader:    return TEXT("UI pixel");
		case ERepresentativeShader::StaticMesh:                 return TEXT("Vertex (static mesh)");
		case ERepresentativeShader::SkeletalMesh:               return TEXT("Vertex (skeletal mesh)");
		case ERepresentativeShader::SkinnedCloth:               return TEXT("Vertex (cloth)");
		case ERepresentativeShader::UIDefaultVertexShader:      return TEXT("UI vertex");
		case ERepresentativeShader::UIInstancedVertexShader:    return TEXT("UI vertex (instanced)");
		case ERepresentativeShader::NaniteMesh:                 return TEXT("Nanite");
		default:                                                return TEXT("Shader");
		}
	}

	static FString BlendModeToString(EBlendMode Mode)
	{
		switch (Mode)
		{
		case BLEND_Opaque:          return TEXT("Opaque");
		case BLEND_Masked:          return TEXT("Masked");
		case BLEND_Translucent:     return TEXT("Translucent");
		case BLEND_Additive:        return TEXT("Additive");
		case BLEND_Modulate:        return TEXT("Modulate");
		case BLEND_AlphaComposite:  return TEXT("AlphaComposite");
		case BLEND_AlphaHoldout:    return TEXT("AlphaHoldout");
		default:                    return FString::Printf(TEXT("Blend(%d)"), (int32)Mode);
		}
	}

	static const TCHAR* ShadingModelToString(EMaterialShadingModel Model)
	{
		switch (Model)
		{
		case MSM_Unlit:             return TEXT("Unlit");
		case MSM_DefaultLit:        return TEXT("DefaultLit");
		case MSM_Subsurface:        return TEXT("Subsurface");
		case MSM_PreintegratedSkin: return TEXT("PreintegratedSkin");
		case MSM_ClearCoat:         return TEXT("ClearCoat");
		case MSM_SubsurfaceProfile: return TEXT("SubsurfaceProfile");
		case MSM_TwoSidedFoliage:   return TEXT("TwoSidedFoliage");
		case MSM_Hair:              return TEXT("Hair");
		case MSM_Cloth:             return TEXT("Cloth");
		case MSM_Eye:               return TEXT("Eye");
		case MSM_SingleLayerWater:  return TEXT("SingleLayerWater");
		case MSM_ThinTranslucent:   return TEXT("ThinTranslucent");
		default:                    return TEXT("Other");
		}
	}

	static bool IsSubsurfaceModel(EMaterialShadingModel Model)
	{
		return Model == MSM_Subsurface || Model == MSM_PreintegratedSkin
			|| Model == MSM_SubsurfaceProfile || Model == MSM_TwoSidedFoliage
			|| Model == MSM_Eye;
	}

	// Shared walk state, threaded through the census recursion.
	struct FWalkContext
	{
		TSet<const UMaterialFunctionInterface*> VisitedFns;
		TSet<const UTexture*> SeenTextures;
		TArray<UMaterialExpression*> SwitchNodes; // static switch (parameter + inline) nodes for deep analysis
	};

	// Recursively census one expression list. VisitedFns guards against function-graph cycles.
	static void WalkExpressions(TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions,
		FWalkContext& Context, FGraphAnalysisResult& Out, int32 Depth);

	static void WalkFunction(UMaterialFunctionInterface* Function, FWalkContext& Context, FGraphAnalysisResult& Out, int32 Depth)
	{
		if (Function == nullptr || Depth > 32 || Context.VisitedFns.Contains(Function))
		{
			return;
		}
		Context.VisitedFns.Add(Function);
		WalkExpressions(Function->GetExpressions(), Context, Out, Depth + 1);
	}

	static void WalkExpressions(TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions,
		FWalkContext& Context, FGraphAnalysisResult& Out, int32 Depth)
	{
		if (Depth > 64)
		{
			return;
		}

		for (UMaterialExpression* Expression : Expressions)
		{
			if (Expression == nullptr)
			{
				continue;
			}
			Out.TotalExpressions++;

			if (const UMaterialExpressionTextureSample* Tex = Cast<UMaterialExpressionTextureSample>(Expression))
			{
				Out.TextureSampleNodes++;
				if (const UTexture* Texture = Tex->Texture)
				{
					if (Context.SeenTextures.Contains(Texture))
					{
						Out.DuplicateTextureSamples++;
					}
					else
					{
						Context.SeenTextures.Add(Texture);
					}
				}
			}
			else if (Expression->IsA<UMaterialExpressionStaticSwitchParameter>())
			{
				Out.StaticSwitchNodes++;
				Context.SwitchNodes.Add(Expression);
			}
			else if (Expression->IsA<UMaterialExpressionStaticSwitch>())
			{
				Out.StaticSwitchNodes++;
				Context.SwitchNodes.Add(Expression);
			}
			else if (Expression->IsA<UMaterialExpressionCustom>())
			{
				Out.CustomHLSLNodes++;
			}
			else if (Expression->IsA<UMaterialExpressionScalarParameter>())
			{
				Out.ScalarParamNodes++;
			}
			else if (Expression->IsA<UMaterialExpressionVectorParameter>())
			{
				Out.VectorParamNodes++;
			}
			else if (UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
			{
				Out.FunctionCallNodes++;
				if (UMaterialFunctionInterface* Function = Call->MaterialFunction)
				{
					const FString FunctionName = Function->GetName();
					if (FunctionName.Contains(TEXT("WorldAligned")) || FunctionName.Contains(TEXT("Triplanar")))
					{
						Out.WorldAlignedNodes++;
					}
					WalkFunction(Function, Context, Out, Depth);
				}
			}
			else if (UMaterialExpressionMaterialAttributeLayers* Layers = Cast<UMaterialExpressionMaterialAttributeLayers>(Expression))
			{
				for (UMaterialFunctionInterface* Layer : Layers->GetLayers())
				{
					WalkFunction(Layer, Context, Out, Depth);
				}
				for (UMaterialFunctionInterface* Blend : Layers->GetBlends())
				{
					WalkFunction(Blend, Context, Out, Depth);
				}
			}
			else
			{
				const FString ClassName = Expression->GetClass()->GetName();
				if (ClassName.Contains(TEXT("Panner")) || ClassName.Contains(TEXT("Rotator")))
				{
					Out.PannerRotatorNodes++;
				}
			}
		}
	}

	static void AddSuggestion(FGraphAnalysisResult& Out, EGraphSeverity Severity, FString Text)
	{
		Out.Suggestions.Add({ Severity, MoveTemp(Text) });
	}

	// Texture-sample-weighted cost of the sub-graph feeding one branch input. Function calls are
	// counted (as a proxy for hidden cost) because their internals aren't traversed here.
	static int32 CountBranchCost(UMaterialExpression* Start)
	{
		if (Start == nullptr)
		{
			return 0;
		}
		int32 Cost = 0;
		TSet<UMaterialExpression*> Visited;
		TArray<UMaterialExpression*> Stack;
		Stack.Push(Start);
		int32 Guard = 0;
		while (Stack.Num() > 0 && Guard++ < 8192)
		{
			UMaterialExpression* Expression = Stack.Pop();
			if (Expression == nullptr || Visited.Contains(Expression))
			{
				continue;
			}
			Visited.Add(Expression);

			if (Expression->IsA<UMaterialExpressionTextureSample>())
			{
				Cost += 1;
			}
			else if (Expression->IsA<UMaterialExpressionMaterialFunctionCall>())
			{
				Cost += 2; // opaque - treat as non-trivial so we don't advise folding it into an If
			}

			const int32 NumInputs = Expression->CountInputs();
			for (int32 InputIndex = 0; InputIndex < NumInputs; ++InputIndex)
			{
				if (const FExpressionInput* Input = Expression->GetInput(InputIndex))
				{
					if (Input->Expression != nullptr)
					{
						Stack.Push(Input->Expression);
					}
				}
			}
		}
		return Cost;
	}

	// Classify each static switch: active path, redundancy, and whether an If (dynamic branch) could
	// replace it to drop a shader permutation.
	static void AnalyzeSwitches(UMaterialInterface* Material, const TArray<UMaterialExpression*>& SwitchNodes, FGraphAnalysisResult& Out)
	{
		TSet<FName> SeenParameters;

		for (UMaterialExpression* Node : SwitchNodes)
		{
			FStaticSwitchInfo Switch;
			UMaterialExpression* TrueBranch = nullptr;
			UMaterialExpression* FalseBranch = nullptr;

			if (UMaterialExpressionStaticSwitchParameter* Param = Cast<UMaterialExpressionStaticSwitchParameter>(Node))
			{
				const FName ParameterName = Param->ParameterName;
				if (SeenParameters.Contains(ParameterName))
				{
					continue; // one row per named parameter, even if it appears in several functions
				}
				SeenParameters.Add(ParameterName);

				Switch.bIsParameter = true;
				Switch.Name = ParameterName.ToString();

				bool bDefault = false;
				FGuid DefaultGuid;
				Material->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(ParameterName)), bDefault, DefaultGuid, /*bOveriddenOnly*/ true);
				Switch.bValueOverridden = DefaultGuid.IsValid();

				bool bEffective = false;
				FGuid EffectiveGuid;
				Material->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(ParameterName)), bEffective, EffectiveGuid);
				Switch.bValue = bEffective;

				TrueBranch = Param->A.Expression;
				FalseBranch = Param->B.Expression;
			}
			else if (UMaterialExpressionStaticSwitch* Inline = Cast<UMaterialExpressionStaticSwitch>(Node))
			{
				Switch.bIsParameter = false;
				Switch.Name = TEXT("(inline switch)");
				Switch.bGraphDriven = (Inline->Value.Expression != nullptr);
				Switch.bValue = (Inline->DefaultValue != 0);
				TrueBranch = Inline->A.Expression;
				FalseBranch = Inline->B.Expression;
			}
			else
			{
				continue;
			}

			Switch.TrueBranchCost = CountBranchCost(TrueBranch);
			Switch.FalseBranchCost = CountBranchCost(FalseBranch);
			Switch.bRedundant = (TrueBranch == FalseBranch);

			if (Switch.bRedundant)
			{
				Switch.Severity = EGraphSeverity::High;
				Switch.Verdict = TEXT("Redundant - both branches resolve to the same expression. Remove the switch.");
			}
			else if (Switch.bGraphDriven)
			{
				Switch.Severity = EGraphSeverity::Info;
				Switch.Verdict = TEXT("Inline switch driven by a graph Value - already a compile-time branch, not a per-instance permutation.");
			}
			else if (Switch.TrueBranchCost == 0 && Switch.FalseBranchCost == 0)
			{
				Switch.Severity = EGraphSeverity::Warning;
				Switch.Verdict = TEXT("Both branches are cheap (no texture samples). Replace with an If/lerp to drop this permutation dimension.");
			}
			else
			{
				Switch.Severity = EGraphSeverity::Info;
				Switch.Verdict = FString::Printf(TEXT("A branch samples textures (True:%d False:%d). Keep as a static switch - an If would evaluate both branches."),
					Switch.TrueBranchCost, Switch.FalseBranchCost);
			}

			Out.StaticSwitches.Add(MoveTemp(Switch));
		}

		Out.StaticSwitches.StableSort([](const FStaticSwitchInfo& A, const FStaticSwitchInfo& B)
		{
			return (int32)A.Severity > (int32)B.Severity;
		});
	}

	static void GenerateSuggestions(FGraphAnalysisResult& Out)
	{
		if (!Out.bStatsPending)
		{
			if (Out.SamplerUsage >= 16)
			{
				AddSuggestion(Out, EGraphSeverity::High,
					FString::Printf(TEXT("Sampler usage %d/16 - at or over the limit. Share samplers or pack textures into arrays."), Out.SamplerUsage));
			}
			else if (Out.SamplerUsage >= 13)
			{
				AddSuggestion(Out, EGraphSeverity::Warning,
					FString::Printf(TEXT("Sampler usage %d/16 - close to the limit; layered materials blow through this fast."), Out.SamplerUsage));
			}
		}

		if (Out.bUsesDisplacement && !Out.bTessellationEnabled)
		{
			AddSuggestion(Out, EGraphSeverity::Warning,
				TEXT("Displacement is used in the graph but tessellation is disabled - a dead height-sample path. Remove it."));
		}

		if (Out.WorldAlignedNodes > 0)
		{
			AddSuggestion(Out, EGraphSeverity::Warning,
				FString::Printf(TEXT("%d world-aligned/triplanar sample node(s) - up to 3x texture samples each. Disable where meshes have real UVs."), Out.WorldAlignedNodes));
		}

		if (Out.DuplicateTextureSamples > 0)
		{
			AddSuggestion(Out, EGraphSeverity::Warning,
				FString::Printf(TEXT("%d duplicate texture-sample node(s) reading a texture already sampled - reuse the sample to cut lookups."), Out.DuplicateTextureSamples));
		}

		if (Out.CustomHLSLNodes > 0)
		{
			AddSuggestion(Out, EGraphSeverity::Warning,
				FString::Printf(TEXT("%d Custom HLSL node(s) - opaque to the optimizer and can defeat constant folding. Verify each is needed."), Out.CustomHLSLNodes));
		}

		if (Out.LayerCount >= 3)
		{
			AddSuggestion(Out, EGraphSeverity::Warning,
				FString::Printf(TEXT("%d material layers (%d active), %d blends - each active layer is a full PBR graph sampled per pixel. Reduce depth or bake to fixed topology."),
					Out.LayerCount, Out.ActiveLayerCount, Out.BlendCount));
		}

		if (Out.StaticSwitchNodes >= 8)
		{
			AddSuggestion(Out, EGraphSeverity::Warning,
				FString::Printf(TEXT("%d static switches - each combination is a separate shader/shading bin. Delete never-varied ones, fold always-on ones into the graph."), Out.StaticSwitchNodes));
		}

		if (Out.StaticSwitchNodes > 0 && !Out.bHasPerInstanceCustomData)
		{
			AddSuggestion(Out, EGraphSeverity::Info,
				TEXT("Variation uses static switches, not per-instance data. Custom Primitive Data / Per-Instance Custom Data collapses permutations into one shader (fixed topology)."));
		}

		if (Out.bUsesWPO)
		{
			AddSuggestion(Out, EGraphSeverity::Info,
				TEXT("World Position Offset in use - forces Nanite programmable raster (own raster bin). Keep it on a dedicated variant, not the common opaque case."));
		}

		if (Out.bUsesPDO)
		{
			AddSuggestion(Out, EGraphSeverity::Info,
				TEXT("Pixel Depth Offset in use - forces Nanite programmable raster (own raster bin)."));
		}

		if (Out.bMasked)
		{
			AddSuggestion(Out, EGraphSeverity::Info,
				TEXT("Masked blend mode - Nanite programmable raster (own raster bin) and no early-Z. Use opaque where possible."));
		}

		if (Out.bTwoSided)
		{
			AddSuggestion(Out, EGraphSeverity::Info,
				TEXT("Two-sided - disables backface culling and can double some shading work."));
		}

		// Roll switch-level findings up into the suggestion list.
		int32 NumRedundant = 0;
		int32 NumFoldable = 0;
		for (const FStaticSwitchInfo& Switch : Out.StaticSwitches)
		{
			NumRedundant += Switch.bRedundant ? 1 : 0;
			NumFoldable += (!Switch.bRedundant && !Switch.bGraphDriven && Switch.TrueBranchCost == 0 && Switch.FalseBranchCost == 0) ? 1 : 0;
		}
		if (NumRedundant > 0)
		{
			AddSuggestion(Out, EGraphSeverity::High,
				FString::Printf(TEXT("%d redundant static switch(es) - both branches identical. Remove them (see the Static Switches table)."), NumRedundant));
		}
		if (NumFoldable > 0)
		{
			AddSuggestion(Out, EGraphSeverity::Warning,
				FString::Printf(TEXT("%d static switch(es) have cheap branches and could be If/lerp instead - each removed switch halves this material's permutations."), NumFoldable));
		}

		if (Out.Suggestions.Num() == 0)
		{
			AddSuggestion(Out, EGraphSeverity::Info, TEXT("No structural issues flagged by the heuristics."));
		}

		// High first, then Warning, then Info; stable within a severity (rule order).
		Out.Suggestions.StableSort([](const FGraphSuggestion& A, const FGraphSuggestion& B)
		{
			return (int32)A.Severity > (int32)B.Severity;
		});
	}
} // namespace GraphPrivate

void FGraphAnalyzer::Analyze(UMaterialInterface* Material, FGraphAnalysisResult& Out)
{
	using namespace GraphPrivate;

	Out = FGraphAnalysisResult();
	if (Material == nullptr)
	{
		Out.StatusMessage = TEXT("No material to analyze.");
		return;
	}

	Out.bValid = true;
	Out.AssetName = Material->GetName();
	Out.AssetPath = Material->GetPathName();
	Out.bIsInstance = Material->IsA<UMaterialInstance>();

	UMaterial* BaseMaterial = Material->GetMaterial();
	Out.BaseMaterialName = BaseMaterial ? BaseMaterial->GetName() : TEXT("(none)");

	// --- Render relevance / flags ---
	Out.BlendMode = BlendModeToString(Material->GetBlendMode());
	Out.bMasked = (Material->GetBlendMode() == BLEND_Masked);

	const FMaterialShadingModelField ShadingModels = Material->GetShadingModels();
	TArray<FString> ModelNames;
	bool bHasSubsurface = false;
	for (int32 ModelIndex = 0; ModelIndex < MSM_NUM; ++ModelIndex)
	{
		const EMaterialShadingModel Model = (EMaterialShadingModel)ModelIndex;
		if (ShadingModels.HasShadingModel(Model))
		{
			ModelNames.Add(ShadingModelToString(Model));
			bHasSubsurface |= IsSubsurfaceModel(Model);
		}
	}
	Out.ShadingModels = ModelNames.Num() > 0 ? FString::Join(ModelNames, TEXT(", ")) : TEXT("(none)");

	const FMaterialRelevance Relevance = Material->GetRelevance_Concurrent(GMaxRHIShaderPlatform);
	Out.bUsesWPO = Relevance.bUsesWorldPositionOffset != 0;
	Out.bUsesPDO = Relevance.bUsesPixelDepthOffset != 0;
	Out.bUsesDisplacement = Relevance.bUsesDisplacement != 0;
	Out.bTwoSided = Relevance.bTwoSided != 0;

	// --- Compiled shader stats ---
	if (FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
	{
		Out.SamplerUsage = Resource->GetSamplerUsage();
		Out.NumCustomizedUVs = Resource->GetNumCustomizedUVs();
		Out.bHasPerInstanceCustomData = Resource->HasPerInstanceCustomData();
		Out.bHasRVTOutput = Resource->HasRuntimeVirtualTextureOutput();
		Out.bTessellationEnabled = Resource->IsTessellationEnabled();

		FShaderStatsInfoMirror Info;
		FMaterialStatsUtils::ExtractMatertialStatsInfo(GMaxRHIShaderPlatform, reinterpret_cast<FShaderStatsInfo&>(Info), Resource);

		Out.SamplersText = Info.SamplersCount.StrDescription;
		Out.TextureSamplesText = Info.TextureSampleCount.StrDescription;
		Out.VTLookupsText = Info.VirtualTextureLookupCount.StrDescription;
		Out.VTStacksText = Info.VirtualTextureStackCount.StrDescription;
		Out.ShaderCountText = Info.ShaderCount.StrDescription;
		Out.ShaderErrors = Info.StrShaderErrors;
		Out.bStatsPending = Info.SamplersCount.StrDescription.Equals(TEXT("Compiling..."));

		for (const TPair<ERepresentativeShader, FShaderStatsInfoMirror::FContent>& Pair : Info.ShaderInstructionCount)
		{
			Out.InstructionCounts.Add({ RepresentativeShaderName(Pair.Key), Pair.Value.StrDescription });
		}
		Out.InstructionCounts.StableSort([](const FGraphMetric& A, const FGraphMetric& B)
		{
			return A.Name < B.Name;
		});
	}
	else
	{
		Out.bStatsPending = true;
	}

	// --- Resolved material layer stack (per this instance) ---
	FMaterialLayersFunctions LayerFunctions;
	if (Material->GetMaterialLayers(LayerFunctions))
	{
		Out.LayerCount = LayerFunctions.Layers.Num();
		Out.BlendCount = 0;
		for (UMaterialFunctionInterface* Blend : LayerFunctions.Blends)
		{
			if (Blend != nullptr)
			{
				Out.BlendCount++;
			}
		}
#if WITH_EDITORONLY_DATA
		const TArray<bool>& States = LayerFunctions.EditorOnly.LayerStates;
		for (int32 LayerIndex = 0; LayerIndex < LayerFunctions.Layers.Num(); ++LayerIndex)
		{
			const bool bActive = States.IsValidIndex(LayerIndex) ? States[LayerIndex] : true;
			if (bActive)
			{
				Out.ActiveLayerCount++;
			}
			UMaterialFunctionInterface* Layer = LayerFunctions.Layers[LayerIndex];
			Out.LayerNames.Add(Layer ? Layer->GetName() : TEXT("(empty)"));
		}
#else
		Out.ActiveLayerCount = Out.LayerCount;
#endif
		for (UMaterialFunctionInterface* Blend : LayerFunctions.Blends)
		{
			Out.BlendNames.Add(Blend ? Blend->GetName() : TEXT("(none)"));
		}
	}

	// --- Structural node census of the effective graph ---
	FWalkContext Context;
	if (BaseMaterial != nullptr)
	{
		WalkExpressions(BaseMaterial->GetExpressions(), Context, Out, 0);
	}
	// Walk the resolved layer/blend functions too (they may be overridden on the instance).
	for (UMaterialFunctionInterface* Layer : LayerFunctions.Layers)
	{
		WalkFunction(Layer, Context, Out, 0);
	}
	for (UMaterialFunctionInterface* Blend : LayerFunctions.Blends)
	{
		WalkFunction(Blend, Context, Out, 0);
	}

	AnalyzeSwitches(Material, Context.SwitchNodes, Out);
	GenerateSuggestions(Out);

	Out.StatusMessage = Out.bStatsPending
		? TEXT("Shaders still compiling - stats shown as 'Compiling...'. Re-analyze once compilation finishes.")
		: TEXT("Analysis complete.");
}

void FGraphAnalyzer::ScanSwitchVariance(UMaterialInterface* Material, FGraphAnalysisResult& InOut)
{
	if (Material == nullptr || InOut.StaticSwitches.Num() == 0)
	{
		return;
	}

	// Keep the analyzed material (and its base) alive across the GC calls in the load loop.
	TStrongObjectPtr<UMaterialInterface> KeepMaterial(Material);
	UMaterial* RootMaterial = Material->GetMaterial();
	TStrongObjectPtr<UMaterial> KeepRoot(RootMaterial);
	if (RootMaterial == nullptr)
	{
		return;
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Build a parent-object-path -> child map from every material instance's "Parent" tag, then BFS
	// down from the root material to collect the whole instance hierarchy (registry-only, no loads).
	TArray<FAssetData> AllInstances;
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		AssetRegistry.GetAssets(Filter, AllInstances);
	}

	TMultiMap<FString, FSoftObjectPath> ChildrenByParent;
	for (const FAssetData& Instance : AllInstances)
	{
		FString ParentExportPath;
		if (Instance.GetTagValue(FName("Parent"), ParentExportPath))
		{
			const FString ParentObjectPath = FPackageName::ExportTextPathToObjectPath(ParentExportPath);
			ChildrenByParent.Add(ParentObjectPath, Instance.GetSoftObjectPath());
		}
	}

	TArray<FSoftObjectPath> InstancePaths;
	{
		TSet<FString> Visited;
		TArray<FString> Frontier;
		Frontier.Add(RootMaterial->GetPathName());
		while (Frontier.Num() > 0)
		{
			const FString Current = Frontier.Pop();
			TArray<FSoftObjectPath> Children;
			ChildrenByParent.MultiFind(Current, Children);
			for (const FSoftObjectPath& Child : Children)
			{
				const FString ChildString = Child.ToString();
				if (!Visited.Contains(ChildString))
				{
					Visited.Add(ChildString);
					InstancePaths.Add(Child);
					Frontier.Add(ChildString);
				}
			}
		}
	}

	// Count the root material's own default value too, so 0-instance materials still resolve.
	auto Tally = [&InOut](UMaterialInterface* Interface)
	{
		for (FStaticSwitchInfo& Switch : InOut.StaticSwitches)
		{
			if (!Switch.bIsParameter)
			{
				continue;
			}
			bool bValue = false;
			FGuid Guid;
			if (Interface->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(FMaterialParameterInfo(FName(*Switch.Name))), bValue, Guid))
			{
				bValue ? ++Switch.InstancesTrue : ++Switch.InstancesFalse;
			}
		}
	};

	FScopedSlowTask SlowTask((float)InstancePaths.Num() + 1.0f,
		FText::FromString(FString::Printf(TEXT("Scanning %d instances for switch variance..."), InstancePaths.Num())));
	SlowTask.MakeDialog(true /*bShowCancelButton*/);

	SlowTask.EnterProgressFrame(1.0f);
	Tally(RootMaterial);
	InOut.InstancesScanned = 0;

	int32 LoadsSinceGC = 0;
	for (const FSoftObjectPath& InstancePath : InstancePaths)
	{
		if (SlowTask.ShouldCancel())
		{
			break;
		}
		SlowTask.EnterProgressFrame(1.0f);

		if (UMaterialInterface* Instance = Cast<UMaterialInterface>(InstancePath.TryLoad()))
		{
			InOut.InstancesScanned++;
			Tally(Instance);
			if (++LoadsSinceGC >= 200)
			{
				LoadsSinceGC = 0;
				CollectGarbage(RF_NoFlags, false /*bPerformFullPurge*/);
			}
		}
	}

	// Classify each parameter switch by its value distribution.
	int32 NumConstant = 0;
	for (FStaticSwitchInfo& Switch : InOut.StaticSwitches)
	{
		if (!Switch.bIsParameter)
		{
			continue;
		}
		Switch.bVarianceScanned = true;
		const int32 Total = Switch.InstancesTrue + Switch.InstancesFalse;
		Switch.bConstantAcrossInstances = (Total > 0) && (Switch.InstancesTrue == 0 || Switch.InstancesFalse == 0);

		if (Switch.bConstantAcrossInstances && !Switch.bRedundant)
		{
			NumConstant++;
			const bool bConstValue = (Switch.InstancesTrue > 0);
			Switch.Severity = EGraphSeverity::High;
			Switch.Verdict = FString::Printf(
				TEXT("Constant %s across all %d instances - delete the switch and wire the %s branch directly (removes a permutation, no runtime cost)."),
				bConstValue ? TEXT("True") : TEXT("False"), Total, bConstValue ? TEXT("True (A)") : TEXT("False (B)"));
		}
		else if (!Switch.bRedundant)
		{
			// Genuinely varies: the earlier branch-cost verdict (If vs static) still applies; annotate it.
			Switch.Verdict = FString::Printf(TEXT("Varies (%d True / %d False). %s"),
				Switch.InstancesTrue, Switch.InstancesFalse, *Switch.Verdict);
		}
	}

	InOut.StaticSwitches.StableSort([](const FStaticSwitchInfo& A, const FStaticSwitchInfo& B)
	{
		return (int32)A.Severity > (int32)B.Severity;
	});

	InOut.bVarianceScanned = true;
	if (NumConstant > 0)
	{
		InOut.Suggestions.Insert({ EGraphSeverity::High,
			FString::Printf(TEXT("%d static switch(es) are CONSTANT across all %d instances - delete each and wire the used branch directly. Biggest permutation win."),
				NumConstant, InOut.InstancesScanned) }, 0);
	}

	InOut.StatusMessage = FString::Printf(TEXT("Scanned %d instances. %d constant switch(es) found."), InOut.InstancesScanned, NumConstant);
}

} // namespace MaterialUsageAnalyzer
