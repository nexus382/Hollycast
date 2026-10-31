<!-- Portions Copyright 2026 The Hollycast Authors -->
# Starred Hollycast game-list previews

The remaining `preview-*` packages are original JSON implementations of the requested Hollycast layouts. They contain no upstream images, fonts, XML, logos or generated media. The JSON layouts use Hollycast's GPL-2.0-or-later license. Game art is loaded from the user's configured local ES-DE media or ScreenScraper cache and remains subject to its own rights.

Visual references were the game-list views in the [ES-DE theme catalog](https://gitlab.com/es-de/themes/themes-list/): Iconic by Siddy212, Horizon, Epic Noir (Revisited), and Art Book Next. Reference theme names identify the requested design direction; these previews are not endorsed or supplied by those theme authors.

Only internal game lists are covered. The application retains its existing toolbar, filters, favourites and per-game actions. The layouts add no console picker, video player, ratings or hardcoded sample game data. A `*` in each display name distinguishes these layouts for local review.

CD Rack is an original shelf layout: local or downloaded spine scans on the left, and the selected game's mix image, front cover and metadata on the right. Missing spine scans get vertical title labels. The shelf browser supports the same mouse, controller, launch and context-menu actions as the other layouts.

`title` binds marquee/wheel media; `physical` binds disc/cartridge art. Missing screenshot and fanart slots try miximage before the primary artwork. Missing disc and marquee slots stay empty. Dates use Hollycast's existing parsed date formatter, and player counts come from the gamelist metadata. Standalone platform labels and fixed Dreamcast logos are omitted.
