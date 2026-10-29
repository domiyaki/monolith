#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithRegistryLookupEfficiencyTest,
	"Monolith.Registry.DirectMetadataLookup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithRegistryLookupEfficiencyTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const FString Namespace = TEXT("monolith_lookup_test_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	ON_SCOPE_EXIT { Registry.UnregisterNamespace(Namespace); };
	TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("fixture"), TEXT("preserved"));
	Registry.RegisterAction(Namespace, TEXT("second"), TEXT("Full second description"), FMonolithActionHandler(), Schema, TEXT("Category"));
	Registry.RegisterAction(Namespace, TEXT("first"), TEXT("Full first description"), FMonolithActionHandler());
	const TArray<FMonolithActionInfo> Baseline = Registry.GetActions(Namespace);
	const TArray<FString> Names = Registry.GetActionNames(Namespace);
	TestEqual(TEXT("Name count matches full listing"), Names.Num(), Baseline.Num());
	for (int32 Index = 0; Index < FMath::Min(Names.Num(), Baseline.Num()); ++Index)
	{
		TestEqual(TEXT("Registration order preserved"), Names[Index], Baseline[Index].Action);
		FMonolithActionInfo Info;
		TestTrue(TEXT("Exact lookup finds action"), Registry.TryGetActionInfo(Namespace, Names[Index], Info));
		TestEqual(TEXT("Full description preserved"), Info.Description, Baseline[Index].Description);
		TestEqual(TEXT("Category preserved"), Info.Category, Baseline[Index].Category);
		TestTrue(TEXT("Schema preserved"), Info.ParamSchema == Baseline[Index].ParamSchema);
	}
	FMonolithActionInfo Info;
	Registry.SetActionAnnotations(Namespace, TEXT("second"), true, false, true, TEXT("New title"));
	Registry.TryGetActionInfo(Namespace, TEXT("second"), Info);
	TestEqual(TEXT("Metadata updates visible immediately"), Info.Title, FString(TEXT("New title")));
	TestFalse(TEXT("Unknown action is a miss"), Registry.TryGetActionInfo(Namespace, TEXT("missing"), Info));
	TestTrue(TEXT("Miss clears previous info"), Info.Action.IsEmpty() && !Info.ParamSchema.IsValid());
	Registry.UnregisterNamespace(Namespace);
	TestTrue(TEXT("Unregistered names disappear"), Registry.GetActionNames(Namespace).IsEmpty());
	TestFalse(TEXT("Unregistered metadata disappears"), Registry.TryGetActionInfo(Namespace, TEXT("second"), Info));
	Registry.RegisterAction(Namespace, TEXT("second"), TEXT("Replacement description"), FMonolithActionHandler());
	TestTrue(TEXT("Re-registration visible"), Registry.TryGetActionInfo(Namespace, TEXT("second"), Info));
	TestEqual(TEXT("No stale metadata after re-registration"), Info.Description, FString(TEXT("Replacement description")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithResponseShapingWideObjectTest,
	"Monolith.ResponseShaping.WideObjectIteratorRemoval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithResponseShapingWideObjectTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Fields;
	for (int32 Index = 0; Index < 1024; ++Index)
	{
		const FString Key = FString::Printf(TEXT("field_%d"), Index);
		Response->SetNumberField(Key, Index);
		if (Index % 2 == 0) Fields.Add(MakeShared<FJsonValueString>(Key));
	}
	Response->SetStringField(TEXT("empty"), TEXT(""));
	Response->SetBoolField(TEXT("false_value"), false);
	Fields.Add(MakeShared<FJsonValueString>(TEXT("empty")));
	Fields.Add(MakeShared<FJsonValueString>(TEXT("false_value")));
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("_fields"), Fields);
	Params->SetBoolField(TEXT("_compact_json"), true);
	TArray<FString> Warnings;
	ApplyResponseShaping(Response, Params, Warnings);
	TestEqual(TEXT("Only selected nonempty fields remain"), Response->Values.Num(), 513);
	TestEqual(TEXT("No new warnings"), Warnings.Num(), 0);
	for (int32 Index = 0; Index < 1024; ++Index)
	{
		const FString Key = FString::Printf(TEXT("field_%d"), Index);
		TestEqual(TEXT("Removal did not skip neighboring keys"), Response->HasField(Key), Index % 2 == 0);
		if (Index % 2 == 0) TestEqual(TEXT("Retained value unchanged"), Response->GetIntegerField(Key), Index);
	}
	TestTrue(TEXT("False is not empty"), Response->HasField(TEXT("false_value")));
	TestFalse(TEXT("Empty string removed"), Response->HasField(TEXT("empty")));
	return true;
}

#endif
