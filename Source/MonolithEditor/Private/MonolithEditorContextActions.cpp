#include "MonolithEditorActions.h"
#include "MonolithEditorNavigationActions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "LevelEditorViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"

namespace
{
	FString TabRoleToString(const ETabRole Role)
	{
		switch (Role)
		{
		case ETabRole::MajorTab: return TEXT("MajorTab");
		case ETabRole::PanelTab: return TEXT("PanelTab");
		case ETabRole::NomadTab: return TEXT("NomadTab");
		case ETabRole::DocumentTab: return TEXT("DocumentTab");
		default: return TEXT("Unknown");
		}
	}

	TSharedPtr<FJsonObject> DescribeEditedAsset(
		UObject* Asset,
		IAssetEditorInstance* Editor,
		const bool bTabManagerMatches)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_name"), Asset ? Asset->GetName() : FString());
		Result->SetStringField(TEXT("asset_path"), Asset ? Asset->GetPathName() : FString());
		Result->SetStringField(TEXT("asset_class"), Asset ? Asset->GetClass()->GetPathName() : FString());
		Result->SetStringField(TEXT("editor_name"), Editor ? Editor->GetEditorName().ToString() : FString());
		Result->SetNumberField(TEXT("last_activation_time"), Editor ? Editor->GetLastActivationTime() : 0.0);
		Result->SetBoolField(TEXT("tab_manager_matches"), bTabManagerMatches);
		Result->SetBoolField(TEXT("is_transient"), Asset && (Asset->HasAnyFlags(RF_Transient)
			|| Asset->GetPathName().StartsWith(TEXT("/Engine/Transient."))));
		if (Asset)
		{
			auto NavigationParams = MakeShared<FJsonObject>();
			NavigationParams->SetStringField(TEXT("asset_path"), Asset->GetPathName());
			const auto Navigation = FMonolithEditorNavigationActions::HandleGetNavigationState(NavigationParams);
			if (Navigation.bSuccess)
				Result->SetObjectField(TEXT("navigation"), Navigation.Result);
		}
		return Result;
	}
}

FMonolithActionResult FMonolithEditorActions::HandleGetCurrentEditorContext(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TSharedPtr<SDockTab> ActiveTab;
	TSharedPtr<FTabManager> ActiveTabManager;
	if (FSlateApplication::IsInitialized())
	{
		const TSharedPtr<SWindow> ActiveWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
		TSharedPtr<FJsonObject> WindowObject = MakeShared<FJsonObject>();
		WindowObject->SetBoolField(TEXT("is_valid"), ActiveWindow.IsValid());
		WindowObject->SetStringField(TEXT("title"), ActiveWindow.IsValid() ? ActiveWindow->GetTitle().ToString() : FString());
		Result->SetObjectField(TEXT("active_window"), WindowObject);

		ActiveTab = FGlobalTabmanager::Get()->GetActiveTab();
		TSharedPtr<FJsonObject> TabObject = MakeShared<FJsonObject>();
		TabObject->SetBoolField(TEXT("is_valid"), ActiveTab.IsValid());
		if (ActiveTab.IsValid())
		{
			ActiveTabManager = ActiveTab->GetTabManagerPtr();
			TabObject->SetStringField(TEXT("label"), ActiveTab->GetTabLabel().ToString());
			TabObject->SetStringField(TEXT("layout_id"), ActiveTab->GetLayoutIdentifier().ToString());
			TabObject->SetStringField(TEXT("role"), TabRoleToString(ActiveTab->GetVisualTabRole()));
		}
		Result->SetObjectField(TEXT("active_tab"), TabObject);

		const TSharedPtr<SWidget> FocusedWidget = FSlateApplication::Get().GetKeyboardFocusedWidget();
		Result->SetStringField(TEXT("focused_widget_type"), FocusedWidget.IsValid() ? FocusedWidget->GetTypeAsString() : FString());
	}

	TArray<TSharedPtr<FJsonValue>> EditedAssetValues;
	TSharedPtr<FJsonObject> BestAsset;
	double BestActivationTime = -1.0;
	bool bBestIsExactTabManagerMatch = false;
	int32 ExactTabManagerMatchCount = 0;

	if (GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			for (UObject* Asset : AssetEditorSubsystem->GetAllEditedAssets())
			{
				if (!IsValid(Asset))
				{
					continue;
				}

				IAssetEditorInstance* Editor = AssetEditorSubsystem->FindEditorForAsset(Asset, false);
				const bool bTabManagerMatches = Editor && ActiveTabManager.IsValid()
					&& Editor->GetAssociatedTabManager() == ActiveTabManager;
				const bool bIsTransientEditorHelper = Asset->HasAnyFlags(RF_Transient)
					|| Asset->GetPathName().StartsWith(TEXT("/Engine/Transient."));
				const double ActivationTime = Editor ? Editor->GetLastActivationTime() : 0.0;
				if (bTabManagerMatches && !bIsTransientEditorHelper)
				{
					++ExactTabManagerMatchCount;
				}

				TSharedPtr<FJsonObject> AssetDescription = DescribeEditedAsset(Asset, Editor, bTabManagerMatches);
				EditedAssetValues.Add(MakeShared<FJsonValueObject>(AssetDescription));

				if (!bIsTransientEditorHelper
					&& ((!bBestIsExactTabManagerMatch && bTabManagerMatches)
						|| (bBestIsExactTabManagerMatch == bTabManagerMatches && ActivationTime > BestActivationTime)))
				{
					BestAsset = AssetDescription;
					BestActivationTime = ActivationTime;
					bBestIsExactTabManagerMatch = bTabManagerMatches;
				}
			}
		}
	}

	Result->SetArrayField(TEXT("edited_assets"), EditedAssetValues);
	TSharedPtr<FJsonObject> ActiveAssetObject = MakeShared<FJsonObject>();
	if (BestAsset.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : BestAsset->Values)
		{
			ActiveAssetObject->SetField(Field.Key, Field.Value);
		}
	}
	const bool bExactAssetMatch = bBestIsExactTabManagerMatch && ExactTabManagerMatchCount == 1;
	ActiveAssetObject->SetBoolField(TEXT("is_confident"), bExactAssetMatch);
	ActiveAssetObject->SetStringField(
		TEXT("selection_reason"),
		bExactAssetMatch ? TEXT("unique_active_tab_manager_match")
			: BestAsset.IsValid() ? TEXT("most_recent_editor_fallback") : TEXT("no_edited_asset"));
	Result->SetObjectField(TEXT("active_asset"), ActiveAssetObject);

	TSharedPtr<FJsonObject> LevelObject = MakeShared<FJsonObject>();
	if (GEditor)
	{
		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			LevelObject->SetStringField(TEXT("world_name"), EditorWorld->GetName());
			LevelObject->SetStringField(TEXT("package_path"), EditorWorld->GetPackage()->GetName());
		}
	}
	Result->SetObjectField(TEXT("active_level"), LevelObject);

	TSharedPtr<FJsonObject> ViewportObject = MakeShared<FJsonObject>();
	const int32 ViewportCount = GEditor ? GEditor->GetLevelViewportClients().Num() : 0;
	ViewportObject->SetNumberField(TEXT("level_viewport_count"), ViewportCount);
	ViewportObject->SetBoolField(TEXT("is_available"), ViewportCount > 0);
	Result->SetObjectField(TEXT("viewport"), ViewportObject);

	TArray<TSharedPtr<FJsonValue>> SelectedActorValues;
	if (GEditor && GEditor->GetSelectedActors())
	{
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (const UObject* SelectedObject = *It)
			{
				SelectedActorValues.Add(MakeShared<FJsonValueString>(SelectedObject->GetPathName()));
			}
		}
	}
	Result->SetArrayField(TEXT("selected_actors"), SelectedActorValues);

	TSharedPtr<FJsonObject> PlayObject = MakeShared<FJsonObject>();
	PlayObject->SetBoolField(TEXT("is_pie"), GEditor && GEditor->PlayWorld != nullptr);
	PlayObject->SetBoolField(TEXT("is_simulating"), GEditor && GEditor->bIsSimulatingInEditor);
	Result->SetObjectField(TEXT("play_session"), PlayObject);

	return FMonolithActionResult::Success(Result);
}
