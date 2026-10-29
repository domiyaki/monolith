#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithBlueprintNodeActions.h"
#include "MonolithBlueprintBuildActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintScopedReuseTest,
	"Monolith.Blueprint.Batch.ScopedReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintScopedReuseTest::RunTest(const FString& Parameters)
{
	using namespace MonolithBlueprintInternal;
	UBlueprint* First = NewObject<UBlueprint>(GetTransientPackage());
	UBlueprint* Second = NewObject<UBlueprint>(GetTransientPackage());
	const FString Path = TEXT("/MonolithBatchTest/ScopedAsset");
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Path);
	FString ResolvedPath;
	TestNull(TEXT("No context before request"), FScopedBlueprintReuse::Find(Path));
	{
		FScopedBlueprintReuse Outer(Path, First);
		// This non-existent disk path proves the loader uses the supplied request asset.
		TestTrue(TEXT("Loader reuses request Blueprint"), LoadBlueprintFromParams(Params, ResolvedPath) == First);
		TestEqual(TEXT("Path preserved"), ResolvedPath, Path);
		TestNull(TEXT("Different assets cannot reuse this object"), FScopedBlueprintReuse::Find(Path + TEXT("Other")));
		{
			FScopedBlueprintReuse Inner(Path, Second);
			TestTrue(TEXT("Nested request has its own object"), FScopedBlueprintReuse::Find(Path) == Second);
			Second->MarkAsGarbage();
			TestNull(TEXT("Invalid object is not reused"), FScopedBlueprintReuse::Find(Path));
		}
		TestTrue(TEXT("Outer request restored"), FScopedBlueprintReuse::Find(Path) == First);
	}
	TestNull(TEXT("No cache survives request"), FScopedBlueprintReuse::Find(Path));
	First->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintCompileOutcomeTest,
	"Monolith.Blueprint.Batch.CompileOutcome",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintCompileOutcomeTest::RunTest(const FString& Parameters)
{
	using namespace MonolithBlueprintInternal;
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetBoolField(TEXT("success"), false);
	TestFalse(TEXT("Compiler errors are not transport success"), DidCompileSucceed(FMonolithActionResult::Success(Payload)));
	Payload->SetBoolField(TEXT("success"), true);
	TestTrue(TEXT("Successful compile"), DidCompileSucceed(FMonolithActionResult::Success(Payload)));
	TestFalse(TEXT("Handler error"), DidCompileSucceed(FMonolithActionResult::Error(TEXT("Missing asset"))));
	TestFalse(TEXT("Missing compile status fails closed"), DidCompileSucceed(FMonolithActionResult::Success(MakeShared<FJsonObject>())));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintBatchPartialFailureTest,
	"Monolith.Blueprint.Batch.PartialFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintBatchPartialFailureTest::RunTest(const FString& Parameters)
{
	using namespace MonolithBlueprintInternal;
	UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage());
	const FString Path = TEXT("/MonolithBatchTest/PartialFailure");
	FScopedBlueprintReuse Scope(Path, Blueprint);
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Path);
	TSharedRef<FJsonObject> UnknownOp = MakeShared<FJsonObject>();
	UnknownOp->SetStringField(TEXT("op"), TEXT("invalid_test_operation"));
	TArray<TSharedPtr<FJsonValue>> Operations;
	Operations.Add(MakeShared<FJsonValueNull>());
	Operations.Add(MakeShared<FJsonValueObject>(UnknownOp));
	Params->SetArrayField(TEXT("operations"), Operations);
	for (bool bStop : {true, false})
	{
		Params->SetBoolField(TEXT("stop_on_error"), bStop);
		const FMonolithActionResult Result = FMonolithBlueprintNodeActions::HandleBatchExecute(Params);
		if (!TestTrue(TEXT("Structured report returned"), Result.Result.IsValid())) continue;
		TestFalse(TEXT("Failed operations cannot report success"), Result.Result->GetBoolField(TEXT("success")));
		TestEqual(TEXT("Executed count"), Result.Result->GetIntegerField(TEXT("executed")), bStop ? 1 : 2);
		TestEqual(TEXT("Skipped count"), Result.Result->GetIntegerField(TEXT("skipped")), bStop ? 1 : 0);
		TestEqual(TEXT("Failed count"), Result.Result->GetIntegerField(TEXT("failed")), bStop ? 1 : 2);
	}
	Blueprint->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintSpecPreflightTest,
	"Monolith.Blueprint.Batch.SpecPreflight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintSpecPreflightTest::RunTest(const FString& Parameters)
{
	// No asset supplied: malformed specs must fail before any asset loading/editing.
	for (const TCHAR* Field : {TEXT("variables"), TEXT("components"), TEXT("nodes"), TEXT("connections"), TEXT("pin_defaults")})
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(Field, 7);
		FMonolithActionResult Result = FMonolithBlueprintBuildActions::HandleBuildBlueprintFromSpec(Params);
		TestFalse(TEXT("Reject non-array section"), Result.bSuccess);
		TestTrue(TEXT("Preflight explains no edits"), Result.ErrorMessage.Contains(TEXT("no edits applied")));
		Params->SetStringField(Field, TEXT("not json"));
		Result = FMonolithBlueprintBuildActions::HandleBuildBlueprintFromSpec(Params);
		TestFalse(TEXT("Reject malformed serialized section"), Result.bSuccess);
		TestTrue(TEXT("Malformed JSON is caught before loading"), Result.ErrorMessage.Contains(TEXT("no edits applied")));
	}
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), TEXT("duplicate"));
	Node->SetStringField(TEXT("type"), TEXT("Comment"));
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(Node));
	Nodes.Add(MakeShared<FJsonValueObject>(Node));
	Params->SetArrayField(TEXT("nodes"), Nodes);
	const FMonolithActionResult Result = FMonolithBlueprintBuildActions::HandleBuildBlueprintFromSpec(Params);
	TestFalse(TEXT("Duplicate node ID rejected"), Result.bSuccess);
	TestTrue(TEXT("Duplicate diagnosed before any nodes exist"), Result.ErrorMessage.Contains(TEXT("Duplicate node id")));
	return true;
}

#endif
