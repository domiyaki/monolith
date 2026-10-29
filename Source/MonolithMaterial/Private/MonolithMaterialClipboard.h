#pragma once

#include "MonolithToolRegistry.h"

/** Builds deliveries exclusively in transient graphs; never edits the destination. */
class FMonolithMaterialClipboard
{
public:
	static void Register(FMonolithToolRegistry& Registry);
	static void Reset();
	static FMonolithActionResult Copy(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult Recopy(const TSharedPtr<FJsonObject>& Params);
	// Preparation is clipboard-independent so automation can exercise failure paths.
	static FMonolithActionResult Prepare(const TSharedPtr<FJsonObject>& Params, FString& OutText);
	#if WITH_DEV_AUTOMATION_TESTS
	static bool TestFailedNativeWrite(FString& Error);
#endif
};
