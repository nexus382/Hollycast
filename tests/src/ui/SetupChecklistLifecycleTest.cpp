// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/setup_checklist_lifecycle.h"

TEST(SetupChecklistLifecycle, FirstRunOpensOnceAndDismissalPersists)
{
	bool persistedPending = true;
	SetupChecklistLifecycle firstSession;
	firstSession.libraryReady(persistedPending);
	EXPECT_TRUE(firstSession.takeOpenRequest(false));
	EXPECT_FALSE(firstSession.takeOpenRequest(false));
	firstSession.dismissed([&](bool pending) { persistedPending = pending; });
	EXPECT_FALSE(persistedPending);
	SetupChecklistLifecycle nextSession;
	nextSession.libraryReady(persistedPending);
	EXPECT_FALSE(nextSession.takeOpenRequest(false));
}

TEST(SetupChecklistLifecycle, AutomaticRequestWaitsForAnotherPopup)
{
	SetupChecklistLifecycle lifecycle;
	lifecycle.libraryReady(true);
	EXPECT_FALSE(lifecycle.takeOpenRequest(true));
	EXPECT_FALSE(lifecycle.takeOpenRequest(true));
	EXPECT_TRUE(lifecycle.takeOpenRequest(false));
	EXPECT_FALSE(lifecycle.takeOpenRequest(false));
}

TEST(SetupChecklistLifecycle, DirectLaunchPreventsPromptOnReturnAndRestart)
{
	bool persistedPending = true;
	SetupChecklistLifecycle lifecycle;
	lifecycle.gameLaunchStarted(persistedPending, [&](bool pending) { persistedPending = pending; });
	EXPECT_FALSE(persistedPending);
	lifecycle.libraryReady(persistedPending);
	EXPECT_FALSE(lifecycle.takeOpenRequest(false));
	SetupChecklistLifecycle nextSession;
	nextSession.libraryReady(persistedPending);
	EXPECT_FALSE(nextSession.takeOpenRequest(false));
}

TEST(SetupChecklistLifecycle, LaunchCancelsQueuedAutomaticRequest)
{
	SetupChecklistLifecycle lifecycle;
	lifecycle.libraryReady(true);
	EXPECT_FALSE(lifecycle.takeOpenRequest(true));
	bool persistedPending = true;
	lifecycle.gameLaunchStarted(persistedPending, [&](bool pending) { persistedPending = pending; });
	EXPECT_FALSE(persistedPending);
	EXPECT_FALSE(lifecycle.takeOpenRequest(false));
	lifecycle.libraryReady(false);
	EXPECT_FALSE(lifecycle.takeOpenRequest(false));
}

TEST(SetupChecklistLifecycle, ManualOpeningStillWorksAfterLaunchAndWaitsForDialogs)
{
	SetupChecklistLifecycle lifecycle;
	bool persistedPending = true;
	lifecycle.gameLaunchStarted(persistedPending, [&](bool pending) { persistedPending = pending; });
	EXPECT_FALSE(persistedPending);
	lifecycle.requestManual();
	EXPECT_FALSE(lifecycle.takeOpenRequest(true));
	EXPECT_TRUE(lifecycle.takeOpenRequest(false));
	lifecycle.dismissed([](bool) {});
	lifecycle.requestManual();
	EXPECT_TRUE(lifecycle.takeOpenRequest(false));
}

TEST(SetupChecklistLifecycle, ManualRequestIsNotDiscardedByLaunch)
{
	SetupChecklistLifecycle lifecycle;
	lifecycle.libraryReady(true);
	lifecycle.requestManual();
	bool persistedPending = true;
	lifecycle.gameLaunchStarted(persistedPending, [&](bool pending) { persistedPending = pending; });
	EXPECT_FALSE(persistedPending);
	EXPECT_TRUE(lifecycle.takeOpenRequest(false));
}

TEST(SetupChecklistLifecycle, ExistingProfileDoesNotOpenAutomatically)
{
	SetupChecklistLifecycle lifecycle;
	lifecycle.libraryReady(false);
	EXPECT_FALSE(lifecycle.takeOpenRequest(false));
	bool saveCalled = false;
	lifecycle.gameLaunchStarted(false, [&](bool) { saveCalled = true; });
	EXPECT_FALSE(saveCalled);
	lifecycle.requestManual();
	EXPECT_TRUE(lifecycle.takeOpenRequest(false));
}

TEST(SetupChecklistLifecycle, LaunchClosesAnAlreadyOpenAutomaticChecklist)
{
	SetupChecklistLifecycle lifecycle;
	lifecycle.libraryReady(true);
	ASSERT_TRUE(lifecycle.takeOpenRequest(false));
	bool pending = true;
	lifecycle.gameLaunchStarted(pending, [&](bool value) { pending = value; });
	EXPECT_FALSE(pending);
	lifecycle.observePopup(true);
	EXPECT_TRUE(lifecycle.takeAutomaticCloseRequest());
	lifecycle.observePopup(false);
	EXPECT_FALSE(lifecycle.takeOpenRequest(false));
}

TEST(SetupChecklistLifecycle, InterruptedAutomaticPopupDoesNotCloseLaterManualRequest)
{
	SetupChecklistLifecycle lifecycle;
	lifecycle.libraryReady(true);
	ASSERT_TRUE(lifecycle.takeOpenRequest(false));
	lifecycle.gameLaunchStarted(true, [](bool) {});
	lifecycle.observePopup(false);
	lifecycle.requestManual();
	EXPECT_TRUE(lifecycle.takeOpenRequest(false));
	EXPECT_FALSE(lifecycle.takeAutomaticCloseRequest());
}
