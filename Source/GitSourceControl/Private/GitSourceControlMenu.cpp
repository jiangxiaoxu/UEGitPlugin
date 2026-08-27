// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlMenu.h"

#include "AssetRegistry/AssetData.h"
#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "ContentBrowserDelegates.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GitSourceControlAssetOperations.h"
#include "GitSourceControlRevision.h"
#include "DiffUtils.h"
#include "GitStandaloneHistory.h"
#include "SGitStandaloneHistoryWindow.h"
#include "GitStandaloneLog.h"
#include "GitSourceControlUtils.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

class FGitSourceControlAssetMutationPhaseState final
{
public:
	void EnterCommitPhase()
	{
		bCommitPhase.Store(true);
	}

	bool IsInCommitPhase() const
	{
		return bCommitPhase.Load();
	}

	void MarkShutdown()
	{
		bShutdownStarted.Store(true);
	}

	bool IsShutdownStarted() const
	{
		return bShutdownStarted.Load();
	}

private:
	TAtomic<bool> bCommitPhase = false;
	TAtomic<bool> bShutdownStarted = false;
};

bool ShouldCancelForModuleShutdown(const TSharedPtr<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe>& MutationPhase)
{
	return !MutationPhase.IsValid() || !MutationPhase->IsInCommitPhase();
}

class FGitSourceControlMenuGameThreadDispatcher final : public TSharedFromThis<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>
{
public:
	class FRequest final
	{
	public:
		explicit FRequest(TFunction<bool()>&& InWork)
			: Work(MoveTemp(InWork))
			, CompletionEvent(FPlatformProcess::GetSynchEventFromPool(true))
		{
		}

		~FRequest()
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

		bool Execute()
		{
			return Work ? Work() : false;
		}

	private:
		FCriticalSection Mutex;
		TFunction<bool()> Work;
		FEvent* CompletionEvent = nullptr;
		bool bCompleted = false;
		bool bResult = false;
	};

	void Start()
	{
		check(IsInGameThread());
		// 该 ticker 仅在一个显式菜单操作的生命周期内存在.
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(AsShared(), &FGitSourceControlMenuGameThreadDispatcher::Tick), 0.0f);
	}

	bool InvokeAndWait(TFunction<bool()>&& Work)
	{
		if (IsInGameThread())
		{
			return Work();
		}

		const TSharedRef<FRequest, ESPMode::ThreadSafe> Request = MakeShared<FRequest, ESPMode::ThreadSafe>(MoveTemp(Work));
		{
			FScopeLock Lock(&Mutex);
			if (!bAcceptingRequests)
			{
				Request->Complete(false);
			}
			else
			{
				PendingRequests.Add(Request);
			}
		}
		return Request->WaitForResult();
	}

	void Close()
	{
		check(IsInGameThread());
		TArray<TSharedRef<FRequest, ESPMode::ThreadSafe>> RequestsToCancel;
		{
			FScopeLock Lock(&Mutex);
			if (!bAcceptingRequests && !TickerHandle.IsValid())
			{
				return;
			}
			bAcceptingRequests = false;
			RequestsToCancel = MoveTemp(PendingRequests);
			if (TickerHandle.IsValid())
			{
				FTSTicker::RemoveTicker(TickerHandle);
				TickerHandle.Reset();
			}
		}
		for (const TSharedRef<FRequest, ESPMode::ThreadSafe>& Request : RequestsToCancel)
		{
			Request->Complete(false);
		}
	}

private:
	bool Tick(const float DeltaTime)
	{
		(void)DeltaTime;
		TArray<TSharedRef<FRequest, ESPMode::ThreadSafe>> Requests;
		{
			FScopeLock Lock(&Mutex);
			if (!bAcceptingRequests)
			{
				TickerHandle.Reset();
				return false;
			}
			Requests = MoveTemp(PendingRequests);
		}
		for (const TSharedRef<FRequest, ESPMode::ThreadSafe>& Request : Requests)
		{
			Request->Complete(Request->Execute());
		}
		FScopeLock Lock(&Mutex);
		if (!bAcceptingRequests)
		{
			TickerHandle.Reset();
			return false;
		}
		return true;
	}

	FCriticalSection Mutex;
	TArray<TSharedRef<FRequest, ESPMode::ThreadSafe>> PendingRequests;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bAcceptingRequests = true;
};

struct FGitSourceControlTrackedCancellationContext
{
	TWeakPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> Context;
	TWeakPtr<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe> MutationPhase;
};

/**
 * UI state 的最终释放必须发生在 GameThread. 它可能持有 Slate weak reference,
 * 因此 thread-pool work 只能保留自身的 weak reference.
 */
class FGitSourceControlMenuGameThreadUiState
{
public:
	virtual ~FGitSourceControlMenuGameThreadUiState()
	{
		check(IsInGameThread());
	}
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
		check(IsInGameThread());
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

	bool TrackDispatcher(const TSharedRef<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>& Dispatcher)
	{
		FScopeLock Lock(&Mutex);
		if (!bAcceptingCallbacks)
		{
			return false;
		}
		Dispatchers.Add(Dispatcher);
		return true;
	}

	void UntrackDispatcher(const TSharedRef<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>& Dispatcher)
	{
		FScopeLock Lock(&Mutex);
		Dispatchers.RemoveAllSwap([&Dispatcher](const TWeakPtr<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>& Existing)
		{
			const TSharedPtr<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe> Pinned = Existing.Pin();
			return !Pinned.IsValid() || Pinned.Get() == &Dispatcher.Get();
		}, EAllowShrinking::No);
	}

	bool TrackExecutionWindow(const TSharedRef<SWindow>& Window)
	{
		check(IsInGameThread());
		FScopeLock Lock(&Mutex);
		if (!bAcceptingCallbacks)
		{
			return false;
		}
		ExecutionWindows.Add(Window);
		return true;
	}

	void UntrackExecutionWindow(const TSharedRef<SWindow>& Window)
	{
		check(IsInGameThread());
		FScopeLock Lock(&Mutex);
		ExecutionWindows.RemoveAllSwap([&Window](const TSharedPtr<SWindow>& ExistingWindow)
		{
			return !ExistingWindow.IsValid() || ExistingWindow == Window;
		}, EAllowShrinking::No);
	}

	bool TrackGameThreadUiState(const TSharedRef<FGitSourceControlMenuGameThreadUiState, ESPMode::ThreadSafe>& UiState)
	{
		check(IsInGameThread());
		FScopeLock Lock(&Mutex);
		if (!bAcceptingCallbacks)
		{
			return false;
		}
		GameThreadUiStates.Add(UiState);
		return true;
	}

	void UntrackGameThreadUiState(const TSharedRef<FGitSourceControlMenuGameThreadUiState, ESPMode::ThreadSafe>& UiState)
	{
		check(IsInGameThread());
		FScopeLock Lock(&Mutex);
		GameThreadUiStates.RemoveAllSwap([&UiState](const TSharedPtr<FGitSourceControlMenuGameThreadUiState, ESPMode::ThreadSafe>& Existing)
		{
			return !Existing.IsValid() || Existing.Get() == &UiState.Get();
		}, EAllowShrinking::No);
	}

	bool TrackCancellationContext(const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& Context,
		const TSharedPtr<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe>& MutationPhase = nullptr)
	{
		FScopeLock Lock(&Mutex);
		if (!bAcceptingCallbacks)
		{
			Context->Cancel();
			return false;
		}
		FGitSourceControlTrackedCancellationContext& Entry = CancellationContexts.AddDefaulted_GetRef();
		Entry.Context = Context;
		Entry.MutationPhase = MutationPhase;
		return true;
	}

	void UntrackCancellationContext(const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& Context)
	{
		FScopeLock Lock(&Mutex);
		CancellationContexts.RemoveAllSwap([&Context](const FGitSourceControlTrackedCancellationContext& Existing)
		{
			const TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> Pinned = Existing.Context.Pin();
			return !Pinned.IsValid() || Pinned.Get() == &Context.Get();
		}, EAllowShrinking::No);
	}

	bool TrackPersistentNotification(const TSharedRef<SNotificationItem>& Notification)
	{
		check(IsInGameThread());
		FScopeLock Lock(&Mutex);
		if (!bAcceptingCallbacks)
		{
			return false;
		}
		PersistentNotifications.Add(Notification);
		return true;
	}

	void UntrackPersistentNotification(const TSharedRef<SNotificationItem>& Notification)
	{
		check(IsInGameThread());
		FScopeLock Lock(&Mutex);
		PersistentNotifications.RemoveAllSwap([&Notification](const TSharedPtr<SNotificationItem>& Existing)
		{
			return !Existing.IsValid() || Existing.Get() == &Notification.Get();
		}, EAllowShrinking::No);
	}

	void StopAcceptingAndWait()
	{
		check(IsInGameThread());
		TArray<TSharedPtr<SWindow>> WindowsToClose;
		TArray<TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>> ContextsToCancel;
		TArray<TSharedPtr<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>> DispatchersToClose;
		TArray<TSharedPtr<SNotificationItem>> NotificationsToExpire;
		TArray<TSharedPtr<FGitSourceControlMenuGameThreadUiState, ESPMode::ThreadSafe>> UiStatesToRelease;
		{
			FScopeLock Lock(&Mutex);
			bAcceptingCallbacks = false;
			for (const TWeakPtr<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>& Dispatcher : Dispatchers)
			{
				if (TSharedPtr<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe> PinnedDispatcher = Dispatcher.Pin())
				{
					DispatchersToClose.Add(MoveTemp(PinnedDispatcher));
				}
			}
			Dispatchers.Empty();
			WindowsToClose = MoveTemp(ExecutionWindows);
			UiStatesToRelease = MoveTemp(GameThreadUiStates);
			for (const FGitSourceControlTrackedCancellationContext& Entry : CancellationContexts)
			{
				if (TSharedPtr<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe> MutationPhase = Entry.MutationPhase.Pin())
				{
					MutationPhase->MarkShutdown();
					if (!ShouldCancelForModuleShutdown(MutationPhase))
					{
						continue;
					}
				}
				if (TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> PinnedContext = Entry.Context.Pin())
				{
					ContextsToCancel.Add(MoveTemp(PinnedContext));
				}
			}
			CancellationContexts.Empty();
			NotificationsToExpire = MoveTemp(PersistentNotifications);
		}
		for (const TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& Context : ContextsToCancel)
		{
			Context->Cancel();
		}
		for (const TSharedPtr<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>& Dispatcher : DispatchersToClose)
		{
			Dispatcher->Close();
		}
		for (const TSharedPtr<SNotificationItem>& Notification : NotificationsToExpire)
		{
			Notification->ExpireAndFadeout();
		}
		if (FSlateApplication::IsInitialized())
		{
			for (const TSharedPtr<SWindow>& Window : WindowsToClose)
			{
				Window->SetOnWindowClosed(FOnWindowClosed());
				Window->SetContent(SNullWidget::NullWidget);
				FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
			}
		}

		// 此前已关闭 dispatcher, shutdown 开始后 worker 无法再排入 GameThread 工作.
		AllTasksCompletedEvent->Wait();
	}

#if WITH_DEV_AUTOMATION_TESTS
	int32 GetActiveTaskCountForTesting() const
	{
		FScopeLock Lock(&Mutex);
		return ActiveTaskCount;
	}

	int32 GetTrackedGameThreadUiStateCountForTesting() const
	{
		FScopeLock Lock(&Mutex);
		return GameThreadUiStates.Num();
	}
#endif

private:
	mutable FCriticalSection Mutex;
	FEvent* AllTasksCompletedEvent = nullptr;
	int32 ActiveTaskCount = 0;
	bool bAcceptingCallbacks = true;
	TArray<TWeakPtr<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>> Dispatchers;
	TArray<TSharedPtr<SWindow>> ExecutionWindows;
	TArray<TSharedPtr<FGitSourceControlMenuGameThreadUiState, ESPMode::ThreadSafe>> GameThreadUiStates;
	TArray<FGitSourceControlTrackedCancellationContext> CancellationContexts;
	TArray<TSharedPtr<SNotificationItem>> PersistentNotifications;
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
class FGitSourceControlAssetOperationUiState final : public FGitSourceControlMenuGameThreadUiState, public TSharedFromThis<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe>
{
public:
	explicit FGitSourceControlAssetOperationUiState(const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& InCancellationContext,
		const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& InLifetimeState)
		: CancellationContext(InCancellationContext)
		, LifetimeState(InLifetimeState)
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
		const TSharedPtr<SNotificationItem> NewNotification = FSlateNotificationManager::Get().AddNotification(Info);
		if (NewNotification.IsValid())
		{
			if (!LifetimeState->TrackPersistentNotification(NewNotification.ToSharedRef()))
			{
				NewNotification->ExpireAndFadeout();
				return;
			}
			PreflightNotification = NewNotification;
			NewNotification->SetCompletionState(SNotificationItem::CS_Pending);
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
		if (const TSharedPtr<SNotificationItem> Notification = PreflightNotification.Pin())
		{
			Notification->SetText(LOCTEXT("GitAssetPreflightCancelling", "Cancelling local Git preflight..."));
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
		if (const TSharedPtr<SNotificationItem> Notification = PreflightNotification.Pin())
		{
			LifetimeState->UntrackPersistentNotification(Notification.ToSharedRef());
			Notification->ExpireAndFadeout();
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

		const TSharedRef<SWindow> NewExecutionWindow = SNew(SWindow)
			.Title(LOCTEXT("GitAssetMutationProgressTitle", "Applying Local Git Asset Operation"))
			.SizingRule(ESizingRule::Autosized)
			.ClientSize(FVector2D(440.0f, 150.0f))
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.HasCloseButton(false);
		NewExecutionWindow->SetContent(
			SNew(SBorder)
			.Padding(24.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("GitAssetMutationProgress", "Applying the local Git operation...\nPlease wait until the asset reload finishes."))
					.AutoWrapText(380.0f)
					.Justification(ETextJustify::Center)
				]
			]);
		if (!LifetimeState->TrackExecutionWindow(NewExecutionWindow))
		{
			return false;
		}
		ExecutionWindow = NewExecutionWindow;
		bExecutionModalClosing = false;
		return true;
	}

	void RunExecutionModalLoop()
	{
		check(IsInGameThread());
		const TSharedPtr<SWindow> Window = ExecutionWindow.Pin();
		if (!Window.IsValid())
		{
			return;
		}

		FSlateApplication& SlateApplication = FSlateApplication::Get();
		// Non-blocking modal 让正常 Editor tick 驱动 operation-local dispatcher,
		// 不重入 engine task queue.
		SlateApplication.AddModalWindow(Window.ToSharedRef(), SlateApplication.GetActiveTopLevelWindow(), true);
		Window->ShowWindow();
	}

	void CloseExecutionModal()
	{
		check(IsInGameThread());
		const TSharedPtr<SWindow> Window = ExecutionWindow.Pin();
		if (!bExecutionModalClosing && Window.IsValid())
		{
			bExecutionModalClosing = true;
			LifetimeState->UntrackExecutionWindow(Window.ToSharedRef());
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
			}
			ExecutionWindow.Reset();
		}
	}

private:
	TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext;
	TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe> LifetimeState;
	TWeakPtr<SNotificationItem> PreflightNotification;
	TWeakPtr<SWindow> ExecutionWindow;
	bool bPreflightFinished = false;
	bool bCancellationRequested = false;
	bool bExecutionModalClosing = false;
};

/** Presentation and cancellation bridge for an explicit standalone history query. */
class FGitSourceControlHistoryLoadUiState final : public FGitSourceControlMenuGameThreadUiState, public TSharedFromThis<FGitSourceControlHistoryLoadUiState, ESPMode::ThreadSafe>
{
public:
	explicit FGitSourceControlHistoryLoadUiState(const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& InCancellationContext,
		const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& InLifetimeState)
		: CancellationContext(InCancellationContext)
		, LifetimeState(InLifetimeState)
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
		const TSharedPtr<SNotificationItem> NewNotification = FSlateNotificationManager::Get().AddNotification(Info);
		if (NewNotification.IsValid())
		{
			if (!LifetimeState->TrackPersistentNotification(NewNotification.ToSharedRef()))
			{
				NewNotification->ExpireAndFadeout();
				return;
			}
			Notification = NewNotification;
			NewNotification->SetCompletionState(SNotificationItem::CS_Pending);
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
		CancellationContext->Cancel();
		if (const TSharedPtr<SNotificationItem> PinnedNotification = Notification.Pin())
		{
			PinnedNotification->SetText(LOCTEXT("GitHistoryLoadCancelling", "Cancelling local Git history load..."));
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
		if (const TSharedPtr<SNotificationItem> PinnedNotification = Notification.Pin())
		{
			LifetimeState->UntrackPersistentNotification(PinnedNotification.ToSharedRef());
			PinnedNotification->ExpireAndFadeout();
			Notification.Reset();
		}
	}

private:
	TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext;
	TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe> LifetimeState;
	TWeakPtr<SNotificationItem> Notification;
	bool bFinished = false;
	bool bCancellationRequested = false;
};

class FGitSourceControlHistoryDiffUiState final : public FGitSourceControlMenuGameThreadUiState, public TSharedFromThis<FGitSourceControlHistoryDiffUiState, ESPMode::ThreadSafe>
{
public:
	explicit FGitSourceControlHistoryDiffUiState(const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>& InCancellationContext,
		const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& InLifetimeState)
		: CancellationContext(InCancellationContext)
		, LifetimeState(InLifetimeState)
	{
	}

	void Show()
	{
		check(IsInGameThread());
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		FNotificationInfo Info(LOCTEXT("GitHistoryDiffPreparing", "Preparing Git revision content for Diff..."));
		Info.bUseThrobber = true;
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("GitHistoryDiffCancel", "Cancel"),
			LOCTEXT("GitHistoryDiffCancelTooltip", "Cancel Git revision export. No workspace files will be changed."),
			FSimpleDelegate::CreateSP(this, &FGitSourceControlHistoryDiffUiState::RequestCancellation)));
		const TSharedPtr<SNotificationItem> NewNotification = FSlateNotificationManager::Get().AddNotification(Info);
		if (NewNotification.IsValid())
		{
			if (!LifetimeState->TrackPersistentNotification(NewNotification.ToSharedRef()))
			{
				NewNotification->ExpireAndFadeout();
				return;
			}
			Notification = NewNotification;
			NewNotification->SetCompletionState(SNotificationItem::CS_Pending);
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
		CancellationContext->Cancel();
		if (const TSharedPtr<SNotificationItem> PinnedNotification = Notification.Pin())
		{
			PinnedNotification->SetText(LOCTEXT("GitHistoryDiffCancelling", "Cancelling Git revision export..."));
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
		if (const TSharedPtr<SNotificationItem> PinnedNotification = Notification.Pin())
		{
			LifetimeState->UntrackPersistentNotification(PinnedNotification.ToSharedRef());
			PinnedNotification->ExpireAndFadeout();
			Notification.Reset();
		}
	}

private:
	TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext;
	TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe> LifetimeState;
	TWeakPtr<SNotificationItem> Notification;
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
			// The standalone asset tool currently has an explicit .uasset-only scope.
			// Maps remain a future extension and must not expose Git actions.
			if (!PackageFilename.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			AddUniquePath(Files, PackageFilename);
		}
		return Files;
	}

	TArray<FString> GetPrimaryPackageFiles(const TArray<FString>& Files)
	{
		TArray<FString> PackageFiles;
		for (const FString& Filename : Files)
		{
			if (Filename.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
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
		UE_LOG(LogGitStandalone, Warning, TEXT("%s"), *Text.ToString());
		ShowNotification(Text, false);
	}

	bool CheckGitActionExecutionCapability(FText& OutUnavailableMessage)
	{
		OutUnavailableMessage = FText::GetEmpty();
		if (GitSourceControlUtils::IsStartupGitCapabilityAvailable())
		{
			return true;
		}
		OutUnavailableMessage = GitSourceControlUtils::GetStartupGitCapabilityMessage();
		return false;
	}

	bool GuardGitActionExecution()
	{
		FText UnavailableMessage;
		if (CheckGitActionExecutionCapability(UnavailableMessage))
		{
			return true;
		}
		FMessageDialog::Open(EAppMsgType::Ok, UnavailableMessage,
			LOCTEXT("GitActionCapabilityUnavailableTitle", "Git (Local) Unavailable"));
		return false;
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

	bool ConfirmLoadedPackages(const TArray<UPackage*>& LoadedPackages, const FText& OperationDescription, const TArray<FString>& AffectedFiles, const bool bForceRestore)
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
		if (!DirtyPackages.IsEmpty() && !bForceRestore)
		{
			TArray<FString> DirtyNames;
			for (const UPackage* Package : DirtyPackages)
			{
				DirtyNames.Add(Package->GetName());
			}
			DirtyNames.Sort();
			const FText Message = FText::Format(
				LOCTEXT("RejectDirtyPackages", "{0}\n\nExact files to change:\n{1}\n\nThe following loaded packages have unsaved Editor changes:\n{2}\n\n{3}Save, revert, or close these packages before using the standalone Git operation. No disk changes were made."),
				OperationDescription,
				FilesDescription,
				FText::FromString(FString::Join(DirtyNames, TEXT("\n"))),
				bReloadsCurrentWorld ? LOCTEXT("CurrentWorldReloadWarning", "The current world will be reloaded.\n\n") : FText::GetEmpty());
			FMessageDialog::Open(EAppMsgType::Ok, Message);
			return false;
		}
		else if (bForceRestore)
		{
			TArray<FString> DirtyNames;
			for (const UPackage* Package : DirtyPackages)
			{
				DirtyNames.Add(Package->GetName());
			}
			DirtyNames.Sort();
			const FText DirtyPackageWarning = DirtyNames.IsEmpty()
				? FText::GetEmpty()
				: FText::Format(
					LOCTEXT("ForceRestoreDirtyPackages", "The following loaded packages have unsaved Editor changes and will be discarded:\n{0}\n\n"),
					FText::FromString(FString::Join(DirtyNames, TEXT("\n"))));
			const FText Message = FText::Format(
				LOCTEXT("ConfirmForceRestoreLocalGitAssetOperation", "{0}\n\nExact files to change:\n{1}\n\n{2}This FORCE RESTORE discards unsaved Editor changes and Git working-tree, staged, or conflicted changes for the selected file. It has no Undo. The Git index will be reset to HEAD and the worktree will be replaced with the selected revision. Git status is informational and does not block this operation.\n\n{3}Choose No to make no disk changes."),
				OperationDescription,
				FilesDescription,
				DirtyPackageWarning,
				bReloadsCurrentWorld ? LOCTEXT("ForceRestoreCurrentWorldReloadWarning", "The current world will be reloaded.\n\n") : FText::GetEmpty());
			if (FMessageDialog::Open(EAppMsgType::YesNo, Message) != EAppReturnType::Yes)
			{
				return false;
			}
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

	struct FPreparedPackageReloadState
	{
		TArray<FString> PackageFiles;
		bool bPackagesUnloaded = false;
		bool bReloadAttempted = false;
	};

	bool PrepareLoadedPackages(const TArray<FString>& AffectedFiles, FPreparedPackageReloadState& InOutState, const bool bAllowDirtyUnload)
	{
		const TArray<UPackage*> LoadedPackages = GatherLoadedPackages(AffectedFiles);
		if (LoadedPackages.IsEmpty())
		{
			return true;
		}

		FText UnloadError;
		if (!UPackageTools::UnloadPackages(LoadedPackages, UnloadError, bAllowDirtyUnload))
		{
			UE_LOG(LogGitStandalone, Warning, TEXT("Could not unload packages before standalone Git mutation: %s"), *UnloadError.ToString());
			return false;
		}
		InOutState.PackageFiles = GetPrimaryPackageFiles(AffectedFiles);
		InOutState.bPackagesUnloaded = !InOutState.PackageFiles.IsEmpty();
		return true;
	}

	bool IsLifetimeAcceptingCallbacks(const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& LifetimeState)
	{
		return LifetimeState->IsAcceptingCallbacks();
	}

	bool InvokeOnGameThreadAndWait(const TSharedRef<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>& Dispatcher, TFunction<bool()>&& Work)
	{
		return Dispatcher->InvokeAndWait(MoveTemp(Work));
	}

	bool ConfirmAssetMutationAndRunExecutionModal(const TSharedRef<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>& Dispatcher,
		const TWeakPtr<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe>& WeakUiState, const FString& Description, const TArray<FString>& AffectedFiles, const bool bForceRestore)
	{
		return Dispatcher->InvokeAndWait([WeakUiState, Description, AffectedFiles, bForceRestore]()
		{
			const TSharedPtr<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe> UiState = WeakUiState.Pin();
			if (!UiState.IsValid())
			{
				return false;
			}
			if (UiState->WasCancellationRequested())
			{
				return false;
			}
			UiState->FinishPreflightNotification();
			const FText ConfirmationDescription = bForceRestore
				? LOCTEXT("ForceRestoreAssetOperationDescription", "Force restore the selected revision to the workspace.")
				: FText::FromString(Description);
			if (!ConfirmLoadedPackages(GatherLoadedPackages(AffectedFiles, true), ConfirmationDescription, AffectedFiles, bForceRestore) || !UiState->PrepareExecutionModal())
			{
				return false;
			}
			UiState->RunExecutionModalLoop();
			return true;
		});
	}

	void ShowNeutralNotification(const FText& Text)
	{
		FNotificationInfo Info(Text);
		Info.ExpireDuration = 8.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	bool RequiresSafeReloadPath(const TArray<FString>& PrimaryPackageFiles, const TArray<UPackage*>& LoadedPackages)
	{
		if (PrimaryPackageFiles.ContainsByPredicate([](const FString& Filename)
		{
			return Filename.Contains(TEXT("__ExternalActors__"), ESearchCase::IgnoreCase)
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

		const TArray<UPackage*> NoLoadedPackages;
		if (RequiresSafeReloadPath(PrimaryPackageFiles, NoLoadedPackages))
		{
			OutOutcome.bSucceeded = false;
			OutOutcome.Detail = TEXT("Standalone Git asset operations do not support maps, external packages, or missing package files.");
			return false;
		}

		for (const FString& Filename : PrimaryPackageFiles)
		{
			FString PackageName;
			if (!FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName))
			{
				OutOutcome.bSucceeded = false;
				OutOutcome.Detail = FString::Printf(TEXT("Could not resolve package name for '%s'."), *Filename);
				return false;
			}
			UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
			if (Package == nullptr || Package->FindAssetInPackage() == nullptr)
			{
				OutOutcome.bSucceeded = false;
				OutOutcome.Detail = FString::Printf(TEXT("Could not reload the standalone Git asset package '%s'."), *Filename);
				return false;
			}
		}
		return true;
	}

	bool ReloadPreparedPackages(FPreparedPackageReloadState& InOutState, FReloadOutcome& OutOutcome)
	{
		if (!InOutState.bPackagesUnloaded || InOutState.bReloadAttempted)
		{
			return OutOutcome.bSucceeded;
		}
		InOutState.bReloadAttempted = true;
		return ReloadAffectedPackages(InOutState.PackageFiles, OutOutcome);
	}

	GitSourceControlAssetOperations::FGitAssetOperationCallbacks MakeAssetOperationCallbacks(const TSharedRef<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>& Dispatcher,
		const TWeakPtr<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe>& WeakUiState, const TSharedRef<FReloadOutcome, ESPMode::ThreadSafe>& ReloadOutcome,
		const TSharedRef<FPreparedPackageReloadState, ESPMode::ThreadSafe>& PreparedPackages,
		const TSharedRef<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe>& MutationPhase, const bool bForceRestore)
	{
		using namespace GitSourceControlAssetOperations;

		FGitAssetOperationCallbacks Callbacks;
		Callbacks.Confirm = [Dispatcher, WeakUiState, bForceRestore](const FString& Description, const TArray<FString>& AffectedFiles)
		{
			return ConfirmAssetMutationAndRunExecutionModal(Dispatcher, WeakUiState, Description, AffectedFiles, bForceRestore);
		};
		Callbacks.PrepareForMutation = [Dispatcher, PreparedPackages, MutationPhase, bForceRestore](const TArray<FString>& AffectedFiles)
		{
			return InvokeOnGameThreadAndWait(Dispatcher, [AffectedFiles, PreparedPackages, MutationPhase, bForceRestore]()
			{
				const bool bPrepared = PrepareLoadedPackages(AffectedFiles, *PreparedPackages, bForceRestore);
				if (bPrepared)
				{
					MutationPhase->EnterCommitPhase();
				}
				return bPrepared;
			});
		};
		Callbacks.ReloadPackages = [Dispatcher, ReloadOutcome, PreparedPackages, MutationPhase](const TArray<FString>& AffectedFiles)
		{
			if (MutationPhase->IsShutdownStarted())
			{
				return true;
			}
			return InvokeOnGameThreadAndWait(Dispatcher, [AffectedFiles, ReloadOutcome, PreparedPackages]()
			{
				PreparedPackages->bReloadAttempted = true;
				return ReloadAffectedPackages(AffectedFiles, *ReloadOutcome);
			});
		};
		return Callbacks;
	}

	enum class EAssetMutationKind : uint8
	{
		DiscardTracked,
		RestoreRevision
	};

	struct FAssetMutationRequest
	{
		EAssetMutationKind Kind = EAssetMutationKind::DiscardTracked;
		TArray<FString> Files;
		FString CommitId;
		FString HistoricalPath;
	};

	FText GetAssetMutationSuccessText(const EAssetMutationKind Kind)
	{
		switch (Kind)
		{
		case EAssetMutationKind::DiscardTracked:
			return LOCTEXT("DiscardGitSucceeded", "Discarded Git changes and reloaded the affected packages.");
		case EAssetMutationKind::RestoreRevision:
			return LOCTEXT("RestoreRevisionSucceeded", "Force-restored the selected revision, reset its Git index entry to HEAD, and reloaded the affected package.");
		default:
			return FText::GetEmpty();
		}
	}

	FText GetAssetMutationUnknownReloadText(const EAssetMutationKind Kind, const TArray<FString>& AffectedFiles)
	{
		const FText Operation = Kind == EAssetMutationKind::RestoreRevision
			? LOCTEXT("RestoreRevisionDiskSucceeded", "The revision was force-restored to disk and its Git index entry was reset to HEAD.")
			: LOCTEXT("DiscardGitDiskSucceeded", "The selected Git changes were discarded on disk.");
		return FText::Format(
			LOCTEXT("ReloadOutcomeUnknown", "{0}\n\nThe Engine world/external-package reload was requested, but does not expose a definitive completion result. Reopen these files if they remain stale:\n{1}"),
			Operation,
			FText::FromString(FString::Join(AffectedFiles, TEXT("\n"))));
	}

	void DispatchAssetMutation(FAssetMutationRequest Request, const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& LifetimeState)
	{
		check(IsInGameThread());
		FString MutationPreflightError;
		if (!GitSourceControlAssetOperations::FGitSourceControlAssetOperations::ValidateStandaloneMutationPreflight(
			Request.Files, GatherLoadedPackages(Request.Files, true), MutationPreflightError))
		{
			ShowFailure(FText::FromString(MutationPreflightError));
			return;
		}
		if (!LifetimeState->TryBeginTask())
		{
			return;
		}
		const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext = MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
		const TSharedRef<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe> MutationPhase = MakeShared<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe>();
		const TSharedRef<FGitSourceControlMenuTask, ESPMode::ThreadSafe> Task = MakeShared<FGitSourceControlMenuTask, ESPMode::ThreadSafe>(LifetimeState);
		if (!LifetimeState->TrackCancellationContext(CancellationContext, MutationPhase))
		{
			return;
		}
		const TSharedRef<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = MakeShared<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>();
		if (!LifetimeState->TrackDispatcher(Dispatcher))
		{
			return;
		}
		Dispatcher->Start();
		const TSharedRef<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe> UiState = MakeShared<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe>(CancellationContext, LifetimeState);
		if (!LifetimeState->TrackGameThreadUiState(StaticCastSharedRef<FGitSourceControlMenuGameThreadUiState>(UiState)))
		{
			LifetimeState->UntrackDispatcher(Dispatcher);
			Dispatcher->Close();
			return;
		}
		UiState->ShowPreflightNotification();
		const TWeakPtr<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe> WeakUiState = UiState;
		Async(EAsyncExecution::ThreadPool, [Request = MoveTemp(Request), LifetimeState, CancellationContext, Dispatcher, WeakUiState, MutationPhase, Task]() mutable
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
			FString GitBinary;
			bool bSucceeded = !Request.Files.IsEmpty();
			if (!bSucceeded)
			{
				ResolveError = TEXT("No asset files were supplied for the standalone Git operation.");
			}
			else
			{
				bSucceeded = GitSourceControlUtils::ResolveStandaloneRepositoryForFile(Request.Files[0], GitBinary, RepositoryRoot, ResolveError);
			}
			const TSharedRef<FReloadOutcome, ESPMode::ThreadSafe> ReloadOutcome = MakeShared<FReloadOutcome, ESPMode::ThreadSafe>();
			const TSharedRef<FPreparedPackageReloadState, ESPMode::ThreadSafe> PreparedPackages = MakeShared<FPreparedPackageReloadState, ESPMode::ThreadSafe>();
			if (!bSucceeded)
			{
				Result.AddError(ResolveError);
			}
			else
			{
				const FGitSourceControlAssetOperations Operations(MoveTemp(GitBinary), MoveTemp(RepositoryRoot));
				const FGitAssetOperationCallbacks Callbacks = MakeAssetOperationCallbacks(Dispatcher, WeakUiState, ReloadOutcome, PreparedPackages, MutationPhase,
					Request.Kind == EAssetMutationKind::RestoreRevision);
				if (bSucceeded)
				{
					switch (Request.Kind)
					{
					case EAssetMutationKind::DiscardTracked:
						bSucceeded = Operations.DiscardTrackedFiles(Request.Files, Callbacks, Result);
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

			const bool bShutdownDuringCommit = MutationPhase->IsInCommitPhase() && MutationPhase->IsShutdownStarted();
			if (!IsLifetimeAcceptingCallbacks(LifetimeState) && !bShutdownDuringCommit)
			{
				return;
			}
			if (bShutdownDuringCommit)
			{
				// The core mutation has completed or rolled back. Shutdown has already
				// released the dispatcher, so never schedule reload UI from this worker.
				return;
			}
			Dispatcher->InvokeAndWait([Request = MoveTemp(Request), Result = MoveTemp(Result), ReloadOutcome, PreparedPackages, LifetimeState, CancellationContext, Dispatcher, WeakUiState, bSucceeded]() mutable
			{
				LifetimeState->UntrackCancellationContext(CancellationContext);
				const TSharedPtr<FGitSourceControlAssetOperationUiState, ESPMode::ThreadSafe> UiState = WeakUiState.Pin();
				ON_SCOPE_EXIT
				{
					if (UiState.IsValid())
					{
						LifetimeState->UntrackGameThreadUiState(StaticCastSharedRef<FGitSourceControlMenuGameThreadUiState>(UiState.ToSharedRef()));
					}
				};
				if (!UiState.IsValid())
				{
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return false;
				}
				if (!IsLifetimeAcceptingCallbacks(LifetimeState))
				{
					UiState->CloseExecutionModal();
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return false;
				}
				UiState->FinishPreflightNotification();
				ReloadPreparedPackages(*PreparedPackages, *ReloadOutcome);
				if (PreparedPackages->bPackagesUnloaded)
				{
					Result.bReloadSucceeded = ReloadOutcome->bSucceeded;
				}
				if (!bSucceeded)
				{
					UiState->CloseExecutionModal();
					if (PreparedPackages->bPackagesUnloaded && !ReloadOutcome->bSucceeded)
					{
						const FString OperationError = Result.Errors.IsEmpty()
							? TEXT("The local Git asset operation did not complete.")
							: FString::Join(Result.Errors, TEXT("\n"));
						ShowFailure(FText::Format(
							LOCTEXT("AssetMutationFailureReloadFailed", "{0}\n\nThe package was unloaded before the operation and could not be reloaded:\n{1}"),
							FText::FromString(OperationError), FText::FromString(ReloadOutcome->Detail)));
					}
					else if (Result.bCancelled || UiState->WasCancellationRequested())
					{
						ShowNeutralNotification(PreparedPackages->bPackagesUnloaded
							? LOCTEXT("GitAssetMutationCancelledReloaded", "The local Git asset operation was cancelled. The unloaded package was reloaded.")
							: LOCTEXT("GitAssetMutationCancelled", "The local Git asset operation was cancelled. No asset files were changed."));
					}
					else
					{
						ShowFailure(FText::FromString(FString::Join(Result.Errors, TEXT("\n"))));
					}
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return true;
				}

				UiState->CloseExecutionModal();
				if (!ReloadOutcome->bKnown)
				{
					ShowNeutralNotification(GetAssetMutationUnknownReloadText(Request.Kind, Result.AffectedFiles));
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return true;
				}
				if (!Result.bReloadSucceeded || !ReloadOutcome->bSucceeded)
				{
					ShowFailure(FText::Format(LOCTEXT("AssetMutationReloadFailed", "The disk operation succeeded, but package reload reported a problem.\n{0}"), FText::FromString(ReloadOutcome->Detail)));
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return true;
				}
				ShowNotification(GetAssetMutationSuccessText(Request.Kind), true);
				LifetimeState->UntrackDispatcher(Dispatcher);
				Dispatcher->Close();
				return true;
			});
		});
	}

	FRevisionInfo MakeHistoryDiffRevisionInfo(const FGitSourceControlRevision& Revision)
	{
		FRevisionInfo Info;
		Info.Revision = Revision.ShortCommitId;
		Info.Changelist = Revision.CommitIdNumber;
		Info.Date = Revision.Date;
		return Info;
	}

	UObject* LoadExportedHistoryRevisionForDiff(const FGitSourceControlRevision& Revision, const FString& ExportedFilename)
	{
		const FPackagePath TempPackagePath = FPackagePath::FromLocalPath(ExportedFilename);
		const FPackagePath OriginalPackagePath = FPackagePath::FromLocalPath(Revision.GetFilename());
		if (TempPackagePath.IsEmpty() || OriginalPackagePath.IsEmpty())
		{
			return nullptr;
		}
		UPackage* Package = DiffUtils::LoadPackageForDiff(TempPackagePath, OriginalPackagePath);
		return Package == nullptr ? nullptr : Package->FindAssetInPackage();
	}

	struct FHistoryDiffExportFiles
	{
		FString Older;
		FString Newer;
	};

	void ReleaseHistoryDiffExports(const FHistoryDiffExportFiles& Exports)
	{
		if (!Exports.Older.IsEmpty())
		{
			GitSourceControlRevision::ReleaseTemporaryExport(Exports.Older);
		}
		if (!Exports.Newer.IsEmpty())
		{
			GitSourceControlRevision::ReleaseTemporaryExport(Exports.Newer);
		}
	}

	TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> DispatchHistoryDiff(const FString& WorkspaceFilename, const TSharedPtr<FGitSourceControlRevision, ESPMode::ThreadSafe>& OlderRevision,
		const TSharedPtr<FGitSourceControlRevision, ESPMode::ThreadSafe>& NewerRevision,
		const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& LifetimeState)
	{
		check(IsInGameThread());
		if (!OlderRevision.IsValid() || !LifetimeState->TryBeginTask())
		{
			return nullptr;
		}

		const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
			MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
		const TSharedRef<FGitSourceControlMenuTask, ESPMode::ThreadSafe> Task = MakeShared<FGitSourceControlMenuTask, ESPMode::ThreadSafe>(LifetimeState);
		if (!LifetimeState->TrackCancellationContext(CancellationContext))
		{
			return nullptr;
		}
		const TSharedRef<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = MakeShared<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>();
		if (!LifetimeState->TrackDispatcher(Dispatcher))
		{
			return nullptr;
		}
		Dispatcher->Start();
		const TSharedRef<FGitSourceControlHistoryDiffUiState, ESPMode::ThreadSafe> UiState =
			MakeShared<FGitSourceControlHistoryDiffUiState, ESPMode::ThreadSafe>(CancellationContext, LifetimeState);
		if (!LifetimeState->TrackGameThreadUiState(StaticCastSharedRef<FGitSourceControlMenuGameThreadUiState>(UiState)))
		{
			LifetimeState->UntrackDispatcher(Dispatcher);
			Dispatcher->Close();
			return nullptr;
		}
		UiState->Show();
		const TWeakPtr<FGitSourceControlHistoryDiffUiState, ESPMode::ThreadSafe> WeakUiState = UiState;

		Async(EAsyncExecution::ThreadPool, [WorkspaceFilename, OlderRevision, NewerRevision, LifetimeState, CancellationContext, Dispatcher, WeakUiState, Task]()
		{
			GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
			const TSharedRef<FHistoryDiffExportFiles, ESPMode::ThreadSafe> Exports = MakeShared<FHistoryDiffExportFiles, ESPMode::ThreadSafe>();
			FString Error;
			bool bSucceeded = OlderRevision->Get(Exports->Older);
			if (!bSucceeded)
			{
				Error = TEXT("Could not export the older Git revision for Diff.");
			}
			if (bSucceeded && !CancellationContext->IsCancellationRequested() && NewerRevision.IsValid())
			{
				bSucceeded = NewerRevision->Get(Exports->Newer);
				if (!bSucceeded)
				{
					Error = TEXT("Could not export the newer Git revision for Diff.");
				}
			}
			if (CancellationContext->IsCancellationRequested())
			{
				bSucceeded = false;
				Error = TEXT("Git revision export was cancelled.");
			}

			const bool bDelivered = Dispatcher->InvokeAndWait([WorkspaceFilename, OlderRevision, NewerRevision, Exports,
				Error = MoveTemp(Error), LifetimeState, CancellationContext, Dispatcher, WeakUiState, bSucceeded]() mutable
			{
				LifetimeState->UntrackCancellationContext(CancellationContext);
				const TSharedPtr<FGitSourceControlHistoryDiffUiState, ESPMode::ThreadSafe> UiState = WeakUiState.Pin();
				ON_SCOPE_EXIT
				{
					if (UiState.IsValid())
					{
						LifetimeState->UntrackGameThreadUiState(StaticCastSharedRef<FGitSourceControlMenuGameThreadUiState>(UiState.ToSharedRef()));
					}
				};
				if (!UiState.IsValid())
				{
					ReleaseHistoryDiffExports(*Exports);
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return false;
				}
				UiState->Finish();
				if (!LifetimeState->IsAcceptingCallbacks())
				{
					ReleaseHistoryDiffExports(*Exports);
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return false;
				}
				if (CancellationContext->IsCancellationRequested() || UiState->WasCancellationRequested())
				{
					ShowNeutralNotification(LOCTEXT("GitHistoryDiffCancelled", "Preparing Git revision content for Diff was cancelled."));
					ReleaseHistoryDiffExports(*Exports);
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return true;
				}
				if (!bSucceeded)
				{
					ShowFailure(FText::FromString(Error.IsEmpty() ? TEXT("Could not prepare Git revision content for Diff.") : Error));
					ReleaseHistoryDiffExports(*Exports);
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return true;
				}

				UObject* OlderAsset = LoadExportedHistoryRevisionForDiff(*OlderRevision, Exports->Older);
				UObject* NewerAsset = nullptr;
				FRevisionInfo NewerInfo;
				if (NewerRevision.IsValid())
				{
					NewerAsset = LoadExportedHistoryRevisionForDiff(*NewerRevision, Exports->Newer);
					NewerInfo = MakeHistoryDiffRevisionInfo(*NewerRevision);
				}
				else
				{
					FString WorkspacePackageName;
					if (FPackageName::TryConvertFilenameToLongPackageName(WorkspaceFilename, WorkspacePackageName))
					{
						if (UPackage* WorkspacePackage = LoadPackage(nullptr, *WorkspacePackageName, LOAD_None))
						{
							NewerAsset = WorkspacePackage->FindAssetInPackage();
						}
					}
				}

				if (OlderAsset == nullptr || NewerAsset == nullptr || OlderAsset->GetClass() != NewerAsset->GetClass())
				{
					ShowFailure(LOCTEXT("StandaloneHistoryDiffFailed", "Unable to load compatible assets for Diff. Content may no longer be supported."));
					ReleaseHistoryDiffExports(*Exports);
				}
				else
				{
					FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().DiffAssets(
						OlderAsset, NewerAsset, MakeHistoryDiffRevisionInfo(*OlderRevision), NewerInfo);
				}
				LifetimeState->UntrackDispatcher(Dispatcher);
				Dispatcher->Close();
				return true;
			});
			if (!bDelivered)
			{
				ReleaseHistoryDiffExports(*Exports);
			}
		});
		return CancellationContext;
	}

	void BeginStandaloneHistoryLoad(const FString& Filename, const EGitLocalSourceControlHistoryMode Mode,
		const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>& LifetimeState)
	{
		if (!LifetimeState->TryBeginTask())
		{
			return;
		}

		const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
			MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
		const TSharedRef<FGitSourceControlMenuTask, ESPMode::ThreadSafe> Task = MakeShared<FGitSourceControlMenuTask, ESPMode::ThreadSafe>(LifetimeState);
		if (!LifetimeState->TrackCancellationContext(CancellationContext))
		{
			return;
		}
		const TSharedRef<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = MakeShared<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>();
		if (!LifetimeState->TrackDispatcher(Dispatcher))
		{
			return;
		}
		Dispatcher->Start();
		const TSharedRef<FGitSourceControlHistoryLoadUiState, ESPMode::ThreadSafe> UiState =
			MakeShared<FGitSourceControlHistoryLoadUiState, ESPMode::ThreadSafe>(CancellationContext, LifetimeState);
		if (!LifetimeState->TrackGameThreadUiState(StaticCastSharedRef<FGitSourceControlMenuGameThreadUiState>(UiState)))
		{
			LifetimeState->UntrackDispatcher(Dispatcher);
			Dispatcher->Close();
			return;
		}
		UiState->Show();
		const TWeakPtr<FGitSourceControlHistoryLoadUiState, ESPMode::ThreadSafe> WeakUiState = UiState;

		Async(EAsyncExecution::ThreadPool, [Filename, Mode, LifetimeState, CancellationContext, Dispatcher, WeakUiState, Task]()
		{
			GitSourceControlUtils::FGitOperationCancellationScope CancellationScope(CancellationContext);
			FString GitBinary;
			FString RepositoryRoot;
			FString ResolveError;
			TArray<FString> Errors;
			TGitSourceControlHistory History;
			FString CapturedHead;
			bool bHeadChanged = false;
			const bool bResolved = GitSourceControlUtils::ResolveStandaloneRepositoryForFile(Filename, GitBinary, RepositoryRoot, ResolveError);
			const bool bSucceeded = bResolved && GitSourceControlUtils::RunGetHistory(
				GitBinary, RepositoryRoot, Filename, false, Mode, CapturedHead, bHeadChanged, Errors, History);
			if (!bResolved && !ResolveError.IsEmpty())
			{
				Errors.Add(ResolveError);
			}

			Dispatcher->InvokeAndWait([Filename, Mode, LifetimeState, CancellationContext, Dispatcher, WeakUiState, bSucceeded, bHeadChanged,
				Errors = MoveTemp(Errors), History = MoveTemp(History)]() mutable
			{
				LifetimeState->UntrackCancellationContext(CancellationContext);
				const TSharedPtr<FGitSourceControlHistoryLoadUiState, ESPMode::ThreadSafe> UiState = WeakUiState.Pin();
				ON_SCOPE_EXIT
				{
					if (UiState.IsValid())
					{
						LifetimeState->UntrackGameThreadUiState(StaticCastSharedRef<FGitSourceControlMenuGameThreadUiState>(UiState.ToSharedRef()));
					}
				};
				if (!UiState.IsValid())
				{
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return false;
				}
				UiState->Finish();
				if (!LifetimeState->IsAcceptingCallbacks())
				{
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return false;
				}
				if (CancellationContext->IsCancellationRequested() || UiState->WasCancellationRequested())
				{
					ShowNeutralNotification(LOCTEXT("GitHistoryLoadCancelled", "Loading local Git history was cancelled."));
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return true;
				}
				if (!bSucceeded)
				{
					const FString Detail = Errors.IsEmpty() ? TEXT("Git did not return parseable history for this file.") : FString::Join(Errors, TEXT("\n"));
					ShowFailure(FText::Format(
						LOCTEXT("GitHistoryLoadFailed", "Failed to load Git history for:\n{0}\n\n{1}"),
						FText::FromString(Filename), FText::FromString(Detail)));
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return true;
				}
				if (History.IsEmpty())
				{
					ShowFailure(LOCTEXT("GitHistoryEmpty", "Git did not return any history for the selected asset."));
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return true;
				}
				if (bHeadChanged)
				{
					ShowNeutralNotification(LOCTEXT("GitHistoryHeadChanged", "Repository HEAD changed while history was loading. Refresh history to update the snapshot."));
				}

				const FGitStandaloneHistoryRestoreDelegate RestoreDelegate = FGitStandaloneHistoryRestoreDelegate::CreateLambda(
					[LifetimeState](FString LocalFilename, FString CommitId, FString HistoricalPath)
					{
						if (!LifetimeState->IsAcceptingCallbacks())
						{
							return;
						}
						FAssetMutationRequest Request;
						Request.Kind = EAssetMutationKind::RestoreRevision;
						Request.Files.Add(MoveTemp(LocalFilename));
						Request.CommitId = MoveTemp(CommitId);
						Request.HistoricalPath = MoveTemp(HistoricalPath);
						DispatchAssetMutation(MoveTemp(Request), LifetimeState);
					});
				const FGitStandaloneHistoryRefreshDelegate RefreshDelegate = FGitStandaloneHistoryRefreshDelegate::CreateLambda(
					[LifetimeState](FString LocalFilename, const EGitLocalSourceControlHistoryMode RefreshMode)
					{
						if (LifetimeState->IsAcceptingCallbacks())
						{
							BeginStandaloneHistoryLoad(LocalFilename, RefreshMode, LifetimeState);
						}
					});
				const FGitStandaloneHistoryDiffDelegate DiffDelegate = FGitStandaloneHistoryDiffDelegate::CreateLambda(
					[LifetimeState](FString LocalFilename, TSharedPtr<FGitSourceControlRevision, ESPMode::ThreadSafe> OlderRevision,
						TSharedPtr<FGitSourceControlRevision, ESPMode::ThreadSafe> NewerRevision)
					{
						if (LifetimeState->IsAcceptingCallbacks())
						{
							return DispatchHistoryDiff(LocalFilename, OlderRevision, NewerRevision, LifetimeState);
						}
						return TSharedPtr<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
					});
				const TSharedRef<SWindow> HistoryWindow = GitSourceControlStandaloneHistory::CreateWindow(
					Filename, Mode, History, RestoreDelegate, RefreshDelegate, DiffDelegate);
				if (!LifetimeState->TrackExecutionWindow(HistoryWindow))
				{
					LifetimeState->UntrackDispatcher(Dispatcher);
					Dispatcher->Close();
					return false;
				}
				const TWeakPtr<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe> WeakLifetimeState = LifetimeState;
				HistoryWindow->SetOnWindowClosed(FOnWindowClosed::CreateLambda([WeakLifetimeState](const TSharedRef<SWindow>& ClosedWindow)
				{
					check(IsInGameThread());
					if (const TSharedPtr<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe> PinnedLifetimeState = WeakLifetimeState.Pin())
					{
						PinnedLifetimeState->UntrackExecutionWindow(ClosedWindow);
					}
				}));
				FSlateApplication::Get().AddWindow(HistoryWindow);
				LifetimeState->UntrackDispatcher(Dispatcher);
				Dispatcher->Close();
				return true;
			});
		});
	}

}

void FGitSourceControlMenu::Register()
{
	if (bRegistered || FApp::IsUnattended() || IsRunningCommandlet())
	{
		return;
	}
	LifetimeState = MakeShared<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>();

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
	Extenders.Add(FContentBrowserMenuExtender_SelectedAssets::CreateRaw(this, &FGitSourceControlMenu::OnExtendContentBrowserAssetSelectionMenu));
	AssetMenuExtenderHandle = Extenders.Last().GetHandle();
	bRegistered = true;
}

void FGitSourceControlMenu::Unregister()
{
	if (!bRegistered)
	{
		return;
	}
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
	AssetMenuExtenderHandle.Reset();
	bRegistered = false;
}

TSharedRef<FExtender> FGitSourceControlMenu::OnExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();
	Extender->AddMenuExtension(TEXT("CommonAssetActions"), EExtensionHook::After, nullptr,
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
			LOCTEXT("ViewGitHistoryTooltip", "Open standalone, fixed-HEAD current-path Git history and asset Diff."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::ViewSelectedAssetHistory, SelectedAssets, EGitLocalSourceControlHistoryMode::CurrentPath)));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ViewGitHistoryExactRenames", "View Git History Across Exact Renames..."),
			LOCTEXT("ViewGitHistoryExactRenamesTooltip", "Also follow committed, single-parent R100 Git renames. This does not infer Unreal asset identity."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::ViewSelectedAssetHistory, SelectedAssets, EGitLocalSourceControlHistoryMode::ExactRenames)));
	}
	MenuBuilder.AddMenuEntry(
		LOCTEXT("DiscardGitChanges", "Discard Git Changes..."),
		LOCTEXT("DiscardGitChangesTooltip", "Restore selected tracked .uasset files from HEAD. This also discards staged changes for exactly those assets."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::DiscardSelectedAssets, SelectedAssets)));
	MenuBuilder.EndSection();
}

void FGitSourceControlMenu::ViewSelectedAssetHistory(TArray<FAssetData> SelectedAssets, const EGitLocalSourceControlHistoryMode Mode)
{
	if (!GitSourceControlMenuPrivate::GuardGitActionExecution())
	{
		return;
	}
	if (!LifetimeState.IsValid() || !LifetimeState->IsAcceptingCallbacks())
	{
		return;
	}
	const TArray<FString> PackageFiles = GitSourceControlMenuPrivate::GetPrimaryPackageFiles(GitSourceControlMenuPrivate::GetAssetFiles(SelectedAssets));
	if (PackageFiles.Num() != 1)
	{
		GitSourceControlMenuPrivate::ShowFailure(LOCTEXT("GitHistoryRequiresOneAsset", "Git History requires exactly one asset file."));
		return;
	}

	GitSourceControlMenuPrivate::BeginStandaloneHistoryLoad(PackageFiles[0], Mode, LifetimeState.ToSharedRef());
}

void FGitSourceControlMenu::DiscardSelectedAssets(TArray<FAssetData> SelectedAssets)
{
	if (!GitSourceControlMenuPrivate::GuardGitActionExecution())
	{
		return;
	}
	if (!LifetimeState.IsValid() || !LifetimeState->IsAcceptingCallbacks())
	{
		return;
	}
	const TArray<FString> Files = GitSourceControlMenuPrivate::GetAssetFiles(SelectedAssets);
	if (Files.IsEmpty())
	{
		return;
	}
	GitSourceControlMenuPrivate::FAssetMutationRequest Request;
	Request.Kind = GitSourceControlMenuPrivate::EAssetMutationKind::DiscardTracked;
	Request.Files = Files;
	GitSourceControlMenuPrivate::DispatchAssetMutation(MoveTemp(Request), LifetimeState.ToSharedRef());
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStandaloneUAssetMenuScopeTest, "Cthulhu.GitSourceControl.Standalone.UAssetMenuScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitStandaloneUAssetMenuScopeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> Files = { TEXT("Content/Example.uasset"), TEXT("Content/Example.umap"), TEXT("Content/Example.txt") };
	const TArray<FString> PackageFiles = GitSourceControlMenuPrivate::GetPrimaryPackageFiles(Files);
	TestEqual(TEXT("Only .uasset files are eligible for standalone Git menu actions"), PackageFiles.Num(), 1);
	TestTrue(TEXT("The .uasset path remains eligible"), PackageFiles.Contains(TEXT("Content/Example.uasset")));
	TestFalse(TEXT("The future .umap scope is not exposed"), PackageFiles.Contains(TEXT("Content/Example.umap")));

	const GitSourceControlUtils::FGitStartupCapability SavedCapability = GitSourceControlUtils::GetStartupGitCapability();
	ON_SCOPE_EXIT
	{
		GitSourceControlUtils::Testing::SetStartupGitCapabilityForTesting(SavedCapability);
	};
	GitSourceControlUtils::FGitStartupCapability PendingCapability;
	PendingCapability.State = GitSourceControlUtils::EGitStartupCapabilityState::Pending;
	PendingCapability.Diagnostic = TEXT("Checking for Git 2.53.0 or newer...");
	GitSourceControlUtils::Testing::SetStartupGitCapabilityForTesting(PendingCapability);
	GitSourceControlUtils::Testing::ResetGitProcessLaunchCount();
	FText CapabilityMessage;
	TestFalse(TEXT("Pending startup capability blocks a clickable Content Browser Git action"),
		GitSourceControlMenuPrivate::CheckGitActionExecutionCapability(CapabilityMessage));
	TestTrue(TEXT("Pending Git action explains that the startup check is still running"), CapabilityMessage.ToString().Contains(TEXT("Checking for Git")));
	TestEqual(TEXT("Pending Content Browser Git action starts no Git process"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
	TestEqual(TEXT("Pending startup capability keeps the .uasset action scope present"),
		GitSourceControlMenuPrivate::GetPrimaryPackageFiles(Files).Num(), 1);
	GitSourceControlUtils::FGitStartupCapability UnavailableCapability;
	UnavailableCapability.State = GitSourceControlUtils::EGitStartupCapabilityState::Unavailable;
	UnavailableCapability.Diagnostic = TEXT("Detected Git executable: C:/Tools/Git/bin/git.exe\nDetected version: git version 2.42.0\nGit 2.53.0 or a newer release is required. Install or upgrade Git, then restart the Editor.");
	GitSourceControlUtils::Testing::SetStartupGitCapabilityForTesting(UnavailableCapability);
	CapabilityMessage = FText::GetEmpty();
	TestFalse(TEXT("Unavailable startup capability blocks a clickable Content Browser Git action"),
		GitSourceControlMenuPrivate::CheckGitActionExecutionCapability(CapabilityMessage));
	TestTrue(TEXT("Unavailable Git action preserves the detected path, required version, and restart guidance"),
		CapabilityMessage.ToString().Contains(TEXT("C:/Tools/Git/bin/git.exe"))
		&& CapabilityMessage.ToString().Contains(TEXT("Git 2.53.0"))
		&& CapabilityMessage.ToString().Contains(TEXT("restart the Editor")));
	TestEqual(TEXT("Unavailable Content Browser Git action starts no Git process"), GitSourceControlUtils::Testing::GetGitProcessLaunchCount(), static_cast<uint64>(0));
	return TestEqual(TEXT("Unavailable startup capability keeps the .uasset action scope present"),
		GitSourceControlMenuPrivate::GetPrimaryPackageFiles(Files).Num(), 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStandaloneMutationShutdownPhaseTest, "Cthulhu.GitSourceControl.Standalone.MutationShutdownPhase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGitStandaloneMutationShutdownPhaseTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe> Phase = MakeShared<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe>();
	TestTrue(TEXT("Preflight mutation is cancelled during module shutdown"), ShouldCancelForModuleShutdown(Phase));
	Phase->EnterCommitPhase();
	TestFalse(TEXT("Committed mutation is not cancelled during module shutdown"), ShouldCancelForModuleShutdown(Phase));
	Phase->MarkShutdown();
	TestTrue(TEXT("Committed mutation records shutdown for reload UI bypass"), Phase->IsShutdownStarted());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStandaloneGameThreadUiLifetimeTest, "Cthulhu.GitSourceControl.Standalone.GameThreadUiLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

struct FGitSourceControlMenuUiLifetimeTestProbe
{
	TAtomic<int32> DestructionCount = 0;
	TAtomic<bool> bDestroyedOnGameThread = false;
	TAtomic<bool> bWorkerPinnedUiState = false;
	TAtomic<bool> bDispatcherWorkExecuted = false;
	TAtomic<bool> bWorkerObservedCancellation = false;
	TAtomic<bool> bWorkerPassedCommitGate = false;
};

class FGitSourceControlMenuUiLifetimeTestState final : public FGitSourceControlMenuGameThreadUiState
{
public:
	explicit FGitSourceControlMenuUiLifetimeTestState(const TSharedRef<FGitSourceControlMenuUiLifetimeTestProbe, ESPMode::ThreadSafe>& InProbe)
		: Probe(InProbe)
	{
	}

	virtual ~FGitSourceControlMenuUiLifetimeTestState() override
	{
		Probe->bDestroyedOnGameThread.Store(IsInGameThread());
		++Probe->DestructionCount;
	}

private:
	TSharedRef<FGitSourceControlMenuUiLifetimeTestProbe, ESPMode::ThreadSafe> Probe;
};

bool FGitStandaloneGameThreadUiLifetimeTest::RunTest(const FString& Parameters)
{
	if (!IsInGameThread())
	{
		AddError(TEXT("The Slate lifetime regression test must run on the GameThread."));
		return false;
	}

	{
		FEvent* const WorkerCompletedEvent = FPlatformProcess::GetSynchEventFromPool(true);
		ON_SCOPE_EXIT
		{
			FPlatformProcess::ReturnSynchEventToPool(WorkerCompletedEvent);
		};

		const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe> LifetimeState =
			MakeShared<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>();
		const TSharedRef<FGitSourceControlMenuUiLifetimeTestProbe, ESPMode::ThreadSafe> Probe =
			MakeShared<FGitSourceControlMenuUiLifetimeTestProbe, ESPMode::ThreadSafe>();
		TSharedPtr<FGitSourceControlMenuUiLifetimeTestState, ESPMode::ThreadSafe> UiState =
			MakeShared<FGitSourceControlMenuUiLifetimeTestState, ESPMode::ThreadSafe>(Probe);
		const TWeakPtr<FGitSourceControlMenuUiLifetimeTestState, ESPMode::ThreadSafe> WeakUiState = UiState;
		TestTrue(TEXT("Normal completion tracks GameThread UI state"), LifetimeState->TrackGameThreadUiState(
			StaticCastSharedRef<FGitSourceControlMenuGameThreadUiState>(UiState.ToSharedRef())));
		TestTrue(TEXT("Normal completion begins its tracked worker task"), LifetimeState->TryBeginTask());
		TSharedPtr<FGitSourceControlMenuTask, ESPMode::ThreadSafe> WorkerTask =
			MakeShared<FGitSourceControlMenuTask, ESPMode::ThreadSafe>(LifetimeState);
		UiState.Reset();
		Async(EAsyncExecution::ThreadPool, [WeakUiState, Probe, WorkerTask = MoveTemp(WorkerTask), WorkerCompletedEvent]() mutable
		{
			TSharedPtr<FGitSourceControlMenuUiLifetimeTestState, ESPMode::ThreadSafe> WorkerPinnedUiState = WeakUiState.Pin();
			Probe->bWorkerPinnedUiState.Store(WorkerPinnedUiState.IsValid());
			WorkerPinnedUiState.Reset();
			WorkerTask.Reset();
			WorkerCompletedEvent->Trigger();
		});
		const bool bNormalWorkerFinished = WorkerCompletedEvent->Wait(5000);
		TestTrue(TEXT("Normal completion worker finished"), bNormalWorkerFinished);
		if (!bNormalWorkerFinished)
		{
			LifetimeState->StopAcceptingAndWait();
			WorkerCompletedEvent->Wait();
			return false;
		}
		TestTrue(TEXT("Normal completion worker received only a temporary weak pin"), Probe->bWorkerPinnedUiState.Load());
		TestEqual(TEXT("Normal completion leaves no active worker task"), LifetimeState->GetActiveTaskCountForTesting(), 0);
		TestEqual(TEXT("Normal completion keeps UI state alive until GameThread untracks it"), Probe->DestructionCount.Load(), 0);
		TSharedPtr<FGitSourceControlMenuUiLifetimeTestState, ESPMode::ThreadSafe> UiStateOwnedByGameThread = WeakUiState.Pin();
		TestTrue(TEXT("Normal completion can pin GameThread-owned UI state"), UiStateOwnedByGameThread.IsValid());
		if (UiStateOwnedByGameThread.IsValid())
		{
			LifetimeState->UntrackGameThreadUiState(StaticCastSharedRef<FGitSourceControlMenuGameThreadUiState>(UiStateOwnedByGameThread.ToSharedRef()));
		}
		UiStateOwnedByGameThread.Reset();
		TestEqual(TEXT("Normal completion destroys UI state exactly once"), Probe->DestructionCount.Load(), 1);
		TestTrue(TEXT("Normal completion destroys UI state on GameThread"), Probe->bDestroyedOnGameThread.Load());
		TestEqual(TEXT("Normal completion clears UI registry"), LifetimeState->GetTrackedGameThreadUiStateCountForTesting(), 0);
	}

	{
		FEvent* const WorkerCompletedEvent = FPlatformProcess::GetSynchEventFromPool(true);
		ON_SCOPE_EXIT
		{
			FPlatformProcess::ReturnSynchEventToPool(WorkerCompletedEvent);
		};

		const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe> LifetimeState =
			MakeShared<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>();
		const TSharedRef<FGitSourceControlMenuUiLifetimeTestProbe, ESPMode::ThreadSafe> Probe =
			MakeShared<FGitSourceControlMenuUiLifetimeTestProbe, ESPMode::ThreadSafe>();
		TSharedPtr<FGitSourceControlMenuUiLifetimeTestState, ESPMode::ThreadSafe> UiState =
			MakeShared<FGitSourceControlMenuUiLifetimeTestState, ESPMode::ThreadSafe>(Probe);
		const TWeakPtr<FGitSourceControlMenuUiLifetimeTestState, ESPMode::ThreadSafe> WeakUiState = UiState;
		TestTrue(TEXT("Dispatcher rejection tracks GameThread UI state"), LifetimeState->TrackGameThreadUiState(
			StaticCastSharedRef<FGitSourceControlMenuGameThreadUiState>(UiState.ToSharedRef())));
		TestTrue(TEXT("Dispatcher rejection begins its tracked worker task"), LifetimeState->TryBeginTask());
		const TSharedRef<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher =
			MakeShared<FGitSourceControlMenuGameThreadDispatcher, ESPMode::ThreadSafe>();
		TestTrue(TEXT("Dispatcher rejection tracks dispatcher"), LifetimeState->TrackDispatcher(Dispatcher));
		TSharedPtr<FGitSourceControlMenuTask, ESPMode::ThreadSafe> WorkerTask =
			MakeShared<FGitSourceControlMenuTask, ESPMode::ThreadSafe>(LifetimeState);
		UiState.Reset();
		Async(EAsyncExecution::ThreadPool, [Dispatcher, WeakUiState, Probe, WorkerTask = MoveTemp(WorkerTask), WorkerCompletedEvent]() mutable
		{
			const bool bInvocationResult = Dispatcher->InvokeAndWait([WeakUiState, Probe]()
			{
				Probe->bDispatcherWorkExecuted.Store(true);
				return WeakUiState.IsValid();
			});
			Probe->bWorkerObservedCancellation.Store(!bInvocationResult);
			WorkerTask.Reset();
			WorkerCompletedEvent->Trigger();
		});
		LifetimeState->StopAcceptingAndWait();
		const bool bRejectedWorkerFinished = WorkerCompletedEvent->Wait(5000);
		TestTrue(TEXT("Dispatcher rejection worker finished"), bRejectedWorkerFinished);
		if (!bRejectedWorkerFinished)
		{
			WorkerCompletedEvent->Wait();
			return false;
		}
		TestTrue(TEXT("Closed dispatcher rejects worker completion"), Probe->bWorkerObservedCancellation.Load());
		TestFalse(TEXT("Closed dispatcher never executes GameThread work"), Probe->bDispatcherWorkExecuted.Load());
		TestEqual(TEXT("Dispatcher rejection destroys UI state exactly once"), Probe->DestructionCount.Load(), 1);
		TestTrue(TEXT("Dispatcher rejection destroys UI state on GameThread"), Probe->bDestroyedOnGameThread.Load());
		TestEqual(TEXT("Dispatcher rejection clears worker tasks"), LifetimeState->GetActiveTaskCountForTesting(), 0);
		TestEqual(TEXT("Dispatcher rejection clears UI registry"), LifetimeState->GetTrackedGameThreadUiStateCountForTesting(), 0);
	}

	{
		FEvent* const WorkerCompletedEvent = FPlatformProcess::GetSynchEventFromPool(true);
		ON_SCOPE_EXIT
		{
			FPlatformProcess::ReturnSynchEventToPool(WorkerCompletedEvent);
		};

		const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe> LifetimeState =
			MakeShared<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>();
		const TSharedRef<FGitSourceControlMenuUiLifetimeTestProbe, ESPMode::ThreadSafe> Probe =
			MakeShared<FGitSourceControlMenuUiLifetimeTestProbe, ESPMode::ThreadSafe>();
		const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
			MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
		const TSharedRef<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe> MutationPhase =
			MakeShared<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe>();
		TSharedPtr<FGitSourceControlMenuUiLifetimeTestState, ESPMode::ThreadSafe> UiState =
			MakeShared<FGitSourceControlMenuUiLifetimeTestState, ESPMode::ThreadSafe>(Probe);
		TestTrue(TEXT("Pre-commit shutdown tracks GameThread UI state"), LifetimeState->TrackGameThreadUiState(
			StaticCastSharedRef<FGitSourceControlMenuGameThreadUiState>(UiState.ToSharedRef())));
		TestTrue(TEXT("Pre-commit shutdown tracks cancellation context"), LifetimeState->TrackCancellationContext(CancellationContext, MutationPhase));
		TestTrue(TEXT("Pre-commit shutdown begins its tracked worker task"), LifetimeState->TryBeginTask());
		TSharedPtr<FGitSourceControlMenuTask, ESPMode::ThreadSafe> WorkerTask =
			MakeShared<FGitSourceControlMenuTask, ESPMode::ThreadSafe>(LifetimeState);
		UiState.Reset();
		Async(EAsyncExecution::ThreadPool, [CancellationContext, Probe, WorkerTask = MoveTemp(WorkerTask), WorkerCompletedEvent]() mutable
		{
			for (int32 Attempt = 0; Attempt < 5000 && !CancellationContext->IsCancellationRequested(); ++Attempt)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			Probe->bWorkerObservedCancellation.Store(CancellationContext->IsCancellationRequested());
			WorkerTask.Reset();
			WorkerCompletedEvent->Trigger();
		});
		LifetimeState->StopAcceptingAndWait();
		const bool bPreCommitWorkerFinished = WorkerCompletedEvent->Wait(5000);
		TestTrue(TEXT("Pre-commit shutdown worker finished"), bPreCommitWorkerFinished);
		if (!bPreCommitWorkerFinished)
		{
			WorkerCompletedEvent->Wait();
			return false;
		}
		TestTrue(TEXT("Pre-commit shutdown cancels the worker context"), Probe->bWorkerObservedCancellation.Load());
		TestTrue(TEXT("Pre-commit shutdown records mutation shutdown"), MutationPhase->IsShutdownStarted());
		TestEqual(TEXT("Pre-commit shutdown destroys UI state exactly once"), Probe->DestructionCount.Load(), 1);
		TestTrue(TEXT("Pre-commit shutdown destroys UI state on GameThread"), Probe->bDestroyedOnGameThread.Load());
		TestEqual(TEXT("Pre-commit shutdown clears worker tasks"), LifetimeState->GetActiveTaskCountForTesting(), 0);
		TestEqual(TEXT("Pre-commit shutdown clears UI registry"), LifetimeState->GetTrackedGameThreadUiStateCountForTesting(), 0);
	}

	{
		FEvent* const WorkerCompletedEvent = FPlatformProcess::GetSynchEventFromPool(true);
		FEvent* const CommitGateEvent = FPlatformProcess::GetSynchEventFromPool(true);
		FEvent* const GateHelperCompletedEvent = FPlatformProcess::GetSynchEventFromPool(true);
		ON_SCOPE_EXIT
		{
			FPlatformProcess::ReturnSynchEventToPool(GateHelperCompletedEvent);
			FPlatformProcess::ReturnSynchEventToPool(CommitGateEvent);
			FPlatformProcess::ReturnSynchEventToPool(WorkerCompletedEvent);
		};

		const TSharedRef<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe> LifetimeState =
			MakeShared<FGitSourceControlMenuLifetimeState, ESPMode::ThreadSafe>();
		const TSharedRef<FGitSourceControlMenuUiLifetimeTestProbe, ESPMode::ThreadSafe> Probe =
			MakeShared<FGitSourceControlMenuUiLifetimeTestProbe, ESPMode::ThreadSafe>();
		const TSharedRef<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe> CancellationContext =
			MakeShared<GitSourceControlUtils::FGitOperationCancellationContext, ESPMode::ThreadSafe>();
		const TSharedRef<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe> MutationPhase =
			MakeShared<FGitSourceControlAssetMutationPhaseState, ESPMode::ThreadSafe>();
		MutationPhase->EnterCommitPhase();
		TSharedPtr<FGitSourceControlMenuUiLifetimeTestState, ESPMode::ThreadSafe> UiState =
			MakeShared<FGitSourceControlMenuUiLifetimeTestState, ESPMode::ThreadSafe>(Probe);
		TestTrue(TEXT("Commit shutdown tracks GameThread UI state"), LifetimeState->TrackGameThreadUiState(
			StaticCastSharedRef<FGitSourceControlMenuGameThreadUiState>(UiState.ToSharedRef())));
		TestTrue(TEXT("Commit shutdown tracks cancellation context"), LifetimeState->TrackCancellationContext(CancellationContext, MutationPhase));
		TestTrue(TEXT("Commit shutdown begins its tracked worker task"), LifetimeState->TryBeginTask());
		TSharedPtr<FGitSourceControlMenuTask, ESPMode::ThreadSafe> WorkerTask =
			MakeShared<FGitSourceControlMenuTask, ESPMode::ThreadSafe>(LifetimeState);
		UiState.Reset();
		Async(EAsyncExecution::ThreadPool, [CancellationContext, Probe, WorkerTask = MoveTemp(WorkerTask), WorkerCompletedEvent, CommitGateEvent]() mutable
		{
			Probe->bWorkerPassedCommitGate.Store(CommitGateEvent->Wait(5000));
			Probe->bWorkerObservedCancellation.Store(CancellationContext->IsCancellationRequested());
			WorkerTask.Reset();
			WorkerCompletedEvent->Trigger();
		});
		Async(EAsyncExecution::ThreadPool, [MutationPhase, CommitGateEvent, GateHelperCompletedEvent]()
		{
			for (int32 Attempt = 0; Attempt < 5000 && !MutationPhase->IsShutdownStarted(); ++Attempt)
			{
				FPlatformProcess::SleepNoStats(0.001f);
			}
			if (MutationPhase->IsShutdownStarted())
			{
				CommitGateEvent->Trigger();
			}
			GateHelperCompletedEvent->Trigger();
		});
		LifetimeState->StopAcceptingAndWait();
		const bool bCommitWorkerFinished = WorkerCompletedEvent->Wait(5000);
		const bool bCommitGateHelperFinished = GateHelperCompletedEvent->Wait(5000);
		TestTrue(TEXT("Commit shutdown worker finished"), bCommitWorkerFinished);
		TestTrue(TEXT("Commit shutdown gate helper finished"), bCommitGateHelperFinished);
		if (!bCommitWorkerFinished || !bCommitGateHelperFinished)
		{
			if (!bCommitWorkerFinished)
			{
				WorkerCompletedEvent->Wait();
			}
			if (!bCommitGateHelperFinished)
			{
				GateHelperCompletedEvent->Wait();
			}
			return false;
		}
		TestTrue(TEXT("Commit shutdown lets the commit worker complete"), Probe->bWorkerPassedCommitGate.Load());
		TestFalse(TEXT("Commit shutdown does not cancel the worker context"), Probe->bWorkerObservedCancellation.Load());
		TestTrue(TEXT("Commit shutdown records mutation shutdown"), MutationPhase->IsShutdownStarted());
		TestEqual(TEXT("Commit shutdown destroys UI state exactly once"), Probe->DestructionCount.Load(), 1);
		TestTrue(TEXT("Commit shutdown destroys UI state on GameThread"), Probe->bDestroyedOnGameThread.Load());
		TestEqual(TEXT("Commit shutdown clears worker tasks"), LifetimeState->GetActiveTaskCountForTesting(), 0);
		TestEqual(TEXT("Commit shutdown clears UI registry"), LifetimeState->GetTrackedGameThreadUiStateCountForTesting(), 0);
	}

	return true;
}
#endif

#undef LOCTEXT_NAMESPACE
