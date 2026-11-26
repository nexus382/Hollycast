package com.hollycast.emulator;

import android.util.Log;

import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.Arrays;
import java.io.InputStream;
import java.io.OutputStream;
import android.os.ParcelFileDescriptor;
import android.net.Uri;
import java.util.UUID;

import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class AndroidStorageTest {
    public static final String TREE_URI = "content://com.android.externalstorage.documents/tree/primary%3AFlycast%2FROMS";

    @Test
    public void test() {
        ActivityScenario<NativeGLActivity> scenario = ActivityScenario.launch(NativeGLActivity.class);
        scenario.onActivity(activity -> {
            try {
                // Configure storage
                AndroidStorage storage = activity.getStorage();
                String rootUri = TREE_URI;
                storage.setStorageDirectories(Arrays.asList(rootUri));

                // Start test
                // exists (root)
                assertTrue(storage.exists(rootUri));
                // listContent (root)
                FileInfo[] kids = storage.listContent(rootUri);
                assertTrue(kids.length > 0);
                // getFileInfo (root)
                FileInfo info = storage.getFileInfo(rootUri);
                assertEquals(info.getPath(), rootUri);
                assertTrue(info.isDirectory());
                assertNotEquals(0, info.getUpdateTime());
                // getParentUri (root)
                // fails on lollipop_mr1, could be because parent folder (/Flycast) is also allowed
                assertEquals("", storage.getParentUri(rootUri));

                boolean directoryDone = false;
                boolean fileDone = false;
                for (FileInfo file : kids) {
                    if (file.isDirectory() && !directoryDone) {
                        // getParentUri
                        String parentUri = storage.getParentUri(file.getPath());
                        // FIXME fails because getParentUri returns a docId, not a treeId
                        //assertEquals(rootUri, parentUri);

                        // getSubPath (from root)
                        String kidUri = storage.getSubPath(rootUri, file.getName());
                        assertEquals(file.getName(), storage.getFileInfo(kidUri).getName());

                        // exists (folder)
                        assertTrue(storage.exists(file.getPath()));

                        // getFileInfo (folder)
                        info = storage.getFileInfo(file.getPath());
                        assertEquals(file.getPath(), info.getPath());
                        assertEquals(file.getName(), info.getName());
                        assertTrue(info.isDirectory());
                        assertNotEquals(0, info.getUpdateTime());

                        // listContent (from folder)
                        FileInfo[] gdkids = storage.listContent(file.getPath());
                        assertTrue(gdkids.length > 0);
                        for (FileInfo sfile : gdkids) {
                            if (!sfile.isDirectory()) {
                                // openFile
                                int fd = storage.openFile(sfile.getPath(), "r");
                                assertNotEquals(-1, fd);
                                // getSubPath (from folder)
                                String uri = storage.getSubPath(file.getPath(), sfile.getName());
                                assertEquals(sfile.getName(), storage.getFileInfo(uri).getName());
                                // getParentUri (from file)
                                uri = storage.getParentUri(sfile.getPath());
                                assertEquals(file.getPath(), uri);
                                // exists (doc)
                                assertTrue(storage.exists(sfile.getPath()));
                                // getFileInfo (doc)
                                info = storage.getFileInfo(sfile.getPath());
                                assertEquals(info.getPath(), sfile.getPath());
                                assertEquals(info.getName(), sfile.getName());
                                assertNotEquals(0, info.getUpdateTime());
                                assertFalse(info.isDirectory());
                            } else {
                                // getParentUri (from subfolder)
                                String uri = storage.getParentUri(sfile.getPath());
                                assertEquals(file.getPath(), uri);
                                // exists (subfolder)
                                assertTrue(storage.exists(sfile.getPath()));
                            }
                        }
                        directoryDone = true;
                    }
                    if (!file.isDirectory() && !fileDone) {
                        // getParentUri
                        String parentUri = storage.getParentUri(file.getPath());
                        // FIXME fails because getParentUri returns a docId, not a treeId
                        //assertEquals(rootUri, parentUri);
                        // getSubPath (from root)
                        String kidUri = storage.getSubPath(rootUri, file.getName());
                        assertEquals(file.getName(), storage.getFileInfo(kidUri).getName());
                        // exists (file)
                        assertTrue(storage.exists(file.getPath()));
                        fileDone = true;
                    }
                }
            } catch (Exception e) {
                throw new RuntimeException(e);
            }
        });
    }

    @Test
    public void saveSlotSurvivesInterruptedReplacement() {
        final String name = "hcast-save-" + UUID.randomUUID() + ".state";
        final String[] destination = { null };
        ActivityScenario<NativeGLActivity> scenario = ActivityScenario.launch(NativeGLActivity.class);
        scenario.onActivity(activity -> {
            AndroidStorage storage = activity.getStorage();
            storage.setStorageDirectories(Arrays.asList(TREE_URI));
            try {
                destination[0] = storage.getSubPath(TREE_URI, name);
                byte[] oldState = "previous complete state".getBytes("UTF-8");
                byte[] newState = "replacement complete state".getBytes("UTF-8");
                String first = storage.createTemporaryFile(destination[0]);
                assertFalse(first.isEmpty());
                writeDocument(activity, first, oldState);
                assertTrue(storage.replaceFile(first, destination[0]));
                assertArrayEquals(oldState, readDocument(storage, activity, destination[0]));
            } catch (Exception e) {
                throw new RuntimeException(e);
            }
        });
        scenario.close();

        ActivityScenario<NativeGLActivity> reopened = ActivityScenario.launch(NativeGLActivity.class);
        reopened.onActivity(activity -> {
            AndroidStorage storage = activity.getStorage();
            storage.setStorageDirectories(Arrays.asList(TREE_URI));
            try {
                byte[] oldState = "previous complete state".getBytes("UTF-8");
                byte[] newState = "replacement complete state".getBytes("UTF-8");
                assertArrayEquals(oldState, readDocument(storage, activity, destination[0]));
                assertFalse(storage.replaceFile("content://invalid.invalid/document/missing", destination[0]));
                assertArrayEquals(oldState, readDocument(storage, activity, destination[0]));
                String overwrite = storage.createTemporaryFile(destination[0]);
                assertFalse(overwrite.isEmpty());
                writeDocument(activity, overwrite, newState);
                assertTrue(storage.replaceFile(overwrite, destination[0]));
                assertArrayEquals(newState, readDocument(storage, activity, destination[0]));
                storage.removeFile(destination[0]);
            } catch (Exception e) {
                throw new RuntimeException(e);
            }
        });
        reopened.close();
    }

    private static byte[] readDocument(AndroidStorage storage, NativeGLActivity activity, String uri) throws Exception {
        int fd = storage.openFile(uri, "r");
        assertNotEquals(-1, fd);
        ParcelFileDescriptor descriptor = ParcelFileDescriptor.adoptFd(fd);
        try (InputStream input = new ParcelFileDescriptor.AutoCloseInputStream(descriptor);
             java.io.ByteArrayOutputStream output = new java.io.ByteArrayOutputStream()) {
            assertNotNull(input);
            byte[] buffer = new byte[1024];
            int count;
            while ((count = input.read(buffer)) != -1)
                output.write(buffer, 0, count);
            return output.toByteArray();
        }
    }

    private static void writeDocument(NativeGLActivity activity, String uri, byte[] data) throws Exception {
        try (OutputStream output = activity.getContentResolver().openOutputStream(Uri.parse(uri), "wt")) {
            assertNotNull(output);
            output.write(data);
            output.flush();
        }
    }

    //@Test
    public void testLargeFolder() {
        ActivityScenario<NativeGLActivity> scenario = ActivityScenario.launch(NativeGLActivity.class);
        scenario.onActivity(activity -> {
            try {
                // Configure storage
                AndroidStorage storage = activity.getStorage();
                String rootUri = TREE_URI;
                storage.setStorageDirectories(Arrays.asList(rootUri));

                // list content
                String uri = storage.getSubPath(rootUri, "textures");
                uri = storage.getSubPath(uri, "T1401N");
                long t0 = System.currentTimeMillis();
                FileInfo[] kids = storage.listContent(uri);
                Log.d("testLargeFolder", "Got " + kids.length + " in " + (System.currentTimeMillis() - t0) + " ms");
                // Got 2307 in 119910 ms !!!
                // retrieving only uri in listContent: Got 2307 in 9007 ms
                // retrieving uri+isDir: Got 2307 in 62281 ms
                // manual listing: Got 2307 in 10212 ms
            } catch (Exception e) {
                throw new RuntimeException(e);
            }
        });
    }
}
