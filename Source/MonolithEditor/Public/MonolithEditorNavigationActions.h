#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithEditorNavigationActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);
	static void Shutdown();
	static void StopHighlight();
	static void SetGuideActive(bool bActive);
	static FMonolithActionResult HandleNavigateTo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetNavigationState(const TSharedPtr<FJsonObject>& Params);
};
