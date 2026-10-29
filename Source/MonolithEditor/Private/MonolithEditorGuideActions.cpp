#include "MonolithEditorGuideActions.h"
#include "MonolithEditorNavigationActions.h"
#include "MonolithParamSchema.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SWindow.h"

namespace MonolithEditorGuide
{
	const FName TabId(TEXT("MonolithGuidedWorkflow"));
	struct FStep
	{
		FString Title, Instruction, Completion, Expected;
		TSharedPtr<FJsonObject> Target;
	};
	TArray<FStep> Steps;
	FString WorkflowId, Title, Status = TEXT("idle"), Message, ActualValue;
	FString AssetPath, GraphPath, NodeId, PropertyPath, PropertyObjectPath;
	int32 Current = INDEX_NONE;
	bool bTargetMatches = false, bLastTargetEditorActive = false;
	bool bConfirmationRequested = false;
	FTSTicker::FDelegateHandle TickHandle;
	TWeakPtr<SDockTab> GuideTab;
	bool bInitialFloatingLayoutHandled = false;

	bool IsActive() { return Status == TEXT("running") || Status == TEXT("blocked"); }
	FText Text(const FString& Value) { return FText::FromString(Value); }
	FString Field(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		FString Value;
		if (Object) Object->TryGetStringField(Name, Value);
		return Value;
	}
	bool Bool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		bool Value = false;
		if (Object) Object->TryGetBoolField(Name, Value);
		return Value;
	}
	TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		return Object && Object->TryGetObjectField(Name, Value) ? *Value : nullptr;
	}

	void Stop(const FString& NewStatus, const FString& Reason)
	{
		Status = NewStatus;
		Message = Reason;
		if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
		FMonolithEditorNavigationActions::StopHighlight();
		FMonolithEditorNavigationActions::SetGuideActive(false);
	}

	TSharedPtr<FJsonObject> State()
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("workflow_id"), WorkflowId);
		Result->SetStringField(TEXT("title"), Title);
		Result->SetStringField(TEXT("status"), Status);
		Result->SetNumberField(TEXT("current_step_index"), Current);
		Result->SetNumberField(TEXT("total_steps"), Steps.Num());
		Result->SetStringField(TEXT("message"), Message);
		Result->SetBoolField(TEXT("target_matches"), bTargetMatches);
		Result->SetStringField(TEXT("actual_value_exported"), ActualValue);
		Result->SetStringField(TEXT("resolved_asset_path"), AssetPath);
		Result->SetStringField(TEXT("resolved_graph_path"), GraphPath);
		TArray<TSharedPtr<FJsonValue>> Descriptions;
		for (int32 I = 0; I < Steps.Num(); ++I)
		{
			auto Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("title"), Steps[I].Title);
			Step->SetStringField(TEXT("instruction"), Steps[I].Instruction);
			Step->SetStringField(TEXT("completion_type"), Steps[I].Completion);
			Step->SetStringField(TEXT("expected_value"), Steps[I].Expected);
			Step->SetStringField(TEXT("state"), Status == TEXT("completed") || I < Current ? TEXT("completed") : I == Current ? TEXT("current") : TEXT("pending"));
			Step->SetObjectField(TEXT("target"), Steps[I].Target);
			Descriptions.Add(MakeShared<FJsonValueObject>(Step));
		}
		Result->SetArrayField(TEXT("steps"), Descriptions);
		return Result;
	}

	bool Reveal()
	{
		if (!IsActive() || !Steps.IsValidIndex(Current)) return false;
		bTargetMatches = false;
		bConfirmationRequested = false;
		bLastTargetEditorActive = false;
		ActualValue.Empty();
		AssetPath.Empty();
		GraphPath.Empty();
		PropertyObjectPath.Empty();
		const FStep& Step = Steps[Current];
		NodeId = Field(Step.Target, TEXT("node_id"));
		PropertyPath = Field(Step.Target, TEXT("property_path"));
		FMonolithEditorNavigationActions::SetGuideActive(true);
		const auto Navigation = FMonolithEditorNavigationActions::HandleNavigateTo(Step.Target);
		if (!Navigation.bSuccess)
		{
			Status = TEXT("blocked");
			Message = TEXT("无法定位目标：") + Navigation.ErrorMessage + TEXT("。修正目标后重新提交引导，或结束引导。");
			return false;
		}
		AssetPath = Field(Navigation.Result, TEXT("asset_path"));
		GraphPath = Field(Step.Target, TEXT("graph_name")).IsEmpty() && NodeId.IsEmpty()
			? FString() : Field(Navigation.Result, TEXT("focused_graph_path"));
		PropertyObjectPath = Field(Navigation.Result, TEXT("property_object_path"));
		Status = TEXT("running");
		Message = TEXT("正在等待目标界面就绪。请按本步说明操作。");
		return true;
	}

	bool NodeMatches(const TSharedPtr<FJsonObject>& StateObject)
	{
		if (NodeId.IsEmpty()) return true;
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!StateObject->TryGetArrayField(TEXT("selected_nodes"), Nodes) || Nodes->Num() != 1) return false;
		const auto Node = (*Nodes)[0]->AsObject();
		if (Field(Node, TEXT("node_name")) == NodeId) return true;
		FGuid Wanted, Selected;
		return FGuid::Parse(NodeId, Wanted) && FGuid::Parse(Field(Node, TEXT("node_id")), Selected) && Wanted == Selected;
	}

	bool EditorIsCurrent()
	{
		if (!GEditor) return false;
		const auto ActiveTab = FGlobalTabmanager::Get()->GetActiveTab();
		if (!ActiveTab) return false;
		if (ActiveTab->GetLayoutIdentifier().TabType == TabId) return bLastTargetEditorActive;
		UObject* Asset = FindObject<UObject>(nullptr, *AssetPath);
		auto* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		IAssetEditorInstance* Editor = Asset && Editors ? Editors->FindEditorForAsset(Asset, false) : nullptr;
		bLastTargetEditorActive = Editor && Editor->GetAssociatedTabManager() == ActiveTab->GetTabManagerPtr();
		return bLastTargetEditorActive;
	}

	void Check(bool bExplicitConfirmation)
	{
		if (Status != TEXT("running") || !Steps.IsValidIndex(Current) || AssetPath.IsEmpty()) return;
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		const auto Readback = FMonolithEditorNavigationActions::HandleGetNavigationState(Params);
		if (!Readback.bSuccess)
		{
			bTargetMatches = false;
			Message = TEXT("目标已不可用，请点击“重新定位”。") + Readback.ErrorMessage;
			return;
		}
		const FStep& Step = Steps[Current];
		const auto Navigation = Readback.Result;
		bTargetMatches = Bool(Navigation, TEXT("editor_open")) && EditorIsCurrent()
			&& (GraphPath.IsEmpty() || Field(Navigation, TEXT("focused_graph_path")) == GraphPath)
			&& NodeMatches(Navigation);
		const auto Property = ObjectField(Navigation, TEXT("last_property_target"));
		if (!PropertyPath.IsEmpty())
		{
			bTargetMatches &= Property && Field(Property, TEXT("property_path")) == PropertyPath
				&& Field(Property, TEXT("object_path")) == PropertyObjectPath
				&& Bool(Property, TEXT("target_available")) && Bool(Property, TEXT("details_target_matches"));
			ActualValue = Field(Property, TEXT("value_exported"));
		}
		if (!bTargetMatches)
		{
			bConfirmationRequested = false;
			Message = TEXT("当前界面与本步目标不一致。请回到目标，或点击“重新定位”；本步不会跳过。");
			return;
		}
		bool bComplete = false;
		if (Step.Completion == TEXT("confirm"))
		{
			bConfirmationRequested |= bExplicitConfirmation;
			bComplete = bConfirmationRequested;
			Message = TEXT("目标已核对。阅读本步说明后，点击“检查并继续”确认。");
		}
		else if (Step.Completion == TEXT("property_clicked"))
		{
			bComplete = Field(Property, TEXT("reveal_status")) == TEXT("acknowledged_by_property_click");
			Message = TEXT("等待你点击被提示的属性行。点击其他位置不会完成本步。");
		}
		else if (Step.Completion == TEXT("property_equals"))
		{
			bComplete = ActualValue == Step.Expected;
			Message = TEXT("检查属性实际值。当前：") + ActualValue + TEXT("；目标：") + Step.Expected;
		}
		if (!bComplete) return;
		// Property acknowledgement originates on mouse-down. Finish the gesture
		// before navigating so a held drag/release cannot land on the next target.
		if (FSlateApplication::Get().GetPressedMouseButtons().Num() != 0)
		{
			Message = TEXT("已满足本步条件，等待鼠标操作结束后进入下一步。");
			return;
		}
		++Current;
		if (Current >= Steps.Num())
		{
			Stop(TEXT("completed"), TEXT("全部步骤已完成。已停止高亮与自动检查。"));
			return;
		}
		Reveal();
	}

	bool Tick(float)
	{
		Check(false);
		return IsActive();
	}

	FText PlanText()
	{
		FString Plan;
		for (int32 I = 0; I < Steps.Num(); ++I)
		{
			const TCHAR* Marker = Status == TEXT("completed") || I < Current ? TEXT("✓") : I == Current ? TEXT("▶") : TEXT("○");
			Plan += FString::Printf(TEXT("%s %d. %s\n"), Marker, I + 1, *Steps[I].Title);
		}
		return Text(Plan);
	}

	TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs&)
	{
		const auto Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab)
		[
			SNew(SBox).MinDesiredWidth(390).MinDesiredHeight(420)
			[
				SNew(SBorder).Padding(16)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
					[ SNew(STextBlock).Text_Lambda([] { return Text(Title.IsEmpty() ? TEXT("操作引导") : Title); }).AutoWrapText(true) ]
					+ SVerticalBox::Slot().FillHeight(0.4f)
					[ SNew(SScrollBox) + SScrollBox::Slot()[ SNew(STextBlock).Text_Static(&PlanText).AutoWrapText(true) ] ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 12)
					[ SNew(STextBlock).Text_Lambda([] { return Text(Steps.IsValidIndex(Current) ? FString::Printf(TEXT("当前 %d / %d：%s"), Current + 1, Steps.Num(), *Steps[Current].Title) : TEXT("引导已结束")); }).AutoWrapText(true) ]
					+ SVerticalBox::Slot().FillHeight(0.6f)
					[ SNew(SScrollBox) + SScrollBox::Slot()[ SNew(STextBlock).Text_Lambda([] { return Text(Steps.IsValidIndex(Current) ? Steps[Current].Instruction : Message); }).AutoWrapText(true) ] ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 12)
					[ SNew(STextBlock).Text_Lambda([] { return Text(Message); }).AutoWrapText(true) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[ SNew(SButton).Text(Text(TEXT("重新定位"))).IsEnabled_Lambda([] { return IsActive(); }).OnClicked_Lambda([] { Reveal(); return FReply::Handled(); }) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[ SNew(SButton).Text(Text(TEXT("检查并继续"))).IsEnabled_Lambda([] { return IsActive(); }).OnClicked_Lambda([] { Check(true); return FReply::Handled(); }) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[ SNew(SButton).Text(Text(TEXT("结束引导"))).IsEnabled_Lambda([] { return IsActive(); }).OnClicked_Lambda([] { Stop(TEXT("stopped"), TEXT("你已结束引导；已完成的操作不会撤销。")); return FReply::Handled(); }) ]
					]
				]
			]
		];
		Tab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateLambda([](TSharedRef<SDockTab>)
		{
			if (IsActive()) Stop(TEXT("stopped"), TEXT("引导面板已关闭，已停止检查与高亮。"));
			GuideTab.Reset();
		}));
		GuideTab = Tab;
		return Tab;
	}

	void SizeInitialFloatingGuide()
	{
		if (bInitialFloatingLayoutHandled) return;
		bInitialFloatingLayoutHandled = true;
		const auto Tab = GuideTab.Pin();
		if (!Tab) return;
		auto& Slate = FSlateApplication::Get();
		const auto Window = Slate.FindWidgetWindow(Tab.ToSharedRef());
		const auto RootWindow = FGlobalTabmanager::Get()->GetRootWindow();
		// A docked tab belongs to a level/asset editor window. Only the newly
		// spawned, guide-only floating window may receive our initial placement.
		if (!Window || Window == RootWindow || Window->GetTitle().ToString() != TEXT("操作引导")) return;
		const FVector2D Anchor = RootWindow ? FVector2D(RootWindow->GetPositionInScreen()) : FVector2D(Window->GetPositionInScreen());
		const FSlateRect WorkArea = Slate.GetWorkArea(FSlateRect(Anchor.X, Anchor.Y, Anchor.X + 1.0, Anchor.Y + 1.0));
		Window->Resize(FVector2D(440.0, 500.0));
		const FVector2D Size = Window->GetSizeInScreen();
		const FVector2D Position(
			FMath::Clamp(Anchor.X + 20.0, static_cast<double>(WorkArea.Left), FMath::Max(static_cast<double>(WorkArea.Left), WorkArea.Right - Size.X)),
			FMath::Clamp(Anchor.Y + 100.0, static_cast<double>(WorkArea.Top), FMath::Max(static_cast<double>(WorkArea.Top), WorkArea.Bottom - Size.Y)));
		Window->MoveWindowTo(Position);
	}
}

void FMonolithEditorGuideActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	using namespace MonolithEditorGuide;
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId, FOnSpawnTab::CreateStatic(&SpawnTab))
		.SetDisplayName(Text(TEXT("操作引导"))).SetMenuType(ETabSpawnerMenuType::Hidden);
	Registry.RegisterAction(TEXT("editor"), TEXT("start_guided_workflow"),
		TEXT("Submit a complete interactive guide once. Opens a dockable guide panel and navigates one step at a time. steps is 1..32 objects {title,instruction,target:{asset_path,graph_name?,node_id?,property_path?},completion:{type:confirm|property_clicked|property_equals,expected_value?:string}}. property conditions require property_path. property_equals compares exact ExportText strings (e.g. 0.250000). confirm waits for the person's Check and Continue button. Automatic checks never edit values. All step structure is validated before replacing an active workflow."),
		FMonolithActionHandler::CreateStatic(&HandleStart), FParamSchemaBuilder()
		.Required(TEXT("title"), TEXT("string"), TEXT("Guide title shown to the person."))
		.Required(TEXT("steps"), TEXT("array"), TEXT("Ordered step objects with title, instruction, target, and completion."))
		.Build());
	Registry.RegisterAction(TEXT("editor"), TEXT("get_guided_workflow_state"), TEXT("Read guide progress, current check result, exact current value, and all step statuses."), FMonolithActionHandler::CreateStatic(&HandleGetState), MakeShared<FJsonObject>());
	Registry.RegisterAction(TEXT("editor"), TEXT("stop_guided_workflow"), TEXT("Stop guide polling and its highlight; preserves all user edits."), FMonolithActionHandler::CreateStatic(&HandleStop), MakeShared<FJsonObject>());
	Registry.RegisterAction(TEXT("editor"), TEXT("reveal_guided_workflow_step"), TEXT("Reopen the current guide target without advancing or modifying its value."), FMonolithActionHandler::CreateStatic(&HandleReveal), MakeShared<FJsonObject>());
}

void FMonolithEditorGuideActions::Shutdown()
{
	using namespace MonolithEditorGuide;
	Stop(TEXT("stopped"), TEXT("Editor module shutdown"));
	if (auto Tab = GuideTab.Pin()) Tab->RequestCloseTab();
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
}

FMonolithActionResult FMonolithEditorGuideActions::HandleStart(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorGuide;
	FString NewTitle;
	const TArray<TSharedPtr<FJsonValue>>* Input = nullptr;
	if (!Params || !Params->TryGetStringField(TEXT("title"), NewTitle) || NewTitle.IsEmpty())
		return FMonolithActionResult::Error(TEXT("title must be a nonempty string"), -32602);
	if (!Params->TryGetArrayField(TEXT("steps"), Input) || Input->Num() < 1 || Input->Num() > 32)
		return FMonolithActionResult::Error(TEXT("steps must contain 1..32 step objects"), -32602);
	TArray<FStep> Parsed;
	for (int32 I = 0; I < Input->Num(); ++I)
	{
		const TSharedPtr<FJsonObject>* Item = nullptr;
		if (!(*Input)[I]->TryGetObject(Item)) return FMonolithActionResult::Error(TEXT("steps entries must be objects"), -32602);
		FStep Step;
		Step.Title = Field(*Item, TEXT("title"));
		Step.Instruction = Field(*Item, TEXT("instruction"));
		Step.Target = ObjectField(*Item, TEXT("target"));
		const auto Completion = ObjectField(*Item, TEXT("completion"));
		Step.Completion = Field(Completion, TEXT("type"));
		const FString Prefix = FString::Printf(TEXT("steps[%d]: "), I);
		if (Step.Title.IsEmpty() || Step.Instruction.IsEmpty()) return FMonolithActionResult::Error(Prefix + TEXT("title and instruction must be nonempty strings"), -32602);
		if (!Step.Target || Field(Step.Target, TEXT("asset_path")).IsEmpty()) return FMonolithActionResult::Error(Prefix + TEXT("target.asset_path is required"), -32602);
		for (const TCHAR* Key : { TEXT("graph_name"), TEXT("node_id"), TEXT("property_path") })
		{
			FString Value;
			if (Step.Target->HasField(Key) && !Step.Target->TryGetStringField(Key, Value)) return FMonolithActionResult::Error(Prefix + TEXT("target selector must be a string: ") + Key, -32602);
		}
		if (Step.Completion != TEXT("confirm") && Step.Completion != TEXT("property_clicked") && Step.Completion != TEXT("property_equals"))
			return FMonolithActionResult::Error(Prefix + TEXT("completion.type must be confirm, property_clicked, or property_equals"), -32602);
		if (Step.Completion != TEXT("confirm") && Field(Step.Target, TEXT("property_path")).IsEmpty())
			return FMonolithActionResult::Error(Prefix + TEXT("target.property_path is required for property completion"), -32602);
		if (Step.Completion == TEXT("property_equals") && (!Completion || !Completion->TryGetStringField(TEXT("expected_value"), Step.Expected)))
			return FMonolithActionResult::Error(Prefix + TEXT("completion.expected_value must be an exact exported-value string"), -32602);
		Parsed.Add(MoveTemp(Step));
	}
	if (!GEditor || !FSlateApplication::IsInitialized()) return FMonolithActionResult::Error(TEXT("An interactive editor is required"));
	Stop(TEXT("stopped"), TEXT("Replaced by a new guide"));
	Steps = MoveTemp(Parsed);
	Title = NewTitle;
	WorkflowId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	Current = 0;
	Status = TEXT("running");
	FGlobalTabmanager::Get()->TryInvokeTab(TabId);
	SizeInitialFloatingGuide();
	Reveal();
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.25f);
	return FMonolithActionResult::Success(State());
}

FMonolithActionResult FMonolithEditorGuideActions::HandleGetState(const TSharedPtr<FJsonObject>&)
{
	return FMonolithActionResult::Success(MonolithEditorGuide::State());
}

FMonolithActionResult FMonolithEditorGuideActions::HandleStop(const TSharedPtr<FJsonObject>&)
{
	MonolithEditorGuide::Stop(TEXT("stopped"), TEXT("引导已结束；已完成的操作不会撤销。"));
	return FMonolithActionResult::Success(MonolithEditorGuide::State());
}

FMonolithActionResult FMonolithEditorGuideActions::HandleReveal(const TSharedPtr<FJsonObject>&)
{
	if (!MonolithEditorGuide::IsActive()) return FMonolithActionResult::Error(TEXT("No active guided workflow"), -32602);
	MonolithEditorGuide::Reveal();
	return FMonolithActionResult::Success(MonolithEditorGuide::State());
}
