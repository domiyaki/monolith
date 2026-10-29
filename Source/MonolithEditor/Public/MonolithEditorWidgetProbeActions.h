#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithEditorWidgetProbeActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);
	static void Shutdown();
	static FMonolithActionResult HandleInspect(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleResolve(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleHighlight(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleClearHighlight(const TSharedPtr<FJsonObject>& Params);
};
