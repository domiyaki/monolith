#include "Misc/AutomationTest.h"
#include "MonolithMaterialActions.h"
#include "MonolithToolRegistry.h"
#include "IMaterialEditor.h"
#include "MaterialEditorModule.h"
#include "MaterialEditorActions.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialEditor/MaterialEditorInstanceConstant.h"
#include "MaterialEditor/DEditorScalarParameterValue.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Framework/Commands/UICommandList.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithOpenMaterialSyncTest,
    "Monolith.Material.EditorSync.PendingEditsAndApply",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithOpenMaterialSyncTest::RunTest(const FString&)
{
    FMonolithMaterialActions::RegisterActions(FMonolithToolRegistry::Get());
    const FString PackageName = TEXT("/Temp/MonolithEditorSync_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UMaterial* Material = NewObject<UMaterial>(CreatePackage(*PackageName), TEXT("SyncFixture"), RF_Public | RF_Standalone | RF_Transactional);
    Material->SetShadingModel(MSM_Unlit);
    auto* A = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -300, 0));
    A->ParameterName = TEXT("ManualValue");
    A->DefaultValue = 0.2f;
    auto* B = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), 0, 0));
    B->Code = TEXT("return Value;");
    B->OutputType = CMOT_Float1;
    FCustomInput Input; Input.InputName = TEXT("Value"); B->Inputs.Add(Input);
    UMaterialEditingLibrary::ConnectMaterialExpressions(A, TEXT(""), B, TEXT("Value"));
    UMaterialEditingLibrary::ConnectMaterialProperty(B, TEXT(""), MP_EmissiveColor);
    const FString AssetPath = Material->GetPathName();
    const FName AName = A->GetFName();
    const FName BName = B->GetFName();
    auto& Module = FModuleManager::LoadModuleChecked<IMaterialEditorModule>(TEXT("MaterialEditor"));
    TSharedRef<IMaterialEditor> Editor = Module.CreateMaterialEditor(EToolkitMode::Standalone, nullptr, Material);
    auto* Preview = Cast<UMaterial>(Editor->GetMaterialInterface());
    auto* PreviewA = FindObject<UMaterialExpressionScalarParameter>(Preview, *AName.ToString());
    if (!TestNotNull(TEXT("Actual toolkit preview expression"), PreviewA)) return false;
    PreviewA->DefaultValue = 0.73f;
    Editor->MarkMaterialDirty();

    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), AssetPath);
    Params->SetStringField(TEXT("expression_name"), BName.ToString());
    Params->SetStringField(TEXT("code"), TEXT("return Value * 0.5;"));
    FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("update_custom_hlsl_node"), Params);
    TestTrue(*R.ErrorMessage, R.bSuccess);
    if (R.bSuccess) TestEqual(TEXT("Editor was synchronized"), R.Result->GetStringField(TEXT("editor_sync")), FString(TEXT("refreshed")));
    Material = FindObject<UMaterial>(nullptr, *AssetPath);
    Preview = Cast<UMaterial>(Editor->GetMaterialInterface());
    TestEqual(TEXT("Unapplied manual edit preserved in source"), FindObject<UMaterialExpressionScalarParameter>(Material, *AName.ToString())->DefaultValue, 0.73f);
    TestEqual(TEXT("Unapplied manual edit preserved in preview"), FindObject<UMaterialExpressionScalarParameter>(Preview, *AName.ToString())->DefaultValue, 0.73f);
    TestEqual(TEXT("Custom code reaches displayed preview"), FindObject<UMaterialExpressionCustom>(Preview, *BName.ToString())->Code, FString(TEXT("return Value * 0.5;")));
    // Reproduce the original regression: a subsequent user Apply must not restore old code.
    Editor->MarkMaterialDirty();
    Editor->GetToolkitCommands()->ExecuteAction(FMaterialEditorCommands::Get().Apply.ToSharedRef());
    Material = FindObject<UMaterial>(nullptr, *AssetPath);
    TestEqual(TEXT("Later Apply retains external edit"), FindObject<UMaterialExpressionCustom>(Material, *BName.ToString())->Code, FString(TEXT("return Value * 0.5;")));
    Editor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted);
    Material->ClearFlags(RF_Standalone);
    Material->GetPackage()->SetDirtyFlag(false);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithInstanceEditorSyncTest,
    "Monolith.Material.EditorSync.InstanceOverrideCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithInstanceEditorSyncTest::RunTest(const FString&)
{
    FMonolithMaterialActions::RegisterActions(FMonolithToolRegistry::Get());
    UPackage* Package = CreatePackage(*(TEXT("/Temp/MonolithInstanceSync_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    UMaterial* Material = NewObject<UMaterial>(Package, TEXT("Base"), RF_Public | RF_Standalone | RF_Transactional);
    Material->SetShadingModel(MSM_Unlit);
    auto* Parameter = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass()));
    Parameter->ParameterName = TEXT("Value");
    Parameter->DefaultValue = 0.2f;
    UMaterialEditingLibrary::ConnectMaterialProperty(Parameter, TEXT(""), MP_EmissiveColor);
    UMaterialEditingLibrary::RecompileMaterial(Material);
    auto* Instance = NewObject<UMaterialInstanceConstant>(Package, TEXT("Instance"), RF_Public | RF_Standalone | RF_Transactional);
    Instance->SetParentEditorOnly(Material);
    Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Value")), 0.3f);
    auto& Module = FModuleManager::LoadModuleChecked<IMaterialEditorModule>(TEXT("MaterialEditor"));
    auto Editor = Module.CreateMaterialInstanceEditor(EToolkitMode::Standalone, nullptr, Instance);
    UMaterialEditorInstanceConstant* Proxy = nullptr;
    for (UObject* Object : *Editor->GetObjectsCurrentlyBeingEdited())
        if (auto* Candidate = Cast<UMaterialEditorInstanceConstant>(Object)) Proxy = Candidate;
    if (!TestNotNull(TEXT("Real instance editor cache"), Proxy)) return false;
    UMaterialInstanceConstant* OriginalPreviewInstance = Proxy->PreviewInstance;
    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), Instance->GetPathName());
    Params->SetStringField(TEXT("parameter_name"), TEXT("Value"));
    Params->SetNumberField(TEXT("scalar_value"), 0.81);
    auto R = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("set_instance_parameter"), Params);
    TestTrue(*R.ErrorMessage, R.bSuccess);
    TestTrue(TEXT("External sync preserves the viewport's preview instance"),
        Proxy->PreviewInstance == OriginalPreviewInstance);

    // The base and instance overlap: both affect this open instance editor.
    // Keep one result per input, but compile the base and refresh the cache once.
    auto BatchParams = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> BatchPaths;
    BatchPaths.Add(MakeShared<FJsonValueString>(Material->GetPathName()));
    BatchPaths.Add(MakeShared<FJsonValueString>(Instance->GetPathName()));
    BatchPaths.Add(MakeShared<FJsonValueString>(Instance->GetPathName()));
    BatchParams->SetArrayField(TEXT("asset_paths"), BatchPaths);
    auto BatchResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("batch_recompile"), BatchParams);
    TestTrue(*BatchResult.ErrorMessage, BatchResult.bSuccess);
    if (BatchResult.bSuccess && BatchResult.Result.IsValid())
    {
        TestEqual(TEXT("One compile for a shared base"), BatchResult.Result->GetNumberField(TEXT("recompile_count")), 1.0);
        TestEqual(TEXT("All requested entries remain successful"), BatchResult.Result->GetNumberField(TEXT("success_count")), 3.0);
        TestEqual(TEXT("Overlapping targets refresh the instance editor once"), BatchResult.Result->GetNumberField(TEXT("synchronized_instance_editors")), 1.0);
        const auto& Rows = BatchResult.Result->GetArrayField(TEXT("results"));
        TestEqual(TEXT("One result per input including duplicates"), Rows.Num(), 3);
        if (Rows.Num() == 3)
        {
            TestFalse(TEXT("First target compiles"), Rows[0]->AsObject()->GetBoolField(TEXT("reused_base_recompile")));
            TestTrue(TEXT("Instance reuses the base compile"), Rows[1]->AsObject()->GetBoolField(TEXT("reused_base_recompile")));
            TestTrue(TEXT("Repeated instance reuses the base compile"), Rows[2]->AsObject()->GetBoolField(TEXT("reused_base_recompile")));
        }
    }
    TestTrue(TEXT("Batch sync preserves the live preview instance"), Proxy->PreviewInstance == OriginalPreviewInstance);
    UDEditorScalarParameterValue* CachedValue = nullptr;
    for (const FEditorParameterGroup& Group : Proxy->ParameterGroups)
        for (const auto& Entry : Group.Parameters)
            if (auto* Scalar = Cast<UDEditorScalarParameterValue>(Entry))
                if (Scalar->ParameterInfo.Name == TEXT("Value")) CachedValue = Scalar;
    if (TestNotNull(TEXT("Visible parameter exists"), CachedValue))
        TestEqual(TEXT("Visible editor cache reflects tool override"), CachedValue->ParameterValue, 0.81f);
    // A later details-panel edit invokes this copy, and must not restore 0.3.
    Proxy->CopyToSourceInstance();
    float Value = 0;
    Instance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Value")), Value);
    TestEqual(TEXT("Later UI copy preserves external override"), Value, 0.81f);

    // A slider sends Interactive changes while the mouse is held. The preview
    // object must receive those changes without waiting for the final commit.
    CachedValue->ParameterValue = 0.64f;
    FProperty* ValueProperty = FindFProperty<FProperty>(
        UDEditorScalarParameterValue::StaticClass(), TEXT("ParameterValue"));
    FPropertyChangedEvent InteractiveChange(ValueProperty, EPropertyChangeType::Interactive);
    Proxy->PostEditChangeProperty(InteractiveChange);
    float PreviewValue = 0;
    Proxy->PreviewInstance->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Value")), PreviewValue);
    TestEqual(TEXT("Interactive slider change reaches the displayed preview instance"), PreviewValue, 0.64f);
    Editor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted);
    Material->ClearFlags(RF_Standalone); Instance->ClearFlags(RF_Standalone);
    Package->SetDirtyFlag(false);
    return true;
}
#endif
