#include "MonolithMaterialClipboard.h"
#include "MonolithMaterialActions.h"
#include "MonolithParamSchema.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Comment.h"
#include "EdGraphUtilities.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/PlatformApplicationMisc.h"
#include "MaterialShared.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformProcess.h"
#include "Framework/Application/SlateApplication.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <ole2.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
FString LastText;
TSharedPtr<FJsonObject> LastReport;

bool WriteClipboard(const FString& Text, FString& Error, bool SimulateFailure = false)
{
#if PLATFORM_WINDOWS
	// Resolve Windows entry points so this module can also be introduced through
	// Live Coding without restarting the user's open, unsaved material editors.
	void* User32 = FPlatformProcess::GetDllHandle(TEXT("user32.dll"));
	void* Ole32 = FPlatformProcess::GetDllHandle(TEXT("ole32.dll"));
	ON_SCOPE_EXIT { if (Ole32) FPlatformProcess::FreeDllHandle(Ole32); if (User32) FPlatformProcess::FreeDllHandle(User32); };
	if (!User32 || !Ole32) { Error = TEXT("Windows clipboard libraries unavailable"); return false; }
#define CLIPBOARD_API(Library, Name) const auto Name = reinterpret_cast<decltype(&::Name)>(FPlatformProcess::GetDllExport(Library, TEXT(#Name)))
	CLIPBOARD_API(User32, OpenClipboard);
	CLIPBOARD_API(User32, CloseClipboard);
	CLIPBOARD_API(User32, EnumClipboardFormats);
	CLIPBOARD_API(User32, GetClipboardData);
	CLIPBOARD_API(User32, SetClipboardData);
	CLIPBOARD_API(User32, EmptyClipboard);
	CLIPBOARD_API(Ole32, OleDuplicateData);
	CLIPBOARD_API(Ole32, ReleaseStgMedium);
#undef CLIPBOARD_API
	if (!OpenClipboard || !CloseClipboard || !EnumClipboardFormats || !GetClipboardData || !SetClipboardData || !EmptyClipboard || !OleDuplicateData || !ReleaseStgMedium)
	{ Error = TEXT("Windows clipboard entry points unavailable"); return false; }
	// Keep one clipboard lock across snapshot, commit and readback. A failed
	// preflight leaves every original format untouched, including non-text data.
	HWND Owner = static_cast<HWND>(const_cast<void*>(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr)));
	if (!Owner || !OpenClipboard(Owner)) { Error = TEXT("Clipboard is busy; previous contents unchanged"); return false; }
	TArray<UINT> Formats;
	TArray<STGMEDIUM> Copies;
	ON_SCOPE_EXIT
	{
		for (auto& Copy : Copies) ReleaseStgMedium(&Copy);
		CloseClipboard();
	};
	UINT Format = 0;
	for (;;)
	{
		::SetLastError(ERROR_SUCCESS);
		Format = EnumClipboardFormats(Format);
		if (!Format)
		{
			if (::GetLastError() != ERROR_SUCCESS) { Error = TEXT("Cannot enumerate clipboard formats; contents unchanged"); return false; }
			break;
		}
		if (Format == CF_ENHMETAFILE || Format == CF_OWNERDISPLAY || Format == CF_DSPBITMAP || Format == CF_DSPMETAFILEPICT || Format == CF_DSPENHMETAFILE || (Format >= CF_GDIOBJFIRST && Format <= CF_GDIOBJLAST))
		{
			Error = FString::Printf(TEXT("Cannot safely preserve clipboard handle format %u; contents unchanged"), Format); return false;
		}
		HANDLE Original = GetClipboardData(Format);
		HANDLE Duplicate = Original ? OleDuplicateData(Original, static_cast<CLIPFORMAT>(Format), GMEM_MOVEABLE) : nullptr;
		if (!Duplicate) { Error = FString::Printf(TEXT("Cannot preserve clipboard format %u; contents unchanged"), Format); return false; }
		STGMEDIUM Copy = {};
		Copy.tymed = Format == CF_BITMAP || Format == CF_PALETTE ? TYMED_GDI
			: Format == CF_METAFILEPICT ? TYMED_MFPICT : Format == CF_ENHMETAFILE ? TYMED_ENHMF : TYMED_HGLOBAL;
		Copy.hGlobal = static_cast<HGLOBAL>(Duplicate);
		Formats.Add(Format); Copies.Add(Copy);
	}
	HGLOBAL Buffer = ::GlobalAlloc(GMEM_MOVEABLE, (Text.Len() + 1) * sizeof(TCHAR));
	if (!Buffer) { Error = TEXT("Clipboard allocation failed; contents unchanged"); return false; }
	void* Data = ::GlobalLock(Buffer);
	if (!Data) { ::GlobalFree(Buffer); Error = TEXT("Clipboard allocation lock failed; contents unchanged"); return false; }
	FMemory::Memcpy(Data, *Text, (Text.Len() + 1) * sizeof(TCHAR));
	::GlobalUnlock(Buffer);
	if (!EmptyClipboard()) { ::GlobalFree(Buffer); Error = TEXT("Cannot clear clipboard; contents unchanged"); return false; }
	bool Written = SetClipboardData(CF_UNICODETEXT, Buffer) != nullptr;
	if (!Written) ::GlobalFree(Buffer);
	if (Written)
	{
		HANDLE ReadHandle = GetClipboardData(CF_UNICODETEXT);
		const TCHAR* Read = static_cast<const TCHAR*>(::GlobalLock(ReadHandle));
		Written = Read && Text == Read;
		if (Read) ::GlobalUnlock(ReadHandle);
	}
	if (Written && !SimulateFailure) return true;
	bool Restored = !!EmptyClipboard();
	for (int32 I = 0; I < Copies.Num(); ++I)
	{
		if (SetClipboardData(Formats[I], Copies[I].hGlobal)) Copies[I].tymed = TYMED_NULL; // ownership transferred
		else Restored = false;
	}
	Error = Restored ? TEXT("Clipboard write/readback failed; original formats restored")
		: TEXT("Clipboard write/readback and restoration failed; successful cache retained");
	return false;
#else
	Error = TEXT("Material clipboard delivery currently requires Windows");
	return false;
#endif
}

FString Str(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
{
	FString S;
	if (O) O->TryGetStringField(Key, S);
	return S;
}

TArray<TSharedPtr<FJsonValue>> Array(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
{
	const TArray<TSharedPtr<FJsonValue>>* A;
	return O && O->TryGetArrayField(Key, A) ? *A : TArray<TSharedPtr<FJsonValue>>();
}

TStrongObjectPtr<UMaterial> NewScratch()
{
	TStrongObjectPtr<UMaterial> M(NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient));
	M->SetShadingModel(MSM_Unlit);
	M->MaterialGraph = NewObject<UMaterialGraph>(M.Get(), NAME_None, RF_Transient);
	M->MaterialGraph->Material = M.Get();
	M->MaterialGraph->Schema = UMaterialGraphSchema::StaticClass();
	return M;
}

UMaterialExpression* ExpressionOf(UEdGraphNode* N)
{
	if (auto* E = Cast<UMaterialGraphNode>(N)) return E->MaterialExpression;
	if (auto* C = Cast<UMaterialGraphNode_Comment>(N)) return C->MaterialExpressionComment;
	return nullptr;
}

// Stable semantic comparison across different transient owners and object names.
FString Fingerprint(const TSet<UEdGraphNode*>& Nodes)
{
	TMap<FString, FString> Names;
	for (auto* N : Nodes)
		if (auto* E = ExpressionOf(N)) Names.Add(E->GetPathName(), N->NodeGuid.ToString());
	TArray<FString> Rows;
	for (auto* N : Nodes)
	{
		UMaterialExpression* E = ExpressionOf(N);
		FString Row = FString::Printf(TEXT("%s|%s|%d|%d"), *N->NodeGuid.ToString(), *N->GetClass()->GetPathName(), N->NodePosX, N->NodePosY);
		if (E)
		{
			Row += E->GetClass()->GetPathName();
			// Named links are non-editable UObject properties, not graph pin links.
			if (auto* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(E))
				Row += TEXT("|Declaration=") + (Usage->Declaration ? Names.FindRef(Usage->Declaration->GetPathName()) : TEXT("unbound"));
			for (TFieldIterator<FProperty> It(E->GetClass()); It; ++It)
			{
				FProperty* P = *It;
				if (!P->HasAnyPropertyFlags(CPF_Edit) || P->HasAnyPropertyFlags(CPF_Transient)) continue;
				FString Value;
				P->ExportText_InContainer(0, Value, E, E, E, PPF_None);
				for (const auto& Name : Names) Value.ReplaceInline(*Name.Key, *Name.Value, ESearchCase::CaseSensitive);
				Row += TEXT("|") + P->GetName() + TEXT("=") + Value;
			}
		}
		for (auto* Pin : N->Pins)
		{
			FString PinName = Pin->PinName.ToString();
			if (Cast<UMaterialExpressionMaterialFunctionCall>(E) && Pin->Direction == EGPD_Input)
			{
				// Material paste strips display-only type suffixes from function inputs.
				const auto InputNames = UMaterialEditingLibrary::GetMaterialExpressionInputNames(E);
				if (InputNames.IsValidIndex(Pin->SourceIndex)) PinName = InputNames[Pin->SourceIndex];
			}
			Row += FString::Printf(TEXT("|%d:%d:%s"), int32(Pin->Direction), Pin->SourceIndex, *PinName);
			TArray<FString> Links;
			for (auto* Link : Pin->LinkedTo)
				Links.Add(Link->GetOwningNode()->NodeGuid.ToString() + TEXT(":") + FString::FromInt(Link->SourceIndex));
			Links.Sort();
			Row += FString::Join(Links, TEXT(","));
		}
		Rows.Add(Row);
	}
	Rows.Sort();
	return FString::Join(Rows, TEXT("\n"));
}

int32 InputIndex(UMaterialExpression* E, const FString& Name)
{
	if (!E) return INDEX_NONE;
	const TArray<FString> Names = UMaterialEditingLibrary::GetMaterialExpressionInputNames(E);
	for (int32 I = 0; I < Names.Num(); ++I)
		if (Names[I] == Name || (Name.IsEmpty() && I == 0)) return I;
	return INDEX_NONE;
}

UMaterialExpression* Sample(UMaterial* M, const TArray<TSharedPtr<FJsonValue>>& V)
{
	for (const auto& X : V) if (!X || X->Type != EJson::Number || !FMath::IsFinite(X->AsNumber())) return nullptr;
	if (V.Num() == 1)
	{
		auto* E = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionConstant::StaticClass()));
		E->R = V[0]->AsNumber(); return E;
	}
	if (V.Num() == 2)
	{
		auto* E = Cast<UMaterialExpressionConstant2Vector>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionConstant2Vector::StaticClass()));
		E->R = V[0]->AsNumber(); E->G = V[1]->AsNumber(); return E;
	}
	if (V.Num() == 3 || V.Num() == 4)
	{
		FLinearColor C(V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber(), V.Num() == 4 ? V[3]->AsNumber() : 1);
		if (V.Num() == 3)
		{
			auto* E = Cast<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionConstant3Vector::StaticClass()));
			E->Constant = C; return E;
		}
		auto* E = Cast<UMaterialExpressionConstant4Vector>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionConstant4Vector::StaticClass()));
		E->Constant = C; return E;
	}
	return nullptr;
}

bool Compile(UMaterial* M, FString& Error)
{
	UMaterialEditingLibrary::RecompileMaterial(M);
	UMaterialEditingLibrary::GetStatistics(M); // waits for this material's compilation
	FMaterialResource* R = M->GetMaterialResource(GShaderPlatformForFeatureLevel[GMaxRHIFeatureLevel]);
	if (!R) { Error = TEXT("No material resource for the active shader platform"); return false; }
	if (R->GetCompileErrors().Num()) { Error = FString::Join(R->GetCompileErrors(), TEXT("\n")); return false; }
	if (!R->IsGameThreadShaderMapComplete()) { Error = TEXT("Material shader map is incomplete"); return false; }
	return true;
}
}

void FMonolithMaterialClipboard::Register(FMonolithToolRegistry& R)
{
	R.RegisterAction(TEXT("material"), TEXT("copy_material_nodes"),
		TEXT("Prepare an editable material node group in transient graphs, compile sample bindings, round-trip native clipboard text, then copy. Does not modify the destination. See Docs/MaterialClipboard.md."),
		FMonolithActionHandler::CreateStatic(&Copy),
		FParamSchemaBuilder().Required(TEXT("graph_spec"), TEXT("object"), TEXT("Existing nodes/custom_hlsl_nodes/connections format; group-internal connections only."))
		.Required(TEXT("summary"), TEXT("string"), TEXT("One observable purpose of this delivery."))
		.Required(TEXT("external_inputs"), TEXT("array"), TEXT("[{node,pin,name,sample:[1..4 numeric components]}]; sample nodes only exist during validation."))
		.Required(TEXT("outputs"), TEXT("array"), TEXT("[{node,pin,name,property}]; property defaults to EmissiveColor, or Normal/Roughness/WPO/etc for validation."))
		.OptionalAssetPath(TEXT("context_asset_path"), TEXT("Material whose domain/blend/shading model is copied into validation only."))
		.Build());
	R.RegisterAction(TEXT("material"), TEXT("recopy_last_material_nodes"), TEXT("Copy the last successful material node delivery again, for this UE session."),
		FMonolithActionHandler::CreateStatic(&Recopy), FParamSchemaBuilder().Build());
}

void FMonolithMaterialClipboard::Reset() { LastText.Empty(); LastReport.Reset(); }

#if WITH_DEV_AUTOMATION_TESTS
bool FMonolithMaterialClipboard::TestFailedNativeWrite(FString& Error)
{
	return WriteClipboard(TEXT("Monolith simulated failed clipboard write"), Error, true);
}
#endif

FMonolithActionResult FMonolithMaterialClipboard::Copy(const TSharedPtr<FJsonObject>& P)
{
	FString Text;
	FMonolithActionResult R = Prepare(P, Text);
	if (!R.bSuccess) return R;
	FString Error;
	if (!WriteClipboard(Text, Error)) return FMonolithActionResult::Error(Error);
	R.Result->SetBoolField(TEXT("clipboard_written"), true);
	LastText = MoveTemp(Text);
	LastReport = R.Result;
	return R;
}

FMonolithActionResult FMonolithMaterialClipboard::Recopy(const TSharedPtr<FJsonObject>&)
{
	if (LastText.IsEmpty() || !LastReport) return FMonolithActionResult::Error(TEXT("No successful node delivery in this UE session."));
	FString Error;
	if (!IsInGameThread()) return FMonolithActionResult::Error(TEXT("Clipboard delivery requires the editor game thread"));
	if (!WriteClipboard(LastText, Error)) return FMonolithActionResult::Error(Error);
	return FMonolithActionResult::Success(LastReport);
}

FMonolithActionResult FMonolithMaterialClipboard::Prepare(const TSharedPtr<FJsonObject>& P, FString& OutText)
{
	OutText.Empty();
	if (!IsInGameThread()) return FMonolithActionResult::Error(TEXT("Material clipboard preparation requires the editor game thread."));
	const TSharedPtr<FJsonObject>* SpecPtr;
	if (!P || !P->TryGetObjectField(TEXT("graph_spec"), SpecPtr)) return FMonolithActionResult::Error(TEXT("graph_spec must be an object"));
	const auto& Spec = *SpecPtr;
	for (const TCHAR* Key : { TEXT("nodes"), TEXT("custom_hlsl_nodes"), TEXT("connections"), TEXT("named_reroutes") })
	{
		const TArray<TSharedPtr<FJsonValue>>* A;
		if (Spec->HasField(Key) && !Spec->TryGetArrayField(Key, A)) return FMonolithActionResult::Error(FString(Key) + TEXT(" must be an array"));
	}
	const TArray<TSharedPtr<FJsonValue>>* ExternalArray;
	if (!P->TryGetArrayField(TEXT("external_inputs"), ExternalArray)) return FMonolithActionResult::Error(TEXT("external_inputs must be an array (empty is allowed)"));
	if (Str(P, TEXT("summary")).IsEmpty()) return FMonolithActionResult::Error(TEXT("summary is required"));
	auto Outputs = Array(P, TEXT("outputs"));
	if (Outputs.IsEmpty()) return FMonolithActionResult::Error(TEXT("Declare at least one output for compilation validation"));
	if (Spec->HasField(TEXT("outputs"))) return FMonolithActionResult::Error(TEXT("Use delivery outputs, not graph_spec.outputs (material root is not copied)."));
	TSet<FString> Ids;
	for (const TCHAR* Key : { TEXT("nodes"), TEXT("custom_hlsl_nodes") })
	{
		for (const auto& V : Array(Spec, Key))
		{
			const TSharedPtr<FJsonObject>* O;
			if (!V->TryGetObject(O)) return FMonolithActionResult::Error(TEXT("Node must be an object"));
			FString Id = Str(*O, TEXT("id"));
			if (Id.IsEmpty() || Ids.Contains(Id)) return FMonolithActionResult::Error(TEXT("Node ids must be unique and nonempty: ") + Id);
			Ids.Add(Id);
		}
	}
	if (Ids.Num() == 0 || Ids.Num() > 128) return FMonolithActionResult::Error(TEXT("A delivery must contain 1..128 nodes"));
	for (const auto& V : Array(Spec, TEXT("connections")))
	{
		const TSharedPtr<FJsonObject>* O;
		if (!V->TryGetObject(O) || (*O)->HasField(TEXT("to_property"))) return FMonolithActionResult::Error(TEXT("Only group-internal expression connections are supported"));
	}
	auto Scratch = NewScratch();
	UMaterial* M = Scratch.Get();
	const FString ContextPath = Str(P, TEXT("context_asset_path"));
	if (!ContextPath.IsEmpty())
	{
		UMaterial* Context = LoadObject<UMaterial>(nullptr, *ContextPath);
		if (!Context) return FMonolithActionResult::Error(TEXT("Context is not a base material: ") + ContextPath);
		M->MaterialDomain = Context->MaterialDomain;
		M->BlendMode = Context->BlendMode;
		M->SetShadingModel(Context->GetShadingModels().GetFirstShadingModel());
		M->TwoSided = Context->TwoSided;
	}
	TMap<FString, UMaterialExpression*> ById;
	TArray<TSharedPtr<FJsonValue>> Errors;
	int32 NodeCount = 0, ConnectionCount = 0;
	auto NodeSpec = MakeShared<FJsonObject>(*Spec);
	NodeSpec->RemoveField(TEXT("connections"));
	auto Create = [M](UClass* C, int32 X, int32 Y)
	{
		if (!C->IsChildOf(UMaterialExpression::StaticClass()) || C->HasAnyClassFlags(CLASS_Abstract)) return (UMaterialExpression*)nullptr;
		if (C->IsChildOf(UMaterialExpressionComment::StaticClass()))
		{
			auto* Comment = NewObject<UMaterialExpressionComment>(M, C, NAME_None, RF_Transient);
			Comment->Material = M;
			Comment->MaterialExpressionEditorX = X; Comment->MaterialExpressionEditorY = Y;
			M->GetExpressionCollection().AddComment(Comment);
			return static_cast<UMaterialExpression*>(Comment);
		}
		return UMaterialEditingLibrary::CreateMaterialExpression(M, C, X, Y);
	};
	FMonolithMaterialActions::BuildGraphFromSpec(NodeSpec, Create, ById, NodeCount, ConnectionCount, Errors);
	// Resolve local object references after all declarations exist. Names are display
	// labels only; the spec IDs and fresh declaration GUIDs define binding identity.
	TSet<UMaterialExpressionNamedRerouteUsage*> BoundUsages;
	for (const auto& V : Array(Spec, TEXT("named_reroutes")))
	{
		const TSharedPtr<FJsonObject>* Binding;
		if (!V->TryGetObject(Binding)) return FMonolithActionResult::Error(TEXT("named_reroutes entries must be objects"));
		auto* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(ById.FindRef(Str(*Binding, TEXT("usage"))));
		auto* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(ById.FindRef(Str(*Binding, TEXT("declaration"))));
		if (!Usage || !Declaration || BoundUsages.Contains(Usage))
			return FMonolithActionResult::Error(TEXT("Invalid or duplicate named_reroutes binding; use local usage/declaration node IDs"));
		if (!Declaration->VariableGuid.IsValid()) Declaration->VariableGuid = FGuid::NewGuid();
		Usage->Declaration = Declaration;
		Usage->DeclarationGuid = Declaration->VariableGuid;
		BoundUsages.Add(Usage);
	}
	for (const auto& Pair : ById)
		if (auto* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Pair.Value))
			if (!BoundUsages.Contains(Usage)) return FMonolithActionResult::Error(TEXT("Named reroute usage requires a local named_reroutes binding: ") + Pair.Key);
	for (const auto& Pair : ById)
		if (auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Pair.Value)) Call->UpdateFromFunctionResource(false);
	TSet<FExpressionInput*> WiredInputs;
	for (const auto& V : Array(Spec, TEXT("connections")))
	{
		auto O = V->AsObject();
		const FString Target = O->HasField(TEXT("to")) ? Str(O, TEXT("to")) : Str(O, TEXT("to_expression"));
		FString Pin = O->HasField(TEXT("to_pin")) ? Str(O, TEXT("to_pin")) : Str(O, TEXT("to_input"));
		UMaterialExpression* E = ById.FindRef(Target);
		int32 I = InputIndex(E, Pin);
		if (I != INDEX_NONE)
		{
			FExpressionInput* Input = E->GetInput(I);
			if (WiredInputs.Contains(Input)) return FMonolithActionResult::Error(TEXT("Duplicate internal input connection: ") + Target + TEXT(".") + Pin);
			WiredInputs.Add(Input);
		}
	}
	auto WireSpec = MakeShared<FJsonObject>();
	WireSpec->SetArrayField(TEXT("connections"), Array(Spec, TEXT("connections")));
	int32 UnusedNodeCount = 0;
	FMonolithMaterialActions::BuildGraphFromSpec(WireSpec, Create, ById, UnusedNodeCount, ConnectionCount, Errors);
	if (Errors.Num() || NodeCount != Ids.Num())
	{
		auto Data = MakeShared<FJsonObject>(); Data->SetArrayField(TEXT("diagnostics"), Errors);
		return FMonolithActionResult::Error(TEXT("Node build failed; clipboard unchanged")).WithErrorData(Data);
	}
	TArray<TSharedPtr<FJsonValue>> Dependencies;
	TSet<FString> DependencyPaths;
	for (const auto& Id : Ids)
	{
		UMaterialExpression* E = ById.FindRef(Id);
		if (E->GetClass()->GetName().Contains(TEXT("FunctionInput")) || E->GetClass()->GetName().Contains(TEXT("FunctionOutput")) || E->GetClass()->GetName().Contains(TEXT("Composite")))
			return FMonolithActionResult::Error(TEXT("V1 supports flat ordinary material graphs: ") + Id);
		for (TFieldIterator<FObjectProperty> It(E->GetClass()); It; ++It)
			if (It->HasAnyPropertyFlags(CPF_Edit))
				if (UObject* O = It->GetObjectPropertyValue_InContainer(E))
					if (O->IsAsset()) DependencyPaths.Add(O->GetPathName());
	}
	for (const auto& Path : DependencyPaths) Dependencies.Add(MakeShared<FJsonValueString>(Path));
	// Every computation must contribute to a declared validation output. Otherwise
	// an unreachable Custom node could hide a shader error from the compiler.
	TSet<UMaterialExpression*> Reachable, Visiting;
	TFunction<bool(UMaterialExpression*)> Visit = [&](UMaterialExpression* E)
	{
		if (!E || Reachable.Contains(E)) return true;
		if (Visiting.Contains(E)) return false;
		Visiting.Add(E);
		if (auto* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(E))
			if (!Visit(Usage->Declaration)) return false;
		for (int32 I = 0; I < UMaterialEditingLibrary::GetMaterialExpressionInputNames(E).Num(); ++I)
			if (auto* In = E->GetInput(I)) if (!Visit(In->Expression)) return false;
		Visiting.Remove(E); Reachable.Add(E); return true;
	};
	for (const auto& V : Outputs)
	{
		const TSharedPtr<FJsonObject>* O;
		if (!V->TryGetObject(O)) return FMonolithActionResult::Error(TEXT("outputs entries must be objects"));
		if (!Visit(ById.FindRef(Str(*O, TEXT("node"))))) return FMonolithActionResult::Error(TEXT("Internal connections contain a cycle"));
	}
	for (const auto& Id : Ids)
		if (!Reachable.Contains(ById[Id]) && !Cast<UMaterialExpressionComment>(ById[Id]))
			return FMonolithActionResult::Error(TEXT("Node is not reachable from any declared output: ") + Id);
	struct FBinding { FExpressionInput* Input; UMaterialExpression* SampleExpression; };
	TArray<FBinding> Bindings;
	TSet<FExpressionInput*> Bound;
	for (const auto& V : Array(P, TEXT("external_inputs")))
	{
		const TSharedPtr<FJsonObject>* O;
		if (!V->TryGetObject(O)) return FMonolithActionResult::Error(TEXT("external_inputs entries must be objects"));
		if (Str(*O, TEXT("name")).IsEmpty()) return FMonolithActionResult::Error(TEXT("External input name is required"));
		UMaterialExpression* E = ById.FindRef(Str(*O, TEXT("node")));
		const FString Pin = Str(*O, TEXT("pin"));
		int32 Index = InputIndex(E, Pin);
		if (Index == INDEX_NONE) return FMonolithActionResult::Error(TEXT("Unknown external input: ") + Str(*O, TEXT("node")) + TEXT(".") + Pin);
		FExpressionInput* Input = E->GetInput(Index);
		if (!Input || Input->Expression || Bound.Contains(Input)) return FMonolithActionResult::Error(TEXT("External input already connected or duplicated: ") + Pin);
		UMaterialExpression* S = Sample(M, Array(*O, TEXT("sample")));
		if (!S) return FMonolithActionResult::Error(TEXT("External input sample must contain 1..4 finite numbers: ") + Pin);
		Input->Connect(0, S); Bound.Add(Input); Bindings.Add({Input, S});
		E->Desc += TEXT("\nInput ") + Str(*O, TEXT("name")) + TEXT(": ") + Pin;
	}
	for (const auto& V : Outputs)
	{
		const TSharedPtr<FJsonObject>* O;
		if (!V->TryGetObject(O)) return FMonolithActionResult::Error(TEXT("outputs entries must be objects"));
		if (Str(*O, TEXT("name")).IsEmpty()) return FMonolithActionResult::Error(TEXT("Output name is required"));
		UMaterialExpression* E = ById.FindRef(Str(*O, TEXT("node")));
		FString Property = Str(*O, TEXT("property"));
		if (Property.IsEmpty()) Property = TEXT("EmissiveColor");
		EMaterialProperty MP = MP_MAX;
		if (Property == TEXT("EmissiveColor")) MP = MP_EmissiveColor;
		else if (Property == TEXT("BaseColor")) MP = MP_BaseColor;
		else if (Property == TEXT("Normal")) MP = MP_Normal;
		else if (Property == TEXT("Roughness")) MP = MP_Roughness;
		else if (Property == TEXT("WorldPositionOffset")) MP = MP_WorldPositionOffset;
		if (!E || MP == MP_MAX)
			return FMonolithActionResult::Error(TEXT("Invalid output endpoint/property: ") + Str(*O, TEXT("node")));
		// A Float2 UV output cannot connect directly to Float3 Emissive. This
		// validation-only adapter consumes every numeric component (Float1..4).
		UMaterialExpressionCustom* Adapter = nullptr;
		UMaterialExpression* RootSource = E;
		FString RootPin = Str(*O, TEXT("pin"));
		if (MP == MP_EmissiveColor)
		{
			Adapter = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(M, UMaterialExpressionCustom::StaticClass()));
			Adapter->Inputs.Empty();
			FCustomInput In; In.InputName = TEXT("Value"); Adapter->Inputs.Add(In);
			Adapter->Code = TEXT("return float3(dot(Value, Value), 0, 0);");
			Adapter->OutputType = CMOT_Float3;
			if (!UMaterialEditingLibrary::ConnectMaterialExpressions(E, RootPin, Adapter, TEXT("Value")))
				return FMonolithActionResult::Error(TEXT("Unknown output pin: ") + Str(*O, TEXT("node")) + TEXT(".") + RootPin);
			RootSource = Adapter; RootPin.Empty();
		}
		if (!UMaterialEditingLibrary::ConnectMaterialProperty(RootSource, RootPin, MP))
			return FMonolithActionResult::Error(TEXT("Invalid output pin: ") + Str(*O, TEXT("node")));
		FString CompileError;
		if (!Compile(M, CompileError)) return FMonolithActionResult::Error(TEXT("Output ") + Str(*O, TEXT("name")) + TEXT(": ") + CompileError);
		*M->GetExpressionInputForProperty(MP) = FExpressionInput();
		if (Adapter) UMaterialEditingLibrary::DeleteMaterialExpression(M, Adapter);
		E->Desc += TEXT("\nOutput: ") + Str(*O, TEXT("name"));
	}
	for (auto& B : Bindings) { *B.Input = FExpressionInput(); }
	for (auto& B : Bindings) UMaterialEditingLibrary::DeleteMaterialExpression(M, B.SampleExpression);
	M->MaterialGraph->RebuildGraph();
	TSet<UObject*> Selection;
	TSet<UEdGraphNode*> OriginalNodes;
	for (UEdGraphNode* N : M->MaterialGraph->Nodes)
		if (ExpressionOf(N)) { Selection.Add(N); OriginalNodes.Add(N); }
	if (Selection.Num() != NodeCount) return FMonolithActionResult::Error(TEXT("Graph node count differs from expression count"));
	const FString Before = Fingerprint(OriginalNodes);
	for (auto* N : OriginalNodes) N->PrepareForCopying();
	FString Text;
	FEdGraphUtilities::ExportNodesToText(Selection, Text);
	for (auto* N : OriginalNodes)
	{
		if (auto* E = Cast<UMaterialGraphNode>(N)) E->PostCopyNode();
		else if (auto* C = Cast<UMaterialGraphNode_Comment>(N)) C->PostCopyNode();
	}
	if (Text.IsEmpty()) return FMonolithActionResult::Error(TEXT("Native node export was empty"));
	auto Imported = NewScratch();
	TSet<UEdGraphNode*> Pasted;
	FEdGraphUtilities::ImportNodesFromText(Imported->MaterialGraph, Text, Pasted);
	// Native material paste registers the expressions and then synchronizes graph
	// links. ImportNodesFromText alone only restores the graph pin connections.
	for (UEdGraphNode* N : Pasted)
	{
		if (UMaterialExpression* E = ExpressionOf(N))
		{
			E->Material = Imported.Get();
			if (auto* C = Cast<UMaterialExpressionComment>(E)) Imported->GetExpressionCollection().AddComment(C);
			else Imported->GetExpressionCollection().AddExpression(E);
		}
	}
	Imported->MaterialGraph->LinkMaterialExpressionsFromGraph();
	TArray<UMaterialExpression*> PastedExpressions;
	for (auto* N : Pasted) if (auto* E = ExpressionOf(N)) PastedExpressions.Add(E);
	// Match material-editor paste: renew variable identities and resolve all usages
	// within the pasted expression set, without falling back to another graph.
	for (auto* E : PastedExpressions) E->PostCopyNode(PastedExpressions);
	TSet<FGuid> DeclarationGuids;
	for (auto* E : PastedExpressions)
		if (auto* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(E))
		{
			if (!Declaration->VariableGuid.IsValid() || DeclarationGuids.Contains(Declaration->VariableGuid))
				return FMonolithActionResult::Error(TEXT("Native roundtrip produced duplicate or invalid named declaration identities"));
			DeclarationGuids.Add(Declaration->VariableGuid);
		}
	for (auto* E : PastedExpressions)
		if (auto* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(E))
			if (!Usage->Declaration || !PastedExpressions.Contains(Usage->Declaration.Get()) ||
				Usage->Declaration->Material != Imported.Get() || Usage->DeclarationGuid != Usage->Declaration->VariableGuid)
				return FMonolithActionResult::Error(TEXT("Native roundtrip named reference escaped the pasted graph or has a mismatched GUID"));
	const FString After = Fingerprint(Pasted);
	if (Pasted.Num() != OriginalNodes.Num() || After != Before)
	{
		int32 At = 0;
		while (At < Before.Len() && At < After.Len() && Before[At] == After[At]) ++At;
		return FMonolithActionResult::Error(FString::Printf(TEXT("Native roundtrip mismatch at %d (nodes %d/%d). Before: %s | After: %s"), At,
			OriginalNodes.Num(), Pasted.Num(), *Before.Mid(FMath::Max(0, At - 60), 400), *After.Mid(FMath::Max(0, At - 60), 400)));
	}
	auto Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("summary"), Str(P, TEXT("summary")));
	Report->SetNumberField(TEXT("node_count"), NodeCount);
	Report->SetArrayField(TEXT("dependencies"), Dependencies);
	Report->SetArrayField(TEXT("external_inputs"), Array(P, TEXT("external_inputs")));
	Report->SetArrayField(TEXT("outputs"), Outputs);
	Report->SetBoolField(TEXT("sample_compile_passed"), true);
	Report->SetBoolField(TEXT("native_roundtrip_passed"), true);
	Report->SetNumberField(TEXT("named_reroute_binding_count"), BoundUsages.Num());
	Report->SetBoolField(TEXT("named_reroute_ownership_passed"), true);
	Report->SetBoolField(TEXT("clipboard_written"), false);
	Report->SetBoolField(TEXT("destination_modified"), false);
	Report->SetBoolField(TEXT("pasted_visual_result_verified"), false);
	OutText = MoveTemp(Text);
	return FMonolithActionResult::Success(Report);
}
