// Portions Copyright 2026 The Hollycast Authors
package com.hollycast.emulator;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.content.pm.PackageInfo;
import com.google.android.play.core.appupdate.AppUpdateInfo;
import com.google.android.play.core.appupdate.AppUpdateManager;
import com.google.android.play.core.appupdate.AppUpdateManagerFactory;
import com.google.android.play.core.appupdate.AppUpdateOptions;
import com.google.android.play.core.install.model.AppUpdateType;
import com.google.android.play.core.install.model.UpdateAvailability;
import java.lang.ref.WeakReference;
import org.json.JSONObject;

/** Play owns availability, consent, signing, downloading and installation for store builds. */
final class PlayAppUpdater {
    private static final int UPDATE_REQUEST = 19581;
    private static AppUpdateManager manager;
    private static WeakReference<Activity> foreground = new WeakReference<>(null);
    private static volatile String snapshot = BuildConfig.GITHUB_UPDATES
            ? "{\"source\":\"github\"}" : "{\"source\":\"play\"}";
    // These fields are accessed only on the Android UI thread. Native rendering reads snapshot.
    private static boolean checking, launching;
    private static long revision;
    private static int availableCode;
    private static long installedCode;
    private static String installedName = "", error = "", message = "";

    private static void publish() {
        try {
            JSONObject value = new JSONObject();
            value.put("revision", ++revision);
            value.put("source", "play"); value.put("checking", checking); value.put("launching", launching);
            value.put("availableCode", availableCode); value.put("installedCode", installedCode);
            value.put("installedName", installedName); value.put("error", error); value.put("message", message);
            snapshot = value.toString();
        } catch (org.json.JSONException e) { throw new IllegalStateException(e); }
    }

    static void resume(Activity activity) {
        if (BuildConfig.GITHUB_UPDATES) return;
        foreground = new WeakReference<>(activity);
        if (manager == null) {
            manager = AppUpdateManagerFactory.create(activity.getApplicationContext());
            try {
                PackageInfo info = activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0);
                installedCode = Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
                installedName = info.versionName;
            } catch (android.content.pm.PackageManager.NameNotFoundException e) { error = "Cannot read the installed version."; }
            publish();
        }
        // Resume only an update already consented to. A normal startup query is initiated
        // by the native screen so the user's Check on startup preference is respected.
        manager.getAppUpdateInfo().addOnSuccessListener(info -> {
            if (info.updateAvailability() == UpdateAvailability.DEVELOPER_TRIGGERED_UPDATE_IN_PROGRESS) {
                launching = false;
                begin(info);
            }
        });
    }

    static void pause(Activity activity) {
        if (foreground.get() == activity) foreground.clear();
    }

    static String control(Activity activity, String action) {
        if (BuildConfig.GITHUB_UPDATES || action.isEmpty()) return snapshot;
        activity.runOnUiThread(() -> {
            if (manager == null) resume(activity);
            if ("check".equals(action)) query(false);
            else if ("install".equals(action)) query(true);
            else if ("testing".equals(action)) {
                try { activity.startActivity(new Intent(Intent.ACTION_VIEW,
                        Uri.parse("https://play.google.com/apps/testing/" + activity.getPackageName()))); }
                catch (RuntimeException e) { error = "Cannot open Google Play testing enrollment."; publish(); }
            }
        });
        return snapshot;
    }

    private static void query(boolean install) {
        if (checking || launching) return;
        checking = true; error = ""; message = ""; publish();
        manager.getAppUpdateInfo().addOnSuccessListener(info -> {
            checking = false;
            availableCode = info.updateAvailability() == UpdateAvailability.UPDATE_AVAILABLE
                    || info.updateAvailability() == UpdateAvailability.DEVELOPER_TRIGGERED_UPDATE_IN_PROGRESS
                    ? info.availableVersionCode() : 0;
            if (install && availableCode > 0) begin(info);
            else { if (availableCode == 0) message = "Google Play has no newer version available for your account."; publish(); }
        }).addOnFailureListener(e -> {
            checking = false; availableCode = 0;
            error = "Cannot check Google Play. Check your connection and that this app was installed through Google Play.";
            publish();
        });
    }

    private static void begin(AppUpdateInfo info) {
        Activity activity = foreground.get();
        if (activity == null || activity.isFinishing() || activity.isDestroyed() || launching) {
            // Availability can return after Android pauses/destroys the Activity. Publish
            // checking=false so returning to the screen offers a fresh, consented retry.
            message = "Return to Hollycast and press Update to continue.";
            publish();
            return;
        }
        try {
            if (info.isUpdateTypeAllowed(AppUpdateType.IMMEDIATE)) {
                launching = manager.startUpdateFlowForResult(info, activity,
                        AppUpdateOptions.newBuilder(AppUpdateType.IMMEDIATE).build(), UPDATE_REQUEST);
                if (!launching) error = "Google Play could not start this update. Please retry.";
            } else {
                // Play can disallow the in-app flow for this account/device. Stay in Play;
                // never fall back to downloading an APK from a different distribution.
                activity.startActivity(new Intent(Intent.ACTION_VIEW,
                        Uri.parse("https://play.google.com/store/apps/details?id=" + activity.getPackageName())));
                message = "Complete the update in Google Play.";
            }
        } catch (Exception e) { launching = false; error = "Google Play could not start this update. Please retry."; }
        publish();
    }

    static boolean result(int request, int result) {
        if (request != UPDATE_REQUEST) return false;
        launching = false;
        message = result == Activity.RESULT_CANCELED ? "Update cancelled. You can keep using Hollycast." : "";
        error = result != Activity.RESULT_OK && result != Activity.RESULT_CANCELED
                ? "Google Play could not finish the update. Please retry." : "";
        publish();
        return true;
    }
}
