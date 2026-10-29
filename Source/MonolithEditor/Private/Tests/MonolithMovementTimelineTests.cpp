#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithPieSmokeSession.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMovementTimelineValidationTest,
	"Monolith.Editor.MovementTimeline.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMovementTimelineValidationTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UInputAction> Forward(NewObject<UInputAction>(GetTransientPackage()));
	TStrongObjectPtr<UInputAction> Strafe(NewObject<UInputAction>(GetTransientPackage()));
	Forward->ValueType = EInputActionValueType::Axis1D;
	Strafe->ValueType = EInputActionValueType::Axis1D;
	const auto Segment = [](UInputAction* Action, double Start, double Duration)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("input_action"), Action->GetPathName());
		Obj->SetNumberField(TEXT("start_seconds"), Start);
		Obj->SetNumberField(TEXT("duration_seconds"), Duration);
		Obj->SetNumberField(TEXT("value"), 1.0);
		return Obj;
	};
	const auto Parse = [](const TArray<TSharedPtr<FJsonObject>>& Entries, double Duration, FString& Error)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Json;
		for (const auto& Entry : Entries) { Json.Add(MakeShared<FJsonValueObject>(Entry)); }
		Params->SetArrayField(TEXT("input_timeline"), Json);
		TArray<FPieSmokeInputSegment> Parsed;
		return ParsePieSmokeInputTimeline(Params, Duration, Parsed, Error);
	};
	FString Error;
	TestTrue(TEXT("Adjacent holds of one action are valid"), Parse({Segment(Forward.Get(), 0, 1), Segment(Forward.Get(), 1, 1)}, 3, Error));
	TestTrue(TEXT("Distinct actions can overlap"), Parse({Segment(Forward.Get(), 0, 2), Segment(Strafe.Get(), 0, 2)}, 3, Error));
	TestFalse(TEXT("Same action overlaps rejected"), Parse({Segment(Forward.Get(), 0, 2), Segment(Forward.Get(), 1, 1)}, 3, Error));
	TestFalse(TEXT("Outside duration rejected"), Parse({Segment(Forward.Get(), 2, 2)}, 3, Error));
	TestFalse(TEXT("Negative start rejected"), Parse({Segment(Forward.Get(), -1, 1)}, 3, Error));
	TestFalse(TEXT("Zero hold duration rejected"), Parse({Segment(Forward.Get(), 0, 0)}, 3, Error));
	TestFalse(TEXT("NaN session duration rejected"), Parse({Segment(Forward.Get(), 0, 1)}, std::numeric_limits<double>::quiet_NaN(), Error));
	TestFalse(TEXT("Infinite start rejected"), Parse({Segment(Forward.Get(), std::numeric_limits<double>::infinity(), 1)}, 3, Error));
	auto WrongType = Segment(Forward.Get(), 0, 1);
	WrongType->SetBoolField(TEXT("value"), true);
	TestFalse(TEXT("Action value shape mismatch rejected"), Parse({WrongType}, 3, Error));
	auto MalformedArray = Segment(Forward.Get(), 0, 1);
	MalformedArray->SetArrayField(TEXT("value"), {MakeShared<FJsonValueString>(TEXT("bad")), MakeShared<FJsonValueNumber>(1)});
	TestFalse(TEXT("Malformed vector rejected without coercion"), Parse({MalformedArray}, 3, Error));
	auto FractionalPlayer = Segment(Forward.Get(), 0, 1);
	FractionalPlayer->SetNumberField(TEXT("player_index"), 0.5);
	TestFalse(TEXT("Fractional player index rejected"), Parse({FractionalPlayer}, 3, Error));
	auto ShortName = Segment(Forward.Get(), 0, 1);
	ShortName->SetStringField(TEXT("input_action"), TEXT("IA_Move"));
	TestFalse(TEXT("Ambiguous short action name rejected"), Parse({ShortName}, 3, Error));
	TSharedPtr<FJsonObject> InfiniteDuration = MakeShared<FJsonObject>();
	InfiniteDuration->SetNumberField(TEXT("duration"), std::numeric_limits<double>::infinity());
	InfiniteDuration->SetArrayField(TEXT("input_timeline"), {MakeShared<FJsonValueObject>(Segment(Forward.Get(), 0, 1))});
	TArray<FPieSmokeInputSegment> Parsed;
	TestFalse(TEXT("Original infinite duration rejected even when caller passed clamped 120"),
		ParsePieSmokeInputTimeline(InfiniteDuration, 120.0, Parsed, Error));
	return true;
}

#endif
