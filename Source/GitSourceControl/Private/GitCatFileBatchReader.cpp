// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitCatFileBatchReader.h"

#include "GitSourceControlUtils.h"

#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Misc/Timespan.h"

namespace GitCatFileBatchReaderPrivate
{
bool IsCancellationRequested(const TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& InCancellationContext)
{
	return InCancellationContext.IsValid() && InCancellationContext->IsCancellationRequested();
}

bool CancelProcessIfRequested(FProcHandle& InProcess,
	const TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& InCancellationContext, FString& OutError)
{
	if (!IsCancellationRequested(InCancellationContext))
	{
		return false;
	}
	if (FPlatformProcess::IsProcRunning(InProcess))
	{
		FPlatformProcess::TerminateProc(InProcess, true);
	}
	OutError = TEXT("Git cat-file metadata read was cancelled.");
	return true;
}

bool WriteCommand(void* InStandardInputWrite, const FString& InCommand,
	const TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& InCancellationContext, FString& OutError)
{
	if (IsCancellationRequested(InCancellationContext))
	{
		OutError = TEXT("Git cat-file metadata read was cancelled.");
		return false;
	}
	int32 NulIndex = INDEX_NONE;
	if (InCommand.FindChar(TEXT('\0'), NulIndex))
	{
		OutError = TEXT("Git cat-file object specification contains a NUL character.");
		return false;
	}

	FTCHARToUTF8 CommandUtf8(*InCommand);
	TArray<uint8> Bytes;
	Bytes.Reserve(CommandUtf8.Length() + 1);
	Bytes.Append(reinterpret_cast<const uint8*>(CommandUtf8.Get()), CommandUtf8.Length());
	Bytes.Add(0);

	int32 BytesWritten = 0;
	if (!FPlatformProcess::WritePipe(InStandardInputWrite, Bytes.GetData(), Bytes.Num(), &BytesWritten) || BytesWritten != Bytes.Num())
	{
		OutError = TEXT("Failed to send a command to git cat-file.");
		return false;
	}
	return true;
}

void DrainPipe(void* InPipe, TArray<uint8>& InOutBytes)
{
	TArray<uint8> Chunk;
	while (FPlatformProcess::ReadPipeToArray(InPipe, Chunk))
	{
		if (Chunk.IsEmpty())
		{
			break;
		}
		InOutBytes.Append(Chunk);
		Chunk.Reset();
	}
}

FString ByteArrayToString(const uint8* InData, const int32 InNumBytes)
{
	FString Result;
	if (InNumBytes > 0)
	{
		FFileHelper::BufferToString(Result, InData, InNumBytes);
	}
	return Result;
}

bool ReadUntil(void* InStandardOutputRead, void* InStandardErrorRead, FProcHandle& InProcess,
	TArray<uint8>& InOutStandardOutput, TArray<uint8>& InOutStandardError, const uint8 InDelimiter,
	const FDateTime& InDeadline, const TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& InCancellationContext,
	FString& OutError)
{
	while (!InOutStandardOutput.Contains(InDelimiter))
	{
		if (CancelProcessIfRequested(InProcess, InCancellationContext, OutError))
		{
			return false;
		}
		DrainPipe(InStandardOutputRead, InOutStandardOutput);
		DrainPipe(InStandardErrorRead, InOutStandardError);
		if (InOutStandardOutput.Contains(InDelimiter))
		{
			break;
		}
		if (!FPlatformProcess::IsProcRunning(InProcess))
		{
			DrainPipe(InStandardOutputRead, InOutStandardOutput);
			DrainPipe(InStandardErrorRead, InOutStandardError);
			OutError = FString::Printf(TEXT("git cat-file ended before a complete response: %s"), *ByteArrayToString(InOutStandardError.GetData(), InOutStandardError.Num()));
			return false;
		}
		if (FDateTime::UtcNow() >= InDeadline)
		{
			OutError = TEXT("Timed out while reading git cat-file output.");
			return false;
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}
	return true;
}

bool ReadBytes(void* InStandardOutputRead, void* InStandardErrorRead, FProcHandle& InProcess,
	TArray<uint8>& InOutStandardOutput, TArray<uint8>& InOutStandardError, const int32 InRequiredBytes,
	const FDateTime& InDeadline, const TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& InCancellationContext,
	FString& OutError)
{
	while (InOutStandardOutput.Num() < InRequiredBytes)
	{
		if (CancelProcessIfRequested(InProcess, InCancellationContext, OutError))
		{
			return false;
		}
		DrainPipe(InStandardOutputRead, InOutStandardOutput);
		DrainPipe(InStandardErrorRead, InOutStandardError);
		if (InOutStandardOutput.Num() >= InRequiredBytes)
		{
			break;
		}
		if (!FPlatformProcess::IsProcRunning(InProcess))
		{
			DrainPipe(InStandardOutputRead, InOutStandardOutput);
			DrainPipe(InStandardErrorRead, InOutStandardError);
			OutError = FString::Printf(TEXT("git cat-file ended during a blob response: %s"), *ByteArrayToString(InOutStandardError.GetData(), InOutStandardError.Num()));
			return false;
		}
		if (FDateTime::UtcNow() >= InDeadline)
		{
			OutError = TEXT("Timed out while reading a git cat-file blob.");
			return false;
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}
	return true;
}

void ConsumePrefix(TArray<uint8>& InOutBytes, const int32 InNumBytes)
{
	check(InNumBytes >= 0 && InNumBytes <= InOutBytes.Num());
	if (InNumBytes == InOutBytes.Num())
	{
		InOutBytes.Reset();
	}
	else if (InNumBytes > 0)
	{
		InOutBytes.RemoveAt(0, InNumBytes, EAllowShrinking::No);
	}
}

bool ConsumeHeader(TArray<uint8>& InOutBytes, const uint8 InDelimiter, FString& OutHeader)
{
	const int32 HeaderEnd = InOutBytes.IndexOfByKey(InDelimiter);
	if (HeaderEnd == INDEX_NONE)
	{
		return false;
	}
	OutHeader = ByteArrayToString(InOutBytes.GetData(), HeaderEnd);
	ConsumePrefix(InOutBytes, HeaderEnd + 1);
	return true;
}

bool ParseObjectHeader(const FString& InHeader, FString& OutObjectId, int64& OutSize, FString& OutError)
{
	OutObjectId.Reset();
	OutSize = 0;
	TArray<FString> Parts;
	InHeader.ParseIntoArrayWS(Parts);
	if (InHeader.EndsWith(TEXT(" missing"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	if (Parts.Num() != 3 || Parts[1] != TEXT("blob") || !LexTryParseString(OutSize, *Parts[2]) || OutSize < 0)
	{
		OutError = FString::Printf(TEXT("Unexpected git cat-file response: %s"), *InHeader);
		return false;
	}
	OutObjectId = MoveTemp(Parts[0]);
	return true;
}
}

bool FGitCatFileBatchReader::ReadBlobs(const FString& InGitBinary, const FString& InRepositoryRoot,
	const TArray<FGitCatFileBatchRequest>& InRequests, TArray<FGitCatFileBatchResult>& OutResults,
	FString& OutError, const int64 InMaxBlobBytes, const int64 InMaxTotalBlobBytes, const double InTimeoutSeconds,
	TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> InCancellationContext)
{
	using namespace GitCatFileBatchReaderPrivate;

	OutResults.Reset();
	OutError.Reset();
	if (InGitBinary.IsEmpty() || InRepositoryRoot.IsEmpty() || InMaxBlobBytes < 0 || InMaxTotalBlobBytes < 0 || InTimeoutSeconds <= 0.0)
	{
		OutError = TEXT("Invalid git cat-file batch reader arguments.");
		return false;
	}
	if (InRequests.IsEmpty())
	{
		return true;
	}

	void* StandardOutputRead = nullptr;
	void* StandardOutputWrite = nullptr;
	void* StandardErrorRead = nullptr;
	void* StandardErrorWrite = nullptr;
	void* StandardInputRead = nullptr;
	void* StandardInputWrite = nullptr;
	if (!FPlatformProcess::CreatePipe(StandardOutputRead, StandardOutputWrite) ||
		!FPlatformProcess::CreatePipe(StandardErrorRead, StandardErrorWrite) ||
		!FPlatformProcess::CreatePipe(StandardInputRead, StandardInputWrite, true))
	{
		if (StandardOutputRead || StandardOutputWrite)
		{
			FPlatformProcess::ClosePipe(StandardOutputRead, StandardOutputWrite);
		}
		if (StandardErrorRead || StandardErrorWrite)
		{
			FPlatformProcess::ClosePipe(StandardErrorRead, StandardErrorWrite);
		}
		if (StandardInputRead || StandardInputWrite)
		{
			FPlatformProcess::ClosePipe(StandardInputRead, StandardInputWrite);
		}
		OutError = TEXT("Failed to create pipes for git cat-file.");
		return false;
	}

	FProcHandle Process = FPlatformProcess::CreateProc(*InGitBinary, TEXT("cat-file --batch-command -Z"), false, true, true,
		nullptr, 0, *InRepositoryRoot, StandardOutputWrite, StandardInputRead, StandardErrorWrite);
	if (!Process.IsValid())
	{
		FPlatformProcess::ClosePipe(StandardOutputRead, StandardOutputWrite);
		FPlatformProcess::ClosePipe(StandardErrorRead, StandardErrorWrite);
		FPlatformProcess::ClosePipe(StandardInputRead, StandardInputWrite);
		OutError = TEXT("Failed to launch git cat-file.");
		return false;
	}

	bool bSuccess = false;
	ON_SCOPE_EXIT
	{
		if (FPlatformProcess::IsProcRunning(Process))
		{
			FPlatformProcess::TerminateProc(Process, true);
		}
		FPlatformProcess::CloseProc(Process);
		FPlatformProcess::ClosePipe(StandardOutputRead, StandardOutputWrite);
		FPlatformProcess::ClosePipe(StandardErrorRead, StandardErrorWrite);
		if (StandardInputRead || StandardInputWrite)
		{
			FPlatformProcess::ClosePipe(StandardInputRead, StandardInputWrite);
		}
	};

	const FDateTime Deadline = FDateTime::UtcNow() + FTimespan::FromSeconds(InTimeoutSeconds);
	TArray<uint8> StandardOutput;
	TArray<uint8> StandardError;
	int64 TotalBlobBytes = 0;
	OutResults.Reserve(InRequests.Num());
	for (const FGitCatFileBatchRequest& Request : InRequests)
	{
		if (CancelProcessIfRequested(Process, InCancellationContext, OutError))
		{
			return false;
		}
		FGitCatFileBatchResult& Result = OutResults.AddDefaulted_GetRef();
		Result.ObjectSpec = Request.ObjectSpec;
		if (!WriteCommand(StandardInputWrite, FString::Printf(TEXT("info %s"), *Request.ObjectSpec), InCancellationContext, OutError) ||
			!ReadUntil(StandardOutputRead, StandardErrorRead, Process, StandardOutput, StandardError, 0, Deadline, InCancellationContext, OutError))
		{
			return false;
		}

		FString InfoHeader;
		check(ConsumeHeader(StandardOutput, 0, InfoHeader));
		FString HeaderError;
		if (!ParseObjectHeader(InfoHeader, Result.ObjectId, Result.BlobSize, HeaderError))
		{
			Result.Error = HeaderError.IsEmpty() ? TEXT("The Git object does not exist.") : MoveTemp(HeaderError);
			continue;
		}
		Result.bFound = true;
		if (Result.BlobSize > InMaxBlobBytes)
		{
			Result.bSkippedBySizeLimit = true;
			Result.Error = FString::Printf(TEXT("Blob is larger than the %lld byte metadata limit."), InMaxBlobBytes);
			continue;
		}
		if (Result.BlobSize > InMaxTotalBlobBytes - TotalBlobBytes)
		{
			Result.bSkippedBySizeLimit = true;
			Result.Error = FString::Printf(TEXT("The refresh metadata byte budget (%lld bytes) was exhausted."), InMaxTotalBlobBytes);
			continue;
		}

		if (!WriteCommand(StandardInputWrite, FString::Printf(TEXT("contents %s"), *Request.ObjectSpec), InCancellationContext, OutError) ||
			!ReadUntil(StandardOutputRead, StandardErrorRead, Process, StandardOutput, StandardError, '\n', Deadline, InCancellationContext, OutError))
		{
			return false;
		}

		FString ContentsHeader;
		check(ConsumeHeader(StandardOutput, '\n', ContentsHeader));
		FString ContentsObjectId;
		int64 ContentsSize = 0;
		if (!ParseObjectHeader(ContentsHeader, ContentsObjectId, ContentsSize, HeaderError) || ContentsObjectId != Result.ObjectId || ContentsSize != Result.BlobSize)
		{
			OutError = HeaderError.IsEmpty() ? TEXT("git cat-file content header did not match its info response.") : MoveTemp(HeaderError);
			return false;
		}
		if (ContentsSize > MAX_int32)
		{
			OutError = TEXT("git cat-file returned an unsupported blob size.");
			return false;
		}
		const int32 ContentsSizeInt = static_cast<int32>(ContentsSize);
		const int32 RequiredBytes = ContentsSizeInt + 1;
		if (!ReadBytes(StandardOutputRead, StandardErrorRead, Process, StandardOutput, StandardError, RequiredBytes, Deadline, InCancellationContext, OutError))
		{
			return false;
		}
		Result.Data.Append(StandardOutput.GetData(), ContentsSizeInt);
		TotalBlobBytes += ContentsSize;
		const uint8 Terminator = StandardOutput[ContentsSizeInt];
		if (Terminator != 0 && Terminator != '\n')
		{
			OutError = TEXT("git cat-file returned an invalid blob terminator.");
			return false;
		}
		ConsumePrefix(StandardOutput, RequiredBytes);
	}

	FPlatformProcess::ClosePipe(StandardInputRead, StandardInputWrite);
	StandardInputRead = nullptr;
	StandardInputWrite = nullptr;
	while (FPlatformProcess::IsProcRunning(Process) && FDateTime::UtcNow() < Deadline)
	{
		if (CancelProcessIfRequested(Process, InCancellationContext, OutError))
		{
			return false;
		}
		DrainPipe(StandardOutputRead, StandardOutput);
		DrainPipe(StandardErrorRead, StandardError);
		FPlatformProcess::SleepNoStats(0.001f);
	}
	if (FPlatformProcess::IsProcRunning(Process))
	{
		OutError = TEXT("Timed out while waiting for git cat-file to exit.");
		return false;
	}

	int32 ReturnCode = -1;
	FPlatformProcess::GetProcReturnCode(Process, &ReturnCode);
	DrainPipe(StandardErrorRead, StandardError);
	if (ReturnCode != 0)
	{
		OutError = FString::Printf(TEXT("git cat-file failed: %s"), *ByteArrayToString(StandardError.GetData(), StandardError.Num()));
		return false;
	}
	bSuccess = true;
	return bSuccess;
}
