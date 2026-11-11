// Portions Copyright 2026 The Hollycast Authors
package com.hollycast.emulator;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import java.io.File;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import static org.junit.Assert.*;
import static org.junit.Assume.*;
import static org.mockito.ArgumentMatchers.*;
import static org.mockito.Mockito.*;

public class AppUpdateInstallerTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();
    private Activity activity;
    private Context context;
    private File files, updates;
    private PackageInfo installed;
    private AtomicReference<Runnable> posted;

    private Object field(String name) throws Exception {
        Field field = AppUpdateInstaller.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(null);
    }
    private void field(String name, Object value) throws Exception {
        Field field = AppUpdateInstaller.class.getDeclaredField(name);
        field.setAccessible(true);
        field.set(null, value);
    }
    private File write(File directory, String name, String contents) throws Exception {
        File result = new File(directory, name);
        Files.write(result.toPath(), contents.getBytes(StandardCharsets.UTF_8));
        return result;
    }
    @Before public void prepare() throws Exception {
        assumeTrue(BuildConfig.GITHUB_UPDATES);
        files = temporary.newFolder("files");
        updates = new File(files, "app-updates"); assertTrue(updates.mkdir());
        activity = mock(Activity.class); context = mock(Context.class);
        PackageManager packages = mock(PackageManager.class);
        installed = mock(PackageInfo.class); installed.versionCode = 10;
        installed.requestedPermissions = new String[]{"android.permission.REQUEST_INSTALL_PACKAGES"};
        when(activity.getApplicationContext()).thenReturn(context);
        when(context.getFilesDir()).thenReturn(files);
        when(context.getPackageName()).thenReturn("com.hollycast.emu");
        when(activity.getPackageName()).thenReturn("com.hollycast.emu");
        when(context.getPackageManager()).thenReturn(packages);
        when(activity.getPackageManager()).thenReturn(packages);
        when(packages.getPackageInfo(anyString(), anyInt())).thenReturn(installed);
        posted = new AtomicReference<>();
        doAnswer(call -> { posted.set(call.getArgument(0)); return null; }).when(activity).runOnUiThread(any(Runnable.class));
        ((AtomicBoolean)field("busy")).set(false);
        field("pendingApk", null); field("pendingWork", null); field("activeWork", null);
        field("foreground", new WeakReference<Activity>(null));
    }
    @Test public void pausedActivityRetainsPendingApkUntilItResumes() throws Exception {
        File work = new File(updates, "update-pending"); assertTrue(work.mkdir());
        File apk = write(work, "update.apk", "pending APK");
        field("pendingApk", apk); field("pendingWork", work);
        AppUpdateInstaller.resume(activity);
        assertNotNull(posted.get());
        AppUpdateInstaller.pause(activity);
        posted.get().run();
        assertSame(apk, field("pendingApk"));
        assertSame(work, field("pendingWork"));
        verify(activity, never()).startActivity(any(Intent.class));
        verify(activity, never()).startActivityForResult(any(Intent.class), anyInt());
    }
    @Test public void newVersionReclaimsCompletedPayloadsButNotCurrentVersionApks() throws Exception {
        File old = new File(updates, "update-old"); assertTrue(old.mkdir());
        File active = new File(updates, "update-current"); assertTrue(active.mkdir());
        write(old, "source-version", "9"); write(active, "source-version", "10");
        File oldApk = write(old, "update.apk", "completed APK");
        File oldZip = write(old, "package.zip", "completed archive");
        File activeApk = write(active, "update.apk", "installer may still read this");
        File unrelated = write(files, "keep.vmu", "user data");
        AppUpdateInstaller.resume(activity);
        assertFalse(oldApk.exists()); assertFalse(oldZip.exists());
        assertTrue(activeApk.exists()); assertTrue(unrelated.exists());
    }
    @Test public void cancelledDownloadDiscardsItsPayloads() throws Exception {
        File work = new File(updates, "update-cancelled"); assertTrue(work.mkdir());
        File zip = write(work, "package.zip", "partial archive");
        File inner = write(work, "inner-0.zip", "partial inner archive");
        File apk = write(work, "update.apk", "partial APK"); write(work, "cancel", "");
        AppUpdateInstaller.install(activity, "https://github.com/nexus382/Hollycast/releases/download/fixture/test.zip",
                new String(new char[64]).replace('\0', 'a'), work.getAbsolutePath(), 100);
        long deadline = System.nanoTime() + 3_000_000_000L;
        while (((AtomicBoolean)field("busy")).get() && System.nanoTime() < deadline) Thread.sleep(10);
        assertFalse(((AtomicBoolean)field("busy")).get());
        assertFalse(zip.exists()); assertFalse(inner.exists()); assertFalse(apk.exists());
        assertTrue(new String(Files.readAllBytes(new File(work, "status").toPath()), StandardCharsets.UTF_8).startsWith("Error\n"));
    }
    @Test public void destroyedActivityRetainsPendingApk() throws Exception {
        File work = new File(updates, "update-destroyed"); assertTrue(work.mkdir());
        File apk = write(work, "update.apk", "pending APK");
        field("pendingApk", apk); field("pendingWork", work);
        AppUpdateInstaller.resume(activity);
        when(activity.isDestroyed()).thenReturn(true);
        posted.get().run();
        assertSame(apk, field("pendingApk"));
        assertSame(work, field("pendingWork"));
        verify(activity, never()).startActivityForResult(any(Intent.class), anyInt());
    }
    @Test public void installerResultReclaimsPayloadsAndPublishesItsOutcome() throws Exception {
        int request = (Integer)field("INSTALL_REQUEST");
        for (int result : new int[]{Activity.RESULT_OK, Activity.RESULT_CANCELED, Activity.RESULT_FIRST_USER}) {
            File work = new File(updates, "update-result-" + result); assertTrue(work.mkdir());
            File apk = write(work, "update.apk", "installer APK");
            File zip = write(work, "package.zip", "archive");
            field("activeWork", work);
            assertFalse(AppUpdateInstaller.result(request + 1, result));
            assertTrue(apk.exists());
            assertTrue(AppUpdateInstaller.result(request, result));
            assertNull(field("activeWork"));
            assertFalse(apk.exists()); assertFalse(zip.exists());
            String expected = result == Activity.RESULT_OK ? "Done\n"
                    : result == Activity.RESULT_CANCELED ? "Cancelled\n" : "Error\n";
            assertTrue(new String(Files.readAllBytes(new File(work, "status").toPath()), StandardCharsets.UTF_8).startsWith(expected));
        }
    }
    @Test public void anotherDownloadCannotReplaceAnActiveInstaller() throws Exception {
        File active = new File(updates, "update-active"); assertTrue(active.mkdir());
        File apk = write(active, "update.apk", "installer is reading this");
        field("activeWork", active);
        File work = new File(updates, "update-another"); assertTrue(work.mkdir());
        AppUpdateInstaller.install(activity, "https://github.com/nexus382/Hollycast/releases/download/fixture/test.zip",
                new String(new char[64]).replace('\0', 'a'), work.getAbsolutePath(), 100);
        assertFalse(((AtomicBoolean)field("busy")).get());
        assertSame(active, field("activeWork"));
        assertTrue(apk.exists());
        assertTrue(new String(Files.readAllBytes(new File(work, "status").toPath()), StandardCharsets.UTF_8).contains("pending Android update"));
    }

}
