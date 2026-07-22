// Copyright Epic Games, Inc. All Rights Reserved.

#include "MaterialUsageAnalyzerWindow.h"

#include "SlateIM.h"
#include "Textures/SlateIcon.h"

namespace MaterialUsageAnalyzer
{

FMaterialUsageAnalyzerWindow::FMaterialUsageAnalyzerWindow()
	: FSlateIMWindowBase(
		TEXT("Material Usage Analyzer"),
		FVector2f(1480.0f, 820.0f),
		TEXT("MaterialUsageAnalyzer"),
		TEXT("Toggles the Material Usage Analyzer window"))
{
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
	{
		Engine.RunDeferredActions();
		Engine.Pump(PumpBudgetMs / 1000.0);
		return true;
	}));
}

FMaterialUsageAnalyzerWindow::~FMaterialUsageAnalyzerWindow()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
}

void FMaterialUsageAnalyzerWindow::DrawWindow(float DeltaTime)
{
	if (Engine.IsBusy())
	{
		SlateIM::BeginHorizontalStack();
		SlateIM::Padding(FMargin(4.0f, 2.0f));
		SlateIM::VAlign(VAlign_Center);
		SlateIM::MinWidth(260.0f);
		SlateIM::ProgressBar(Engine.GetProgress());
		SlateIM::Padding(FMargin(6.0f, 2.0f));
		SlateIM::VAlign(VAlign_Center);
		SlateIM::Text(Engine.GetPhaseLabel());
		SlateIM::Padding(FMargin(6.0f, 2.0f));
		if (SlateIM::Button(TEXT("Cancel")))
		{
			Engine.Cancel();
		}
		SlateIM::EndHorizontalStack();
	}

	SlateIM::Fill();
	SlateIM::HAlign(HAlign_Fill);
	SlateIM::VAlign(VAlign_Fill);
	SlateIM::BeginTabGroup(TEXT("MaterialUsageAnalyzer.Tabs"));
	SlateIM::BeginTabStack();

	if (SlateIM::BeginTab(TEXT("MIHierarchy"), FSlateIcon(), NSLOCTEXT("MaterialUsageAnalyzer", "HierarchyTab", "MI Hierarchy Analysis")))
	{
		HierarchyTab.Draw(Engine);
	}
	SlateIM::EndTab();

	if (SlateIM::BeginTab(TEXT("MaterialBatch"), FSlateIcon(), NSLOCTEXT("MaterialUsageAnalyzer", "BatchTab", "Material Batch Analysis")))
	{
		BatchTab.Draw(Engine);
	}
	SlateIM::EndTab();

	if (SlateIM::BeginTab(TEXT("GraphAnalysis"), FSlateIcon(), NSLOCTEXT("MaterialUsageAnalyzer", "GraphTab", "Graph Analysis")))
	{
		GraphTab.Draw(Engine);
	}
	SlateIM::EndTab();

	SlateIM::EndTabStack();
	SlateIM::EndTabGroup();
}

} // namespace MaterialUsageAnalyzer
