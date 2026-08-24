// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlMenu.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "ContentBrowserDelegates.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlAssetOperations.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlRevision.h"
#include "GitSourceControlState.h"
#include "GitSourceControlUtils.h"
#include "HAL/FileManager.h"
#include "ISourceControlModule.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PackageTools.h"
#include "ObjectTools.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"
#include "SourceControlWindows.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/PackageFileSummary.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

#if ENGINE_MAJOR_VERSION >= 5
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "ToolMenuContext.h"
#endif

#define LOCTEXT_NAMESPACE "GitSourceControl"

class FGitSourceControlMenuBridgeState final
{
public:
	FGitSourceControlMenuBridgeState()
		: CompletionEvent(FPlatformProcess::GetSynchEventFromPool(true))
	{
	}

	~FGitSourceControlMenuBridgeState()
	{
		FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);
	}

	void Complete(const bool bInResult)
	{
		FScopeLock Lock(&Mutex);
		if (!bCompleted)
		{
			bResult = bInResult;
			bCompleted = true;
			CompletionEvent->Trigger();
		}
	}

	bool WaitForResult()
	{
		CompletionEvent->Wait();
		FScopeLock Lock(&Mutex);
		return bResult;
	}

private:
	FCriticalSection Mutex;
	FEvent* CompletionEvent = nullptr;
	bool bCompleted = false;
	bool bResult = false;
};

class FGitSourceControlMenuLifetimeState final : public TSharedFromThis<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>
{
public:
	FGitSourceControlMenuLifetimeState()
		: AllTasksCompletedEvent(FPlatformProcess::GetSynchEventFromPool(true))
	{
		AllTasksCompletedEvent->Trigger();
	}

	~FGitSourceControlMenuLifetimeState()
	{
		FPlatformProcess::ReturnSynchEventToPool(AllTasksCompletedEvent);
	}

	bool TryBeginTask()
	{
		FScopeLock Lock(&Mutex);
		if (!bAcceptingCallbacks)
		{
			return false;
		}
		++ActiveTaskCount;
		AllTasksCompletedEvent->Reset();
		return true;
	}

	void EndTask()
	{
		FScopeLock Lock(&Mutex);
		check(ActiveTaskCount > 0);
		if (--ActiveTaskCount == 0)
		{
			AllTasksCompletedEvent->Trigger();
		}
	}

	bool IsAcceptingCallbacks() const
	{
		FScopeLock Lock(&Mutex);
		return bAcceptingCallbacks;
	}

	bool TrackBridge(const TSharedRef<FGitSourceControlMenuBridgeState, ESPMode::ThreadSafe>& Bridge)
	{
		FScopeLock Lock(&Mutex);
		if (!bAcceptingCallbacks)
		{
			Bridge->Complete(false);
			return false;
		}
		PendingBridges.Add(Bridge);
		return true;
	}

	void UntrackBridge(const TSharedRef<FGitSourceControlMenuBridgeState, ESPMode::ThreadSafe>& Bridge)
	{
		FScopeLock Lock(&Mutex);
		PendingBridges.RemoveSingleSwap(Bridge, EAllowShrinking::No);
	}

	void StopAcceptingAndWait()
	{
		TArray<TSharedRef<FGitSourceControlMenuBridgeState, ESPMode::ThreadSafe>> BridgesToCancel;
		{
			FScopeLock Lock(&Mutex);
			bAcceptingCallbacks = false;
			BridgesToCancel = PendingBridges;
		}
		for (const TSharedRef<FGitSourceControlMenuBridgeState, ESPMode::ThreadSafe>& Bridge : BridgesToCancel)
		{
			Bridge->Complete(false);
		}

		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		// Bridge waiters were cancelled above, and every Git process has its own
		// timeout. Never unload this module while a worker can still execute its code.
		AllTasksCompletedEvent->Wait();
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}

private:
	mutable FCriticalSection Mutex;
	FEvent* AllTasksCompletedEvent = nullptr;
	int32 ActiveTaskCount = 0;
	bool bAcceptingCallbacks = true;
	TArray<TSharedRef<FGitSourceControlMenuBridgeState, ESPMode::ThreadSafe>> PendingBridges;
};

class FGitSourceControlMenuTask final
{
public:
	explicit FGitSourceControlMenuTask(const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& InLifetimeState)
		: LifetimeState(InLifetimeState)
	{
	}

	~FGitSourceControlMenuTask()
	{
		LifetimeState->EndTask();
	}

private:
	TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe> LifetimeState;
};

/**
 * Game-thread owned presentation for one asset mutation. The cancellation context
 * itself is thread-safe and deliberately outlives the notification that exposes it.
 */
class FGitSourceControlAssetOperationUiState final : public TSharedFromThis<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe>
{
public:
	explicit FGitSourceControlAssetOperationUiState(const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& InCancellationContext)
		: CancellationContext(InCancellationContext)
	{
	}

	void ShowPreflightNotification()
	{
		check(IsInGameThread());
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}

		FNotificationInfo Info(LOCTEXT("GitAssetPreflightRunning", "Checking the selected local Git files..."));
		Info.bUseThrobber = true;
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("GitAssetPreflightCancel", "Cancel"),
			LOCTEXT("GitAssetPreflightCancelTooltip", "Cancel the read-only Git preflight. No asset files will be changed."),
			FSimpleDelegate::CreateSP(this, &FGitSourceControlAssetOperationUiState::RequestPreflightCancellation)));
		PreflightNotification = FSlateNotificationManager::Get().AddNotification(Info);
		if (PreflightNotification.IsValid())
		{
			PreflightNotification->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}

	void RequestPreflightCancellation()
	{
		check(IsInGameThread());
		if (bPreflightFinished)
		{
			return;
		}

		bCancellationRequested = true;
		CancellationContext->Cancel();
		if (PreflightNotification.IsValid())
		{
			PreflightNotification->SetText(LOCTEXT("GitAssetPreflightCancelling", "Cancelling local Git preflight..."));
		}
	}

	bool WasCancellationRequested() const
	{
		return bCancellationRequested;
	}

	void FinishPreflightNotification()
	{
		check(IsInGameThread());
		bPreflightFinished = true;
		if (PreflightNotification.IsValid())
		{
			PreflightNotification->ExpireAndFadeout();
			PreflightNotification.Reset();
		}
	}

	bool PrepareExecutionModal()
	{
		check(IsInGameThread());
		FinishPreflightNotification();
		if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().CanAddModalWindow())
		{
			return false;
		}

		ExecutionWindow = SNew(SWindow)
			.Title(LOCTEXT("GitAssetMutationProgressTitle", "Applying Local Git Asset Operation"))
			.SizingRule(ESizingRule::Autosized)
			.ClientSize(FVector2D(440.0f, 150.0f))
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.HasCloseButton(false);
		ExecutionWindow->SetContent(
			SNew(SBorder)
			.Padding(24.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("GitAssetMutationProgress", "Applying the local Git operation...\nPlease wait until the asset reload and status refresh finish."))
					.AutoWrapText(380.0f)
					.Justification(ETextJustify::Center)
				]
			]);
		return true;
	}

	void RunExecutionModalLoop()
	{
		check(IsInGameThread());
		if (!ExecutionWindow.IsValid())
		{
			return;
		}

		FSlateApplication& SlateApplication = FSlateApplication::Get();
		ModalTickHandle = SlateApplication.GetOnModalLoopTickEvent().AddSP(this, &FGitSourceControlAssetOperationUiState::PumpGameThreadWork);
		SlateApplication.AddModalWindow(ExecutionWindow.ToSharedRef(), SlateApplication.GetActiveTopLevelWindow(), false);
		if (ModalTickHandle.IsValid())
		{
			SlateApplication.GetOnModalLoopTickEvent().Remove(ModalTickHandle);
			ModalTickHandle.Reset();
		}
		ExecutionWindow.Reset();
	}

	void CloseExecutionModal()
	{
		check(IsInGameThread());
		if (ExecutionWindow.IsValid())
		{
			FSlateApplication::Get().RequestDestroyWindow(ExecutionWindow.ToSharedRef());
		}
	}

private:
	void PumpGameThreadWork(float)
	{
		check(IsInGameThread());
		// AddModalWindow owns a nested Slate loop, not the normal Editor tick. Pump
		// the queued UI bridges and provider completions so worker-side Prepare,
		// Reload and UpdateStatus phases cannot deadlock behind this modal window.
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		if (FModuleManager::Get().IsModuleLoaded(TEXT("GitSourceControl")))
		{
			FGitSourceControlModule::Get().GetProvider().Tick();
		}
	}

	TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext;
	TSharedPtr<SNotificationItem> PreflightNotification;
	TSharedPtr<SWindow> ExecutionWindow;
	FDelegateHandle ModalTickHandle;
	bool bPreflightFinished = false;
	bool bCancellationRequested = false;
};

/** Presentation and cancellation bridge for the read-only history prefetch. */
class FGitSourceControlHistoryLoadUiState final : public TSharedFromThis<FGitSourceControlHistoryLoadUiState, ESPMode::ThreadSafe>
{
public:
	explicit FGitSourceControlHistoryLoadUiState(const FSourceControlOperationRef& InOperation)
		: Operation(InOperation)
	{
	}

	void Show()
	{
		check(IsInGameThread());
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}

		FNotificationInfo Info(LOCTEXT("GitHistoryLoadRunning", "Loading local Git history for the selected asset..."));
		Info.bUseThrobber = true;
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("GitHistoryLoadCancel", "Cancel"),
			LOCTEXT("GitHistoryLoadCancelTooltip", "Cancel loading history. No asset files will be changed."),
			FSimpleDelegate::CreateSP(this, &FGitSourceControlHistoryLoadUiState::RequestCancellation)));
		Notification = FSlateNotificationManager::Get().AddNotification(Info);
		if (Notification.IsValid())
		{
			Notification->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}

	void RequestCancellation()
	{
		check(IsInGameThread());
		if (bFinished || bCancellationRequested)
		{
			return;
		}

		bCancellationRequested = true;
		if (FModuleManager::Get().IsModuleLoaded(TEXT("GitSourceControl")))
		{
			FGitSourceControlModule::Get().GetProvider().CancelOperation(Operation);
		}
		if (Notification.IsValid())
		{
			Notification->SetText(LOCTEXT("GitHistoryLoadCancelling", "Cancelling local Git history load..."));
		}
	}

	bool WasCancellationRequested() const
	{
		return bCancellationRequested;
	}

	void Finish()
	{
		check(IsInGameThread());
		bFinished = true;
		if (Notification.IsValid())
		{
			Notification->ExpireAndFadeout();
			Notification.Reset();
		}
	}

private:
	FSourceControlOperationRef Operation;
	TSharedPtr<SNotificationItem> Notification;
	bool bFinished = false;
	bool bCancellationRequested = false;
};

namespace GitSourceControlMenuPrivate
{

	void AddUniquePath(TArray<FString>& InOutFiles, FString Filename)
	{
		Filename = FPaths::ConvertRelativePathToFull(Filename);
		FPaths::NormalizeFilename(Filename);
		if (!InOutFiles.ContainsByPredicate([&Filename](const FString& Existing)
		{
			return Existing.Equals(Filename, ESearchCase::IgnoreCase);
		}))
		{
			InOutFiles.Add(MoveTemp(Filename));
		}
	}

	TArray<FString> GetAssetFiles(const TArray<FAssetData>& SelectedAssets)
	{
		TArray<FString> Files;
		for (const FAssetData& AssetData : SelectedAssets)
		{
			FString PackageFilename;
			if (!FPackageName::DoesPackageExist(AssetData.PackageName.ToString(), &PackageFilename))
			{
				continue;
			}

			AddUniquePath(Files, PackageFilename);
			for (const TCHAR* Extension : { TEXT("uexp"), TEXT("ubulk"), TEXT("uptnl") })
			{
				const FString SidecarFilename = FPaths::ChangeExtension(PackageFilename, Extension);
				if (IFileManager::Get().FileExists(*SidecarFilename))
				{
					AddUniquePath(Files, SidecarFilename);
				}
			}
		}
		return Files;
	}

	TArray<FString> GetPrimaryPackageFiles(const TArray<FString>& Files)
	{
		TArray<FString> PackageFiles;
		for (const FString& Filename : Files)
		{
			if (Filename.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase) || Filename.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
			{
				PackageFiles.Add(Filename);
			}
		}
		return PackageFiles;
	}

	void ShowFailure(const FText& Text);

	void ShowNotification(const FText& Text, const bool bSuccess)
	{
		FNotificationInfo Info(Text);
		Info.bUseSuccessFailIcons = true;
		Info.ExpireDuration = 6.0f;
		FSlateNotificationManager::Get().AddNotification(Info)->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}

	void ShowFailure(const FText& Text)
	{
		UE_LOG(LogSourceControl, Warning, TEXT("%s"), *Text.ToString());
		ShowNotification(Text, false);
	}

	TArray<UPackage*> GatherLoadedPackages(const TArray<FString>& Files, const bool bIncludeConservativeDirtyWorlds = false)
	{
		TSet<UPackage*> UniquePackages;
		const bool bMayReloadExternalWorld = Files.ContainsByPredicate([](const FString& Filename)
		{
			return Filename.Contains(TEXT("__ExternalActors__"), ESearchCase::IgnoreCase) || Filename.Contains(TEXT("__ExternalObjects__"), ESearchCase::IgnoreCase);
		});
		for (const FString& Filename : Files)
		{
			FString PackageName;
			if (FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName))
			{
				if (UPackage* Package = FindPackage(nullptr, *PackageName))
				{
					if (UObject* Asset = Package->FindAssetInPackage())
					{
						if (Asset->IsPackageExternal() && Asset->GetWorld() && Asset->GetWorld()->GetPackage())
						{
							UniquePackages.Add(Package);
							UniquePackages.Add(Asset->GetWorld()->GetPackage());
							continue;
						}
					}
					UniquePackages.Add(Package);
				}
			}
		}
		if (bMayReloadExternalWorld && bIncludeConservativeDirtyWorlds)
		{
			// A missing external package cannot be resolved to one owning world before
			// mutation. Keep every dirty loaded world in the confirmation closure rather
			// than letting the later Engine reload discard it without a prompt.
			TArray<UPackage*> DirtyWorldPackages;
			FEditorFileUtils::GetDirtyWorldPackages(DirtyWorldPackages);
			for (UPackage* Package : DirtyWorldPackages)
			{
				UniquePackages.Add(Package);
			}
		}
		return UniquePackages.Array();
	}

	bool ConfirmLoadedPackages(const TArray<UPackage*>& LoadedPackages, const FText& OperationDescription, const TArray<FString>& AffectedFiles)
	{
		TArray<FString> SortedFiles = AffectedFiles;
		SortedFiles.Sort();
		const FText FilesDescription = FText::FromString(FString::Join(SortedFiles, TEXT("\n")));
		TArray<UPackage*> DirtyPackages;
		for (UPackage* Package : LoadedPackages)
		{
			if (Package && Package->IsDirty())
			{
				DirtyPackages.Add(Package);
			}
		}

		bool bReloadsCurrentWorld = false;
		if (GEditor)
		{
			if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
			{
				bReloadsCurrentWorld = LoadedPackages.Contains(EditorWorld->GetOutermost());
			}
		}
		if (!DirtyPackages.IsEmpty())
		{
			TArray<FString> DirtyNames;
			for (const UPackage* Package : DirtyPackages)
			{
				DirtyNames.Add(Package->GetName());
			}
			DirtyNames.Sort();
			const FText Message = FText::Format(
				LOCTEXT("DiscardDirtyPackages", "{0}\n\nExact files to change:\n{1}\n\nThe following loaded packages have unsaved Editor changes:\n{2}\n\n{3}No disk changes will be made unless you explicitly select the checkbox below and continue."),
				OperationDescription,
				FilesDescription,
				FText::FromString(FString::Join(DirtyNames, TEXT("\n"))),
				bReloadsCurrentWorld ? LOCTEXT("CurrentWorldReloadWarning", "The current world will be reloaded.\n\n") : FText::GetEmpty());

			if (!FSlateApplication::IsInitialized())
			{
				return false;
			}

			const TSharedRef<bool, ESPMode::ThreadSafe> bConfirmed = MakeShared<bool, ESPMode::ThreadSafe>(false);
			const TSharedRef<bool, ESPMode::ThreadSafe> bDiscardEditorChanges = MakeShared<bool, ESPMode::ThreadSafe>(false);
			const TSharedRef<SWindow> ConfirmationWindow = SNew(SWindow)
				.Title(LOCTEXT("DiscardDirtyPackagesTitle", "Confirm Local Git Asset Operation"))
				.SizingRule(ESizingRule::Autosized)
				.ClientSize(FVector2D(680.0f, 460.0f))
				.SupportsMaximize(false)
				.SupportsMinimize(false);
			ConfirmationWindow->SetContent(
				SNew(SBorder)
				.Padding(16.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SNew(STextBlock)
							.Text(Message)
							.AutoWrapText(620.0f)
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 12.0f, 0.0f, 0.0f)
					[
						SNew(SCheckBox)
						.OnCheckStateChanged_Lambda([bDiscardEditorChanges](const ECheckBoxState NewState)
						{
							*bDiscardEditorChanges = NewState == ECheckBoxState::Checked;
						})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("DiscardUnsavedEditorChangesCheckbox", "同时丢弃未保存的 Editor 改动"))
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 16.0f, 0.0f, 0.0f)
					.HAlign(HAlign_Right)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("CancelDirtyPackageOperation", "Cancel"))
							.OnClicked_Lambda([ConfirmationWindow]()
							{
								FSlateApplication::Get().RequestDestroyWindow(ConfirmationWindow);
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.IsEnabled_Lambda([bDiscardEditorChanges]()
							{
								return *bDiscardEditorChanges;
							})
							.Text(LOCTEXT("ConfirmDirtyPackageOperation", "Continue"))
							.OnClicked_Lambda([ConfirmationWindow, bConfirmed]()
							{
								*bConfirmed = true;
								FSlateApplication::Get().RequestDestroyWindow(ConfirmationWindow);
								return FReply::Handled();
							})
						]
					]
				]);
			FSlateApplication::Get().AddModalWindow(ConfirmationWindow, FSlateApplication::Get().GetActiveTopLevelWindow(), false);
			return *bConfirmed && *bDiscardEditorChanges;
		}
		else
		{
			const FText Message = FText::Format(
				LOCTEXT("ConfirmLocalGitAssetOperation", "{0}\n\nExact files to change:\n{1}\n\n{2}Only the selected files will be changed. Choose No to make no disk changes."),
				OperationDescription,
				FilesDescription,
				bReloadsCurrentWorld ? LOCTEXT("ConfirmCurrentWorldReloadWarning", "The current world will be reloaded.\n\n") : FText::GetEmpty());
			if (FMessageDialog::Open(EAppMsgType::YesNo, Message) != EAppReturnType::Yes)
			{
				return false;
			}
		}

		return true;
	}

	bool PrepareLoadedPackages(const TArray<UPackage*>& LoadedPackages)
	{
		TArray<UObject*> PackagesToReset;
		for (UPackage* Package : LoadedPackages)
		{
			if (!Package)
			{
				continue;
			}
			if (!Package->IsFullyLoaded())
			{
				FlushAsyncLoading();
				Package->FullyLoad();
			}
			PackagesToReset.Add(Package);
		}
		if (!PackagesToReset.IsEmpty())
		{
			ResetLoaders(PackagesToReset);
		}
		return true;
	}

	bool IsLifetimeAcceptingCallbacks(const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& LifetimeState)
	{
		return LifetimeState->IsAcceptingCallbacks();
	}

	bool InvokeOnGameThreadAndWait(const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& LifetimeState, TFunction<bool()>&& Work)
	{
		if (!IsLifetimeAcceptingCallbacks(LifetimeState))
		{
			return false;
		}
		if (IsInGameThread())
		{
			return Work();
		}

		const TSharedRef<FGitSourceControlMenuBridgeState, ESPMode::ThreadSafe> Bridge = MakeShared<FGitSourceControlMenuBridgeState, ESPMode::ThreadSafe>();
		if (!LifetimeState->TrackBridge(Bridge))
		{
			return false;
		}
		AsyncTask(ENamedThreads::GameThread, [LifetimeState, Bridge, Work = MoveTemp(Work)]() mutable
		{
			if (IsLifetimeAcceptingCallbacks(LifetimeState))
			{
				Bridge->Complete(Work());
			}
			else
			{
				Bridge->Complete(false);
			}
			LifetimeState->UntrackBridge(Bridge);
		});
		const bool bResult = Bridge->WaitForResult();
		LifetimeState->UntrackBridge(Bridge);
		return bResult;
	}

	bool ConfirmAssetMutationAndRunExecutionModal(const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& LifetimeState,
		const TSharedRef<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe>& UiState, const FString& Description, const TArray<FString>& AffectedFiles)
	{
		if (!IsLifetimeAcceptingCallbacks(LifetimeState))
		{
			return false;
		}

		const TSharedRef<FGitSourceControlMenuBridgeState, ESPMode::ThreadSafe> Bridge = MakeShared<FGitSourceControlMenuBridgeState, ESPMode::ThreadSafe>();
		if (!LifetimeState->TrackBridge(Bridge))
		{
			return false;
		}

		AsyncTask(ENamedThreads::GameThread, [LifetimeState, UiState, Description, AffectedFiles, Bridge]()
		{
			bool bConfirmed = false;
			if (IsLifetimeAcceptingCallbacks(LifetimeState) && !UiState->WasCancellationRequested())
			{
				UiState->FinishPreflightNotification();
				bConfirmed = ConfirmLoadedPackages(GatherLoadedPackages(AffectedFiles, true), FText::FromString(Description), AffectedFiles);
			}

			if (!bConfirmed || !IsLifetimeAcceptingCallbacks(LifetimeState) || !UiState->PrepareExecutionModal())
			{
				Bridge->Complete(false);
				LifetimeState->UntrackBridge(Bridge);
				return;
			}

			// Signal the worker before entering the nested modal loop. The loop pumps
			// task-graph callbacks, allowing it to request package preparation/reload.
			Bridge->Complete(true);
			LifetimeState->UntrackBridge(Bridge);
			UiState->RunExecutionModalLoop();
		});

		const bool bResult = Bridge->WaitForResult();
		LifetimeState->UntrackBridge(Bridge);
		return bResult;
	}

	void ShowNeutralNotification(const FText& Text)
	{
		FNotificationInfo Info(Text);
		Info.ExpireDuration = 8.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	void QueueStatusRefresh(const TArray<FString>& Files, const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& LifetimeState, const FText& FailureText,
		TFunction<void(ECommandResult::Type)> Completion = TFunction<void(ECommandResult::Type)>())
	{
		check(IsInGameThread());
		const TSharedRef<TFunction<void(ECommandResult::Type)>, ESPMode::ThreadSafe> CompletionHandler = MakeShared<TFunction<void(ECommandResult::Type)>, ESPMode::ThreadSafe>(MoveTemp(Completion));
		auto Finish = [LifetimeState, FailureText, CompletionHandler](const ECommandResult::Type Result)
		{
			if (!IsLifetimeAcceptingCallbacks(LifetimeState))
			{
				return;
			}
			if (*CompletionHandler)
			{
				(*CompletionHandler)(Result);
			}
			else if (Result == ECommandResult::Failed)
			{
				ShowFailure(FailureText);
			}
		};
		if (Files.IsEmpty() || !IsLifetimeAcceptingCallbacks(LifetimeState) || !FModuleManager::Get().IsModuleLoaded(TEXT("GitSourceControl")))
		{
			Finish(ECommandResult::Failed);
			return;
		}

		FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
		const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FUpdateStatus>();
		const FSourceControlOperationComplete CompletionDelegate = FSourceControlOperationComplete::CreateLambda([Finish = MoveTemp(Finish)](const FSourceControlOperationRef&, const ECommandResult::Type Result) mutable
		{
			if (IsInGameThread())
			{
				Finish(Result);
				return;
			}
			AsyncTask(ENamedThreads::GameThread, [Finish = MoveTemp(Finish), Result]() mutable
			{
				Finish(Result);
			});
		});
#if ENGINE_MAJOR_VERSION >= 5
		Provider.Execute(Operation, FSourceControlChangelistPtr(), Files, EConcurrency::Asynchronous, CompletionDelegate);
#else
		Provider.Execute(Operation, Files, EConcurrency::Asynchronous, CompletionDelegate);
#endif
	}

	bool RequiresSafeReloadPath(const TArray<FString>& PrimaryPackageFiles, const TArray<UPackage*>& LoadedPackages)
	{
		if (PrimaryPackageFiles.ContainsByPredicate([](const FString& Filename)
		{
			return Filename.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase)
				|| Filename.Contains(TEXT("__ExternalActors__"), ESearchCase::IgnoreCase)
				|| Filename.Contains(TEXT("__ExternalObjects__"), ESearchCase::IgnoreCase)
				|| !IFileManager::Get().FileExists(*Filename);
		}))
		{
			return true;
		}

		return LoadedPackages.ContainsByPredicate([](const UPackage* Package)
		{
			return Package && UWorld::FindWorldInPackage(const_cast<UPackage*>(Package));
		});
	}

	struct FReloadOutcome
	{
		bool bKnown = true;
		bool bSucceeded = true;
		FString Detail;
	};

	bool ReloadAffectedPackages(const TArray<FString>& AffectedFiles, FReloadOutcome& OutOutcome)
	{
		const TArray<FString> PrimaryPackageFiles = GetPrimaryPackageFiles(AffectedFiles);
		if (PrimaryPackageFiles.IsEmpty())
		{
			return true;
		}

		const TArray<UPackage*> LoadedPackages = GatherLoadedPackages(AffectedFiles);
		if (RequiresSafeReloadPath(PrimaryPackageFiles, LoadedPackages))
		{
			// Engine 的 world/external reload 路径没有公开每个 package 的完成结果.
			OutOutcome.bKnown = false;
			const bool bOperationAccepted = USourceControlHelpers::ApplyOperationAndReloadPackages(
				PrimaryPackageFiles,
				[](const TArray<FString>&)
				{
					return true;
				},
				true,
				false);
			if (!bOperationAccepted)
			{
				OutOutcome.bKnown = true;
				OutOutcome.bSucceeded = false;
				OutOutcome.Detail = TEXT("The Engine rejected the package/world reload request.");
			}
			return bOperationAccepted;
		}

		if (LoadedPackages.IsEmpty())
		{
			return true;
		}

		FText ReloadError;
		const bool bReloaded = UPackageTools::ReloadPackages(LoadedPackages, ReloadError, UPackageTools::EReloadPackagesInteractionMode::AssumePositive);
		if (!bReloaded)
		{
			OutOutcome.bSucceeded = false;
			OutOutcome.Detail = ReloadError.IsEmpty()
				? TEXT("The Engine did not report a package reload.")
				: ReloadError.ToString();
		}
		return bReloaded;
	}

	GitSourceControlAssetOperations::FGitAssetOperationCallbacks MakeAssetOperationCallbacks(const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& LifetimeState,
		const TSharedRef<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe>& UiState, const TSharedRef<FReloadOutcome, ESPMode::ThreadSafe>& ReloadOutcome)
	{
		using namespace GitSourceControlAssetOperations;

		FGitAssetOperationCallbacks Callbacks;
		Callbacks.Confirm = [LifetimeState, UiState](const FString& Description, const TArray<FString>& AffectedFiles)
		{
			return ConfirmAssetMutationAndRunExecutionModal(LifetimeState, UiState, Description, AffectedFiles);
		};
		Callbacks.PrepareForMutation = [LifetimeState](const TArray<FString>& AffectedFiles)
		{
			return InvokeOnGameThreadAndWait(LifetimeState, [AffectedFiles]()
			{
				return PrepareLoadedPackages(GatherLoadedPackages(AffectedFiles));
			});
		};
		Callbacks.ReloadPackages = [LifetimeState, ReloadOutcome](const TArray<FString>& AffectedFiles)
		{
			return InvokeOnGameThreadAndWait(LifetimeState, [AffectedFiles, ReloadOutcome]()
			{
				return ReloadAffectedPackages(AffectedFiles, *ReloadOutcome);
			});
		};
		return Callbacks;
	}

	enum class EAssetMutationKind : uint8
	{
		DiscardTracked,
		DeleteUntracked,
		RestoreRevision
	};

	struct FAssetMutationRequest
	{
		EAssetMutationKind Kind = EAssetMutationKind::DiscardTracked;
		TArray<FString> Files;
		FString RevisionSelector;
		FString CommitId;
		FString HistoricalPath;
	};

	/** Resolve the short/full revision selector captured by the History menu against fresh Git history. */
	bool ResolveHistoryRevision(const TGitSourceControlHistory& InHistory, const FString& InSelector,
		FString& OutCommitId, FString& OutHistoricalPath, FString& OutError)
	{
		OutCommitId.Reset();
		OutHistoricalPath.Reset();
		OutError.Reset();
		const FString Selector = InSelector.TrimStartAndEnd();
		if (Selector.IsEmpty())
		{
			OutError = TEXT("The selected history revision has no commit identifier. Reopen History and retry.");
			return false;
		}

		TArray<TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>> Matches;
		for (const TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>& Revision : InHistory)
		{
			const bool bExactCommit = Revision->CommitId.Equals(Selector, ESearchCase::IgnoreCase);
			const bool bCommitPrefix = Revision->CommitId.StartsWith(Selector, ESearchCase::IgnoreCase);
			const bool bShortSelector = Revision->ShortCommitId.Equals(Selector, ESearchCase::IgnoreCase);
			if ((bExactCommit || bCommitPrefix || bShortSelector) && !Revision->CommitId.IsEmpty() && !Revision->Filename.IsEmpty())
			{
				Matches.Add(Revision);
			}
		}

		if (Matches.Num() != 1)
		{
			OutError = Matches.Num() == 0
				? TEXT("The selected history revision is not present in the current Git history. Reopen History and retry.")
				: TEXT("The selected history revision is ambiguous in the current Git history. Reopen History and choose it again.");
			return false;
		}

		OutCommitId = Matches[0]->CommitId;
		OutHistoricalPath = Matches[0]->Filename;
		return true;
	}

	FText GetAssetMutationSuccessText(const EAssetMutationKind Kind)
	{
		switch (Kind)
		{
		case EAssetMutationKind::DiscardTracked:
			return LOCTEXT("DiscardGitSucceeded", "Discarded Git changes and reloaded the affected packages.");
		case EAssetMutationKind::DeleteUntracked:
			return LOCTEXT("DeleteNewSucceeded", "Deleted the selected new asset files and reloaded the affected packages.");
		case EAssetMutationKind::RestoreRevision:
			return LOCTEXT("RestoreRevisionSucceeded", "Restored the selected revision, reloaded affected packages, and left the Git index unchanged.");
		default:
			return FText::GetEmpty();
		}
	}

	FText GetAssetMutationUnknownReloadText(const EAssetMutationKind Kind, const TArray<FString>& AffectedFiles)
	{
		const FText Operation = Kind == EAssetMutationKind::RestoreRevision
			? LOCTEXT("RestoreRevisionDiskSucceeded", "The revision was restored to disk and the Git index was not changed.")
			: Kind == EAssetMutationKind::DeleteUntracked
				? LOCTEXT("DeleteNewDiskSucceeded", "The selected new files were deleted from disk.")
				: LOCTEXT("DiscardGitDiskSucceeded", "The selected Git changes were discarded on disk.");
		return FText::Format(
			LOCTEXT("ReloadOutcomeUnknown", "{0}\n\nThe Engine world/external-package reload was requested, but does not expose a definitive completion result. Reopen these files if they remain stale:\n{1}"),
			Operation,
			FText::FromString(FString::Join(AffectedFiles, TEXT("\n"))));
	}

	void DispatchAssetMutation(FAssetMutationRequest Request, FString GitBinary, FString RepositoryRootFallback, const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& LifetimeState)
	{
		if (!LifetimeState->TryBeginTask())
		{
			return;
		}
		check(IsInGameThread());
		const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext = MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
		const TSharedRef<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe> UiState = MakeShared<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe>(CancellationContext);
		UiState->ShowPreflightNotification();
		const TSharedRef<FGitSourceControlMenuTask, ESPMode::ThreadSafe> Task = MakeShared<FGitSourceControlMenuTask, ESPMode::ThreadSafe>(LifetimeState);
		Async(EAsyncExecution::ThreadPool, [Request = MoveTemp(Request), GitBinary = MoveTemp(GitBinary), RepositoryRootFallback = MoveTemp(RepositoryRootFallback), LifetimeState, CancellationContext, UiState, Task]() mutable
		{
			using namespace GitSourceControlAssetOperations;
			GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
			if (!IsLifetimeAcceptingCallbacks(LifetimeState))
			{
				return;
			}

			FGitAssetOperationResult Result;
			FString RepositoryRoot;
			FString ResolveError;
			bool bSucceeded = FGitSourceControlAssetOperations::ResolveSingleRepositoryRoot(Request.Files, RepositoryRootFallback, RepositoryRoot, ResolveError);
			const TSharedRef<FReloadOutcome, ESPMode::ThreadSafe> ReloadOutcome = MakeShared<FReloadOutcome, ESPMode::ThreadSafe>();
			if (!bSucceeded)
			{
				Result.AddError(ResolveError);
			}
			else
			{
				if (Request.Kind == EAssetMutationKind::RestoreRevision)
				{
					TGitSourceControlHistory History;
					TArray<FString> HistoryErrors;
					if (!GitSourceControlUtils::RunGetHistory(GitBinary, RepositoryRoot, Request.Files[0], false, HistoryErrors, History))
					{
						bSucceeded = false;
						ResolveError = FString::Join(HistoryErrors, TEXT("\n"));
					}
					else
					{
						bSucceeded = ResolveHistoryRevision(History, Request.RevisionSelector, Request.CommitId, Request.HistoricalPath, ResolveError);
					}
				}

				const FGitSourceControlAssetOperations Operations(MoveTemp(GitBinary), MoveTemp(RepositoryRoot));
				const FGitAssetOperationCallbacks Callbacks = MakeAssetOperationCallbacks(LifetimeState, UiState, ReloadOutcome);
				if (bSucceeded)
				{
					switch (Request.Kind)
					{
					case EAssetMutationKind::DiscardTracked:
						bSucceeded = Operations.DiscardTrackedFiles(Request.Files, Callbacks, Result);
						break;
					case EAssetMutationKind::DeleteUntracked:
						bSucceeded = Operations.DeleteUntrackedFiles(Request.Files, Callbacks, Result);
						break;
					case EAssetMutationKind::RestoreRevision:
						bSucceeded = Operations.RestoreRevisionToWorkspace(Request.Files[0], Request.CommitId, Request.HistoricalPath, Callbacks, Result);
						break;
					}
				}
				if (!bSucceeded && !ResolveError.IsEmpty())
				{
					Result.AddError(ResolveError);
				}
			}

			if (!IsLifetimeAcceptingCallbacks(LifetimeState))
			{
				return;
			}
			AsyncTask(ENamedThreads::GameThread, [Request = MoveTemp(Request), Result = MoveTemp(Result), ReloadOutcome, LifetimeState, UiState, bSucceeded]() mutable
			{
				if (!IsLifetimeAcceptingCallbacks(LifetimeState))
				{
					UiState->CloseExecutionModal();
					return;
				}
				UiState->FinishPreflightNotification();
				if (!bSucceeded)
				{
					UiState->CloseExecutionModal();
					if (Result.bCancelled || UiState->WasCancellationRequested())
					{
						ShowNeutralNotification(LOCTEXT("GitAssetMutationCancelled", "The local Git asset operation was cancelled. No asset files were changed."));
					}
					else
					{
						ShowFailure(FText::FromString(FString::Join(Result.Errors, TEXT("\n"))));
					}
					return;
				}

				QueueStatusRefresh(Result.AffectedFiles, LifetimeState, LOCTEXT("RefreshAfterMutationFailed", "The local Git status could not be refreshed after the asset operation."),
					[Request = MoveTemp(Request), Result = MoveTemp(Result), ReloadOutcome, UiState](const ECommandResult::Type RefreshResult) mutable
					{
						UiState->CloseExecutionModal();
						if (RefreshResult != ECommandResult::Succeeded)
						{
							ShowFailure(LOCTEXT("RefreshAfterMutationFailed", "The local Git status could not be refreshed after the asset operation."));
							return;
						}
						if (!ReloadOutcome->bKnown)
						{
							ShowNeutralNotification(GetAssetMutationUnknownReloadText(Request.Kind, Result.AffectedFiles));
							return;
						}
						if (!Result.bReloadSucceeded || !ReloadOutcome->bSucceeded)
						{
							ShowFailure(FText::Format(LOCTEXT("AssetMutationReloadFailed", "The disk operation succeeded, but package reload reported a problem.\n{0}"), FText::FromString(ReloadOutcome->Detail)));
							return;
						}
						ShowNotification(GetAssetMutationSuccessText(Request.Kind), true);
					});
			});
		});
	}

}

void FGitSourceControlMenu::Register()
{
	if (FApp::IsUnattended() || IsRunningCommandlet())
	{
		return;
	}
	LifetimeState = MakeShared<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>();

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	Extenders.Add(FContentBrowserMenuExtender_SelectedAssets::CreateRaw(this, &FGitSourceControlMenu::OnExtendContentBrowserAssetSelectionMenu));
	AssetMenuExtenderHandle = Extenders.Last().GetHandle();

#if ENGINE_MAJOR_VERSION >= 5
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		FToolMenuOwnerScoped OwnerScoped(TEXT("GitSourceControlMenu"));
		if (UToolMenu* SourceControlMenu = ToolMenus->ExtendMenu(TEXT("StatusBar.ToolBar.SourceControl")))
		{
			AddToolbarEntries(SourceControlMenu->FindOrAddSection(TEXT("GitSourceControlLocalActions"), LOCTEXT("LocalGit", "Git")));
		}
		if (UToolMenu* HistoryMenu = ToolMenus->ExtendMenu(TEXT("RevisionControl.History.ContextMenu")))
		{
			HistoryMenu->AddDynamicSection(TEXT("GitSourceControlRestoreRevision"), FNewToolMenuDelegate::CreateRaw(this, &FGitSourceControlMenu::AddHistoryMenuEntries));
		}
	}
#endif
}

void FGitSourceControlMenu::Unregister()
{
	if (LifetimeState.IsValid())
	{
		LifetimeState->StopAcceptingAndWait();
		LifetimeState.Reset();
	}

	if (FContentBrowserModule* ContentBrowserModule = FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser")))
	{
		ContentBrowserModule->GetAllAssetViewContextMenuExtenders().RemoveAll([Handle = AssetMenuExtenderHandle](const FContentBrowserMenuExtender_SelectedAssets& Delegate)
		{
			return Delegate.GetHandle() == Handle;
		});
	}

#if ENGINE_MAJOR_VERSION >= 5
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		ToolMenus->UnregisterOwnerByName(TEXT("GitSourceControlMenu"));
	}
#endif
}

TSharedRef<FExtender> FGitSourceControlMenu::OnExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();
	Extender->AddMenuExtension(TEXT("AssetSourceControlActions"), EExtensionHook::After, nullptr,
		FMenuExtensionDelegate::CreateRaw(this, &FGitSourceControlMenu::AddAssetMenuEntries, SelectedAssets));
	return Extender;
}

void FGitSourceControlMenu::AddAssetMenuEntries(FMenuBuilder& MenuBuilder, const TArray<FAssetData> SelectedAssets)
{
	if (GitSourceControlMenuPrivate::GetAssetFiles(SelectedAssets).IsEmpty())
	{
		return;
	}

	MenuBuilder.BeginSection(TEXT("GitLocalAssetActions"), LOCTEXT("GitLocalAssetActions", "Git (Local)"));
	if (SelectedAssets.Num() == 1)
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ViewGitHistory", "View Git History..."),
			LOCTEXT("ViewGitHistoryTooltip", "Open Unreal's revision history and asset Diff window for this tracked file."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::ViewSelectedAssetHistory, SelectedAssets)));
	}
	MenuBuilder.AddMenuEntry(
		LOCTEXT("DiscardGitChanges", "Discard Git Changes..."),
		LOCTEXT("DiscardGitChangesTooltip", "Restore the selected tracked asset files from HEAD. This also discards their staged changes and never touches unrelated files."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::DiscardSelectedAssets, SelectedAssets)));
	MenuBuilder.AddMenuEntry(
		LOCTEXT("DeleteNewAssetFiles", "Delete New Asset Files..."),
		LOCTEXT("DeleteNewAssetFilesTooltip", "Permanently delete only selected untracked or index-added asset files. Git cannot restore this action."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::DeleteSelectedUntrackedAssets, SelectedAssets)));
	MenuBuilder.AddMenuEntry(
		LOCTEXT("RefreshSelectedGitStatus", "Refresh Selected Asset Status"),
		LOCTEXT("RefreshSelectedGitStatusTooltip", "Refresh local Git status for the selected assets only. No network operation is performed."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::RefreshSelectedAssets, SelectedAssets)));
	MenuBuilder.EndSection();
}

void FGitSourceControlMenu::ViewSelectedAssetHistory(TArray<FAssetData> SelectedAssets)
{
	if (!LifetimeState.IsValid() || !LifetimeState->IsAcceptingCallbacks() || !FModuleManager::Get().IsModuleLoaded(TEXT("GitSourceControl")))
	{
		return;
	}
	const TArray<FString> PackageFiles = GitSourceControlMenuPrivate::GetPrimaryPackageFiles(GitSourceControlMenuPrivate::GetAssetFiles(SelectedAssets));
	if (PackageFiles.Num() != 1)
	{
		GitSourceControlMenuPrivate::ShowFailure(LOCTEXT("GitHistoryRequiresOneAsset", "Git History requires exactly one asset file."));
		return;
	}

	check(IsInGameThread());
	FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = ISourceControlOperation::Create<FUpdateStatus>();
	Operation->SetUpdateHistory(true);
	Operation->SetForceUpdate(true);
	const TSharedRef<FGitSourceControlHistoryLoadUiState, ESPMode::ThreadSafe> UiState = MakeShared<FGitSourceControlHistoryLoadUiState, ESPMode::ThreadSafe>(Operation);
	UiState->Show();

	const FSourceControlOperationComplete CompletionDelegate = FSourceControlOperationComplete::CreateLambda(
		[PackageFiles, LifetimeState = LifetimeState.ToSharedRef(), Operation, UiState](const FSourceControlOperationRef&, const ECommandResult::Type Result)
		{
			auto CompleteOnGameThread = [PackageFiles, LifetimeState, Operation, UiState, Result]()
			{
				if (!LifetimeState->IsAcceptingCallbacks())
				{
					return;
				}

				UiState->Finish();
				if (UiState->WasCancellationRequested() || Result == ECommandResult::Cancelled)
				{
					GitSourceControlMenuPrivate::ShowNeutralNotification(LOCTEXT("GitHistoryLoadCancelled", "Loading local Git history was cancelled."));
					return;
				}
				if (Result != ECommandResult::Succeeded)
				{
					const TArray<FText>& ErrorMessages = Operation->GetResultInfo().ErrorMessages;
					FString ErrorDetail = TEXT("Git did not return parseable history for this file.");
					if (!ErrorMessages.IsEmpty())
					{
						TArray<FString> ErrorStrings;
						ErrorStrings.Reserve(ErrorMessages.Num());
						for (const FText& ErrorMessage : ErrorMessages)
						{
							ErrorStrings.Add(ErrorMessage.ToString());
						}
						ErrorDetail = FString::Join(ErrorStrings, TEXT("\n"));
					}
					GitSourceControlMenuPrivate::ShowFailure(FText::Format(
						LOCTEXT("GitHistoryLoadFailed", "Failed to load Git history for:\n{0}\n\n{1}"),
						FText::FromString(PackageFiles[0]),
						FText::FromString(ErrorDetail)));
					return;
				}

				// The provider records this exact history request as fresh. The native
				// window makes its own synchronous UpdateStatus call, which now uses that
				// generation-valid cache path and must not spawn a second git log.
				FSourceControlWindows::DisplayRevisionHistory(PackageFiles);
			};
			if (IsInGameThread())
			{
				CompleteOnGameThread();
			}
			else
			{
				AsyncTask(ENamedThreads::GameThread, MoveTemp(CompleteOnGameThread));
			}
		});
#if ENGINE_MAJOR_VERSION >= 5
	Provider.Execute(Operation, FSourceControlChangelistPtr(), PackageFiles, EConcurrency::Asynchronous, CompletionDelegate);
#else
	Provider.Execute(Operation, PackageFiles, EConcurrency::Asynchronous, CompletionDelegate);
#endif
}

void FGitSourceControlMenu::AddToolbarEntries(FToolMenuSection& InSection)
{
	InSection.AddMenuEntry(
		TEXT("GitDiscardCachedChanges"),
		LOCTEXT("DiscardCachedGitChanges", "Discard Cached Git Changes..."),
		LOCTEXT("DiscardCachedGitChangesTooltip", "Restore locally cached deleted Git assets. The confirmation lists every exact file."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::DiscardCachedGitChanges)));
	InSection.AddMenuEntry(
		TEXT("GitRefreshCachedLocalState"),
		LOCTEXT("RefreshCachedLocalState", "Refresh Cached Local Git Status"),
		LOCTEXT("RefreshCachedLocalStateTooltip", "Refresh only files already known to the Git provider. No network operation is performed."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::RefreshCachedLocalState)));
}

void FGitSourceControlMenu::AddHistoryMenuEntries(UToolMenu* InToolMenu)
{
#if ENGINE_MAJOR_VERSION >= 5
	USourceControlHistoryWidgetContext* Context = InToolMenu->FindContext<USourceControlHistoryWidgetContext>();
	if (!Context || Context->GetSelectedItems().Num() != 1)
	{
		return;
	}

	const USourceControlHistoryWidgetContext::SelectedItem& Item = Context->GetSelectedItems()[0];
	FToolMenuSection& Section = InToolMenu->AddSection(TEXT("GitRestoreRevision"));
	Section.AddMenuEntry(
		TEXT("GitRestoreRevisionToWorkspace"),
		LOCTEXT("RestoreRevision", "Restore This Revision to Workspace..."),
		LOCTEXT("RestoreRevisionTooltip", "Write this revision to the current workspace file. The Git index, branch and remotes remain unchanged."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::RestoreRevisionToWorkspace, Item.FileName, Item.Revision)));
#endif
}

void FGitSourceControlMenu::RefreshSelectedAssets(TArray<FAssetData> SelectedAssets)
{
	if (!LifetimeState.IsValid() || !LifetimeState->IsAcceptingCallbacks())
	{
		return;
	}
	const TArray<FString> Files = GitSourceControlMenuPrivate::GetAssetFiles(SelectedAssets);
	if (Files.IsEmpty())
	{
		return;
	}
	GitSourceControlMenuPrivate::QueueStatusRefresh(Files, LifetimeState.ToSharedRef(), LOCTEXT("RefreshSelectedFailed", "Failed to refresh the selected local Git status."));
}

void FGitSourceControlMenu::RefreshCachedLocalState()
{
	if (!LifetimeState.IsValid() || !LifetimeState->IsAcceptingCallbacks() || !FModuleManager::Get().IsModuleLoaded(TEXT("GitSourceControl")))
	{
		return;
	}
	const TArray<FString> Files = FGitSourceControlModule::Get().GetProvider().GetFilesInCache();
	if (!Files.IsEmpty())
	{
		GitSourceControlMenuPrivate::QueueStatusRefresh(Files, LifetimeState.ToSharedRef(), LOCTEXT("RefreshCachedFailed", "Failed to refresh cached local Git status."));
	}
}

void FGitSourceControlMenu::DiscardCachedGitChanges()
{
	if (!LifetimeState.IsValid() || !LifetimeState->IsAcceptingCallbacks() || !FModuleManager::Get().IsModuleLoaded(TEXT("GitSourceControl")))
	{
		return;
	}
	FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	TArray<FString> Files;
	for (const FString& Filename : Provider.GetFilesInCache())
	{
		const TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(Filename);
		if (State->IsSourceControlled() && !State->IsConflicted() && State->State.FileState == EFileState::Deleted)
		{
			Files.Add(Filename);
		}
	}
	if (Files.IsEmpty())
	{
		GitSourceControlMenuPrivate::ShowFailure(LOCTEXT("DiscardCachedNone", "There are no cached deleted Git assets to restore."));
		return;
	}

	GitSourceControlMenuPrivate::FAssetMutationRequest Request;
	Request.Kind = GitSourceControlMenuPrivate::EAssetMutationKind::DiscardTracked;
	Request.Files = MoveTemp(Files);
	GitSourceControlMenuPrivate::DispatchAssetMutation(MoveTemp(Request), Provider.GetGitBinaryPath(), Provider.GetPathToRepositoryRoot(), LifetimeState.ToSharedRef());
}

void FGitSourceControlMenu::DiscardSelectedAssets(TArray<FAssetData> SelectedAssets)
{
	if (!LifetimeState.IsValid() || !LifetimeState->IsAcceptingCallbacks() || !FModuleManager::Get().IsModuleLoaded(TEXT("GitSourceControl")))
	{
		return;
	}
	const TArray<FString> Files = GitSourceControlMenuPrivate::GetAssetFiles(SelectedAssets);
	if (Files.IsEmpty())
	{
		return;
	}
	FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	GitSourceControlMenuPrivate::FAssetMutationRequest Request;
	Request.Kind = GitSourceControlMenuPrivate::EAssetMutationKind::DiscardTracked;
	Request.Files = Files;
	GitSourceControlMenuPrivate::DispatchAssetMutation(MoveTemp(Request), Provider.GetGitBinaryPath(), Provider.GetPathToRepositoryRoot(), LifetimeState.ToSharedRef());
}

void FGitSourceControlMenu::DeleteSelectedUntrackedAssets(TArray<FAssetData> SelectedAssets)
{
	if (!LifetimeState.IsValid() || !LifetimeState->IsAcceptingCallbacks() || !FModuleManager::Get().IsModuleLoaded(TEXT("GitSourceControl")))
	{
		return;
	}
	const TArray<FString> Files = GitSourceControlMenuPrivate::GetAssetFiles(SelectedAssets);
	if (Files.IsEmpty())
	{
		return;
	}
	FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	GitSourceControlMenuPrivate::FAssetMutationRequest Request;
	Request.Kind = GitSourceControlMenuPrivate::EAssetMutationKind::DeleteUntracked;
	Request.Files = Files;
	GitSourceControlMenuPrivate::DispatchAssetMutation(MoveTemp(Request), Provider.GetGitBinaryPath(), Provider.GetPathToRepositoryRoot(), LifetimeState.ToSharedRef());
}

void FGitSourceControlMenu::RestoreRevisionToWorkspace(FString Filename, FString Revision)
{
	if (!LifetimeState.IsValid() || !LifetimeState->IsAcceptingCallbacks() || !FModuleManager::Get().IsModuleLoaded(TEXT("GitSourceControl")))
	{
		return;
	}
	Filename = FPaths::ConvertRelativePathToFull(Filename);
	FPaths::NormalizeFilename(Filename);

	FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	GitSourceControlMenuPrivate::FAssetMutationRequest Request;
	Request.Kind = GitSourceControlMenuPrivate::EAssetMutationKind::RestoreRevision;
	Request.Files.Add(Filename);
	Request.RevisionSelector = Revision;
	GitSourceControlMenuPrivate::DispatchAssetMutation(MoveTemp(Request), Provider.GetGitBinaryPath(), Provider.GetPathToRepositoryRoot(), LifetimeState.ToSharedRef());
}

#undef LOCTEXT_NAMESPACE
