// Portions Copyright 2026 The Hollycast Authors
#pragma once

namespace readiness {

// These facts are supplied by the UI so the checklist remains independent of
// platform-specific device enumeration and filesystem APIs.
struct Inputs {
	bool hasGameFolder = false;
	bool hasBios = false;
	bool hasController = false;
	bool keyboardAvailable = false;
	bool touchAvailable = false;
};

enum class Status { Ready, Attention, Optional };

struct Checklist {
	Status games;
	Status bios;
	Status controls;
};

inline Checklist evaluate(const Inputs& inputs)
{
	return {
		inputs.hasGameFolder ? Status::Ready : Status::Attention,
		// Dreamcast games generally fall back to HLE when a BIOS is absent.
		inputs.hasBios ? Status::Ready : Status::Optional,
		(inputs.hasController || inputs.keyboardAvailable || inputs.touchAvailable)
			? Status::Ready : Status::Attention,
	};
}

} // namespace readiness
