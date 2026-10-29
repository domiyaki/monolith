#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithEditorGuideActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);
	static void Shutdown();
	static FMonolithActionResult HandleStart(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetState(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleStop(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleReveal(const TSharedPtr<FJsonObject>& Params);
};
