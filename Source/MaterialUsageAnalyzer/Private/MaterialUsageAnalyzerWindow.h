// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "MaterialUsageAnalysisEngine.h"
#include "MaterialUsageBatchTab.h"
#include "MaterialUsageGraphTab.h"
#include "MaterialUsageHierarchyTab.h"
#include "SlateIMWidgetBase.h"

namespace MaterialUsageAnalyzer
{

/**
 * SlateIM tool window: material usage flag analyzer.
 * Tab 1 analyzes/edits one parent material's instance hierarchy,
 * Tab 2 bulk-analyzes many materials with CSV export.
 * Toggled with the 'MaterialUsageAnalyzer' console command (registered by the base class).
 */
class FMaterialUsageAnalyzerWindow : public FSlateIMWindowBase
{
public:
	FMaterialUsageAnalyzerWindow();
	virtual ~FMaterialUsageAnalyzerWindow() override;

protected:
	virtual void DrawWindow(float DeltaTime) override;

private:
	FMaterialUsageAnalysisEngine Engine;
	FMaterialUsageHierarchyTab HierarchyTab;
	FMaterialUsageBatchTab BatchTab;
	FMaterialUsageGraphTab GraphTab;

	/**
	 * Deferred actions and the analysis pump run from this core ticker, never from DrawWindow:
	 * asset loads / save prompts can pop modal dialogs that tick Slate, which would re-enter
	 * the SlateIM root and assert.
	 */
	FTSTicker::FDelegateHandle TickerHandle;

	/** Per-tick analysis budget in milliseconds. */
	float PumpBudgetMs = 10.0f;
};

} // namespace MaterialUsageAnalyzer
