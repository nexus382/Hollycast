// Portions Copyright 2026 The Hollycast Authors
package com.hollycast.emulator.emu;

import org.junit.Test;

import java.lang.reflect.Method;
import java.time.Instant;
import java.util.Locale;
import java.util.TimeZone;

import static org.junit.Assert.assertEquals;

public class LocaleUtilsTest {
    @Test
    public void datesFollowDeviceZoneIncludingDstAndZoneChanges() throws Exception {
        Locale originalLocale = Locale.getDefault();
        TimeZone originalZone = TimeZone.getDefault();
        Method formatter = LocaleUtils.class.getDeclaredMethod("formatShortDateTime", long.class);
        formatter.setAccessible(true);
        try {
            Locale.setDefault(Locale.US);
            String[][] cases = {
                    {"America/New_York", "2026-01-15T02:30:00Z", "1/14/26, 9:30 PM"},
                    {"America/New_York", "2026-07-15T02:30:00Z", "7/14/26, 10:30 PM"},
                    {"America/New_York", "2026-03-08T06:59:00Z", "3/8/26, 1:59 AM"},
                    {"America/New_York", "2026-03-08T07:00:00Z", "3/8/26, 3:00 AM"},
                    {"Asia/Kolkata", "2026-01-15T02:30:00Z", "1/15/26, 8:00 AM"},
                    {"Asia/Tokyo", "2026-01-15T20:30:00Z", "1/16/26, 5:30 AM"},
                    {"UTC", "2026-01-15T02:30:00Z", "1/15/26, 2:30 AM"}
            };
            for (String[] test : cases) {
                TimeZone.setDefault(TimeZone.getTimeZone(test[0]));
                String actual = (String) formatter.invoke(null, Instant.parse(test[1]).getEpochSecond());
                // JDK locale data may use a thin space before AM/PM, as Android does.
                assertEquals(test[0] + " at " + test[1], test[2], actual.replace('\u2009', ' '));
            }
        } finally {
            TimeZone.setDefault(originalZone);
            Locale.setDefault(originalLocale);
        }
    }
}
