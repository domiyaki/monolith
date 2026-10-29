#include "MonolithEditorWidgetProbeActions.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Children.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SCanvas.h"
#include "Widgets/Layout/SBorder.h"
#include "Styling/AppStyle.h"

namespace MonolithWidgetProbe
{
	// Tokens identify live widget instances in one bounded observation only.
	// They deliberately do not keep editors alive or promise persistent identity.
	TMap<FString, TWeakPtr<SWidget>> Tokens;
	TMap<const SWidget*, FString> ReverseTokens;
	FString Snapshot;
	int32 NextToken = 0;
	TWeakPtr<SWindow> HighlightWindow;
	TSharedPtr<SWidget> HighlightOverlay;

	void ClearHighlight()
	{
		if (const auto Window = HighlightWindow.Pin(); Window && HighlightOverlay)
			Window->RemoveOverlaySlot(HighlightOverlay.ToSharedRef());
		HighlightOverlay.Reset();
		HighlightWindow.Reset();
	}

	// Parent-chain validation also hides the marker if a still-alive widget is detached.
	bool IsAttachedAndVisible(const TWeakPtr<SWidget>& Target, const TWeakPtr<SWindow>& Owner)
	{
		const auto Window = Owner.Pin();
		if (!Window || !Window->IsVisible()) return false;
		auto Current = Target.Pin();
		for (int32 Depth = 0; Current && Depth < 128; ++Depth)
		{
			if (!Current->GetVisibility().IsVisible()) return false;
			if (Current.Get() == Window.Get()) return true;
			Current = Current->GetParentWidget();
		}
		return false;
	}

	bool IsButton(const SWidget& Widget)
	{
		const FName Type = Widget.GetType();
		return Type == TEXT("SButton") || Type == TEXT("SComboButton");
	}

	FString OwnText(const TSharedRef<SWidget>& Widget)
	{
		return Widget->GetType() == TEXT("STextBlock")
			? StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString().Left(1024) : FString();
	}

	void GatherLabel(const TSharedRef<SWidget>& Widget, FString& Label, int32 Depth, int32& Budget)
	{
		if (--Budget < 0 || Depth > 12 || Label.Len() >= 1024 || !Widget->GetVisibility().IsVisible()) return;
		const FString Value = OwnText(Widget);
		if (!Value.IsEmpty()) { if (!Label.IsEmpty()) Label += TEXT(" | "); Label += Value; }
		if (FChildren* Children = Widget->GetChildren())
			for (int32 I = 0; I < Children->Num() && Budget > 0; ++I)
				GatherLabel(Children->GetChildAt(I), Label, Depth + 1, Budget);
	}

	FString Label(const TSharedRef<SWidget>& Widget)
	{
		FString Value = OwnText(Widget);
		if (Value.IsEmpty() && IsButton(Widget.Get())) { int32 Budget = 128; GatherLabel(Widget, Value, 0, Budget); }
		return Value.Left(1024);
	}

	FString Token(const TSharedRef<SWidget>& Widget)
	{
		if (const FString* Existing = ReverseTokens.Find(&Widget.Get())) return *Existing;
		const FString Value = Snapshot + TEXT(":") + FString::FromInt(++NextToken);
		Tokens.Add(Value, Widget);
		ReverseTokens.Add(&Widget.Get(), Value);
		return Value;
	}

	TSharedPtr<FJsonObject> Describe(const TSharedRef<SWidget>& Widget, const FString& WindowTitle,
		const TArray<FString>& Path, bool bVisible, bool bEnabled, bool bIssueToken)
	{
		auto Result = MakeShared<FJsonObject>();
		if (bIssueToken) Result->SetStringField(TEXT("token"), Token(Widget));
		Result->SetStringField(TEXT("window_title"), WindowTitle);
		Result->SetStringField(TEXT("type"), Widget->GetType().ToString());
		Result->SetStringField(TEXT("tag"), Widget->GetTag().ToString());
		Result->SetStringField(TEXT("text"), OwnText(Widget));
		Result->SetStringField(TEXT("label"), Label(Widget));
		Result->SetBoolField(TEXT("visible"), bVisible);
		Result->SetBoolField(TEXT("enabled"), bEnabled);
		Result->SetBoolField(TEXT("path_is_persistent_identity"), false);
		Result->SetBoolField(TEXT("pixel_visibility_verified"), false);
		TArray<TSharedPtr<FJsonValue>> PathValues;
		for (const FString& Segment : Path) PathValues.Add(MakeShared<FJsonValueString>(Segment));
		Result->SetArrayField(TEXT("widget_path"), PathValues);
		const FSlateRect Bounds = Widget->GetCachedGeometry().GetLayoutBoundingRect();
		auto Geometry = MakeShared<FJsonObject>();
		Geometry->SetNumberField(TEXT("left"), Bounds.Left);
		Geometry->SetNumberField(TEXT("top"), Bounds.Top);
		Geometry->SetNumberField(TEXT("right"), Bounds.Right);
		Geometry->SetNumberField(TEXT("bottom"), Bounds.Bottom);
		Geometry->SetBoolField(TEXT("has_area"), Bounds.Right > Bounds.Left && Bounds.Bottom > Bounds.Top);
		Result->SetObjectField(TEXT("absolute_cached_bounds"), Geometry);
		return Result;
	}

	struct FWalk
	{
		FString WindowFilter, TypeFilter, TextFilter;
		int32 Limit = 100, MaxNodes = 30000, Visited = 0;
		bool bTruncated = false;
		TArray<TSharedPtr<FJsonValue>> Matches;
		SWidget* Wanted = nullptr;
		TSharedPtr<FJsonObject> Resolved;
		TSet<const SWidget*> Seen;

		void Visit(const TSharedRef<SWidget>& Widget, const FString& WindowTitle, TArray<FString>& Path,
			TArray<TSharedRef<SWidget>>& Ancestors, bool bParentVisible, bool bParentEnabled, int32 SiblingIndex)
		{
			if (Visited >= MaxNodes || Path.Num() >= 128) { bTruncated = true; return; }
			if (Seen.Contains(&Widget.Get())) return;
			Seen.Add(&Widget.Get());
			++Visited;
			const bool bVisible = bParentVisible && Widget->GetVisibility().IsVisible();
			const bool bEnabled = bParentEnabled && Widget->IsEnabled();
			Path.Add(FString::Printf(TEXT("%s[tag=%s,index=%d]"), *Widget->GetType().ToString(), *Widget->GetTag().ToString(), SiblingIndex));
			if (Wanted)
			{
				if (&Widget.Get() == Wanted) Resolved = Describe(Widget, WindowTitle, Path, bVisible, bEnabled, false);
			}
			else if ((TypeFilter.IsEmpty() || Widget->GetType().ToString() == TypeFilter)
				&& (TextFilter.IsEmpty() || Label(Widget).Contains(TextFilter)))
			{
				if (Matches.Num() < Limit)
				{
					auto Description = Describe(Widget, WindowTitle, Path, bVisible, bEnabled, true);
					for (int32 A = Ancestors.Num() - 1; A >= 0; --A)
					{
						if (IsButton(Ancestors[A].Get()))
						{
							auto Button = MakeShared<FJsonObject>();
							Button->SetStringField(TEXT("token"), Token(Ancestors[A]));
							Button->SetStringField(TEXT("type"), Ancestors[A]->GetType().ToString());
							Button->SetStringField(TEXT("tag"), Ancestors[A]->GetTag().ToString());
							Button->SetStringField(TEXT("label"), Label(Ancestors[A]));
							Description->SetObjectField(TEXT("nearest_button_ancestor"), Button);
							break;
						}
					}
					Matches.Add(MakeShared<FJsonValueObject>(Description));
				}
				else bTruncated = true;
			}
			if (!Resolved)
			{
				Ancestors.Add(Widget);
				if (FChildren* Children = Widget->GetChildren())
					for (int32 I = 0; I < Children->Num() && Visited < MaxNodes && !Resolved; ++I)
						Visit(Children->GetChildAt(I), WindowTitle, Path, Ancestors, bVisible, bEnabled, I);
				Ancestors.Pop();
			}
			Path.Pop();
		}

		void Run()
		{
			TArray<TSharedRef<SWindow>> Windows;
			FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
			for (const auto& Window : Windows)
			{
				const FString WindowTitle = Window->GetTitle().ToString();
				if (!WindowFilter.IsEmpty() && !WindowTitle.Contains(WindowFilter)) continue;
				TArray<FString> Path;
				TArray<TSharedRef<SWidget>> Ancestors;
				Visit(Window, WindowTitle, Path, Ancestors, true, true, 0);
				if (Resolved) break;
				if (Visited >= MaxNodes) { bTruncated = true; break; }
			}
		}
	};
}

void FMonolithEditorWidgetProbeActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("editor"), TEXT("inspect_editor_widgets"),
		TEXT("Read-only generic Slate widget discovery across visible editor windows, independent of asset/editor classes. Filters window-title substring, exact widget type, or label substring. Text comes from STextBlock and button descendants. Returns all matches up to limit, ancestor paths, weak snapshot tokens, and cached bounds. Text/path are not guaranteed unique or persistent; hidden/unrealized UI may not exist in the tree. Each inspect invalidates previous snapshot tokens."),
		FMonolithActionHandler::CreateStatic(&HandleInspect), FParamSchemaBuilder()
		.Optional(TEXT("window_title"), TEXT("string"), TEXT("Window title substring."))
		.Optional(TEXT("type"), TEXT("string"), TEXT("Exact Slate widget class name."))
		.Optional(TEXT("text"), TEXT("string"), TEXT("Label substring; generic STextBlock/button text only."))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Returned matches, clamped 1..200; default 100."))
		.Optional(TEXT("max_nodes"), TEXT("integer"), TEXT("Visited nodes, clamped 1..30000; default 30000."))
		.Build());
	Registry.RegisterAction(TEXT("editor"), TEXT("resolve_editor_widget"),
		TEXT("Re-resolve a weak token from the latest inspect_editor_widgets snapshot. Reads current geometry and membership in the currently visible window tree. Rejects expired, detached, or previous-snapshot tokens; does not click or highlight."),
		FMonolithActionHandler::CreateStatic(&HandleResolve), FParamSchemaBuilder()
		.Required(TEXT("token"), TEXT("string"), TEXT("Weak widget token from the latest snapshot."))
		.Build());
	Registry.RegisterAction(TEXT("editor"), TEXT("highlight_editor_widget"),
		TEXT("Draw a noninteractive window overlay around a discovered live widget using generic Slate geometry. Follows layout; clears on next inspect or clear_widget_highlight. Does not modify or invoke the target."),
		FMonolithActionHandler::CreateStatic(&HandleHighlight), FParamSchemaBuilder()
		.Required(TEXT("token"), TEXT("string"), TEXT("Widget or nearest_button_ancestor token from latest snapshot."))
		.Build());
	Registry.RegisterAction(TEXT("editor"), TEXT("clear_widget_highlight"),
		TEXT("Remove the generic widget discovery highlight overlay."),
		FMonolithActionHandler::CreateStatic(&HandleClearHighlight), FParamSchemaBuilder().Build());
}

FMonolithActionResult FMonolithEditorWidgetProbeActions::HandleInspect(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithWidgetProbe;
	if (!FSlateApplication::IsInitialized()) return FMonolithActionResult::Error(TEXT("Slate is unavailable"));
	FWalk Walk;
	if (Params)
	{
		Params->TryGetStringField(TEXT("window_title"), Walk.WindowFilter);
		Params->TryGetStringField(TEXT("type"), Walk.TypeFilter);
		Params->TryGetStringField(TEXT("text"), Walk.TextFilter);
		double Number;
		if (Params->TryGetNumberField(TEXT("limit"), Number)) Walk.Limit = static_cast<int32>(FMath::Clamp(Number, 1.0, 200.0));
		if (Params->TryGetNumberField(TEXT("max_nodes"), Number)) Walk.MaxNodes = static_cast<int32>(FMath::Clamp(Number, 1.0, 30000.0));
	}
	ClearHighlight();
	Tokens.Reset(); ReverseTokens.Reset(); NextToken = 0;
	Snapshot = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Walk.Run();
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("snapshot_id"), Snapshot);
	Result->SetNumberField(TEXT("visited_nodes"), Walk.Visited);
	Result->SetNumberField(TEXT("returned_matches"), Walk.Matches.Num());
	Result->SetBoolField(TEXT("truncated"), Walk.bTruncated);
	Result->SetStringField(TEXT("identity_scope"), TEXT("weak live widget instance; latest snapshot only"));
	Result->SetArrayField(TEXT("matches"), Walk.Matches);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorWidgetProbeActions::HandleResolve(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithWidgetProbe;
	FString Value;
	if (!FSlateApplication::IsInitialized() || !Params || !Params->TryGetStringField(TEXT("token"), Value))
		return FMonolithActionResult::Error(TEXT("Slate and token are required"), -32602);
	const auto* Weak = Tokens.Find(Value);
	const auto Widget = Weak ? Weak->Pin() : nullptr;
	if (!Widget) return FMonolithActionResult::Error(TEXT("Token is expired or belongs to a previous snapshot. Inspect again."), -32602);
	FWalk Walk;
	Walk.Wanted = Widget.Get();
	Walk.Run();
	if (!Walk.Resolved) return FMonolithActionResult::Error(Walk.bTruncated
		? TEXT("Traversal limit reached before resolving target; membership is unknown.")
		: TEXT("Widget is no longer in a visible editor window tree. Inspect again."), -32602);
	Walk.Resolved->SetStringField(TEXT("token"), Value);
	Walk.Resolved->SetBoolField(TEXT("in_current_window_tree"), true);
	Walk.Resolved->SetNumberField(TEXT("visited_nodes"), Walk.Visited);
	return FMonolithActionResult::Success(Walk.Resolved);
}

FMonolithActionResult FMonolithEditorWidgetProbeActions::HandleHighlight(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithWidgetProbe;
	FString Value;
	if (!FSlateApplication::IsInitialized() || !Params || !Params->TryGetStringField(TEXT("token"), Value))
		return FMonolithActionResult::Error(TEXT("Slate and token are required"), -32602);
	const auto* Found = Tokens.Find(Value);
	const auto Target = Found ? Found->Pin() : nullptr;
	if (!Target) return FMonolithActionResult::Error(TEXT("Token has expired. Inspect again."), -32602);
	FWalk Walk;
	Walk.Wanted = Target.Get();
	Walk.Run();
	if (!Walk.Resolved || !Walk.Resolved->GetBoolField(TEXT("visible")))
		return FMonolithActionResult::Error(TEXT("Target is not resolved and visible in the current window tree."), -32602);
	const auto Window = FSlateApplication::Get().FindWidgetWindow(Target.ToSharedRef());
	if (!Window) return FMonolithActionResult::Error(TEXT("Target has no owning Slate window."));
	ClearHighlight();
	const TWeakPtr<SWidget> WeakTarget = Target;
	const TWeakPtr<SWindow> WeakWindow = Window;
	HighlightWindow = Window;
	HighlightOverlay = SNew(SCanvas)
		.Visibility(EVisibility::HitTestInvisible)
		+ SCanvas::Slot()
		.Position_Lambda([WeakTarget, WeakWindow]() -> FVector2D
		{
			const auto Widget = WeakTarget.Pin(); const auto Owner = WeakWindow.Pin();
			if (!Widget || !Owner) return FVector2D::ZeroVector;
			const auto Bounds = Widget->GetCachedGeometry().GetLayoutBoundingRect();
			return Owner->GetWindowGeometryInScreen().AbsoluteToLocal(FVector2D(Bounds.Left, Bounds.Top));
		})
		.Size_Lambda([WeakTarget, WeakWindow]() -> FVector2D
		{
			const auto Widget = WeakTarget.Pin(); const auto Owner = WeakWindow.Pin();
			if (!Widget || !Owner) return FVector2D::ZeroVector;
			const auto Bounds = Widget->GetCachedGeometry().GetLayoutBoundingRect();
			const auto Geometry = Owner->GetWindowGeometryInScreen();
			return Geometry.AbsoluteToLocal(FVector2D(Bounds.Right, Bounds.Bottom))
				- Geometry.AbsoluteToLocal(FVector2D(Bounds.Left, Bounds.Top));
		})
		[
			SNew(SBorder)
			.Visibility_Lambda([WeakTarget, WeakWindow]() { return IsAttachedAndVisible(WeakTarget, WeakWindow)
				? EVisibility::HitTestInvisible : EVisibility::Hidden; })
			.BorderImage(FAppStyle::GetBrush(TEXT("FocusRectangle")))
			.BorderBackgroundColor(FLinearColor(1.0f, 0.45f, 0.02f, 1.0f))
		];
	Window->AddOverlaySlot(10000)[HighlightOverlay.ToSharedRef()];
	Walk.Resolved->SetStringField(TEXT("token"), Value);
	Walk.Resolved->SetBoolField(TEXT("highlight_overlay_added"), true);
	Walk.Resolved->SetStringField(TEXT("highlight_style"), TEXT("steady outline; prototype"));
	return FMonolithActionResult::Success(Walk.Resolved);
}

FMonolithActionResult FMonolithEditorWidgetProbeActions::HandleClearHighlight(const TSharedPtr<FJsonObject>& Params)
{
	MonolithWidgetProbe::ClearHighlight();
	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("cleared"), true);
	return FMonolithActionResult::Success(Result);
}

void FMonolithEditorWidgetProbeActions::Shutdown()
{
	MonolithWidgetProbe::ClearHighlight();
	MonolithWidgetProbe::Tokens.Reset();
	MonolithWidgetProbe::ReverseTokens.Reset();
}
