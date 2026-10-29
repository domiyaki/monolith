// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"

// One action invocation only: repeated FTS chunks share file resolution and I/O.
// FIFO bounds retained memory; oversized files may occupy one slot by themselves.
// References returned by Get are valid only until the next Get call.
class FMonolithSourceRequestFileCache
{
public:
	struct FFile
	{
		int64 Id = 0;
		FString Path;
		TArray<FString> Lines;
		bool bLoaded = false;
		SIZE_T Bytes = 0;
	};

	using FResolve = TFunction<FString(int64)>;
	using FLoad = TFunction<bool(const FString&, TArray<FString>&)>;

	FMonolithSourceRequestFileCache(FResolve InResolve, FLoad InLoad,
		int32 InMaxFiles = 8, SIZE_T InMaxBytes = 8 * 1024 * 1024)
		: Resolve(MoveTemp(InResolve)), Load(MoveTemp(InLoad)),
		  MaxFiles(FMath::Max(1, InMaxFiles)), MaxBytes(InMaxBytes) {}

	const FFile& Get(int64 Id)
	{
		if (const FFile* Existing = Files.FindByPredicate([Id](const FFile& File) { return File.Id == Id; }))
		{
			return *Existing;
		}
		FFile File;
		File.Id = Id;
		File.Path = Resolve(Id);
		File.bLoaded = Load(File.Path, File.Lines);
		if (!File.bLoaded) File.Lines.Reset();
		File.Bytes = File.Path.GetAllocatedSize() + File.Lines.GetAllocatedSize();
		for (const FString& Line : File.Lines) File.Bytes += Line.GetAllocatedSize();
		while (Files.Num() > 0 && (Files.Num() >= MaxFiles || File.Bytes > MaxBytes || RetainedBytes > MaxBytes - File.Bytes))
		{
			RetainedBytes -= Files[0].Bytes;
			Files.RemoveAt(0);
		}
		RetainedBytes += File.Bytes;
		Files.Add(MoveTemp(File));
		return Files.Last();
	}

private:
	FResolve Resolve;
	FLoad Load;
	int32 MaxFiles;
	SIZE_T MaxBytes;
	SIZE_T RetainedBytes = 0;
	TArray<FFile> Files;
};
