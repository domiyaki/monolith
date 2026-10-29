// Copyright tumourlove. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "MonolithToolRegistry.h"

namespace MonolithEditorNavigationTests
{
	// These are resolution failures, so no generated class, editor, saved package,
	// or project asset is required. Each fixture exists only for this test call.
	struct FFixture
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* FirstGraph = nullptr;
		UEdGraph* SecondGraph = nullptr;
		UEdGraphNode* FirstNode = nullptr;
		UEdGraphNode* SecondNode = nullptr;
		UAssetEditorSubsystem* Editors = nullptr;
		TArray<UObject*> PreviouslyEditedAssets;
		bool bPreviouslyDirty = false;

		FFixture()
		{
			Blueprint = NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient);
			Blueprint->AddToRoot();
			FirstGraph = NewObject<UEdGraph>(Blueprint, TEXT("FirstGraph"), RF_Transient);
			SecondGraph = NewObject<UEdGraph>(Blueprint, TEXT("SecondGraph"), RF_Transient);
			Blueprint->FunctionGraphs.Add(FirstGraph);
			Blueprint->FunctionGraphs.Add(SecondGraph);
			FirstNode = NewObject<UEdGraphNode>(FirstGraph, TEXT("RepeatedNode"), RF_Transient);
			SecondNode = NewObject<UEdGraphNode>(SecondGraph, TEXT("RepeatedNode"), RF_Transient);
			FirstNode->CreateNewGuid();
			SecondNode->CreateNewGuid();
			FirstGraph->Nodes.Add(FirstNode);
			SecondGraph->Nodes.Add(SecondNode);
			Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
			if (Editors) { PreviouslyEditedAssets = Editors->GetAllEditedAssets(); }
			bPreviouslyDirty = Blueprint->GetOutermost()->IsDirty();
		}

		~FFixture()
		{
			if (Editors && Editors->FindEditorForAsset(Blueprint, false))
			{
				Editors->CloseAllEditorsForAsset(Blueprint);
			}
			Blueprint->RemoveFromRoot();
		}

		TSharedPtr<FJsonObject> Params() const
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
			return Result;
		}

		void CheckUnchanged(FAutomationTestBase& Test) const
		{
			Test.TestEqual(TEXT("Resolution failure preserves package dirty state"),
				Blueprint->GetOutermost()->IsDirty(), bPreviouslyDirty);
			if (!Test.TestNotNull(TEXT("Asset editor subsystem available"), Editors)) { return; }
			Test.TestNull(TEXT("Resolution failure does not open fixture editor"),
				Editors->FindEditorForAsset(Blueprint, false));
			const TArray<UObject*> After = Editors->GetAllEditedAssets();
			Test.TestEqual(TEXT("Existing editor asset count unchanged"), After.Num(), PreviouslyEditedAssets.Num());
			for (UObject* Asset : PreviouslyEditedAssets)
			{
				Test.TestTrue(TEXT("Existing edited asset retained"), After.Contains(Asset));
			}
		}
	};

	static FMonolithActionResult Navigate(const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("navigate_to"), Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNavigationMissingGraphTest,
	"Monolith.Editor.Navigation.MissingGraphPreservesEditors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNavigationMissingGraphTest::RunTest(const FString&)
{
	using namespace MonolithEditorNavigationTests;
	FFixture Fixture;
	auto Params = Fixture.Params();
	Params->SetStringField(TEXT("graph_name"), TEXT("MissingGraph"));
	const FMonolithActionResult Result = Navigate(Params);
	TestFalse(TEXT("Missing graph is rejected"), Result.bSuccess);
	TestTrue(TEXT("Failure identifies graph resolution"), Result.ErrorMessage.Contains(TEXT("graph"), ESearchCase::IgnoreCase));
	Fixture.CheckUnchanged(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNavigationAmbiguousNodeTest,
	"Monolith.Editor.Navigation.AmbiguousNodePreservesEditors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNavigationAmbiguousNodeTest::RunTest(const FString&)
{
	using namespace MonolithEditorNavigationTests;
	FFixture Fixture;
	auto Params = Fixture.Params();
	Params->SetStringField(TEXT("node_id"), TEXT("RepeatedNode"));
	const FMonolithActionResult Result = Navigate(Params);
	TestFalse(TEXT("Same object name in two graphs is rejected"), Result.bSuccess);
	TestEqual(TEXT("Ambiguity is an invalid-target error"), Result.ErrorCode, -32602);
	if (TestTrue(TEXT("Ambiguity includes structured resolution data"), Result.ErrorData.IsValid()))
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (TestTrue(TEXT("Resolution data is an object"), Result.ErrorData->TryGetObject(Data) && Data))
		{
			const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
			if (TestTrue(TEXT("Candidates are returned"), (*Data)->TryGetArrayField(TEXT("candidates"), Candidates) && Candidates))
			{
				TestEqual(TEXT("Both conflicting nodes are returned"), Candidates->Num(), 2);
				TSet<FString> GraphPaths;
				for (const auto& Candidate : *Candidates)
				{
					const TSharedPtr<FJsonObject>* Node = nullptr;
					if (Candidate->TryGetObject(Node) && Node)
					{
						FString GraphPath;
						if ((*Node)->TryGetStringField(TEXT("graph_path"), GraphPath)) { GraphPaths.Add(GraphPath); }
					}
				}
				TestTrue(TEXT("Candidates identify first graph"), GraphPaths.Contains(Fixture.FirstGraph->GetPathName()));
				TestTrue(TEXT("Candidates identify second graph"), GraphPaths.Contains(Fixture.SecondGraph->GetPathName()));
			}
		}
	}
	Fixture.CheckUnchanged(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNavigationGraphScopeTest,
	"Monolith.Editor.Navigation.NodeCannotEscapeGraphScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNavigationGraphScopeTest::RunTest(const FString&)
{
	using namespace MonolithEditorNavigationTests;
	FFixture Fixture;
	auto Params = Fixture.Params();
	Params->SetStringField(TEXT("graph_name"), Fixture.FirstGraph->GetName());
	// A valid GUID belonging to the other graph must not override graph scope.
	Params->SetStringField(TEXT("node_id"), Fixture.SecondNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	const FMonolithActionResult Result = Navigate(Params);
	TestFalse(TEXT("Node outside requested graph is rejected"), Result.bSuccess);
	TestTrue(TEXT("Failure identifies node resolution"), Result.ErrorMessage.Contains(TEXT("node"), ESearchCase::IgnoreCase));
	Fixture.CheckUnchanged(*this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNavigationInvalidPropertyTest,
	"Monolith.Editor.Navigation.InvalidPropertyPreservesEditors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNavigationInvalidPropertyTest::RunTest(const FString&)
{
	using namespace MonolithEditorNavigationTests;
	FFixture Fixture;
	auto Params = Fixture.Params();
	Params->SetStringField(TEXT("graph_name"), Fixture.FirstGraph->GetPathName());
	Params->SetStringField(TEXT("node_id"), TEXT("RepeatedNode"));
	Params->SetStringField(TEXT("property_path"), TEXT("MissingNavigationProperty"));
	const FMonolithActionResult Result = Navigate(Params);
	TestFalse(TEXT("Unknown property is rejected before editor navigation"), Result.bSuccess);
	// Reaching property resolution also demonstrates that a full graph path
	// disambiguates the same node name, without opening a synthetic BP editor.
	TestTrue(TEXT("Scoped node resolved and failure identifies property"),
		Result.ErrorMessage.Contains(TEXT("property"), ESearchCase::IgnoreCase));
	Fixture.CheckUnchanged(*this);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
