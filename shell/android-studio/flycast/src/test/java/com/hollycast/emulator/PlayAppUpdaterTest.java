// Portions Copyright 2026 The Hollycast Authors
package com.hollycast.emulator;

import android.app.Activity;
import com.google.android.gms.tasks.OnFailureListener;
import com.google.android.gms.tasks.OnSuccessListener;
import com.google.android.gms.tasks.Task;
import com.google.android.play.core.appupdate.AppUpdateInfo;
import com.google.android.play.core.appupdate.AppUpdateManager;
import com.google.android.play.core.appupdate.AppUpdateOptions;
import com.google.android.play.core.install.model.AppUpdateType;
import com.google.android.play.core.install.model.UpdateAvailability;
import java.lang.ref.WeakReference;
import java.lang.reflect.Field;
import org.json.JSONObject;
import org.junit.Before;
import org.junit.Test;
import static org.junit.Assert.*;
import static org.junit.Assume.*;
import static org.mockito.ArgumentMatchers.*;
import static org.mockito.Mockito.*;

public class PlayAppUpdaterTest {
    private Activity activity;
    private AppUpdateManager manager;
    private AppUpdateInfo info;
    private Task<AppUpdateInfo> task;
    private OnSuccessListener<AppUpdateInfo> completion;
    private OnFailureListener failure;

    private void field(String name, Object value) throws Exception {
        Field field = PlayAppUpdater.class.getDeclaredField(name);
        field.setAccessible(true);
        field.set(null, value);
    }

    @Before @SuppressWarnings("unchecked")
    public void prepare() throws Exception {
        activity = mock(Activity.class);
        manager = mock(AppUpdateManager.class);
        info = mock(AppUpdateInfo.class);
        task = mock(Task.class);
        field("manager", manager);
        field("foreground", new WeakReference<>(activity));
        field("checking", false); field("launching", false); field("revision", 0L);
        field("availableCode", 0); field("installedCode", 13L); field("installedName", "1.2");
        field("error", ""); field("message", "");
        field("snapshot", BuildConfig.GITHUB_UPDATES ? "{\"source\":\"github\"}" : "{\"source\":\"play\"}");
        doAnswer(call -> { call.<Runnable>getArgument(0).run(); return null; }).when(activity).runOnUiThread(any(Runnable.class));
        when(manager.getAppUpdateInfo()).thenReturn(task);
        when(task.addOnSuccessListener(any())).thenAnswer(call -> { completion = call.getArgument(0); return task; });
        when(task.addOnFailureListener(any())).thenAnswer(call -> { failure = call.getArgument(0); return task; });
        when(info.availableVersionCode()).thenReturn(14);
        when(info.updateAvailability()).thenReturn(UpdateAvailability.UPDATE_AVAILABLE);
        when(info.isUpdateTypeAllowed(AppUpdateType.IMMEDIATE)).thenReturn(true);
        when(manager.startUpdateFlowForResult(any(AppUpdateInfo.class), same(activity), any(AppUpdateOptions.class), anyInt())).thenReturn(true);
    }

    private JSONObject snapshot() throws Exception { return new JSONObject(PlayAppUpdater.control(activity, "")); }

    @Test public void newerCiReleaseDoesNotCreatePlayUpdate() throws Exception {
        assumeFalse(BuildConfig.GITHUB_UPDATES);
        when(info.updateAvailability()).thenReturn(UpdateAvailability.UPDATE_NOT_AVAILABLE);
        PlayAppUpdater.control(activity, "check"); completion.onSuccess(info);
        assertEquals("play", snapshot().getString("source"));
        assertEquals(0, snapshot().getInt("availableCode"));
        assertFalse(snapshot().getBoolean("checking"));
        verify(manager, never()).startUpdateFlowForResult(any(AppUpdateInfo.class), any(Activity.class), any(AppUpdateOptions.class), anyInt());
    }

    @Test public void availableVersionComesFromPlayAndDoesNotInstallWithoutClick() throws Exception {
        assumeFalse(BuildConfig.GITHUB_UPDATES);
        PlayAppUpdater.control(activity, "check"); completion.onSuccess(info);
        assertEquals(14, snapshot().getInt("availableCode"));
        verify(manager, never()).startUpdateFlowForResult(any(AppUpdateInfo.class), any(Activity.class), any(AppUpdateOptions.class), anyInt());
    }

    @Test public void pausedDuringInstallCheckCanRetry() throws Exception {
        assumeFalse(BuildConfig.GITHUB_UPDATES);
        PlayAppUpdater.control(activity, "install");
        PlayAppUpdater.pause(activity); completion.onSuccess(info);
        assertFalse(snapshot().getBoolean("checking"));
        assertFalse(snapshot().getBoolean("launching"));
        field("foreground", new WeakReference<>(activity));
        PlayAppUpdater.control(activity, "install"); completion.onSuccess(info);
        assertTrue(snapshot().getBoolean("launching"));
    }

    @Test public void cancellationAllowsFreshConsentedRetry() throws Exception {
        assumeFalse(BuildConfig.GITHUB_UPDATES);
        PlayAppUpdater.control(activity, "install"); completion.onSuccess(info);
        assertTrue(snapshot().getBoolean("launching"));
        assertTrue(PlayAppUpdater.result(19581, Activity.RESULT_CANCELED));
        assertFalse(snapshot().getBoolean("launching"));
        PlayAppUpdater.control(activity, "install"); completion.onSuccess(info);
        verify(manager, times(2)).getAppUpdateInfo();
        assertTrue(snapshot().getBoolean("launching"));
    }

    @Test public void failedPlayCheckDoesNotOfferCiFallback() throws Exception {
        assumeFalse(BuildConfig.GITHUB_UPDATES);
        PlayAppUpdater.control(activity, "check"); failure.onFailure(new IllegalStateException("offline"));
        assertEquals("play", snapshot().getString("source"));
        assertEquals(0, snapshot().getInt("availableCode"));
        assertFalse(snapshot().getBoolean("checking"));
        assertFalse(snapshot().getString("error").isEmpty());
    }

    @Test public void githubApkNeverQueriesOrStartsPlay() throws Exception {
        assumeTrue(BuildConfig.GITHUB_UPDATES);
        PlayAppUpdater.resume(activity);
        assertEquals("github", new JSONObject(PlayAppUpdater.control(activity, "check")).getString("source"));
        PlayAppUpdater.control(activity, "install");
        verifyNoInteractions(manager);
    }
}
