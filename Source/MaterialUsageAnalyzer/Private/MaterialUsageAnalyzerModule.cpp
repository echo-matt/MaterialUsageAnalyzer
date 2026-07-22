// Copyright Epic Games, Inc. All Rights Reserved.

#include "MaterialUsageAnalyzerWindow.h"
#include "Modules/ModuleManager.h"

class FMaterialUsageAnalyzerModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		Window = MakeUnique<MaterialUsageAnalyzer::FMaterialUsageAnalyzerWindow>();
	}

	virtual void ShutdownModule() override
	{
		Window.Reset();
	}

private:
	TUniquePtr<MaterialUsageAnalyzer::FMaterialUsageAnalyzerWindow> Window;
};

IMPLEMENT_MODULE(FMaterialUsageAnalyzerModule, MaterialUsageAnalyzer)
