// Copyright tumourlove. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "MonolithToolRegistry.h"

namespace MonolithEditorGuideTests
{
	static FMonolithActionResult Execute(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), Action, Params);
	}

	static TSharedPtr<FJsonObject> Step(const FString& CompletionType, bool bIncludeProperty = true)
	{
		auto Target = MakeShared<FJsonObject>();
		// Intentionally nonexistent: structural rejection must occur before any
		// attempt to load or navigate to the otherwise well-formed first step.
		Target->SetStringField(TEXT("asset_path"), TEXT("/Game/__MonolithGuideValidationAssetMustNotLoad"));
		if (bIncludeProperty) Target->SetStringField(TEXT("property_path"), TEXT("TestProperty"));
		auto Completion = MakeShared<FJsonObject>();
		Completion->SetStringField(TEXT("type"), CompletionType);
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("title"), TEXT("Validation fixture"));
		Result->SetStringField(TEXT("instruction"), TEXT("This step is never executed."));
		Result->SetObjectField(TEXT("target"), Target);
		Result->SetObjectField(TEXT("completion"), Completion);
		return Result;
	}

	static TSharedPtr<FJsonObject> Workflow(const TArray<TSharedPtr<FJsonObject>>& Steps)
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("title"), TEXT("Rejected guide must preserve the current session"));
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const auto& StepObject : Steps) Values.Add(MakeShared<FJsonValueObject>(StepObject));
		Result->SetArrayField(TEXT("steps"), Values);
		return Result;
	}

	static bool CheckRejected(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Params,
		const FString& ExpectedErrorFragment)
	{
		UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (!Test.TestNotNull(TEXT("Asset editor subsystem available"), Editors)) return false;
		const auto BeforeAssets = Editors->GetAllEditedAssets();
		const auto Before = Execute(TEXT("get_guided_workflow_state"), MakeShared<FJsonObject>());
		if (!Test.TestTrue(TEXT("Existing guide state can be inspected"), Before.bSuccess && Before.Result.IsValid())) return false;

		const auto Rejected = Execute(TEXT("start_guided_workflow"), Params);
		Test.TestFalse(TEXT("Invalid guide is rejected"), Rejected.bSuccess);
		Test.TestEqual(TEXT("Structural error reports invalid params"), Rejected.ErrorCode, -32602);
		Test.TestTrue(TEXT("Error identifies the invalid condition before loading an asset"),
			Rejected.ErrorMessage.Contains(ExpectedErrorFragment, ESearchCase::IgnoreCase));

		const auto After = Execute(TEXT("get_guided_workflow_state"), MakeShared<FJsonObject>());
		if (Test.TestTrue(TEXT("Current guide is still inspectable"), After.bSuccess && After.Result.IsValid()))
		{
			for (const TCHAR* Field : { TEXT("workflow_id"), TEXT("title"), TEXT("status") })
			{
				FString BeforeValue, AfterValue;
				Test.TestTrue(FString(Field) + TEXT(" exists before"), Before.Result->TryGetStringField(Field, BeforeValue));
				Test.TestTrue(FString(Field) + TEXT(" exists after"), After.Result->TryGetStringField(Field, AfterValue));
				Test.TestEqual(FString(Field) + TEXT(" preserved"), AfterValue, BeforeValue);
			}
			for (const TCHAR* Field : { TEXT("current_step_index"), TEXT("total_steps") })
			{
				double BeforeValue = -999.0, AfterValue = -998.0;
				Test.TestTrue(FString(Field) + TEXT(" exists before"), Before.Result->TryGetNumberField(Field, BeforeValue));
				Test.TestTrue(FString(Field) + TEXT(" exists after"), After.Result->TryGetNumberField(Field, AfterValue));
				Test.TestEqual(FString(Field) + TEXT(" preserved"), AfterValue, BeforeValue);
			}
		}
		const auto AfterAssets = Editors->GetAllEditedAssets();
		Test.TestEqual(TEXT("No editor opened or closed"), AfterAssets.Num(), BeforeAssets.Num());
		for (UObject* Asset : BeforeAssets) Test.TestTrue(TEXT("Existing edited asset retained"), AfterAssets.Contains(Asset));
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGuideEmptyStepsTest,
	"Monolith.Editor.Guide.EmptyStepsPreserveSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithGuideEmptyStepsTest::RunTest(const FString&)
{
	using namespace MonolithEditorGuideTests;
	return CheckRejected(*this, Workflow({}), TEXT("steps"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGuideUnknownCompletionTest,
	"Monolith.Editor.Guide.UnknownCompletionValidatedBeforeNavigation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithGuideUnknownCompletionTest::RunTest(const FString&)
{
	using namespace MonolithEditorGuideTests;
	return CheckRejected(*this, Workflow({ Step(TEXT("confirm"), false), Step(TEXT("skip_without_checking")) }), TEXT("completion"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGuideMissingExpectedValueTest,
	"Monolith.Editor.Guide.PropertyEqualsRequiresExpectedValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithGuideMissingExpectedValueTest::RunTest(const FString&)
{
	using namespace MonolithEditorGuideTests;
	return CheckRejected(*this, Workflow({ Step(TEXT("confirm"), false), Step(TEXT("property_equals")) }), TEXT("expected_value"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGuideMissingPropertyTest,
	"Monolith.Editor.Guide.PropertyClickRequiresPropertyPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithGuideMissingPropertyTest::RunTest(const FString&)
{
	using namespace MonolithEditorGuideTests;
	return CheckRejected(*this, Workflow({ Step(TEXT("confirm"), false), Step(TEXT("property_clicked"), false) }), TEXT("property_path"));
}

#endif // WITH_DEV_AUTOMATION_TESTS
