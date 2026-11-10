<!-- Portions Copyright 2026 The Hollycast Authors -->

# CD Rack source notes

Original Hollycast layout, GPL-2.0-or-later. The shelves, jewel-case edges and
fallback spine labels are drawn by Hollycast. No third-party theme assets or
sample game artwork are bundled.

Spines come from local `spines` / `box-2d-side` folders or ScreenScraper's
`box-2D-side` media. Missing spines display a vertical game title. Horizontal
spine scans rotate to fit; front covers are never cropped into fake spines.
Scans fill their case height while preserving their image proportions.
Wider PAL spines receive proportionally wider slots rather than shorter artwork;
shelves wrap when full. Library Icon Size resizes the cases, shelf spacing,
cover and game information; the same Library Scale control is in Library theme
settings. Cases stay inside the shelf viewport. Larger details can scroll.
General UI Scaling does not change the library's 100% UI baseline.

When a game is launched, its case pulls out and turns toward the player before
loading. A ScreenScraper `box-texture` supplies the wrapped artwork; otherwise
the case uses the existing cover and spine. Only downloaded artwork is used.

The selected game's mix image sits behind its front cover on the right. Cover
art is the fallback when a mix image is unavailable. Mouse hover and keyboard
or controller navigation update the artwork and metadata together. All shelves
scroll, and normal launch, favourites and context-menu actions remain available.
