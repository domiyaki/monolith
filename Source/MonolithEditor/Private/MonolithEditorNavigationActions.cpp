#include "MonolithEditorNavigationActions.h"
#include "MonolithParamSchema.h"
#include "BlueprintEditor.h"
#include "BlueprintEditorTabs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/IInputProcessor.h"
#include "Layout/WidgetPath.h"
#include "Widgets/Views/STableRow.h"
#include "HAL/PlatformTime.h"
#include "Widgets/Docking/SDockTab.h"
#include "GraphEditor.h"
#include "IDetailsView.h"
#include "PropertyPath.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "SKismetInspector.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
	// Weak references preserve the last comparison target without retaining assets.
	TWeakObjectPtr<UObject> LastPropertyAsset;
	TWeakObjectPtr<UObject> LastPropertyObject;
	FString LastPropertyPath;
	FString LastPropertyBaseline;
	FDelegateHandle RevealHandle;
	uint64 NavigationGeneration = 0;
	FString RevealStatus = TEXT("not_requested");
	bool bHighlightRequested = false;
	bool bBlinkActive = false;
	bool bHighlightVisible = false;
	bool bGuideActive = false;
	TWeakPtr<IDetailsView> HighlightDetails;

	// The public table-row base exposes the same linear index returned by
	// IDetailsView::GetPropertyRowNumbers. No private PropertyEditor headers.
	int32 PropertyRowIndex(const TSharedPtr<IDetailsView>& Details, const FString& Path)
	{
		for (const auto& Row : Details->GetPropertyRowNumbers())
			if (Row.Value.ToString() == Path) return Row.Key;
		return INDEX_NONE;
	}

	bool IsPropertyRow(const TSharedRef<SWidget>& Widget, int32 Index)
	{
		return Index != INDEX_NONE && Widget->GetType() == FName(TEXT("SDetailSingleItemRow"))
			&& StaticCastSharedRef<STableRow<TSharedPtr<FDetailTreeNode>>>(Widget)->GetIndexInList() == Index;
	}

	bool HasVisiblePropertyRow(const TSharedRef<SWidget>& Widget, int32 Index, const FSlateRect& Clip)
	{
		if (!Widget->GetVisibility().IsVisible()) return false;
		if (IsPropertyRow(Widget, Index))
		{
			const auto Bounds = Widget->GetPaintSpaceGeometry().GetLayoutBoundingRect();
			const auto& Clipping = Widget->GetCurrentClippingState();
			const FVector2D Center((Bounds.Left + Bounds.Right) * 0.5, (Bounds.Top + Bounds.Bottom) * 0.5);
			if (Clipping.IsSet() && (!Clipping->IsPointInside(Center)
				|| !Clipping->IsPointInside(FVector2D(Center.X, Bounds.Top + 1.0))
				|| !Clipping->IsPointInside(FVector2D(Center.X, Bounds.Bottom - 1.0)))) return false;
			return Bounds.Right > Clip.Left && Bounds.Left < Clip.Right
				&& Bounds.Bottom > Clip.Top && Bounds.Top < Clip.Bottom;
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 I = 0; Children && I < Children->Num(); ++I)
			if (HasVisiblePropertyRow(Children->GetChildAt(I), Index, Clip)) return true;
		return false;
	}

	class FPropertyClickAcknowledgement final : public IInputProcessor
	{
	public:
		TWeakPtr<IDetailsView> Details;
		FString Path;
		bool bAcknowledged = false;
		virtual void Tick(float, FSlateApplication&, TSharedRef<ICursor>) override {}
		virtual bool HandleMouseButtonDownEvent(FSlateApplication& App, const FPointerEvent& Event) override
		{
			auto View = Details.Pin();
			if (!View) return false;
			const int32 Index = PropertyRowIndex(View, Path);
			const FWidgetPath Hit = App.LocateWindowUnderMouse(Event.GetScreenSpacePosition(), App.GetInteractiveTopLevelWindows());
			bool bInTargetDetails = false;
			for (int32 I = 0; I < Hit.Widgets.Num(); ++I)
			{
				const FArrangedWidget& Entry = Hit.Widgets[I];
				bInTargetDetails |= &Entry.Widget.Get() == View.Get();
				if (bInTargetDetails && IsPropertyRow(Entry.Widget, Index)) bAcknowledged = true;
			}
			return false; // The original click still edits/selects normally.
		}
	};
	TSharedPtr<FPropertyClickAcknowledgement> ClickAcknowledgement;

	void CancelReveal()
	{
		if (RevealHandle.IsValid() && FSlateApplication::IsInitialized())
			FSlateApplication::Get().OnPostTick().Remove(RevealHandle);
		RevealHandle.Reset();
		if (auto Details = HighlightDetails.Pin()) Details->HighlightProperty(FPropertyPath());
		HighlightDetails.Reset();
		if (ClickAcknowledgement && FSlateApplication::IsInitialized())
			FSlateApplication::Get().UnregisterInputPreProcessor(ClickAcknowledgement);
		ClickAcknowledgement.Reset();
		bBlinkActive = false;
		bHighlightVisible = false;
	}

	bool DetailsMatches(const TSharedPtr<IDetailsView>& Details, const UObject* Object)
	{
		return Details && Details->GetSelectedObjects().Num() == 1
			&& Details->GetSelectedObjects()[0].Get() == Object;
	}

	bool ReadProperty(UObject* Object, const FString& Path, FString& Value, FPropertyPath& DetailPath, FString& Error)
	{
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."), false);
		UStruct* ContainerType = Object->GetClass();
		void* Container = Object;
		for (int32 Index = 0; Index < Parts.Num(); ++Index)
		{
			FProperty* Field = FindFProperty<FProperty>(ContainerType, *Parts[Index]);
			if (!Field || Parts[Index].IsEmpty())
			{
				Error = TEXT("Property segment not found: ") + Parts[Index] + TEXT(" in ") + Path;
				return false;
			}
			DetailPath.AddProperty(FPropertyInfo(Field));
			void* Address = Field->ContainerPtrToValuePtr<void>(Container);
			if (Index == Parts.Num() - 1)
			{
				Field->ExportTextItem_Direct(Value, Address, nullptr, Object, PPF_None);
				return true;
			}
			FStructProperty* StructField = CastField<FStructProperty>(Field);
			if (!StructField || Field->ArrayDim != 1)
			{
				Error = TEXT("Intermediate property must be a scalar struct; object/array traversal is unsupported: ") + Parts[Index];
				return false;
			}
			Container = Address;
			ContainerType = StructField->Struct;
		}
		Error = TEXT("Property path is empty");
		return false;
	}

	TSharedPtr<FBlueprintEditor> FindBlueprintEditor(UObject* Asset)
	{
		if (!Cast<UBlueprint>(Asset)) return nullptr;
		return StaticCastSharedPtr<FBlueprintEditor>(FKismetEditorUtilities::GetIBlueprintEditorForObject(Asset, false));
	}

	void QueueReveal(const TSharedPtr<FBlueprintEditor>& Editor, UObject* Object, const FString& Path,
		UEdGraph* TargetGraph, UEdGraphNode* TargetNode)
	{
		CancelReveal();
		if (!FSlateApplication::IsInitialized()) { RevealStatus = TEXT("slate_unavailable"); return; }
		const uint64 Generation = NavigationGeneration;
		const TWeakPtr<FBlueprintEditor> WeakEditor = Editor;
		const TWeakObjectPtr<UObject> WeakObject = Object;
		const TWeakObjectPtr<UEdGraph> ExpectedGraph = TargetGraph ? TargetGraph : Editor->GetFocusedGraph();
		const bool bExpectNode = TargetNode != nullptr;
		TSet<TWeakObjectPtr<UObject>> ExpectedSelection;
		if (TargetNode) ExpectedSelection.Add(TargetNode);
		else for (UObject* Selected : Editor->GetSelectedNodes()) ExpectedSelection.Add(Selected);
		RevealStatus = TEXT("waiting_for_details_binding");
		RevealHandle = FSlateApplication::Get().OnPostTick().AddLambda(
			[WeakEditor, WeakObject, ExpectedGraph, ExpectedSelection, bExpectNode, Path, Generation, Attempts = 0, StartedAt = -1.0, PreviousPhase = -1](float) mutable
			{
				if (Generation != NavigationGeneration) return;
				if (ClickAcknowledgement && ClickAcknowledgement->bAcknowledged)
				{
					RevealStatus = TEXT("acknowledged_by_property_click"); CancelReveal(); return;
				}
				++Attempts;
				auto CurrentEditor = WeakEditor.Pin();
				UObject* Target = WeakObject.Get();
				if (!CurrentEditor || !Target)
				{
					RevealStatus = TEXT("cancelled_target_unavailable"); CancelReveal(); return;
				}
				const auto Selection = CurrentEditor->GetSelectedNodes();
				bool bSelectionMatches = Selection.Num() == ExpectedSelection.Num();
				for (UObject* Selected : Selection) bSelectionMatches &= ExpectedSelection.Contains(Selected);
				const auto ActiveTab = FGlobalTabmanager::Get()->GetActiveTab();
				const bool bGuideTabActive = bGuideActive && ActiveTab
					&& ActiveTab->GetLayoutIdentifier().TabType == FName(TEXT("MonolithGuidedWorkflow"));
				const bool bEditorActive = ActiveTab && (ActiveTab->GetTabManagerPtr() == CurrentEditor->GetTabManager() || bGuideTabActive);
				if (CurrentEditor->GetFocusedGraph() != ExpectedGraph.Get() || !bEditorActive)
				{
					RevealStatus = TEXT("cancelled_editor_or_selection_changed"); CancelReveal(); return;
				}
				if (!bSelectionMatches)
				{
					// JumpToNode can defer selecting its target until the graph panel tick.
					// Only an empty selection may settle; a different selected node is user intent.
					if (bExpectNode && Selection.Num() == 0 && Attempts < 2)
					{
						RevealStatus = TEXT("waiting_for_node_selection"); return;
					}
					RevealStatus = TEXT("cancelled_editor_or_selection_changed"); CancelReveal(); return;
				}
				auto Details = CurrentEditor->GetInspector()->GetPropertyView();
				if (!DetailsMatches(Details, Target))
				{
					if (Attempts >= 2) { RevealStatus = TEXT("cancelled_details_target_mismatch"); CancelReveal(); }
					return;
				}
				FString Value, Error;
				FPropertyPath DetailPath;
				if (ReadProperty(Target, Path, Value, DetailPath, Error))
				{
					if (StartedAt < 0.0)
					{
						StartedAt = FPlatformTime::Seconds();
						HighlightDetails = Details;
						ClickAcknowledgement = MakeShared<FPropertyClickAcknowledgement>();
						ClickAcknowledgement->Details = Details;
						ClickAcknowledgement->Path = DetailPath.ToString();
						FSlateApplication::Get().RegisterInputPreProcessor(ClickAcknowledgement);
						bBlinkActive = true;
						bHighlightRequested = true;
						RevealStatus = TEXT("blinking_until_property_click");
					}
					const int32 Phase = static_cast<int32>((FPlatformTime::Seconds() - StartedAt) / 0.6) % 2;
					if (Phase != PreviousPhase)
					{
						// Only the first reveal may scroll. Later flashes never pull an
						// off-screen property back while the person browses the panel.
						const bool bVisible = PreviousPhase < 0 || HasVisiblePropertyRow(Details.ToSharedRef(),
							PropertyRowIndex(Details, DetailPath.ToString()), Details->GetCachedGeometry().GetLayoutBoundingRect());
						bHighlightVisible = Phase == 0 && bVisible;
						Details->HighlightProperty(bHighlightVisible ? DetailPath : FPropertyPath());
						PreviousPhase = Phase;
					}
				}
				else { RevealStatus = TEXT("cancelled_property_unavailable"); CancelReveal(); }
			});
	}

	TSharedPtr<FJsonObject> DescribeNode(UEdGraphNode* Node)
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("node_name"), Node->GetName());
		Result->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Result->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		Result->SetStringField(TEXT("graph_path"), Node->GetGraph()->GetPathName());
		return Result;
	}

	TSharedPtr<FJsonObject> ReadState(UObject* Asset)
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
		Result->SetBoolField(TEXT("package_dirty"), Asset->GetOutermost()->IsDirty());
		Result->SetBoolField(TEXT("pixel_visibility_verified"), false);
		if (LastPropertyAsset.Get() == Asset)
		{
			auto Comparison = MakeShared<FJsonObject>();
			Comparison->SetStringField(TEXT("property_path"), LastPropertyPath);
			Comparison->SetBoolField(TEXT("target_available"), LastPropertyObject.IsValid());
			Comparison->SetStringField(TEXT("reveal_status"), RevealStatus);
			Comparison->SetBoolField(TEXT("highlight_requested"), bHighlightRequested);
			Comparison->SetBoolField(TEXT("blink_active"), bBlinkActive);
			Comparison->SetBoolField(TEXT("highlight_visible"), bHighlightVisible);
			Comparison->SetBoolField(TEXT("property_presentation_verified"), false);
			if (UObject* Object = LastPropertyObject.Get())
			{
				FString Value, Error;
				FPropertyPath DetailPath;
				Comparison->SetStringField(TEXT("object_path"), Object->GetPathName());
				if (ReadProperty(Object, LastPropertyPath, Value, DetailPath, Error))
				{
					Comparison->SetStringField(TEXT("value_exported"), Value);
					Comparison->SetStringField(TEXT("baseline_exported"), LastPropertyBaseline);
					Comparison->SetBoolField(TEXT("changed_since_navigation"), Value != LastPropertyBaseline);
					bool bMatches = false, bListed = false, bRowListed = false;
					TArray<TSharedPtr<FJsonValue>> Paths;
					if (auto Editor = FindBlueprintEditor(Asset))
					{
						auto Details = Editor->GetInspector()->GetPropertyView();
						bMatches = DetailsMatches(Details, Object);
						if (bMatches)
						{
							for (const FPropertyPath& Displayed : Details->GetPropertiesInOrderDisplayed())
							{
								Paths.Add(MakeShared<FJsonValueString>(Displayed.ToString()));
								bListed |= Displayed.ToString() == DetailPath.ToString();
							}
							for (const auto& Row : Details->GetPropertyRowNumbers())
								bRowListed |= Row.Value.ToString() == DetailPath.ToString();
						}
					}
					Comparison->SetBoolField(TEXT("details_target_matches"), bMatches);
					Comparison->SetBoolField(TEXT("exact_property_path_listed"), bListed);
					Comparison->SetBoolField(TEXT("property_row_listed"), bRowListed);
					Comparison->SetArrayField(TEXT("details_property_paths"), Paths);
				}
				else Comparison->SetStringField(TEXT("read_error"), Error);
			}
			Result->SetObjectField(TEXT("last_property_target"), Comparison);
		}
		UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		IAssetEditorInstance* Instance = Subsystem->FindEditorForAsset(Asset, false);
		Result->SetBoolField(TEXT("editor_open"), Instance != nullptr);
		Result->SetStringField(TEXT("editor_name"), Instance ? Instance->GetEditorName().ToString() : FString());
		if (auto Editor = FindBlueprintEditor(Asset))
		{
			UEdGraph* Graph = Editor->GetFocusedGraph();
			Result->SetStringField(TEXT("focused_graph_path"), Graph ? Graph->GetPathName() : FString());
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (UObject* Object : Editor->GetSelectedNodes())
				if (UEdGraphNode* Node = Cast<UEdGraphNode>(Object)) Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(Node)));
			Result->SetArrayField(TEXT("selected_nodes"), Nodes);
			TArray<TSharedPtr<FJsonValue>> Objects;
			if (auto Details = Editor->GetInspector()->GetPropertyView())
				for (const auto& Object : Details->GetSelectedObjects())
					if (Object.IsValid()) Objects.Add(MakeShared<FJsonValueString>(Object->GetPathName()));
			Result->SetArrayField(TEXT("details_object_paths"), Objects);
		}
		return Result;
	}

	FMonolithActionResult TargetError(const FString& Message, const TArray<TSharedPtr<FJsonValue>>& Candidates)
	{
		auto Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("candidates"), Candidates);
		auto Result = FMonolithActionResult::Error(Message, -32602);
		Result.WithErrorData(Data);
		return Result;
	}
}

void FMonolithEditorNavigationActions::Shutdown()
{
	++NavigationGeneration;
	CancelReveal();
	LastPropertyAsset.Reset();
	LastPropertyObject.Reset();
	bGuideActive = false;
}

void FMonolithEditorNavigationActions::StopHighlight()
{
	++NavigationGeneration;
	CancelReveal();
	RevealStatus = TEXT("stopped_explicitly");
}

void FMonolithEditorNavigationActions::SetGuideActive(bool bActive)
{
	bGuideActive = bActive;
}

void FMonolithEditorNavigationActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("editor"), TEXT("navigate_to"),
		TEXT("Open an asset, or focus a Blueprint graph and select a node. Exact graph names/full object paths and node names/GUIDs only; ambiguous targets return candidates. Reads actual editor selection and package dirty state. property_path accepts reflected fields and nested structs: reveals Blueprint Details and blinks the property row until it is clicked or selection changes. Reports blink state; pixel visibility needs separate verification. Does not edit or save assets."),
		FMonolithActionHandler::CreateStatic(&HandleNavigateTo),
		FParamSchemaBuilder().Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact asset object or package path."))
		.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Exact graph name or full graph object path."))
		.Optional(TEXT("node_id"), TEXT("string"), TEXT("Exact node object name or GUID. Searches all graphs when graph_name is omitted."))
		.Optional(TEXT("property_path"), TEXT("string"), TEXT("Exact reflected property path, including nested structs such as Node.BlendTime. Arrays and object traversal are unsupported."))
		.Build());
	Registry.RegisterAction(TEXT("editor"), TEXT("get_navigation_state"),
		TEXT("Read an already loaded asset's editor state: actual focused Blueprint graph, selected nodes, Details bound objects and dirty flag. Reads the last navigated property through weak references and compares its current exported value with the navigation baseline. Does not open an editor or verify pixel visibility."),
		FMonolithActionHandler::CreateStatic(&HandleGetNavigationState),
		FParamSchemaBuilder().Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact asset object path; asset must already be loaded."))
		.Build());
}

FMonolithActionResult FMonolithEditorNavigationActions::HandleGetNavigationState(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	if (!GEditor || !Params->TryGetStringField(TEXT("asset_path"), Path) || Path.IsEmpty())
		return FMonolithActionResult::Error(TEXT("asset_path and editor are required"), -32602);
	UObject* Asset = FindObject<UObject>(nullptr, *Path);
	if (!Asset) return FMonolithActionResult::Error(TEXT("Asset is not loaded. Supply its full object path."), -32602);
	return FMonolithActionResult::Success(ReadState(Asset));
}

FMonolithActionResult FMonolithEditorNavigationActions::HandleNavigateTo(const TSharedPtr<FJsonObject>& Params)
{
	++NavigationGeneration;
	CancelReveal();
	RevealStatus = TEXT("not_requested");
	bHighlightRequested = false;
	FString Path, GraphName, NodeId, PropertyName;
	if (!GEditor || !Params->TryGetStringField(TEXT("asset_path"), Path) || Path.IsEmpty())
		return FMonolithActionResult::Error(TEXT("asset_path and editor are required"), -32602);
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	Params->TryGetStringField(TEXT("node_id"), NodeId);
	Params->TryGetStringField(TEXT("property_path"), PropertyName);
	UObject* Asset = LoadObject<UObject>(nullptr, *Path);
	if (!Asset) return FMonolithActionResult::Error(TEXT("Asset not found: ") + Path, -32602);
	const bool bDirtyBefore = Asset->GetOutermost()->IsDirty();
	UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	UEdGraph* TargetGraph = nullptr;
	UEdGraphNode* TargetNode = nullptr;
	if (!GraphName.IsEmpty() || !NodeId.IsEmpty())
	{
		if (!Blueprint) return FMonolithActionResult::Error(TEXT("Graph/node navigation requires a Blueprint asset"), -32602);
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		if (!GraphName.IsEmpty())
		{
			TArray<UEdGraph*> Matches;
			TArray<TSharedPtr<FJsonValue>> Candidates;
			for (UEdGraph* Graph : Graphs)
			{
				Candidates.Add(MakeShared<FJsonValueString>(Graph->GetPathName()));
				if (Graph->GetName() == GraphName || Graph->GetPathName() == GraphName) Matches.Add(Graph);
			}
			if (Matches.Num() != 1) return TargetError(TEXT("graph_name must identify exactly one graph"), Candidates);
			TargetGraph = Matches[0];
			Graphs = Matches;
		}
		if (!NodeId.IsEmpty())
		{
			FGuid Guid;
			const bool bGuid = FGuid::Parse(NodeId, Guid);
			TArray<UEdGraphNode*> Matches;
			TArray<TSharedPtr<FJsonValue>> Candidates;
			for (UEdGraph* Graph : Graphs)
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node) continue;
					Candidates.Add(MakeShared<FJsonValueObject>(DescribeNode(Node)));
					if (Node->GetName() == NodeId || (bGuid && Node->NodeGuid == Guid)) Matches.Add(Node);
				}
			if (Matches.Num() != 1) return TargetError(TEXT("node_id must identify exactly one node"), Candidates);
			TargetNode = Matches[0];
			TargetGraph = TargetNode->GetGraph();
		}
	}
	UObject* PropertyObject = TargetNode ? static_cast<UObject*>(TargetNode) : Asset;
	if (!TargetNode && Blueprint && Blueprint->GeneratedClass) PropertyObject = Blueprint->GeneratedClass->GetDefaultObject();
	const bool bHasProperty = !PropertyName.IsEmpty();
	FPropertyPath DetailPath;
	FString PropertyValue;
	if (bHasProperty)
	{
		FString Error;
		if (!ReadProperty(PropertyObject, PropertyName, PropertyValue, DetailPath, Error))
			return FMonolithActionResult::Error(Error, -32602);
	}
	UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Subsystem->OpenEditorForAsset(Asset)) return FMonolithActionResult::Error(TEXT("Asset editor could not be opened"));
	bool bDetailsRequested = false;
	if (auto Editor = FindBlueprintEditor(Asset))
	{
		if (TargetGraph)
		{
			auto GraphEditor = Editor->OpenGraphAndBringToFront(TargetGraph, true);
			if (!GraphEditor) return FMonolithActionResult::Error(TEXT("Graph editor could not be opened"));
			if (TargetNode)
			{
				GraphEditor->ClearSelectionSet();
				GraphEditor->JumpToNode(TargetNode, false, true);
			}
		}
		if (bHasProperty)
		{
			Editor->GetTabManager()->TryInvokeTab(FBlueprintEditorTabs::DetailsID);
			Editor->GetInspector()->ShowDetailsForSingleObject(PropertyObject);
			if (auto Details = Editor->GetInspector()->GetPropertyView())
			{
				Details->SetSearchText(FText::GetEmpty());
			}
			QueueReveal(Editor, PropertyObject, PropertyName, TargetGraph, TargetNode);
			bDetailsRequested = true;
		}
	}
	LastPropertyAsset.Reset();
	LastPropertyObject.Reset();
	if (bHasProperty)
	{
		LastPropertyAsset = Asset;
		LastPropertyObject = PropertyObject;
		LastPropertyPath = PropertyName;
		LastPropertyBaseline = PropertyValue;
	}
	auto Result = ReadState(Asset);
	Result->SetBoolField(TEXT("package_dirty_before"), bDirtyBefore);
	Result->SetBoolField(TEXT("navigation_requested"), true);
	if (bHasProperty)
	{
		Result->SetStringField(TEXT("property_object_path"), PropertyObject->GetPathName());
		Result->SetStringField(TEXT("property_path"), PropertyName);
		Result->SetStringField(TEXT("property_value_exported"), PropertyValue);
		Result->SetBoolField(TEXT("details_binding_requested"), bDetailsRequested);
		Result->SetBoolField(TEXT("property_presentation_verified"), false);
		Result->SetStringField(TEXT("property_navigation_status"), bDetailsRequested ? TEXT("details_binding_and_highlight_requested; rendering_unverified") : TEXT("reflection_only; native_details_adapter_unavailable"));
	}
	return FMonolithActionResult::Success(Result);
}
