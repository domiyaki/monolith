#pragma once

#include "MonolithToolRegistry.h"

/** Keeps the asset and the actual open toolkit in agreement around material writes. */
namespace MonolithMaterialEditorSync
{
void RegisterAction(FMonolithToolRegistry& Registry, const FString& Namespace,
    const FString& Action, const FString& Description,
    const FMonolithActionHandler& Handler, const TSharedPtr<FJsonObject>& Schema);
}
