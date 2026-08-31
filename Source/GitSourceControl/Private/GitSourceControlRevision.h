// Copyright (c) 2014-2023 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "ISourceControlRevision.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Misc/DateTime.h"

/** Private standalone Diff adapter for one immutable Git revision snapshot. */
class FGitSourceControlRevision : public ISourceControlRevision
{
public:
#if ENGINE_MAJOR_VERSION >= 5
	virtual bool Get(FString& InOutFilename, EConcurrency::Type InConcurrency = EConcurrency::Synchronous) const override;
#else
	virtual bool Get(FString& InOutFilename) const override;
#endif

	virtual bool GetAnnotated(TArray<FAnnotationLine>& OutLines) const override;
	virtual bool GetAnnotated(FString& InOutFilename) const override;
	virtual const FString& GetFilename() const override;
	virtual int32 GetRevisionNumber() const override;
	virtual const FString& GetRevision() const override;
	virtual const FString& GetDescription() const override;
	virtual const FString& GetUserName() const override;
	virtual const FString& GetClientSpec() const override;
	virtual const FString& GetAction() const override;
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> GetBranchSource() const override;
	virtual const FDateTime& GetDate() const override;
	virtual int32 GetCheckInIdentifier() const override;
	virtual int32 GetFileSize() const override;

	bool ExportToFile(const FString& InFilename) const;

	FString LocalFilename;
	FString Filename;
	FString CommitId;
	FString ShortCommitId;
	int32 CommitIdNumber = 0;
	int32 RevisionNumber = 0;
	FString FileHash;
	FString Description;
	FString UserName;
	FString Action;
	TSharedPtr<FGitSourceControlRevision, ESPMode::ThreadSafe> BranchSource;
	FDateTime Date;
	int32 FileSize = 0;
	FString GitBinary;
	FString RepositoryRoot;
};

using TGitSourceControlHistory = TArray<TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>>;

namespace GitSourceControlRevision
{
	/** Release an export that was not handed to a successfully opened Diff window. */
	void ReleaseTemporaryExport(const FString& Filename);
	/** 注册一个包含主 package 及 sidecar 的 Diff materialization 精确目录. */
	GITSOURCECONTROL_API void RegisterTemporaryExportDirectory(const FString& Directory);
	GITSOURCECONTROL_API void ReleaseTemporaryExportDirectory(const FString& Directory);
	void CleanupTemporaryExports();
}
