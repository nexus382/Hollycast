<!-- Portions Copyright 2026 The Hollycast Authors -->
# Starred Hollycast game-list previews

The six `preview-*` packages are original JSON implementations of the requested Hollycast mockups. They contain no upstream images, fonts, XML, logos or generated media. The JSON layouts use Hollycast's GPL-2.0-or-later license. Game art is loaded from the user's configured local ES-DE media and remains subject to its own rights.

Visual references were the game-list views in the [ES-DE theme catalog](https://gitlab.com/es-de/themes/themes-list/): Iconic by Siddy212, Horizon, Epic Noir (Revisited), and Art Book Next. The existing Iconic and Art Book Next ports retain their separate notices in their own directories. Reference theme names identify the requested design direction; these previews are not endorsed or supplied by those theme authors.

Only internal game lists are covered. The application retains its existing toolbar, filters, favourites and per-game actions. No console picker, video player, ratings, new scraping source or hardcoded sample game data is introduced. A `*` in each display name distinguishes these layouts for local review.

Both Iconic variants follow the highlighted game's miximage. Browser cover grids use front covers independently of the user's primary cover-media setting. `title` binds marquee/wheel media; `physical` binds disc/cartridge art. Missing screenshot and fanart slots try miximage before the primary artwork. Missing disc and marquee slots stay empty. Dates use Hollycast's existing parsed date formatter, and player counts come from the gamelist metadata. Standalone platform labels and fixed Dreamcast logos are omitted.
