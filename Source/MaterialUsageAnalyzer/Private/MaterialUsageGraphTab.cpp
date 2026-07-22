// Copyright Epic Games, Inc. All Rights Reserved.

#include "MaterialUsageGraphTab.h"

#include "AssetRegistry/AssetData.h"
#include "MaterialUsageAnalysisEngine.h"
#include "MaterialUsageEditorActions.h"
#include "MaterialUsageFlags.h"
#include "Materials/MaterialInterface.h"
#include "SlateIM.h"

namespace MaterialUsageAnalyzer
{

namespace GraphTabPrivate
{
	static const TCHAR* YesNo(bool bValue) { return bValue ? TEXT("Yes") : TEXT("No"); }

	static FLinearColor SeverityColor(EGraphSeverity Severity)
	{
		switch (Severity)
		{
		case EGraphSeverity::High:    return GetStatusColorMissing();
		case EGraphSeverity::Warning: return GetStatusColorWasteful();
		default:                      return FLinearColor(0.72f, 0.72f, 0.72f);
		}
	}

	static const TCHAR* SeverityLabel(EGraphSeverity Severity)
	{
		switch (Severity)
		{
		case EGraphSeverity::High:    return TEXT("HIGH");
		case EGraphSeverity::Warning: return TEXT("WARN");
		default:                      return TEXT("INFO");
		}
	}

	// A key/value line as a horizontal stack. Stack rows autosize safely; only ONE Fill table
	// may live in the tab (SlateIM virtualization feedback loop otherwise).
	static void Row(const TCHAR* Key, const FString& Value, FLinearColor ValueColor = FLinearColor::White)
	{
		SlateIM::BeginHorizontalStack();
		SlateIM::Padding(FMargin(6.0f, 1.0f, 4.0f, 1.0f));
		SlateIM::VAlign(VAlign_Center);
		SlateIM::MinWidth(220.0f);
		SlateIM::Text(Key, FLinearColor(0.7f, 0.7f, 0.7f));
		SlateIM::Padding(FMargin(4.0f, 1.0f));
		SlateIM::VAlign(VAlign_Center);
		SlateIM::Text(Value, ValueColor);
		SlateIM::EndHorizontalStack();
	}

	static void SectionHeader(const FString& Text)
	{
		SlateIM::Padding(FMargin(4.0f, 6.0f, 2.0f, 1.0f));
		SlateIM::Text(Text, FLinearColor(0.9f, 0.9f, 0.6f));
	}
}

void FMaterialUsageGraphTab::Draw(FMaterialUsageAnalysisEngine& Engine)
{
	DrawToolbar(Engine);

	if (!StatusMessage.IsEmpty())
	{
		SlateIM::Padding(FMargin(6.0f, 2.0f));
		SlateIM::Text(StatusMessage, FLinearColor(0.7f, 0.7f, 0.7f));
	}

	if (Result.bValid)
	{
		DrawSummary();
		DrawSuggestions();
		DrawSwitches(); // the single Fill table - must be last
	}
}

void FMaterialUsageGraphTab::DrawToolbar(FMaterialUsageAnalysisEngine& Engine)
{
	SlateIM::BeginHorizontalStack();

	SlateIM::Padding(FMargin(4.0f, 4.0f, 2.0f, 2.0f));
	SlateIM::SetToolTip(TEXT("Analyzes the first material or material instance selected in the Content Browser."));
	if (SlateIM::Button(TEXT("Analyze Selected Material / Instance")) && !Engine.IsBusy())
	{
		// Deferred: loads the asset and reads its shader map, which must not happen inside the SlateIM root.
		Engine.Defer([this]()
		{
			FAssetData Asset;
			if (!EditorActions::GetSelectedMaterialInterface(Asset))
			{
				StatusMessage = TEXT("Select a material or material instance in the Content Browser first.");
				return;
			}
			if (UMaterialInterface* Material = Cast<UMaterialInterface>(Asset.GetAsset()))
			{
				AnalyzedPath = Asset.GetSoftObjectPath();
				FGraphAnalyzer::Analyze(Material, Result);
				StatusMessage = Result.StatusMessage;
			}
			else
			{
				StatusMessage = TEXT("Failed to load the selected asset.");
			}
		});
	}

	// Cross-instance variance scan: loads every instance to find switches that never vary.
	if (Result.bValid && Result.StaticSwitches.Num() > 0 && !AnalyzedPath.IsNull())
	{
		SlateIM::Padding(FMargin(6.0f, 4.0f, 2.0f, 2.0f));
		SlateIM::SetToolTip(TEXT("Loads every instance in this material's hierarchy to find static switches that are constant across all of them (safe to delete)."));
		if (SlateIM::Button(TEXT("Scan Instance Variance")) && !Engine.IsBusy())
		{
			Engine.Defer([this]()
			{
				if (UMaterialInterface* Material = Cast<UMaterialInterface>(AnalyzedPath.TryLoad()))
				{
					FGraphAnalyzer::ScanSwitchVariance(Material, Result);
					StatusMessage = Result.StatusMessage;
				}
			});
		}
	}

	if (Result.bValid)
	{
		SlateIM::Padding(FMargin(12.0f, 6.0f, 2.0f, 2.0f));
		SlateIM::VAlign(VAlign_Center);
		SlateIM::Text(FString::Printf(TEXT("%s  (%s of %s)"),
			*Result.AssetName,
			Result.bIsInstance ? TEXT("instance") : TEXT("material"),
			*Result.BaseMaterialName));
	}

	SlateIM::EndHorizontalStack();
}

void FMaterialUsageGraphTab::DrawSummary()
{
	using namespace GraphTabPrivate;

	// Three side-by-side columns so the summary spreads horizontally instead of running tall.
	SlateIM::BeginHorizontalStack();

	// Column 1 - material / shader stats.
	SlateIM::Fill();
	SlateIM::BeginVerticalStack();
	{
		SectionHeader(TEXT("Material / Shader Stats"));
		Row(TEXT("Blend mode"), Result.BlendMode);
		Row(TEXT("Shading models"), Result.ShadingModels);
		Row(TEXT("Two-sided"), YesNo(Result.bTwoSided));
		Row(TEXT("World Position Offset"), YesNo(Result.bUsesWPO), Result.bUsesWPO ? GetStatusColorWasteful() : FLinearColor::White);
		Row(TEXT("Pixel Depth Offset"), YesNo(Result.bUsesPDO), Result.bUsesPDO ? GetStatusColorWasteful() : FLinearColor::White);
		Row(TEXT("Displacement"), YesNo(Result.bUsesDisplacement));
		Row(TEXT("Tessellation enabled"), YesNo(Result.bTessellationEnabled));
		Row(TEXT("Per-instance custom data"), YesNo(Result.bHasPerInstanceCustomData));
		Row(TEXT("Customized UVs"), FString::Printf(TEXT("%d"), Result.NumCustomizedUVs));

		const bool bSamplersHot = Result.SamplerUsage >= 13;
		Row(TEXT("Sampler usage"),
			Result.bStatsPending ? Result.SamplersText : FString::Printf(TEXT("%d / 16"), Result.SamplerUsage),
			bSamplersHot ? GetStatusColorMissing() : FLinearColor::White);
		Row(TEXT("Texture samples"), Result.TextureSamplesText);
		Row(TEXT("Virtual texture lookups"), Result.VTLookupsText);
		Row(TEXT("Virtual texture stacks"), Result.VTStacksText);
		Row(TEXT("Shader count"), Result.ShaderCountText);
		for (const FGraphMetric& Metric : Result.InstructionCounts)
		{
			Row(*Metric.Name, Metric.Value);
		}
		if (!Result.ShaderErrors.IsEmpty())
		{
			Row(TEXT("Shader errors"), Result.ShaderErrors, GetStatusColorMissing());
		}
	}
	SlateIM::EndVerticalStack();

	// Column 2 - graph structure.
	SlateIM::Fill();
	SlateIM::BeginVerticalStack();
	{
		SectionHeader(TEXT("Graph Structure"));
		Row(TEXT("Total expressions"), FString::Printf(TEXT("%d"), Result.TotalExpressions));
		Row(TEXT("Texture samples"), FString::Printf(TEXT("%d"), Result.TextureSampleNodes));
		Row(TEXT("Duplicate texture samples"), FString::Printf(TEXT("%d"), Result.DuplicateTextureSamples),
			Result.DuplicateTextureSamples > 0 ? GetStatusColorWasteful() : FLinearColor::White);
		Row(TEXT("World-aligned / triplanar"), FString::Printf(TEXT("%d"), Result.WorldAlignedNodes),
			Result.WorldAlignedNodes > 0 ? GetStatusColorWasteful() : FLinearColor::White);
		Row(TEXT("Static switches"), FString::Printf(TEXT("%d"), Result.StaticSwitchNodes));
		Row(TEXT("Custom HLSL nodes"), FString::Printf(TEXT("%d"), Result.CustomHLSLNodes),
			Result.CustomHLSLNodes > 0 ? GetStatusColorWasteful() : FLinearColor::White);
		Row(TEXT("Scalar parameters"), FString::Printf(TEXT("%d"), Result.ScalarParamNodes));
		Row(TEXT("Vector parameters"), FString::Printf(TEXT("%d"), Result.VectorParamNodes));
		Row(TEXT("Function calls"), FString::Printf(TEXT("%d"), Result.FunctionCallNodes));
		Row(TEXT("Panner / rotator"), FString::Printf(TEXT("%d"), Result.PannerRotatorNodes));
	}
	SlateIM::EndVerticalStack();

	// Column 3 - layer stack + path.
	SlateIM::Fill();
	SlateIM::BeginVerticalStack();
	{
		SectionHeader(TEXT("Material Layer Stack"));
		if (Result.LayerCount > 0)
		{
			Row(TEXT("Layers"), FString::Printf(TEXT("%d (%d active)"), Result.LayerCount, Result.ActiveLayerCount),
				Result.LayerCount >= 3 ? GetStatusColorWasteful() : FLinearColor::White);
			Row(TEXT("Blends"), FString::Printf(TEXT("%d"), Result.BlendCount));
			Row(TEXT("Layer assets"), FString::Join(Result.LayerNames, TEXT(", ")));
			if (Result.BlendNames.Num() > 0)
			{
				Row(TEXT("Blend assets"), FString::Join(Result.BlendNames, TEXT(", ")));
			}
		}
		else
		{
			Row(TEXT("Layers"), TEXT("none"));
		}
		Row(TEXT("Path"), Result.AssetPath);
	}
	SlateIM::EndVerticalStack();

	SlateIM::EndHorizontalStack();
}

void FMaterialUsageGraphTab::DrawSuggestions()
{
	using namespace GraphTabPrivate;

	SectionHeader(FString::Printf(TEXT("Suggestions (%d)"), Result.Suggestions.Num()));
	for (const FGraphSuggestion& Suggestion : Result.Suggestions)
	{
		const FLinearColor Color = SeverityColor(Suggestion.Severity);
		SlateIM::BeginHorizontalStack();
		SlateIM::Padding(FMargin(6.0f, 1.0f, 4.0f, 1.0f));
		SlateIM::VAlign(VAlign_Center);
		SlateIM::MinWidth(48.0f);
		SlateIM::Text(SeverityLabel(Suggestion.Severity), Color);
		SlateIM::Padding(FMargin(4.0f, 1.0f));
		SlateIM::VAlign(VAlign_Center);
		SlateIM::Text(Suggestion.Text, Color);
		SlateIM::EndHorizontalStack();
	}
}

void FMaterialUsageGraphTab::DrawSwitches()
{
	using namespace GraphTabPrivate;

	SectionHeader(FString::Printf(TEXT("Static Switches (%d) - path taken on this instance + permutation advice"), Result.StaticSwitches.Num()));

	// The one Fill/scrolling table in the tab.
	SlateIM::Fill();
	SlateIM::HAlign(HAlign_Fill);
	SlateIM::VAlign(VAlign_Fill);
	SlateIM::BeginTable();
	SlateIM::InitialTableColumnWidth(240.0f);
	SlateIM::AddTableColumn(TEXT("Switch"));
	SlateIM::FixedTableColumnWidth(120.0f);
	SlateIM::AddTableColumn(TEXT("Active path"));
	SlateIM::FixedTableColumnWidth(90.0f);
	SlateIM::AddTableColumn(TEXT("Source"));
	SlateIM::FixedTableColumnWidth(120.0f);
	SlateIM::AddTableColumn(TEXT("Branch cost T/F"));
	SlateIM::FixedTableColumnWidth(140.0f);
	SlateIM::AddTableColumn(TEXT("Instances T/F"));
	SlateIM::InitialTableColumnWidth(560.0f);
	SlateIM::AddTableColumn(TEXT("Verdict"));

	for (const FStaticSwitchInfo& Switch : Result.StaticSwitches)
	{
		const FLinearColor Color = SeverityColor(Switch.Severity);

		if (SlateIM::NextTableCell())
		{
			SlateIM::Padding(FMargin(4.0f, 1.0f));
			SlateIM::Text(Switch.Name, Switch.bIsParameter ? FLinearColor::White : FLinearColor(0.6f, 0.6f, 0.6f));
		}
		if (SlateIM::NextTableCell())
		{
			SlateIM::Padding(FMargin(4.0f, 1.0f));
			SlateIM::HAlign(HAlign_Center);
			const TCHAR* Path = Switch.bGraphDriven ? TEXT("graph-driven") : (Switch.bValue ? TEXT("True (A)") : TEXT("False (B)"));
			SlateIM::Text(Path, Switch.bValue ? GetStatusColorNeeded() : FLinearColor(0.8f, 0.8f, 0.8f));
		}
		if (SlateIM::NextTableCell())
		{
			SlateIM::Padding(FMargin(4.0f, 1.0f));
			SlateIM::HAlign(HAlign_Center);
			const TCHAR* Source = !Switch.bIsParameter ? TEXT("inline") : (Switch.bValueOverridden ? TEXT("override") : TEXT("default"));
			SlateIM::Text(Source, FLinearColor(0.8f, 0.8f, 0.8f));
		}
		if (SlateIM::NextTableCell())
		{
			SlateIM::Padding(FMargin(4.0f, 1.0f));
			SlateIM::HAlign(HAlign_Center);
			SlateIM::Text(FString::Printf(TEXT("%d / %d"), Switch.TrueBranchCost, Switch.FalseBranchCost));
		}
		if (SlateIM::NextTableCell())
		{
			SlateIM::Padding(FMargin(4.0f, 1.0f));
			SlateIM::HAlign(HAlign_Center);
			if (!Switch.bIsParameter || !Switch.bVarianceScanned)
			{
				SlateIM::Text(TEXT("-"), FLinearColor(0.4f, 0.4f, 0.4f));
			}
			else
			{
				SlateIM::Text(FString::Printf(TEXT("%d / %d"), Switch.InstancesTrue, Switch.InstancesFalse),
					Switch.bConstantAcrossInstances ? GetStatusColorMissing() : FLinearColor::White);
			}
		}
		if (SlateIM::NextTableCell())
		{
			SlateIM::Padding(FMargin(4.0f, 1.0f));
			SlateIM::Text(Switch.Verdict, Color);
		}
	}

	SlateIM::EndTable();
}

} // namespace MaterialUsageAnalyzer
