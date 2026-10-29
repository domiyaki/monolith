// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"
#include "MonolithSourceRequestFileCache.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSourceRequestFileCacheTest,
	"Monolith.Source.RequestFileCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceRequestFileCacheTest::RunTest(const FString& Parameters)
{
	int32 Resolves = 0;
	int32 Reads = 0;
	FString Current = TEXT("original");
	auto Resolve = [&Resolves](int64 Id) { ++Resolves; return FString::FromInt(static_cast<int32>(Id)); };
	auto Load = [&Reads, &Current](const FString& Path, TArray<FString>& Lines)
	{
		++Reads;
		Lines.Add(Current);
		return Path != TEXT("99");
	};
	{
		FMonolithSourceRequestFileCache Cache(Resolve, Load, 2);
		for (int32 i = 0; i < 400; ++i)
		{
			const auto& File = Cache.Get(1);
			TestTrue(TEXT("Repeated chunk has loaded file"), File.bLoaded);
			TestEqual(TEXT("Repeated chunk content preserved"), File.Lines[0], Current);
		}
		TestEqual(TEXT("400 hits resolve path once"), Resolves, 1);
		TestEqual(TEXT("400 hits read file once"), Reads, 1);
		TestFalse(TEXT("Missing file fails"), Cache.Get(99).bLoaded);
		TestEqual(TEXT("Failed partial read is cleared"), Cache.Get(99).Lines.Num(), 0);
		TestEqual(TEXT("Missing file attempted once"), Reads, 2);
		Cache.Get(2); // FIFO evicts 1.
		Current = TEXT("edited");
		TestEqual(TEXT("Evicted file is read again"), Cache.Get(1).Lines[0], Current);
		TestEqual(TEXT("Bounded cache reload count"), Reads, 4);
	}
	Current = TEXT("next request");
	FMonolithSourceRequestFileCache NextRequest(Resolve, Load);
	TestEqual(TEXT("New request sees changed contents"), NextRequest.Get(1).Lines[0], Current);
	TestEqual(TEXT("No cross-request read cache"), Reads, 5);
	{
		// Zero byte budget: one oversized file remains usable, then is evicted.
		FMonolithSourceRequestFileCache TinyCache(Resolve, Load, 8, 0);
		TinyCache.Get(1);
		TinyCache.Get(1);
		TinyCache.Get(2);
		TinyCache.Get(1);
		TestEqual(TEXT("Byte budget evicts even below file-count limit"), Reads, 8);
	}
	return true;
}
#endif
