#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "MonolithIndexDatabase.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithIndexQueryEfficiencyTest,
	"Monolith.Index.QueryEfficiency.Semantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithIndexQueryEfficiencyTest::RunTest(const FString& Parameters)
{
	// Keep test databases separate from the live project index. CI and local
	// integrations can choose an artifact root without editing the source.
	FString ArtifactRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("MONOLITH_TEST_ARTIFACTS"));
	if (ArtifactRoot.IsEmpty())
	{
		ArtifactRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MonolithTests"));
	}
	FMonolithIndexDatabase DB;
	const FString Path = FPaths::Combine(ArtifactRoot, TEXT("Index"),
		FString::Printf(TEXT("QueryEfficiency_%s.db"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestTrue(TEXT("database opens"), DB.Open(Path))) return false;
	auto AddAsset = [&DB](const TCHAR* Name, const TCHAR* Module)
	{
		FIndexedAsset Asset;
		Asset.PackagePath = FString(TEXT("/Game/Test/")) + Name;
		Asset.AssetName = Name;
		Asset.AssetClass = TEXT("Blueprint");
		Asset.ModuleName = Module;
		return DB.InsertAsset(Asset);
	};
	const int64 Other = AddAsset(TEXT("Other"), TEXT("OtherModule"));
	const int64 First = AddAsset(TEXT("First"), TEXT("Wanted"));
	const int64 Second = AddAsset(TEXT("Second"), TEXT("Wanted"));
	const int64 Third = AddAsset(TEXT("Third"), TEXT("Wanted"));
	TestTrue(TEXT("fixture inserted"), Other > 0 && First > 0 && Second > 0 && Third > 0);
	const auto FirstPage = DB.FindByType(TEXT("Blueprint"), 2, 0, TEXT("wanted"));
	TestEqual(TEXT("filter fills first page"), FirstPage.Num(), 2);
	if (FirstPage.Num() == 2)
	{
		TestEqual(TEXT("first matching asset"), FirstPage[0].Id, First);
		TestEqual(TEXT("second matching asset"), FirstPage[1].Id, Second);
	}
	const auto SecondPage = DB.FindByType(TEXT("Blueprint"), 2, 2, TEXT("Wanted"));
	TestEqual(TEXT("offset applies to filtered results"), SecondPage.Num(), 1);
	if (SecondPage.Num() == 1) TestEqual(TEXT("third matching asset"), SecondPage[0].Id, Third);
	const auto Unfiltered = DB.FindByType(TEXT("Blueprint"), 1, 0);
	if (TestEqual(TEXT("unfiltered page"), Unfiltered.Num(), 1))
		TestEqual(TEXT("unfiltered order retained"), Unfiltered[0].Id, Other);
	TestEqual(TEXT("missing module is empty"), DB.FindByType(TEXT("Blueprint"), 2, 0, TEXT("Absent")).Num(), 0);
	const int64 UnicodeModule = AddAsset(TEXT("Unicode"), TEXT("M\u00f3dule"));
	const auto UnicodePage = DB.FindByType(TEXT("Blueprint"), 1, 0, TEXT("M\u00f3dule"));
	if (TestEqual(TEXT("non-ASCII module fallback"), UnicodePage.Num(), 1))
		TestEqual(TEXT("non-ASCII result"), UnicodePage[0].Id, UnicodeModule);

	auto AddEdge = [&DB](int64 Source, int64 Target, const TCHAR* Type)
	{
		FIndexedDependency Edge;
		Edge.SourceAssetId = Source;
		Edge.TargetAssetId = Target;
		Edge.DependencyType = Type;
		return DB.InsertDependency(Edge) > 0;
	};
	TestTrue(TEXT("hard edge"), AddEdge(First, Second, TEXT("Hard")));
	TestTrue(TEXT("soft edge same target"), AddEdge(First, Second, TEXT("Soft")));
	TestTrue(TEXT("duplicate edge"), AddEdge(First, Second, TEXT("Hard")));
	TestTrue(TEXT("incoming edge"), AddEdge(Third, First, TEXT("Soft")));
	TestTrue(TEXT("self edge"), AddEdge(First, First, TEXT("Hard")));
	const auto Refs = DB.FindReferences(TEXT("/Game/Test/First"));
	const auto& Outgoing = Refs->GetArrayField(TEXT("depends_on"));
	const auto& Incoming = Refs->GetArrayField(TEXT("referenced_by"));
	TestEqual(TEXT("duplicates and self edge retained"), Outgoing.Num(), 4);
	TestEqual(TEXT("incoming includes self"), Incoming.Num(), 2);
	if (Outgoing.Num() == 4)
	{
		TestEqual(TEXT("target path"), Outgoing[0]->AsObject()->GetStringField(TEXT("path")), FString(TEXT("/Game/Test/Second")));
		TestEqual(TEXT("edge type preserved"), Outgoing[1]->AsObject()->GetStringField(TEXT("type")), FString(TEXT("Soft")));
		TestEqual(TEXT("self edge order"), Outgoing[3]->AsObject()->GetStringField(TEXT("path")), FString(TEXT("/Game/Test/First")));
	}
	TestEqual(TEXT("isolated asset has empty references"), DB.FindReferences(TEXT("/Game/Test/Other"))->GetArrayField(TEXT("depends_on")).Num(), 0);
	TestEqual(TEXT("unknown asset keeps existing empty-object contract"), DB.FindReferences(TEXT("/Game/Test/Missing"))->Values.Num(), 0);
	return true;
}

#endif
