#include "MonolithMaterialEditorSync.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Framework/Commands/UICommandList.h"
#include "IMaterialEditor.h"
#include "MaterialEditorActions.h"
#include "MaterialEditor/MaterialEditorInstanceConstant.h"
#include "MaterialShared.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace MonolithMaterialEditorSync
{
namespace
{
// An explicit list keeps read-only actions and clipboard preparation side-effect free.
bool IsWrite(const FString& Action)
{
    static const TSet<FString> Writes = {
        TEXT("disconnect_expression"), TEXT("build_material_graph"), TEXT("import_material_graph"),
        TEXT("create_custom_hlsl_node"), TEXT("set_material_property"), TEXT("delete_expression"),
        TEXT("set_instance_parameter"), TEXT("recompile_material"), TEXT("set_expression_property"),
        TEXT("connect_expressions"), TEXT("auto_layout"), TEXT("duplicate_expression"),
        TEXT("move_expression"), TEXT("set_instance_parameters"), TEXT("set_instance_parent"),
        TEXT("clear_instance_parameter"), TEXT("save_material"), TEXT("update_custom_hlsl_node"),
        TEXT("replace_expression"), TEXT("rename_expression"), TEXT("build_function_graph"),
        TEXT("set_function_metadata"), TEXT("update_material_function"), TEXT("delete_function_expression"),
        TEXT("set_function_instance_parameter"), TEXT("layout_function_expressions"),
        TEXT("rename_function_parameter_group"), TEXT("clear_graph"), TEXT("delete_expressions"),
        TEXT("batch_set_material_property"), TEXT("batch_recompile")
    };
    return Writes.Contains(Action);
}

TArray<IMaterialEditor*> OpenMaterialEditors()
{
    TArray<IMaterialEditor*> Result;
    if (!GEditor) return Result;
    UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!Subsystem) return Result;
    // Toolkits may register the preview object rather than the source asset.
    // Enumerate toolkits, then match their registered objects by full path.
    for (UObject* Object : Subsystem->GetAllEditedAssets())
    {
        for (IAssetEditorInstance* Instance : Subsystem->FindEditorsForAsset(Object))
        {
            const FName Name = Instance->GetEditorName();
            if (Name == TEXT("MaterialEditor") || Name == TEXT("MaterialInstanceEditor"))
                Result.AddUnique(static_cast<IMaterialEditor*>(Instance));
        }
    }
    return Result;
}

bool EditsAsset(IMaterialEditor* Editor, const FString& Path)
{
    const TArray<UObject*>* Objects = Editor->GetObjectsCurrentlyBeingEdited();
    if (!Objects) return false;
    for (UObject* Object : *Objects)
    {
        if (Object && Object->GetPathName() == Path) return true;
        if (const auto* Instance = Cast<UMaterialEditorInstanceConstant>(Object))
        {
            if (Instance->SourceInstance && Instance->SourceInstance->GetPathName() == Path) return true;
            if (Instance->SourceFunction && Instance->SourceFunction->GetPathName() == Path) return true;
        }
    }
    return false;
}

bool PreservePendingChanges(IMaterialEditor* Editor, FString& Error)
{
    const auto Apply = FMaterialEditorCommands::Get().Apply;
    const auto Commands = Editor->GetToolkitCommands();
    if (!Apply.IsValid() || !Commands->CanExecuteAction(Apply.ToSharedRef())) return true;

    // Native Apply can display a modal compile-error prompt. Leave the editor
    // untouched instead of risking a discarded edit or a blocked tool request.
    if (UMaterial* Preview = Cast<UMaterial>(Editor->GetMaterialInterface()))
    {
        for (EShaderPlatform Platform : {GMaxRHIShaderPlatform, GEditor->PreviewPlatform.GetShaderPlatform()})
        {
            const FMaterialResource* Resource = Preview->GetMaterialResource(Platform);
            if (Resource && Resource->GetCompileErrors().Num() > 0)
            {
                Error = TEXT("The open material editor has unapplied shader errors. Its edits were preserved; fix the errors before an external write.");
                return false;
            }
        }
    }
    Commands->ExecuteAction(Apply.ToSharedRef());
    if (Commands->CanExecuteAction(Apply.ToSharedRef()))
    {
        Error = TEXT("The open material editor could not apply its pending changes. No external write was performed.");
        return false;
    }
    return true;
}

int32 RefreshInstances(const TArray<IMaterialEditor*>& Editors, const TSet<UObject*>& ChangedAssets)
{
    int32 Refreshed = 0;
    if (ChangedAssets.IsEmpty()) return Refreshed;
    bool bHasChangedFunction = false;
    for (UObject* Asset : ChangedAssets)
        if (Cast<UMaterialFunctionInterface>(Asset)) { bHasChangedFunction = true; break; }
    // Refresh only instance toolkits, never discard dirty dependent material graphs.
    for (IMaterialEditor* Editor : Editors)
    {
        if (Editor->GetEditorName() != TEXT("MaterialInstanceEditor")) continue;
        const TArray<UObject*>* Objects = Editor->GetObjectsCurrentlyBeingEdited();
        if (!Objects) continue;
        for (UObject* Object : *Objects)
        {
            auto* Proxy = Cast<UMaterialEditorInstanceConstant>(Object);
            if (!Proxy || !Proxy->SourceInstance) continue;
            bool bAffected = ChangedAssets.Contains(Proxy->SourceInstance) || ChangedAssets.Contains(Proxy->SourceFunction);
            if (!bAffected && bHasChangedFunction)
            {
                TArray<UMaterialFunctionInterface*> Dependencies;
                Proxy->SourceInstance->GetDependentFunctions(Dependencies);
                for (UMaterialFunctionInterface* Dependency : Dependencies)
                    bAffected |= ChangedAssets.Contains(Dependency);
                if (!bAffected && Proxy->SourceFunction)
                {
                    Dependencies.Reset();
                    Proxy->SourceFunction->GetDependentFunctions(Dependencies);
                    for (UMaterialFunctionInterface* Dependency : Dependencies)
                        bAffected |= ChangedAssets.Contains(Dependency);
                }
            }
            TSet<UMaterialInterface*> VisitedParents;
            for (UMaterialInterface* Parent = Proxy->SourceInstance->Parent; Parent && !bAffected; )
            {
                // Invalid parent cycles must not hang an MCP request.
                if (VisitedParents.Contains(Parent)) break;
                VisitedParents.Add(Parent);
                bAffected |= ChangedAssets.Contains(Parent);
                UMaterialInstance* ParentInstance = Cast<UMaterialInstance>(Parent);
                Parent = ParentInstance ? ParentInstance->Parent.Get() : nullptr;
            }
            if (bAffected)
            {
                if (Proxy->SourceFunction)
                {
                    // A function-instance editor may itself have unapplied edits.
                    // Do not replace that cache when refreshing a dependency.
                    if (Proxy->bIsFunctionInstanceDirty) continue;
                    Proxy->SetSourceFunction(Proxy->SourceFunction);
                }
                else
                {
                    // SetSourceInstance creates a new PreviewInstance. The instance
                    // editor viewport keeps rendering the old object, so interactive
                    // slider changes then appear only after the final value reaches
                    // the source instance. Refresh the proxy data in place instead;
                    // its existing PreviewInstance is already parented to SourceInstance.
                    Proxy->Parent = Proxy->SourceInstance->Parent;
                    Proxy->bNaniteOverride = Proxy->SourceInstance->NaniteOverrideMaterial.bEnableOverride;
                    Proxy->NaniteOverrideMaterial = Proxy->SourceInstance->NaniteOverrideMaterial.OverrideMaterialEditor;
                    Proxy->EnumerationObjects = Proxy->SourceInstance->EnumerationObjects;
                    Proxy->CopyBasePropertiesFromParent();
                    Proxy->RegenerateArrays();
                }
                Editor->NotifyExternalMaterialChange();
                ++Refreshed;
                break;
            }
        }
    }
    return Refreshed;
}
}

void RegisterAction(FMonolithToolRegistry& Registry, const FString& Namespace,
    const FString& Action, const FString& Description,
    const FMonolithActionHandler& Handler, const TSharedPtr<FJsonObject>& Schema)
{
    if (!IsWrite(Action))
    {
        Registry.RegisterAction(Namespace, Action, Description, Handler, Schema);
        return;
    }
    Registry.RegisterAction(Namespace, Action, Description,
        FMonolithActionHandler::CreateLambda([Handler](const TSharedPtr<FJsonObject>& Params)
        {
            TArray<FString> AssetPaths;
            FString AssetPath;
            if (Params->TryGetStringField(TEXT("asset_path"), AssetPath)) AssetPaths.Add(AssetPath);
            const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
            if (Params->TryGetArrayField(TEXT("asset_paths"), Paths))
                for (const auto& Path : *Paths)
                    if (Path->TryGetString(AssetPath)) AssetPaths.AddUnique(AssetPath);
            if (AssetPaths.IsEmpty()) return Handler.Execute(Params);
            if (GIsTransacting)
                return FMonolithActionResult::Error(TEXT("Material editor synchronization cannot run during Undo/Redo. Retry after the transaction finishes."));
            TArray<IMaterialEditor*> Editors;
            const auto OpenEditors = OpenMaterialEditors();
            if (!OpenEditors.IsEmpty())
            {
                for (const FString& Path : AssetPaths)
                    if (UObject* Asset = UEditorAssetLibrary::LoadAsset(Path))
                        for (IMaterialEditor* Editor : OpenEditors)
                            if (EditsAsset(Editor, Asset->GetPathName())) Editors.AddUnique(Editor);
            }
            FString Error;
            for (IMaterialEditor* Editor : Editors)
                if (!PreservePendingChanges(Editor, Error)) return FMonolithActionResult::Error(Error);

            // Apply can replace the source UObject. The handler must load it anew.
            FMonolithActionResult Result = Handler.Execute(Params);
            int32 RefreshedGraphs = 0;
            int32 RefreshedInstances = 0;
            // Reacquire toolkits after the handler instead of dereferencing raw
            // editor pointers captured before callbacks/compilation ran.
            const auto CurrentEditors = OpenMaterialEditors();
            TSet<UObject*> ChangedAssets;
            TSet<FString> ChangedPaths;
            if (!CurrentEditors.IsEmpty())
                for (const FString& Path : AssetPaths)
                    if (UObject* Asset = UEditorAssetLibrary::LoadAsset(Path))
                    {
                        ChangedAssets.Add(Asset);
                        ChangedPaths.Add(Asset->GetPathName());
                    }
            for (IMaterialEditor* Editor : CurrentEditors)
            {
                bool bIsTarget = false;
                for (const FString& Path : ChangedPaths)
                    if (EditsAsset(Editor, Path)) { bIsTarget = true; break; }
                if (!bIsTarget) continue;
                if (Editor->GetEditorName() == TEXT("MaterialEditor"))
                {
                    UMaterial* Before = Cast<UMaterial>(Editor->GetMaterialInterface());
                    UMaterialFunction* BeforeFunction = Before && Before->MaterialGraph ? Before->MaterialGraph->MaterialFunction.Get() : nullptr;
                    Editor->RefreshGraphFromOriginal();
                    UMaterial* After = Cast<UMaterial>(Editor->GetMaterialInterface());
                    UMaterialFunction* AfterFunction = After && After->MaterialGraph ? After->MaterialGraph->MaterialFunction.Get() : nullptr;
                    if (After != Before || AfterFunction != BeforeFunction) ++RefreshedGraphs;
                }
            }
            RefreshedInstances = RefreshInstances(CurrentEditors, ChangedAssets);
            if (Result.Result.IsValid())
            {
                Result.Result->SetNumberField(TEXT("synchronized_material_editors"), RefreshedGraphs);
                Result.Result->SetNumberField(TEXT("synchronized_instance_editors"), RefreshedInstances);
                Result.Result->SetStringField(TEXT("editor_sync"), RefreshedGraphs + RefreshedInstances > 0 ? TEXT("refreshed") :
                    (Editors.IsEmpty() ? TEXT("no_target_editor_open") : TEXT("not_refreshed")));
            }
            return Result;
        }), Schema);
}
}
