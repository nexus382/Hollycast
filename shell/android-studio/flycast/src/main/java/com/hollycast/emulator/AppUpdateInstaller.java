// Portions Copyright 2026 The Hollycast Authors
package com.hollycast.emulator;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.os.SystemClock;
import android.provider.Settings;
import androidx.core.content.FileProvider;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.ref.WeakReference;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Downloads only after Update is pressed; Android owns permission and package-signature checks. */
final class AppUpdateInstaller {
    private static final int INSTALL_REQUEST = 19582;
    private static final long LIMIT = 512L * 1024 * 1024;
    private static final AtomicBoolean busy = new AtomicBoolean();
    private static WeakReference<Activity> foreground = new WeakReference<>(null);
    private static File pendingApk;
    private static File pendingWork;
    private static File activeWork;
    private static File downloadingWork;

    static synchronized void resume(Activity activity) {
        foreground = new WeakReference<>(activity);
        if (BuildConfig.GITHUB_UPDATES) reclaimCompleted(activity.getApplicationContext());
        present();
    }

    static synchronized void pause(Activity activity) {
        if (foreground.get() == activity) foreground.clear();
    }

    private static void status(File work, String text) throws IOException {
        File temporary = new File(work, "status.tmp");
        try (FileOutputStream out = new FileOutputStream(temporary)) {
            out.write(text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            out.getFD().sync();
        }
        if (!temporary.renameTo(new File(work, "status"))) throw new IOException("Cannot publish update progress.");
    }

    private static void error(File work, Exception error) {
        try { status(work, "Error\n" + error.getMessage()); }
        catch (IOException ignored) { android.util.Log.e("HollycastUpdate", "Cannot publish update failure"); }
    }

    private static long versionCode(PackageInfo info) {
        return Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
    }

    private static void validateWork(Context context, File work) throws IOException {
        File root = new File(context.getFilesDir(), "app-updates").getCanonicalFile();
        if (!work.isDirectory() || !work.getName().startsWith("update-")
                || !root.equals(work.getCanonicalFile().getParentFile()))
            throw new IOException("Invalid update staging directory.");
    }

    private static void discardPayloads(File work, boolean includeApk) {
        // Keep progress markers until the native UI has consumed the terminal status.
        for (String name : new String[]{"package.zip", "inner-0.zip", "inner-1.zip", "inner-2.zip", "update.apk"}) {
            if (!includeApk && name.equals("update.apk")) continue;
            File file = new File(work, name);
            if (file.exists() && !file.delete())
                android.util.Log.w("HollycastUpdate", "Cannot remove staged update payload: " + name);
        }
    }

    private static void reclaimCompleted(Context context) {
        try {
            long current = versionCode(context.getPackageManager().getPackageInfo(context.getPackageName(), 0));
            File[] folders = new File(context.getFilesDir(), "app-updates").listFiles();
            if (folders == null) return;
            for (File work : folders) {
                if (work.equals(activeWork) || work.equals(pendingWork) || work.equals(downloadingWork)) continue;
                try {
                    validateWork(context, work);
                    File marker = new File(work, "source-version");
                    if (marker.length() == 0 || marker.length() > 32) continue;
                    try (java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(
                            new FileInputStream(marker), java.nio.charset.StandardCharsets.UTF_8))) {
                        long source = Long.parseLong(in.readLine());
                        File progress = new File(work, "status");
                        String state = "";
                        if (progress.length() > 0 && progress.length() <= 4096) {
                            try (java.io.BufferedReader status = new java.io.BufferedReader(new java.io.InputStreamReader(
                                    new FileInputStream(progress), java.nio.charset.StandardCharsets.UTF_8))) {
                                state = status.readLine();
                            }
                        }
                        // A killed download has no surviving worker. The system installer
                        // may still read an APK after process death, so retain that handoff
                        // until a newer installed version proves replacement completed.
                        if (current > source || (current == source && ("Downloading".equals(state)
                                || "Error".equals(state) || "Cancelled".equals(state)))) {
                            discardPayloads(work, true);
                            if ("Downloading".equals(state)) status(work, "Cancelled\nInterrupted download discarded. Please retry.");
                        }
                    }
                } catch (IOException | NumberFormatException ignored) {
                    // Unknown folders and incomplete markers are not ours to reclaim.
                }
            }
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            android.util.Log.w("HollycastUpdate", "Cannot inspect the installed update version");
        }
    }

    private static boolean ensureInstallPermission(Activity activity, File work) throws IOException {
        if (Build.VERSION.SDK_INT < 26 || activity.getPackageManager().canRequestPackageInstalls()) return true;
        status(work, "Error\nAllow Hollycast to install updates, then press Update again.");
        activity.runOnUiThread(() -> {
            synchronized (AppUpdateInstaller.class) {
                if (foreground.get() != activity || activity.isFinishing() || activity.isDestroyed()) return;
            }
            try {
                activity.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                        Uri.parse("package:" + activity.getPackageName())));
            } catch (Exception e) { error(work, e); }
        });
        return false;
    }

    private static synchronized void present() {
        Activity activity = foreground.get();
        if (activity == null || pendingApk == null) return;
        activity.runOnUiThread(() -> {
            final File apk, work;
            synchronized (AppUpdateInstaller.class) {
                // The Activity can pause or be recreated while this runnable waits on the UI thread.
                // Leave the payload pending until a live foreground Activity can actually use it.
                if (foreground.get() != activity || activity.isFinishing() || activity.isDestroyed()
                        || pendingApk == null) return;
                apk = pendingApk;
                work = pendingWork;
                pendingApk = null;
                pendingWork = null;
                activeWork = work;
            }
            boolean launched = false;
            try {
                checkCancelled(work);
                if (!ensureInstallPermission(activity, work)) return;
                Uri uri = FileProvider.getUriForFile(activity, activity.getPackageName() + ".provider", apk);
                Intent intent = new Intent(Intent.ACTION_INSTALL_PACKAGE);
                intent.setDataAndType(uri, "application/vnd.android.package-archive");
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                // Retain the APK while Android reads it, then reclaim it on either install or cancellation.
                intent.putExtra(Intent.EXTRA_RETURN_RESULT, true);
                status(work, "Installer\n");
                activity.startActivityForResult(intent, INSTALL_REQUEST);
                launched = true;
            } catch (Exception e) { error(work, e); }
            finally {
                if (!launched) {
                    synchronized (AppUpdateInstaller.class) { activeWork = null; }
                    discardPayloads(work, true);
                }
            }
        });
    }

    static synchronized boolean result(int requestCode, int resultCode) {
        if (requestCode != INSTALL_REQUEST) return false;
        File work = activeWork;
        activeWork = null;
        if (work != null) {
            discardPayloads(work, true);
            try {
                if (resultCode == Activity.RESULT_OK) status(work, "Done\n");
                else if (resultCode == Activity.RESULT_CANCELED) status(work, "Cancelled\n");
                else status(work, "Error\nAndroid could not install the update.");
            } catch (IOException e) { error(work, e); }
        }
        return true;
    }

    private static void checkCancelled(File work) throws IOException {
        if (new File(work, "cancel").exists()) throw new IOException("Update cancelled.");
    }

    private static void copy(InputStream input, File output, long limit, File work, long deadline) throws IOException {
        try (FileOutputStream out = new FileOutputStream(output)) {
            byte[] buffer = new byte[65536];
            long total = 0;
            for (int read; (read = input.read(buffer)) != -1;) {
                checkCancelled(work);
                if (SystemClock.elapsedRealtime() > deadline) throw new IOException("The update download timed out.");
                total += read;
                if (total > limit) throw new IOException("The update exceeds the size limit.");
                out.write(buffer, 0, read);
            }
            out.getFD().sync();
        }
    }

    static void install(Activity activity, String url, String digest, String directory, long size) {
        File work = new File(directory);
        final Context context = activity.getApplicationContext();
        try { validateWork(context, work); }
        catch (IOException e) { android.util.Log.e("HollycastUpdate", "Invalid update staging directory"); return; }
        if (!busy.compareAndSet(false, true)) { error(work, new IOException("An update is already downloading.")); return; }
        try {
            PackageInfo current = context.getPackageManager().getPackageInfo(context.getPackageName(),
                    android.content.pm.PackageManager.GET_PERMISSIONS);
            boolean githubBuild = current.requestedPermissions != null
                    && java.util.Arrays.asList(current.requestedPermissions).contains("android.permission.REQUEST_INSTALL_PACKAGES");
            if (!BuildConfig.GITHUB_UPDATES || !githubBuild) throw new IOException("Use your app store to update this installation. In-app APK installation is available in GitHub builds.");
            synchronized (AppUpdateInstaller.class) {
                if (pendingWork != null || activeWork != null) throw new IOException("Finish the pending Android update before starting another.");
                downloadingWork = work;
            }
            if (!digest.matches("[a-f0-9]{64}")
                    || !url.startsWith("https://github.com/nexus382/Hollycast/releases/download/")
                    || size <= 0 || size > LIMIT) throw new IOException("Invalid official update request.");
            if (work.getUsableSpace() < size * 3) throw new IOException("Not enough free space to stage the update.");
            checkCancelled(work);
            if (!ensureInstallPermission(activity, work)) {
                synchronized (AppUpdateInstaller.class) { downloadingWork = null; }
                busy.set(false); return;
            }
            try (FileOutputStream out = new FileOutputStream(new File(work, "source-version"))) {
                out.write(Long.toString(versionCode(current)).getBytes(java.nio.charset.StandardCharsets.UTF_8));
                out.getFD().sync();
            }
            status(work, "Downloading\n");
        } catch (Exception e) {
            synchronized (AppUpdateInstaller.class) { downloadingWork = null; }
            discardPayloads(work, true); busy.set(false); error(work, e); return;
        }
        new Thread(() -> {
            try {
                File archive = new File(work, "package.zip");
                final long deadline = SystemClock.elapsedRealtime() + 5 * 60 * 1000;
                URL destination = new URL(url);
                for (int redirects = 0; ; ++redirects) {
                    checkCancelled(work);
                    if (redirects > 5 || !"https".equals(destination.getProtocol())) throw new IOException("Unsafe update redirect.");
                    HttpURLConnection connection = (HttpURLConnection) destination.openConnection();
                    try {
                        connection.setConnectTimeout(15000);
                        connection.setReadTimeout(30000);
                        connection.setInstanceFollowRedirects(false);
                        connection.setRequestProperty("User-Agent", "Hollycast-Updater");
                        int code = connection.getResponseCode();
                        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                            String location = connection.getHeaderField("Location");
                            if (location == null) throw new IOException("Invalid update redirect.");
                            destination = new URL(destination, location);
                            continue;
                        }
                        if (code != 200) throw new IOException("Update download failed (HTTP " + code + ").");
                        try (InputStream input = connection.getInputStream()) { copy(input, archive, size, work, deadline); }
                        break;
                    } finally { connection.disconnect(); }
                }
                if (archive.length() != size) throw new IOException("The downloaded package has the wrong size.");
                MessageDigest sha = MessageDigest.getInstance("SHA-256");
                try (InputStream in = new FileInputStream(archive)) {
                    byte[] bytes = new byte[65536];
                    for (int n; (n = in.read(bytes)) != -1;) { checkCancelled(work); sha.update(bytes, 0, n); }
                }
                StringBuilder actual = new StringBuilder();
                for (byte value : sha.digest()) actual.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
                if (!actual.toString().equals(digest)) throw new IOException("The update failed SHA-256 verification.");
                File apk = null;
                for (int depth = 0; depth < 3; ++depth) {
                    File nested = null;
                    int matches = 0, entries = 0;
                    try (ZipInputStream zip = new ZipInputStream(new FileInputStream(archive))) {
                        for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                            checkCancelled(work);
                            if (SystemClock.elapsedRealtime() > deadline) throw new IOException("Update extraction timed out.");
                            if (++entries > 10000) throw new IOException("The update archive has too many files.");
                            if (entry.isDirectory()) continue;
                            String name = entry.getName().toLowerCase(java.util.Locale.ROOT);
                            if (name.endsWith(".apk")) {
                                if (++matches > 1) throw new IOException("The update contains multiple APKs.");
                                apk = new File(work, "update.apk");
                                copy(zip, apk, LIMIT, work, deadline);
                            } else if (name.endsWith(".zip") && apk == null) {
                                if (nested != null) throw new IOException("The wrapped update is ambiguous.");
                                nested = new File(work, "inner-" + depth + ".zip");
                                copy(zip, nested, LIMIT, work, deadline);
                            }
                        }
                    }
                    if (apk != null) break;
                    if (nested == null) throw new IOException("The update archive does not contain an APK.");
                    archive = nested;
                }
                if (apk == null) throw new IOException("The nested update format is unsupported.");
                PackageInfo info = context.getPackageManager().getPackageArchiveInfo(apk.getAbsolutePath(), 0);
                if (info == null || !context.getPackageName().equals(info.packageName))
                    throw new IOException("The update is for a different Android application.");
                PackageInfo installed = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
                long installedVersion = versionCode(installed);
                long updateVersion = versionCode(info);
                if (updateVersion < installedVersion)
                    throw new IOException("Android cannot install an older build over this version. Choose a newer build on the selected track.");
                checkCancelled(work);
                discardPayloads(work, false);
                synchronized (AppUpdateInstaller.class) { pendingApk = apk; pendingWork = work; present(); }
            } catch (Exception e) { discardPayloads(work, true); error(work, e); }
            finally {
                synchronized (AppUpdateInstaller.class) { downloadingWork = null; }
                busy.set(false);
            }
        }, "HollycastUpdate").start();
    }
}
