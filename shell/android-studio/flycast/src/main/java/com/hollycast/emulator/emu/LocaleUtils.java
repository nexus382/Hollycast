/*
	Copyright 2025 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
package com.hollycast.emulator.emu;

import java.time.Instant;
import java.time.ZoneId;
import java.time.format.DateTimeFormatter;
import java.time.format.FormatStyle;
import java.util.Locale;

public class LocaleUtils {
    // called by native code
    private static String formatShortDateTime(long t) {
        // Resolve the device zone on every call so changes take effect without restarting.
        String s = Instant.ofEpochSecond(t).atZone(ZoneId.systemDefault())
                .format(DateTimeFormatter.ofLocalizedDateTime(FormatStyle.SHORT));
        // 'Narrow No-Break Space' isn't defined in our imgui font. Replace with 'Thin Space'
        s = s.replace('\u202F', '\u2009');
        return s;
    }

    // called by native code
    private static String getCurrentLocale() {
        return Locale.getDefault().toString();
    }

}
