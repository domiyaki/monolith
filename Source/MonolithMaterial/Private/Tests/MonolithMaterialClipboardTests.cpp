#include "Misc/AutomationTest.h"
#include "MonolithMaterialClipboard.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "EdGraphUtilities.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
TSharedPtr<FJsonObject> Fixture()
{
	const FString Json = TEXT(R"JSON({
 "summary":"Clipboard roundtrip fixture",
 "graph_spec":{
  "nodes":[{"id":"constant","class":"Constant","pos":[-300,0],"props":{"R":0.25}},{"id":"note","class":"Comment","pos":[-350,-150],"props":{"Text":"Editable delivery","SizeX":650,"SizeY":400}}],
  "custom_hlsl_nodes":[{"id":"sum","pos":[0,0],"description":"Sum","output_type":"Float3","code":"return float3(Value + Offset, 0, 0);","inputs":[{"name":"Value"},{"name":"Offset"}]}],
  "connections":[{"from":"constant","to":"sum","to_pin":"Value"}]
 },
 "external_inputs":[{"node":"sum","pin":"Offset","name":"Offset","sample":[0.5]}],
 "outputs":[{"node":"sum","pin":"","name":"Sum"}]
})JSON");
	TSharedPtr<FJsonObject> P;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), P);
	return P;
}

TSharedPtr<FJsonObject> NamedRerouteFixture()
{
	const FString Json = TEXT(R"JSON({
 "summary":"Named reroute ownership and shared normal fixture",
 "graph_spec":{
  "nodes":[
   {"id":"normal","class":"NamedRerouteDeclaration","pos":[-900,0],"props":{"Name":"ClipboardTest_NormalWS"}},
   {"id":"envNormal","class":"NamedRerouteUsage","pos":[-600,-200]},
   {"id":"specNormal","class":"NamedRerouteUsage","pos":[-600,200]},
   {"id":"sum","class":"Add","pos":[-300,0]},
   {"id":"result","class":"NamedRerouteDeclaration","pos":[0,0],"props":{"Name":"ClipboardTest_Response"}},
   {"id":"output","class":"NamedRerouteUsage","pos":[600,0]}
  ],
  "connections":[
   {"from":"envNormal","to":"sum","to_pin":"A"},
   {"from":"specNormal","to":"sum","to_pin":"B"},
   {"from":"sum","to":"result"}
  ],
  "named_reroutes":[
   {"usage":"envNormal","declaration":"normal"},
   {"usage":"specNormal","declaration":"normal"},
   {"usage":"output","declaration":"result"}
  ]
 },
 "external_inputs":[{"node":"normal","pin":"","name":"NormalWS","sample":[0,0,1]}],
 "outputs":[{"node":"output","pin":"","name":"Response"}]
})JSON");
	TSharedPtr<FJsonObject> P;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), P);
	return P;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialClipboardNamedReroutes, "Monolith.Material.Clipboard.NamedReroutes.RoundtripOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialClipboardNamedReroutes::RunTest(const FString&)
{
	FString Text;
	const auto R = FMonolithMaterialClipboard::Prepare(NamedRerouteFixture(), Text);
	if (!TestTrue(*R.ErrorMessage, R.bSuccess)) return false;
	TestTrue(TEXT("Named dependency graph compiled"), R.Result->GetBoolField(TEXT("sample_compile_passed")));
	TestTrue(TEXT("Named dependency graph round-tripped"), R.Result->GetBoolField(TEXT("native_roundtrip_passed")));
	TestTrue(TEXT("Imported named reference ownership verified"), R.Result->GetBoolField(TEXT("named_reroute_ownership_passed")));
	TestEqual(TEXT("Three named bindings reported"), R.Result->GetNumberField(TEXT("named_reroute_binding_count")), 3.0);
	TestEqual(TEXT("Six delivery nodes, no validation samples"), R.Result->GetNumberField(TEXT("node_count")), 6.0);

	// Independently import the delivered text. A self-consistent fingerprint alone
	// must not hide a Usage still pointing to an expression in the source scratch.
	TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient));
	Material->MaterialGraph = NewObject<UMaterialGraph>(Material.Get(), NAME_None, RF_Transient);
	Material->MaterialGraph->Material = Material.Get();
	Material->MaterialGraph->Schema = UMaterialGraphSchema::StaticClass();
	TSet<UEdGraphNode*> Imported;
	FEdGraphUtilities::ImportNodesFromText(Material->MaterialGraph, Text, Imported);
	TSet<UMaterialExpression*> Expressions;
	for (UEdGraphNode* Node : Imported)
	{
		if (const auto* GraphNode = Cast<UMaterialGraphNode>(Node))
		{
			UMaterialExpression* Expression = GraphNode->MaterialExpression;
			Expression->Material = Material.Get();
			Material->GetExpressionCollection().AddExpression(Expression);
			Expressions.Add(Expression);
		}
	}
	Material->MaterialGraph->LinkMaterialExpressionsFromGraph();
	TArray<UMaterialExpression*> PastedExpressions = Expressions.Array();
	for (UMaterialExpression* Expression : PastedExpressions) Expression->PostCopyNode(PastedExpressions);
	int32 UsageCount = 0;
	TMap<UObject*, int32> ReferencesPerDeclaration;
	for (UMaterialExpression* Expression : Expressions)
	{
		if (Expression->GetClass()->GetName() != TEXT("MaterialExpressionNamedRerouteUsage")) continue;
		++UsageCount;
		const auto* DeclarationProperty = FindFProperty<FObjectPropertyBase>(Expression->GetClass(), TEXT("Declaration"));
		if (!TestNotNull(TEXT("Usage has declaration reference"), DeclarationProperty)) continue;
		UObject* Declaration = DeclarationProperty->GetObjectPropertyValue_InContainer(Expression);
		TestTrue(TEXT("Usage references a declaration in this paste, not the source graph"),
			Expressions.Contains(Cast<UMaterialExpression>(Declaration)));
		const auto* Usage = CastChecked<UMaterialExpressionNamedRerouteUsage>(Expression);
		if (const auto* TypedDeclaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(Declaration))
			TestTrue(TEXT("Usage GUID matches the imported declaration GUID"), Usage->DeclarationGuid == TypedDeclaration->VariableGuid);
		if (Declaration) ++ReferencesPerDeclaration.FindOrAdd(Declaration);
	}
	TestEqual(TEXT("Three local usages survive import"), UsageCount, 3);
	TestEqual(TEXT("Two distinct declarations survive import"), ReferencesPerDeclaration.Num(), 2);
	bool bSharedNormalRemainsOpen = false;
	for (const auto& Pair : ReferencesPerDeclaration)
	{
		if (Pair.Value != 2) continue;
		const FExpressionInput* Input = CastChecked<UMaterialExpression>(Pair.Key)->GetInput(0);
		bSharedNormalRemainsOpen = Input && !Input->Expression;
	}
	TestTrue(TEXT("Shared normal declaration is reachable but its external input stays empty"), bSharedNormalRemainsOpen);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialClipboardNamedRerouteFailures, "Monolith.Material.Clipboard.NamedReroutes.InvalidBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialClipboardNamedRerouteFailures::RunTest(const FString&)
{
	auto CheckRejected = [this](const TCHAR* Label, const TSharedPtr<FJsonObject>& P)
	{
		FString Text;
		const auto R = FMonolithMaterialClipboard::Prepare(P, Text);
		TestFalse(Label, R.bSuccess);
		TestTrue(TEXT("Rejected named graph exports no partial text"), Text.IsEmpty());
	};
	for (const TCHAR* Key : {TEXT("usage"), TEXT("declaration")})
	{
		auto P = NamedRerouteFixture();
		P->GetObjectField(TEXT("graph_spec"))->GetArrayField(TEXT("named_reroutes"))[0]->AsObject()->SetStringField(Key, TEXT("missing"));
		CheckRejected(TEXT("Missing binding ID rejected"), P);
		P = NamedRerouteFixture();
		P->GetObjectField(TEXT("graph_spec"))->GetArrayField(TEXT("named_reroutes"))[0]->AsObject()->SetStringField(Key, TEXT("sum"));
		CheckRejected(TEXT("Binding to wrong expression class rejected"), P);
		P = NamedRerouteFixture();
		P->GetObjectField(TEXT("graph_spec"))->GetArrayField(TEXT("named_reroutes"))[0]->AsObject()->RemoveField(Key);
		CheckRejected(TEXT("Omitted binding ID rejected"), P);
	}
	auto P = NamedRerouteFixture();
	auto Bindings = P->GetObjectField(TEXT("graph_spec"))->GetArrayField(TEXT("named_reroutes"));
	Bindings.RemoveAt(0);
	P->GetObjectField(TEXT("graph_spec"))->SetArrayField(TEXT("named_reroutes"), Bindings);
	CheckRejected(TEXT("Unbound usage rejected before compilation"), P);
	P = NamedRerouteFixture();
	Bindings = P->GetObjectField(TEXT("graph_spec"))->GetArrayField(TEXT("named_reroutes"));
	const TSharedPtr<FJsonValue> DuplicateBinding = Bindings[0];
	Bindings.Add(DuplicateBinding);
	P->GetObjectField(TEXT("graph_spec"))->SetArrayField(TEXT("named_reroutes"), Bindings);
	CheckRejected(TEXT("Duplicate usage binding rejected"), P);
	P = NamedRerouteFixture();
	auto Nodes = P->GetObjectField(TEXT("graph_spec"))->GetArrayField(TEXT("nodes"));
	auto Unused = MakeShared<FJsonObject>();
	Unused->SetStringField(TEXT("id"), TEXT("unused"));
	Unused->SetStringField(TEXT("class"), TEXT("NamedRerouteDeclaration"));
	Nodes.Add(MakeShared<FJsonValueObject>(Unused));
	P->GetObjectField(TEXT("graph_spec"))->SetArrayField(TEXT("nodes"), Nodes);
	CheckRejected(TEXT("Unused declaration is not exempt from reachability"), P);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialClipboardRoundtrip, "Monolith.Material.Clipboard.Roundtrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialClipboardRoundtrip::RunTest(const FString&)
{
	// Also makes new actions discoverable after a Live Coding patch (StartupModule
	// runs only on a normal module load).
	FMonolithMaterialClipboard::Register(FMonolithToolRegistry::Get());
	FString Text;
	auto R = FMonolithMaterialClipboard::Prepare(Fixture(), Text);
	TestTrue(*R.ErrorMessage, R.bSuccess);
	TestTrue(TEXT("Native graph node text"), Text.Contains(TEXT("MaterialGraphNode")));
	TestFalse(TEXT("Validation value is not delivered"), Text.Contains(TEXT("R=0.500000")));
	if (R.bSuccess) TestEqual(TEXT("Only delivery nodes"), R.Result->GetNumberField(TEXT("node_count")), 3.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialClipboardFailures, "Monolith.Material.Clipboard.Failures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialClipboardFailures::RunTest(const FString&)
{
	FString Text;
	auto P = Fixture();
	P->GetArrayField(TEXT("external_inputs"))[0]->AsObject()->SetStringField(TEXT("pin"), TEXT("MissingPin"));
	TestFalse(TEXT("Unknown external pin rejected"), FMonolithMaterialClipboard::Prepare(P, Text).bSuccess);
	TestTrue(TEXT("No partial export"), Text.IsEmpty());
	P = Fixture();
	P->GetObjectField(TEXT("graph_spec"))->GetArrayField(TEXT("nodes"))[0]->AsObject()->GetObjectField(TEXT("props"))->SetNumberField(TEXT("TypoProperty"), 1);
	TestFalse(TEXT("Unknown property rejected"), FMonolithMaterialClipboard::Prepare(P, Text).bSuccess);
	P = Fixture();
	auto Node = P->GetObjectField(TEXT("graph_spec"))->GetArrayField(TEXT("nodes"))[0]->AsObject();
	Node->SetStringField(TEXT("class"), TEXT("TextureObject"));
	auto Props = MakeShared<FJsonObject>(); Props->SetStringField(TEXT("Texture"), TEXT("/Game/MonolithClipboardMissingDependency"));
	Node->SetObjectField(TEXT("props"), Props);
	AddExpectedError(TEXT("MonolithClipboardMissingDependency"), EAutomationExpectedErrorFlags::Contains, 0);
	TestFalse(TEXT("Missing dependency rejected"), FMonolithMaterialClipboard::Prepare(P, Text).bSuccess);
	FString Before, After, Error;
	FPlatformApplicationMisc::ClipboardPaste(Before);
	TestFalse(TEXT("Native failed write is detected"), FMonolithMaterialClipboard::TestFailedNativeWrite(Error));
	FPlatformApplicationMisc::ClipboardPaste(After);
	TestEqual(TEXT("Native rollback preserves clipboard text"), After, Before);
	TestTrue(TEXT("Native rollback completed"), Error.Contains(TEXT("original formats restored")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialClipboardBadShader, "Monolith.Material.Clipboard.BadShader",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialClipboardBadShader::RunTest(const FString&)
{
	auto P = Fixture();
	P->GetObjectField(TEXT("graph_spec"))->GetArrayField(TEXT("custom_hlsl_nodes"))[0]->AsObject()->SetStringField(TEXT("code"), TEXT("return MissingClipboardShaderIdentifier;"));
	AddExpectedError(TEXT("MissingClipboardShaderIdentifier"), EAutomationExpectedErrorFlags::Contains, 0);
	FString Text;
	auto R = FMonolithMaterialClipboard::Prepare(P, Text);
	TestFalse(TEXT("Bad HLSL rejected before copying"), R.bSuccess);
	TestTrue(TEXT("Compile diagnostic identifies shader error"), R.ErrorMessage.Contains(TEXT("MissingClipboardShaderIdentifier")));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialClipboardCache, "Monolith.Material.Clipboard.TemplateAndCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialClipboardCache::RunTest(const FString&)
{
	const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (!TestTrue(TEXT("Plugin found"), Plugin.IsValid())) return false;
	FString Json;
	if (!TestTrue(TEXT("Portable UV template found"), FFileHelper::LoadFileToString(Json,
		*FPaths::Combine(Plugin->GetBaseDir(), TEXT("Templates/MaterialClipboard/HighlightLookupUV.json"))))) return false;
	TSharedPtr<FJsonObject> P;
	if (!TestTrue(TEXT("Template JSON valid"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), P))) return false;
	auto R = FMonolithMaterialClipboard::Copy(P);
	if (!TestTrue(*R.ErrorMessage, R.bSuccess)) return false;
	FString Before, After;
	FPlatformApplicationMisc::ClipboardPaste(Before);
	P->GetArrayField(TEXT("external_inputs"))[0]->AsObject()->SetStringField(TEXT("pin"), TEXT("InvalidPin"));
	TestFalse(TEXT("Failed delivery rejected"), FMonolithMaterialClipboard::Copy(P).bSuccess);
	FPlatformApplicationMisc::ClipboardPaste(After);
	TestEqual(TEXT("Failed delivery preserves clipboard"), After, Before);
	FPlatformApplicationMisc::ClipboardCopy(TEXT("Monolith test overwrite"));
	R = FMonolithMaterialClipboard::Recopy(MakeShared<FJsonObject>());
	TestTrue(TEXT("Recopy last successful delivery"), R.bSuccess);
	FPlatformApplicationMisc::ClipboardPaste(After);
	TestEqual(TEXT("Failed delivery did not replace successful cache"), After, Before);
	return true;
}
#endif
