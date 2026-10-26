// Portions Copyright 2026 The Hollycast Authors
#pragma once

// Tracks why the checklist was requested. Persistence and ImGui remain owned by
// the caller so the same transitions are used for both automatic and manual UI.
class SetupChecklistLifecycle
{
public:
	void libraryReady(bool pendingFirstRun)
	{
		if (startupChecked)
			return;
		startupChecked = true;
		if (pendingFirstRun && request == Request::None)
			request = Request::Automatic;
	}

	void requestManual() { request = Request::Manual; }

	// Called only after a launch has been accepted. Persist the end of
	// automatic first-run eligibility, without discarding a manual request.
	template <typename SavePending>
	void gameLaunchStarted(bool pendingFirstRun, SavePending savePending)
	{
		startupChecked = true;
		if (request == Request::Automatic)
			request = Request::None;
		if (active == Request::Automatic)
		{
			active = Request::None;
			closeAutomaticPopup = true;
		}
		if (pendingFirstRun)
			savePending(false);
	}

	template <typename SavePending>
	void dismissed(SavePending savePending)
	{
		request = Request::None;
		active = Request::None;
		closeAutomaticPopup = false;
		savePending(false);
	}

	// Another popup can replace the checklist after it has opened. Preserve
	// its request until the user finishes or launches a game.
	void observePopup(bool checklistOpen)
	{
		if (!checklistOpen)
			closeAutomaticPopup = false;
		if (active != Request::None && !checklistOpen)
		{
			if (request == Request::None)
				request = active;
			active = Request::None;
		}
	}

	bool takeAutomaticCloseRequest()
	{
		const bool close = closeAutomaticPopup;
		closeAutomaticPopup = false;
		return close;
	}

	bool takeOpenRequest(bool anotherPopupOpen)
	{
		if (request == Request::None || anotherPopupOpen)
			return false;
		active = request;
		request = Request::None;
		return true;
	}

private:
	enum class Request { None, Automatic, Manual };
	Request request = Request::None;
	Request active = Request::None;
	bool startupChecked = false;
	bool closeAutomaticPopup = false;
};
