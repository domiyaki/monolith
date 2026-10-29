#pragma once

#include "CoreMinimal.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace MonolithAssetOperationValidation
{
// Compare package boundaries, not string prefixes (/Game/Test != /Game/TestOther).
inline bool IsWithinDeletePrefix(const FString& AssetPath, FString Prefix)
{
    while (Prefix.EndsWith(TEXT("/"))) Prefix.LeftChopInline(1);
    if (Prefix.IsEmpty()) return false;
    const FString Package = FPackageName::ObjectPathToPackageName(AssetPath);
    return Package == Prefix || Package.StartsWith(Prefix + TEXT("/"));
}

inline bool ValidateDeleteTarget(const UObject* Asset, bool bForce, FString& Error)
{
    if (!IsValid(Asset)) { Error = TEXT("Target became invalid; resolve it again."); return false; }
    if (bForce && Asset->IsA<UBlueprint>())
    {
        Error = TEXT("Force deletion of Blueprints is disabled: package unloading/reinstancing can assert. Use normal deletion after resolving references; a refusal is not a reason to retry through Python force-delete.");
        return false;
    }
    if (Asset->GetOutermost()->IsDirty())
    {
        Error = TEXT("Target package has unsaved changes. Save or explicitly resolve them before deleting; delete_assets does not clear dirty flags.");
        return false;
    }
    return true;
}

inline bool TrySaveFilename(const FString& PackageName, const FString& Extension, FString& Filename)
{
    return FPackageName::IsValidLongPackageName(PackageName) &&
        !PackageName.StartsWith(TEXT("/Temp/")) &&
        FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, Extension);
}
}
