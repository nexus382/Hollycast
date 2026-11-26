/*
	Copyright 2023 flyinghead

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
package com.hollycast.emulator;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.ContentUris;
import android.content.DialogInterface;
import android.content.Intent;
import android.database.Cursor;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.util.Log;

import androidx.documentfile.provider.DocumentFile;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.zip.CRC32;

public class AndroidStorage {
    public static final int ADD_STORAGE_ACTIVITY_REQUEST = 15012010;
    public static final int EXPORT_HOME_ACTIVITY_REQUEST = 15012011;
    public static final int IMPORT_HOME_ACTIVITY_REQUEST = 15012012;

    private Activity activity;

    private List<String> storageDirectories;
    private int storageIntentPerms;

    public AndroidStorage(Activity activity) {
        this.activity = activity;
        init();
    }

    public void setStorageDirectories(List<String> storageDirectories) {
        this.storageDirectories = storageDirectories;
    }

    public native void init();
    public native void addStorageCallback(String path);
    public native void reloadConfig();

    public void onAddStorageResult(Intent data)
    {
        Uri uri = data == null ? null : data.getData();
        if (uri == null) {
            // Cancelled
            addStorageCallback(null);
        }
        else {
            try {
                activity.getContentResolver().takePersistableUriPermission(uri, storageIntentPerms);
            } catch (SecurityException e) {
                Log.w("Flycast", "takePersistableUriPermission failed", e);
                AlertDialog.Builder dlgAlert  = new AlertDialog.Builder(activity);
                dlgAlert.setMessage(R.string.folder_permissions);
                dlgAlert.setTitle(R.string.storage_error);
                dlgAlert.setPositiveButton(R.string.OK,
                        new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog,int id) {
                                addStorageCallback(null);
                            }
                        });
                dlgAlert.setIcon(android.R.drawable.ic_dialog_alert);
                dlgAlert.setCancelable(false);
                dlgAlert.create().show();
                return;
            }
            addStorageCallback(uri.toString());
        }
    }

    public synchronized int openFile(String uri, String mode) throws FileNotFoundException {
        try {
            recoverInterruptedReplacement(uri);
        } catch (IOException e) {
            FileNotFoundException failure = new FileNotFoundException("Could not recover " + uri + ": " + e.getMessage());
            failure.initCause(e);
            throw failure;
        }
        ParcelFileDescriptor pfd = activity.getContentResolver().openFileDescriptor(Uri.parse(uri), mode);
        if (pfd == null)
            throw new FileNotFoundException(uri);
        return pfd.detachFd();
    }

    public synchronized String createTemporaryFile(String destination) {
        try {
            recoverInterruptedReplacement(destination);
            String parent = parentUriFor(destination);
            if (parent.isEmpty())
                return "";
            String name = documentName(destination);
            Uri temporary = createChild(parent, "." + name + ".hollycast-tmp-" + UUID.randomUUID());
            return temporary == null ? "" : temporary.toString();
        } catch (Exception e) {
            Log.w("Flycast", "Could not create save-state staging document", e);
            return "";
        }
    }

    public synchronized boolean replaceFile(String temporary, String destination) {
        Uri marker = null;
        Uri backup = null;
        boolean hadPrevious = false;
        try {
            recoverInterruptedReplacement(destination);
            String parent = parentUriFor(destination);
            if (parent.isEmpty())
                return false;
            String name = documentName(destination);
            marker = childOrCreate(parent, "." + name + ".hollycast-transaction");
            backup = childOrCreate(parent, "." + name + ".hollycast-backup");
            hadPrevious = checkedExists(destination);
            if (hadPrevious) {
                if (!copyAndVerify(destination, backup.toString())) {
                    deleteDocument(backup);
                    return false;
                }
            } else {
                deleteDocument(backup);
            }
            try (OutputStream stream = activity.getContentResolver().openOutputStream(marker, "wt")) {
                if (stream == null)
                    return false;
                stream.write(hadPrevious ? 1 : 0);
                stream.flush();
            }

            if (!hadPrevious && !checkedExists(destination)) {
                Uri created = createChild(parent, name);
                if (created == null)
                    return false;
                if (!created.toString().equals(destination)) {
                    deleteDocument(created);
                    deleteDocument(marker);
                    return false;
                }
            }

            if (!copyAndVerify(temporary, destination)) {
                recoverInterruptedReplacement(destination);
                return false;
            }
            // Removing the marker first makes the verified new document authoritative.
            if (!deleteDocument(marker))
                return false;
            try {
                if (hadPrevious)
                    deleteDocument(backup);
                deleteDocument(Uri.parse(temporary));
            } catch (IOException cleanupError) {
                Log.w("Flycast", "Save-state committed; staging cleanup will be retried", cleanupError);
            }
            return true;
        } catch (Exception e) {
            Log.w("Flycast", "Save-state replacement failed", e);
            try {
                if (marker != null && exists(marker.toString()))
                    recoverInterruptedReplacement(destination);
            } catch (Exception recoveryError) {
                Log.e("Flycast", "Save-state recovery will be retried on next access", recoveryError);
            }
            return false;
        }
    }

    public boolean removeFile(String uri) {
        try {
            return deleteDocument(Uri.parse(uri));
        } catch (Exception e) {
            return false;
        }
    }

    private Uri createChild(String parent, String name) throws FileNotFoundException {
        Uri parentUri = Uri.parse(parent);
        String documentId = DocumentsContract.isDocumentUri(activity, parentUri)
                ? DocumentsContract.getDocumentId(parentUri)
                : DocumentsContract.getTreeDocumentId(parentUri);
        Uri treeParent = DocumentsContract.buildDocumentUriUsingTree(parentUri, documentId);
        return DocumentsContract.createDocument(activity.getContentResolver(), treeParent,
                "application/octet-stream", name);
    }

    private Uri childOrCreate(String parent, String name) throws IOException {
        String child = getSubPath(parent, name);
        if (checkedExists(child))
            return Uri.parse(child);
        Uri created = createChild(parent, name);
        if (created == null)
            throw new FileNotFoundException(child);
        return created;
    }

    private boolean deleteDocument(Uri uri) throws IOException {
        if (uri == null || !checkedExists(uri.toString()))
            return true;
        try {
            return DocumentsContract.deleteDocument(activity.getContentResolver(), uri);
        } catch (RuntimeException e) {
            throw new IOException("Could not delete " + uri, e);
        }
    }

    private boolean checkedExists(String uriString) throws IOException {
        try (Cursor cursor = activity.getContentResolver().query(Uri.parse(uriString),
                new String[] { DocumentsContract.Document.COLUMN_DOCUMENT_ID }, null, null, null)) {
            if (cursor == null)
                throw new IOException("Provider returned no query result for " + uriString);
            return cursor.moveToFirst();
        } catch (RuntimeException e) {
            throw new IOException("Could not query " + uriString, e);
        }
    }

    private boolean copyAndVerify(String source, String destination) throws IOException {
        CRC32 expected = new CRC32();
        long expectedSize = 0;
        try (InputStream input = activity.getContentResolver().openInputStream(Uri.parse(source));
             OutputStream output = activity.getContentResolver().openOutputStream(Uri.parse(destination), "wt")) {
            if (input == null || output == null)
                return false;
            byte[] buffer = new byte[65536];
            int count;
            while ((count = input.read(buffer)) != -1) {
                output.write(buffer, 0, count);
                expected.update(buffer, 0, count);
                expectedSize += count;
            }
            output.flush();
        }
        CRC32 actual = new CRC32();
        long actualSize = 0;
        try (InputStream input = activity.getContentResolver().openInputStream(Uri.parse(destination))) {
            if (input == null)
                return false;
            byte[] buffer = new byte[65536];
            int count;
            while ((count = input.read(buffer)) != -1) {
                actual.update(buffer, 0, count);
                actualSize += count;
            }
        }
        return expectedSize == actualSize && expected.getValue() == actual.getValue();
    }

    private void recoverInterruptedReplacement(String destination) throws IOException {
        String parent = parentUriFor(destination);
        if (parent.isEmpty())
            return;
        String name = documentName(destination);
        String markerPath = getSubPath(parent, "." + name + ".hollycast-transaction");
        String backupPath = getSubPath(parent, "." + name + ".hollycast-backup");
        if (!checkedExists(markerPath)) {
            // A backup without a marker is from a commit whose verified new copy won.
            deleteDocument(Uri.parse(backupPath));
            return;
        }
        // The verified backup is the durable record of whether the slot existed.
        // This also handles interruption while the small marker itself was written.
        boolean hadPrevious = checkedExists(backupPath);
        if (hadPrevious) {
            if (!copyAndVerify(backupPath, destination))
                throw new IOException("verified save-state backup is unavailable");
            deleteDocument(Uri.parse(backupPath));
        } else {
            deleteDocument(Uri.parse(destination));
        }
        deleteDocument(Uri.parse(markerPath));
    }

    private String documentName(String uriString) {
        try {
            return getFileInfo(uriString).getName();
        } catch (FileNotFoundException missing) {
            // New slots are represented by the same name-derived document URI used by getSubPath.
            String id = DocumentsContract.getDocumentId(Uri.parse(uriString));
            int slash = id.lastIndexOf('/');
            return slash < 0 ? id : id.substring(slash + 1);
        }
    }

    private String parentUriFor(String uriString) {
        try {
            String parent = getParentUri(uriString);
            if (!parent.isEmpty())
                return parent;
        } catch (Exception ignored) {
            // New documents have no provider path yet; derive the selected parent from their tree ID.
        }
        Uri uri = Uri.parse(uriString);
        String id;
        try {
            id = DocumentsContract.getDocumentId(uri);
        } catch (IllegalArgumentException notDocumentUri) {
            id = DocumentsContract.getTreeDocumentId(uri);
        }
        int slash = id.lastIndexOf('/');
        if (slash < 0)
            return "";
        return DocumentsContract.buildDocumentUriUsingTree(uri, id.substring(0, slash)).toString();
    }

    public InputStream openInputStream(String uri) throws FileNotFoundException {
        return activity.getContentResolver().openInputStream(Uri.parse(uri));
    }
    public OutputStream openOutputStream(String parent, String name) throws FileNotFoundException {
        Uri uri = Uri.parse(parent);
        String subpath = getSubPath(parent, name);
        if (!exists(subpath)) {
            String documentId;
            if (DocumentsContract.isDocumentUri(activity, uri))
                documentId = DocumentsContract.getDocumentId(uri);
            else
                documentId = DocumentsContract.getTreeDocumentId(uri);
            uri = DocumentsContract.buildDocumentUriUsingTree(uri, documentId);
            uri = DocumentsContract.createDocument(activity.getContentResolver(), uri,
                    "application/octet-stream", name);
        }
        else {
            uri = Uri.parse(subpath);
        }
        return activity.getContentResolver().openOutputStream(uri);
    }

    public FileInfo[] listContent(String uri)
    {
        Uri treeUri = Uri.parse(uri);
        String documentId;
        if (DocumentsContract.isDocumentUri(activity, treeUri))
            documentId = DocumentsContract.getDocumentId(treeUri);
        else
            documentId = DocumentsContract.getTreeDocumentId(treeUri);
        Uri docUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId);
        final Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(docUri,
                DocumentsContract.getDocumentId(docUri));
        final ArrayList<FileInfo> results = new ArrayList<>();

        Cursor c = null;
        try {
            final ContentResolver resolver = activity.getContentResolver();
            c = resolver.query(childrenUri, new String[] {
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE }, null, null, null);
            while (c.moveToNext())
            {
                final String childId = c.getString(0);
                final Uri childUri = DocumentsContract.buildDocumentUriUsingTree(docUri, childId);
                FileInfo info = new FileInfo();
                info.setPath(childUri.toString());
                info.setName(c.getString(1));
                info.setDirectory(DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2)));
                results.add(info);
            }
        } catch (Exception e) {
            Log.w("Flycast", "Failed query: " + e);
            throw new RuntimeException(e);
        } finally {
            if (c != null) {
                try {
                    c.close();
                } catch (RuntimeException rethrown) {
                    throw rethrown;
                } catch (Exception ignored) {
                }
            }
        }
        return results.toArray(new FileInfo[results.size()]);
     }

    public String getParentUri(String uriString) throws FileNotFoundException {
        if (uriString.isEmpty())
            return "";
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            try {
                Uri uri = Uri.parse(uriString);
                DocumentsContract.Path path = DocumentsContract.findDocumentPath(activity.getContentResolver(), uri);
                List<String> comps = path.getPath();
                if (comps.size() > 1)
                    return DocumentsContract.buildDocumentUriUsingTree(uri, comps.get(comps.size() - 2)).toString();
            } catch (IllegalArgumentException e) {
                // Happens for root storage uri:
                // DocumentsContract: Failed to find path: Invalid URI: content://com.android.externalstorage.documents/tree/primary%3AFlycast
                return "";
            }
        }
        // Hack the uri manually
        int i = uriString.lastIndexOf("%2F");
        if (i == -1)
            return "";
        else
            return uriString.substring(0, i);
    }

    public String getSubPath(String reference, String relative)
    {
        Uri refUri = Uri.parse(reference);
        String docId;
        if (DocumentsContract.isDocumentUri(activity, refUri))
            docId = DocumentsContract.getDocumentId(refUri);
        else
            docId = DocumentsContract.getTreeDocumentId(refUri);
        return DocumentsContract.buildDocumentUriUsingTree(refUri, docId + "/" + relative).toString();
    }

    public FileInfo getFileInfo(String uriString) throws FileNotFoundException
    {
        Uri uri = Uri.parse(uriString);
        DocumentFile docFile = null;
        if (DocumentsContract.isDocumentUri(activity, uri))
            docFile = DocumentFile.fromSingleUri(activity, uri);
        else
            docFile = DocumentFile.fromTreeUri(activity, uri);
        if (docFile == null || !docFile.exists())
            throw new FileNotFoundException(uriString);
        FileInfo info = new FileInfo();
        info.setPath(uriString);
        info.setName(docFile.getName());
        info.setDirectory(docFile.isDirectory());
        info.setSize(docFile.length());
        info.setWritable(docFile.canWrite());
        info.setUpdateTime(docFile.lastModified() / 1000);

        return info;
    }

    public boolean exists(String uriString)
    {
        Uri uri = Uri.parse(uriString);
        if (!DocumentsContract.isDocumentUri(activity, uri))
        {
            String documentId = DocumentsContract.getTreeDocumentId(uri);
            uri = DocumentsContract.buildDocumentUriUsingTree(uri, documentId);
        }
        Cursor cursor = null;
        try {
            cursor = activity.getContentResolver().query(uri, new String[]{ DocumentsContract.Document.COLUMN_DISPLAY_NAME },
                    null, null, null);
            boolean ret = cursor != null && cursor.moveToNext();
            return ret;
        } catch (Exception e) {
            return false;
        } finally {
            if (cursor != null)
                cursor.close();
        }
    }

    public String mkdir(String parent, String name) throws FileNotFoundException
    {
        Uri parentUri = Uri.parse(parent);
        if (!DocumentsContract.isDocumentUri(activity, parentUri)) {
            String documentId = DocumentsContract.getTreeDocumentId(parentUri);
            parentUri = DocumentsContract.buildDocumentUriUsingTree(parentUri, documentId);
        }
        Uri newDirUri = DocumentsContract.createDocument(activity.getContentResolver(), parentUri, DocumentsContract.Document.MIME_TYPE_DIR, name);
        return newDirUri.toString();
    }

    private String getResString(int id) {
        return activity.getResources().getString(id);
    }

    public boolean addStorage(boolean isDirectory, boolean writeAccess, String description, String mimeType)
    {
        Intent intent = new Intent(isDirectory ? Intent.ACTION_OPEN_DOCUMENT_TREE : Intent.ACTION_OPEN_DOCUMENT);
        if (!isDirectory) {
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            if (!mimeType.isEmpty())
                intent.setType(mimeType);
            else
                intent.setType("application/*");
        }
        intent = Intent.createChooser(intent, description);
        storageIntentPerms = Intent.FLAG_GRANT_READ_URI_PERMISSION | (writeAccess ? Intent.FLAG_GRANT_WRITE_URI_PERMISSION : 0);
        intent.addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | storageIntentPerms);
        try {
            activity.startActivityForResult(intent, ADD_STORAGE_ACTIVITY_REQUEST);
        } catch (ActivityNotFoundException aex) {
            Log.w("hollycast", "Unable to start file/folder picker", aex);
            activity.runOnUiThread(new Runnable() {
                public void run() {
                    String message;
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
                        // Android 10+ requires SAF
                        message = getResString(R.string.no_file_manager_android10);
                    else
                        message = getResString(R.string.no_file_manager_android9);
                    new AlertDialog.Builder(activity)
                            .setTitle(R.string.file_manager_not_found)
                            .setMessage(message)
                            .setPositiveButton(R.string.OK, new DialogInterface.OnClickListener() {
                                @Override
                                public void onClick(DialogInterface dialog, int id) {
                                    addStorageCallback(null);
                                }
                            })
                            .show();
                }
            });
        }

        return true;
    }

    private String getRealPath(final Uri uri) {
        return getRealPathFromURI_API19(uri);
    }

    // From https://github.com/HBiSoft/PickiT
    // Copyright (c) [2020] [HBiSoft]
    String getRealPathFromURI_API19(final Uri uri)
    {
        final boolean isTree = Build.VERSION.SDK_INT >= Build.VERSION_CODES.N && DocumentsContract.isTreeUri(uri);
        if (DocumentsContract.isDocumentUri(activity, uri) || isTree)
        {
            if (isExternalStorageDocument(uri))
            {
                final String docId = isTree ? DocumentsContract.getTreeDocumentId(uri) : DocumentsContract.getDocumentId(uri);
                final String[] split = docId.split(":");
                final String type = split[0];

                if ("primary".equalsIgnoreCase(type)) {
                    if (split.length > 1)
                        return Environment.getExternalStorageDirectory() + "/" + split[1];
                    else
                        return Environment.getExternalStorageDirectory() + "/";
                }
                else {
                    // Some devices does not allow access to the SD Card using the UID, for example /storage/6551-1152/folder/video.mp4
                    // Instead, we first have to get the name of the SD Card, for example /storage/sdcard1/folder/video.mp4

                    // We first have to check if the device allows this access
                    if (new File("storage" + "/" + docId.replace(":", "/")).exists()) {
                        return "/storage/" + docId.replace(":", "/");
                    }
                    if (storageDirectories != null) {
                        // If the file is not available, look at the storage directories
                        for (String s : storageDirectories) {
                            String root;
                            if (split[1].startsWith("/")) {
                                root = s + split[1];
                            } else {
                                root = s + "/" + split[1];
                            }
                            String path;
                            if (root.contains(type)) {
                                path = "storage" + "/" + docId.replace(":", "/");
                            } else {
                                if (root.startsWith("/storage/") || root.startsWith("storage/")) {
                                    path = root;
                                } else if (root.startsWith("/")) {
                                    path = "/storage" + root;
                                } else {
                                    path = "/storage/" + root;
                                }
                            }
                            if (new File(path).exists())
                                return path;
                        }
                    }
                }

            }
            else if (!isTree && isRawDownloadsDocument(uri)){
                String fileName = getFilePath(uri);
                String subFolderName = getSubFolders(uri);

                if (fileName != null) {
                    return Environment.getExternalStorageDirectory().toString() + "/Download/"+subFolderName + fileName;
                }
                String id = DocumentsContract.getDocumentId(uri);

                final Uri contentUri = ContentUris.withAppendedId(Uri.parse("content://downloads/public_downloads"), Long.valueOf(id));
                return getDataColumn(contentUri, null, null);
            }
            else if (!isTree && isDownloadsDocument(uri)) {
                String fileName = getFilePath(uri);

                if (fileName != null) {
                    return Environment.getExternalStorageDirectory().toString() + "/Download/"+ fileName;
                }
                String id = DocumentsContract.getDocumentId(uri);
                if (id.startsWith("raw:")) {
                    id = id.replaceFirst("raw:", "");
                    File file = new File(id);
                    if (file.exists())
                        return id;
                }
                if (id.startsWith("raw%3A%2F")){
                    id = id.replaceFirst("raw%3A%2F", "");
                    File file = new File(id);
                    if (file.exists())
                        return id;
                }
                final Uri contentUri = ContentUris.withAppendedId(Uri.parse("content://downloads/public_downloads"), Long.valueOf(id));
                return getDataColumn(contentUri, null, null);
            }
            else if (!isTree && isMediaDocument(uri)) {
                final String docId = DocumentsContract.getDocumentId(uri);
                final String[] split = docId.split(":");
                final String type = split[0];

                Uri contentUri = null;
                if ("image".equals(type)) {
                    contentUri = MediaStore.Images.Media.EXTERNAL_CONTENT_URI;
                } else if ("video".equals(type)) {
                    contentUri = MediaStore.Video.Media.EXTERNAL_CONTENT_URI;
                } else if ("audio".equals(type)) {
                    contentUri = MediaStore.Audio.Media.EXTERNAL_CONTENT_URI;
                }

                final String selection = "_id=?";
                final String[] selectionArgs = new String[]{
                        split[1]
                };

                return getDataColumn(contentUri, selection, selectionArgs);
            }
        }
        else if ("content".equalsIgnoreCase(uri.getScheme())) {
            if (isGooglePhotosUri(uri))
                return uri.getLastPathSegment();
            else
                return getDataColumn(uri, null, null);
        }
        else if ("file".equalsIgnoreCase(uri.getScheme())) {
            return uri.getPath();
        }

        return null;
    }

    private static String getSubFolders(Uri uri) {
        String replaceChars = String.valueOf(uri).replace("%2F", "/").replace("%20", " ").replace("%3A",":");
        String[] bits = replaceChars.split("/");
        String sub5 = bits[bits.length - 2];
        String sub4 = bits[bits.length - 3];
        String sub3 = bits[bits.length - 4];
        String sub2 = bits[bits.length - 5];
        String sub1 = bits[bits.length - 6];
        if (sub1.equals("Download")){
            return sub2+"/"+sub3+"/"+sub4+"/"+sub5+"/";
        }
        else if (sub2.equals("Download")){
            return sub3+"/"+sub4+"/"+sub5+"/";
        }
        else if (sub3.equals("Download")){
            return sub4+"/"+sub5+"/";
        }
        else if (sub4.equals("Download")){
            return sub5+"/";
        }
        else {
            return "";
        }
    }

    private String getDataColumn(Uri uri, String selection, String[] selectionArgs) {
        Cursor cursor = null;
        final String column = "_data";
        final String[] projection = {column};
        try {
            cursor = activity.getContentResolver().query(uri, projection, selection, selectionArgs, null);
            if (cursor != null && cursor.moveToFirst()) {
                final int index = cursor.getColumnIndexOrThrow(column);
                return cursor.getString(index);
            }
        } catch (Exception e) {
        } finally {
            if (cursor != null)
                cursor.close();
        }
        return null;
    }

    private String getFilePath(Uri uri) {
        Cursor cursor = null;
        final String[] projection = {MediaStore.Files.FileColumns.DISPLAY_NAME};
        try {
            cursor = activity.getContentResolver().query(uri, projection, null, null,
                    null);
            if (cursor != null && cursor.moveToFirst()) {
                final int index = cursor.getColumnIndexOrThrow(MediaStore.Files.FileColumns.DISPLAY_NAME);
                return cursor.getString(index);
            }
        } catch (Exception e) {
        } finally {
            if (cursor != null)
                cursor.close();
        }
        return null;
    }

    private static boolean isExternalStorageDocument(Uri uri) {
        return "com.android.externalstorage.documents".equals(uri.getAuthority());
    }

    private static boolean isDownloadsDocument(Uri uri) {
        return "com.android.providers.downloads.documents".equals(uri.getAuthority());
    }

    private static boolean isRawDownloadsDocument(Uri uri) {
        String uriToString = String.valueOf(uri);
        return uriToString.contains("com.android.providers.downloads.documents/document/raw");
    }

    private static boolean isMediaDocument(Uri uri) {
        return "com.android.providers.media.documents".equals(uri.getAuthority());
    }

    private static boolean isGooglePhotosUri(Uri uri) {
        return "com.google.android.apps.photos.content".equals(uri.getAuthority());
    }

    public void saveScreenshot(String name, byte data[])
    {
        File path = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES);
        File file = new File(path, name);

        try {
            // Make sure the Pictures directory exists.
            path.mkdirs();

            OutputStream os = new FileOutputStream(file);
            try {
                os.write(data);
            } catch (IOException e) {
                try { os.close(); } catch (IOException e1) {}
                file.delete();
                throw e;
            }
            os.close();

            // Tell the media scanner about the new file so that it is
            // immediately available to the user.
            MediaScannerConnection.scanFile(activity,
                    new String[] { file.toString() }, null, null);
        } catch (IOException e) {
            // Unable to create file, likely because external storage is
            // not currently mounted.
            Log.w("hollycast", "saveScreenshot: Error writing " + file, e);
            throw new RuntimeException(e.getMessage());
        }
    }

    public void exportHomeDirectory()
    {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent = Intent.createChooser(intent, getResString(R.string.select_an_export_folder));
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        activity.startActivityForResult(intent, EXPORT_HOME_ACTIVITY_REQUEST);
    }

    public void onExportHomeResult(Intent data)
    {
        Uri uri = data == null ? null : data.getData();
        if (uri == null)
            // Cancelled
            return;
        HomeMover mover = new HomeMover(activity, this);
        mover.copyHome(activity.getExternalFilesDir(null).toURI().toString(), uri.toString(), getResString(R.string.exporting_home_folder));
    }

    public void importHomeDirectory()
    {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent = Intent.createChooser(intent, getResString(R.string.select_an_import_folder));
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        activity.startActivityForResult(intent, IMPORT_HOME_ACTIVITY_REQUEST);
    }

    public void onImportHomeResult(Intent data)
    {
        Uri uri = data == null ? null : data.getData();
        if (uri == null)
            // Cancelled
            return;
        HomeMover mover = new HomeMover(activity, this);
        mover.setReloadConfigOnCompletion(true);
        mover.copyHome(uri.toString(), activity.getExternalFilesDir(null).toURI().toString(), getResString(R.string.importing_home_folder));
    }

    public boolean requiresSafFilePicker() {
        // Android 10+ requires ASS
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q;
    }
}
