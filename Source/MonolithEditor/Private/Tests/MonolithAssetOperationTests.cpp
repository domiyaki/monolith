#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithAssetOperationValidation.h"
#include "MonolithEditorActions.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithDeleteBoundaryTest,
    "Monolith.Editor.AssetOperations.DeleteBoundaries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithDeleteBoundaryTest::RunTest(const FString&)
{
    using namespace MonolithAssetOperationValidation;
    TestTrue(TEXT("Folder child allowed"), IsWithinDeletePrefix(TEXT("/Game/Test/Asset.Asset"), TEXT("/Game/Test/")));
    TestTrue(TEXT("Exact package allowed"), IsWithinDeletePrefix(TEXT("/Game/Test.Test"), TEXT("/Game/Test")));
    TestFalse(TEXT("Sibling prefix rejected"), IsWithinDeletePrefix(TEXT("/Game/TestOther/Asset.Asset"), TEXT("/Game/Test")));
    TestFalse(TEXT("Empty prefix rejected"), IsWithinDeletePrefix(TEXT("/Game/Test/Asset"), TEXT("/")));
    auto Params = MakeShared<FJsonObject>();
    Params->SetArrayField(TEXT("asset_paths"), {MakeShared<FJsonValueNull>()});
    TestFalse(TEXT("Null entry rejected before asset loading"), FMonolithEditorActions::HandleDeleteAssets(Params).bSuccess);
    Params->SetArrayField(TEXT("asset_paths"), {MakeShared<FJsonValueString>(TEXT("/Game/TestOther/Asset"))});
    Params->SetArrayField(TEXT("allowed_prefixes"), {MakeShared<FJsonValueString>(TEXT("/Game/Test"))});
    TestFalse(TEXT("Handler enforces boundary before loading"), FMonolithEditorActions::HandleDeleteAssets(Params).bSuccess);
    Params->SetArrayField(TEXT("allowed_prefixes"), {});
    TestFalse(TEXT("Explicit empty restriction cannot disable protection"), FMonolithEditorActions::HandleDeleteAssets(Params).bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithDeletePreflightTest,
    "Monolith.Editor.AssetOperations.DeletePreflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithDeletePreflightTest::RunTest(const FString&)
{
    UPackage* Package = CreatePackage(*(TEXT("/Temp/MonolithDeletePreflight_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    auto* Material = NewObject<UMaterial>(Package, TEXT("Material"));
    auto* Blueprint = NewObject<UBlueprint>(Package, TEXT("Blueprint"));
    FString Error;
    Package->SetDirtyFlag(true);
    TestFalse(TEXT("Dirty material rejected"), MonolithAssetOperationValidation::ValidateDeleteTarget(Material, false, Error));
    TestTrue(TEXT("Preflight preserves dirty state"), Package->IsDirty());
    Package->SetDirtyFlag(false);
    TestFalse(TEXT("Blueprint forced deletion rejected"), MonolithAssetOperationValidation::ValidateDeleteTarget(Blueprint, true, Error));
    TestTrue(TEXT("Normal Blueprint deletion remains eligible"), MonolithAssetOperationValidation::ValidateDeleteTarget(Blueprint, false, Error));
    TestTrue(TEXT("Clean non-Blueprint remains eligible"), MonolithAssetOperationValidation::ValidateDeleteTarget(Material, true, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSavePlanTest,
    "Monolith.Editor.AssetOperations.SavePlan", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithSavePlanTest::RunTest(const FString&)
{
    const FString Name = TEXT("/Game/MonolithSavePlan_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UPackage* Package = CreatePackage(*Name);
    NewObject<UMaterial>(Package, TEXT("Material"), RF_Public);
    Package->SetDirtyFlag(false);
    auto Params = MakeShared<FJsonObject>();
    Params->SetArrayField(TEXT("packages"), {MakeShared<FJsonValueString>(Name)});
    Params->SetBoolField(TEXT("dry_run"), true);
    auto Explicit = FMonolithEditorActions::HandleSavePackages(Params);
    if (!TestTrue(TEXT("Explicit save plan returned"), Explicit.bSuccess && Explicit.Result.IsValid())) return false;
    TestTrue(TEXT("Clean explicit plan succeeds"), Explicit.Result->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Default preserves explicit clean save"), Explicit.Result->GetNumberField(TEXT("would_save")), 1.0);
    Params->SetBoolField(TEXT("only_dirty"), true);
    auto Clean = FMonolithEditorActions::HandleSavePackages(Params);
    TestTrue(TEXT("Skipped clean package is not failure"), Clean.Result->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("No redundant save planned"), Clean.Result->GetNumberField(TEXT("would_save")), 0.0);
    Package->SetDirtyFlag(true);
    auto Dirty = FMonolithEditorActions::HandleSavePackages(Params);
    TestEqual(TEXT("Dirty package planned"), Dirty.Result->GetNumberField(TEXT("would_save")), 1.0);
    TestTrue(TEXT("Dry-run preserves dirty state"), Package->IsDirty());
    Package->SetDirtyFlag(false);
    Params->SetBoolField(TEXT("dry_run"), false);
    auto Skipped = FMonolithEditorActions::HandleSavePackages(Params);
    TestTrue(TEXT("Actual only-dirty skip succeeds"), Skipped.Result->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("No disk save performed"), Skipped.Result->GetNumberField(TEXT("saved")), 0.0);
    FString Filename;
    TestTrue(TEXT("Fixture resolves"), MonolithAssetOperationValidation::TrySaveFilename(Name, TEXT(".uasset"), Filename));
    TestFalse(TEXT("No fixture file created"), FPaths::FileExists(Filename));
    const FString Unloaded = Name + TEXT("_Unloaded");
    Params->SetBoolField(TEXT("dry_run"), true);
    Params->SetArrayField(TEXT("packages"), {MakeShared<FJsonValueString>(Unloaded)});
    auto Unknown = FMonolithEditorActions::HandleSavePackages(Params);
    TestFalse(TEXT("Unloaded state reported as unknown/error"), Unknown.Result->GetBoolField(TEXT("ok")));
    TestNull(TEXT("Dry-run did not create/load package"), FindPackage(nullptr, *Unloaded));
    TestFalse(TEXT("Filesystem path rejected"), MonolithAssetOperationValidation::TrySaveFilename(TEXT("C:/Unsafe"), TEXT(".uasset"), Filename));
    TestFalse(TEXT("Unregistered mount rejected"), MonolithAssetOperationValidation::TrySaveFilename(TEXT("/MonolithUnknownMount/Asset"), TEXT(".uasset"), Filename));
    return true;
}
#endif
