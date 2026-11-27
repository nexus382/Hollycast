package com.hollycast.emulator;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.UUID;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.CRC32;

/**
 * Crash-recoverable replacement for document providers without atomic rename.
 * The provider is deliberately small so the transaction protocol can be tested
 * on a host JVM without Android framework classes.
 */
final class SaveSlotJournal {
    static final class Child {
        final String uri;
        final String name;
        Child(String uri, String name) {
            this.uri = uri;
            this.name = name;
        }
    }

    interface Provider {
        String parentOf(String document) throws IOException;
        String nameOf(String document) throws IOException;
        /** Returns null only when the child is absent. Query failures must throw. */
        String findChild(String parent, String name) throws IOException;
        List<Child> listChildren(String parent) throws IOException;
        String createChild(String parent, String name) throws IOException;
        InputStream openInput(String document) throws IOException;
        OutputStream openOutput(String document) throws IOException;
        /** Returns false only when deletion failed. */
        boolean delete(String document) throws IOException;
    }

    private static final String MARKER_SUFFIX = ".hollycast-transaction-";
    private static final String BACKUP_SUFFIX = ".hollycast-backup-";
    private static final String TEMP_SUFFIX = ".hollycast-tmp-";
    private static final String MARKER_MAGIC = "HCSJ1";
    private final Provider provider;

    SaveSlotJournal(Provider provider) {
        this.provider = provider;
    }

    synchronized boolean replace(String temporary, String destination) {
        String marker = null;
        String readyMarker = null;
        String backup = null;
        boolean prepareVerified = false;
        boolean readyVerified = false;
        try {
            recover(destination);
            String parent = provider.parentOf(destination);
            String name = provider.nameOf(destination);
            if (parent == null || parent.isEmpty() || name == null || name.isEmpty())
                throw new IOException("Save slot has no parent or name");

            String token = UUID.randomUUID().toString();
            String prepareName = markerName(name, 'p', token);
            String readyName = markerName(name, 'r', token);
            String current = provider.findChild(parent, name);
            boolean hadPrevious = current != null;
            String backupName = hadPrevious ? backupPrefix(name) + token : "-";

            // PREPARE is durable before creating a backup. Recovery knows this
            // phase never changed the live slot, even if the backup is partial.
            marker = createNamedChild(parent, prepareName);
            writeAndVerify(marker, markerBytes(false, hadPrevious, backupName));
            prepareVerified = true;

            if (current != null) {
                backup = createNamedChild(parent, backupName);
                if (!copyAndVerify(current, backup)) {
                    recover(destination);
                    return false;
                }
            }

            readyMarker = createNamedChild(parent, readyName);
            writeAndVerify(readyMarker, markerBytes(true, hadPrevious, backupName));
            readyVerified = true;

            // READY is verified before PREPARE is removed. The slot is not
            // changed until the complete rollback record is durable.
            if (!provider.delete(marker))
                throw new IOException("Could not remove the save preparation marker");
            marker = null;

            if (current == null) {
                current = createNamedChild(parent, name);
            }
            if (!copyAndVerify(temporary, current)) {
                recover(destination);
                return false;
            }

            // Once the marker is gone the new destination is authoritative.
            // In rollback, the marker remains until the old bytes are restored.
            if (!provider.delete(readyMarker))
                throw new IOException("Could not remove the save transaction marker");
            readyMarker = null;
            if (backup != null) {
                try {
                    if (!provider.delete(backup))
                        throw new IOException("Could not remove the save backup");
                } catch (IOException | RuntimeException cleanupError) {
                    // The new slot is committed. An unreferenced unique backup is harmless.
                }
            }
            try {
                provider.delete(temporary);
            } catch (IOException | RuntimeException ignored) {
                // The committed slot is valid; a failed staging cleanup can be retried later.
            }
            return true;
        } catch (IOException | RuntimeException error) {
            if (readyVerified || prepareVerified) {
                try {
                    recover(destination);
                } catch (IOException | RuntimeException recoveryError) {
                    error.addSuppressed(recoveryError);
                }
            } else {
                // No destination bytes are changed before READY is verified.
                removeQuietly(marker);
                removeQuietly(readyMarker);
                removeQuietly(backup);
            }
            return false;
        }
    }

    synchronized void recover(String destination) throws IOException {
        String parent = provider.parentOf(destination);
        String name = provider.nameOf(destination);
        if (parent == null || parent.isEmpty() || name == null || name.isEmpty())
            return;
        List<MarkerDocument> markers = findMarkers(parent, name);
        if (markers.isEmpty()) {
            removeOrphanBackups(parent, name);
            return;
        }
        MarkerDocument ready = null;
        MarkerDocument prepare = null;
        for (MarkerDocument marker : markers) {
            if (marker.state == null)
                continue;
            if (marker.state.ready) {
                if (ready != null)
                    throw new IOException("Multiple save transactions need recovery");
                ready = marker;
            } else {
                if (prepare != null)
                    throw new IOException("Multiple save preparations need recovery");
                prepare = marker;
            }
        }

        if (ready == null) {
            // PREPARE precedes backup creation and all destination writes.
            // The slot is untouched; remove the records before best-effort
            // cleanup so a refused backup deletion cannot block later reads.
            for (MarkerDocument marker : markers)
                deleteMarker(marker.uri);
            removeOrphanBackups(parent, name);
            return;
        }

        Marker state = ready.state;
        String destinationFile = provider.findChild(parent, name);
        if (state.hadPrevious) {
            String backup = provider.findChild(parent, state.backupName);
            if (backup == null)
                throw new IOException("Save transaction backup is missing");
            if (destinationFile == null)
                destinationFile = createNamedChild(parent, name);
            if (!sameContents(backup, destinationFile) && !copyAndVerify(backup, destinationFile))
                throw new IOException("Could not restore the verified save backup");
        } else {
            if (destinationFile != null && !provider.delete(destinationFile))
                throw new IOException("Could not remove the incomplete first save");
        }

        // Remove records only after rollback is complete. If either deletion
        // is interrupted, the remaining record repeats the verified rollback.
        if (prepare != null)
            deleteMarker(prepare.uri);
        deleteMarker(ready.uri);
        for (MarkerDocument marker : markers)
            if (marker != ready && marker != prepare)
                deleteMarker(marker.uri);
        removeOrphanBackups(parent, name);
    }

    private String createNamedChild(String parent, String name) throws IOException {
        if (provider.findChild(parent, name) != null)
            throw new IOException("A document already occupies save transaction name " + name);
        String child = provider.createChild(parent, name);
        if (child == null)
            throw new IOException("Could not create save transaction document " + name);
        if (!name.equals(provider.nameOf(child))) {
            removeQuietly(child);
            throw new IOException("Provider changed save transaction document name " + name);
        }
        String resolved = provider.findChild(parent, name);
        if (!child.equals(resolved)) {
            removeQuietly(child);
            throw new IOException("Provider returned an ambiguous save transaction document " + name);
        }
        return child;
    }

    private void writeAndVerify(String document, byte[] bytes) throws IOException {
        try (OutputStream output = provider.openOutput(document)) {
            if (output == null)
                throw new IOException("Could not open save transaction marker");
            output.write(bytes);
            output.flush();
        }
        byte[] actual = readAll(document);
        if (!java.util.Arrays.equals(bytes, actual))
            throw new IOException("Save transaction marker verification failed");
    }

    private boolean copyAndVerify(String source, String destination) throws IOException {
        CRC32 expected = new CRC32();
        long expectedSize = 0;
        try (InputStream input = provider.openInput(source);
             OutputStream output = provider.openOutput(destination)) {
            if (input == null || output == null)
                throw new IOException("Could not open save transaction copy streams");
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
        try (InputStream input = provider.openInput(destination)) {
            if (input == null)
                throw new IOException("Could not verify save transaction copy");
            byte[] buffer = new byte[65536];
            int count;
            while ((count = input.read(buffer)) != -1) {
                actual.update(buffer, 0, count);
                actualSize += count;
            }
        }
        return expectedSize == actualSize && expected.getValue() == actual.getValue();
    }

    private boolean sameContents(String first, String second) throws IOException {
        try (InputStream left = provider.openInput(first); InputStream right = provider.openInput(second)) {
            if (left == null || right == null)
                throw new IOException("Could not compare save transaction documents");
            byte[] leftBuffer = new byte[65536];
            byte[] rightBuffer = new byte[65536];
            while (true) {
                int leftCount = readChunk(left, leftBuffer);
                int rightCount = readChunk(right, rightBuffer);
                if (leftCount != rightCount)
                    return false;
                if (leftCount == -1)
                    return true;
                for (int i = 0; i < leftCount; i++)
                    if (leftBuffer[i] != rightBuffer[i])
                        return false;
            }
        }
    }

    private int readChunk(InputStream input, byte[] buffer) throws IOException {
        int total = 0;
        while (total < buffer.length) {
            int count = input.read(buffer, total, buffer.length - total);
            if (count == -1)
                return total == 0 ? -1 : total;
            total += count;
        }
        return total;
    }

    private byte[] readAll(String document) throws IOException {
        try (InputStream input = provider.openInput(document);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            if (input == null)
                throw new IOException("Could not read save transaction document");
            byte[] buffer = new byte[256];
            int count;
            while ((count = input.read(buffer)) != -1) {
                if (output.size() + count > 4096)
                    throw new IOException("Save transaction marker is too large");
                output.write(buffer, 0, count);
            }
            return output.toByteArray();
        }
    }

    private byte[] markerBytes(boolean ready, boolean hadPrevious, String backupName) {
        String value = MARKER_MAGIC + "\t" + (ready ? "R" : "P") + "\t"
                + (hadPrevious ? "1" : "0") + "\t" + backupName + "\n";
        return value.getBytes(StandardCharsets.UTF_8);
    }

    private Marker parseMarker(byte[] bytes, String name, char phase, String token) {
        String[] fields = new String(bytes, StandardCharsets.UTF_8).split("\\t", -1);
        if (fields.length != 4 || !MARKER_MAGIC.equals(fields[0])
                || !(phase == 'p' ? "P" : "R").equals(fields[1]) || !fields[3].endsWith("\n"))
            return null;
        String backupName = fields[3].substring(0, fields[3].length() - 1);
        if ("0".equals(fields[2]) && "-".equals(backupName))
            return new Marker(phase == 'r', false, "");
        if ("1".equals(fields[2]) && (backupPrefix(name) + token).equals(backupName))
            return new Marker(phase == 'r', true, backupName);
        return null;
    }

    private List<MarkerDocument> findMarkers(String parent, String name) throws IOException {
        String prefix = markerPrefix(name);
        List<MarkerDocument> result = new ArrayList<>();
        for (Child child : provider.listChildren(parent)) {
            if (!child.name.startsWith(prefix))
                continue;
            String suffix = child.name.substring(prefix.length());
            if (suffix.length() < 3 || suffix.charAt(1) != '-')
                continue;
            char phase = suffix.charAt(0);
            String token = suffix.substring(2);
            if ((phase != 'p' && phase != 'r') || !isTransactionToken(token))
                continue;
            Marker state = parseMarker(readAll(child.uri), name, phase, token);
            // A recognized attempt name with an incomplete payload was created
            // before READY and cannot have changed the live slot.
            result.add(new MarkerDocument(child.uri, state));
        }
        return result;
    }

    private void deleteMarker(String marker) throws IOException {
        if (!provider.delete(marker))
            throw new IOException("Could not remove save transaction marker");
    }

    private void removeOrphanBackups(String parent, String name) throws IOException {
        String prefix = backupPrefix(name);
        for (Child child : provider.listChildren(parent)) {
            if (!child.name.startsWith(prefix)
                    || !isTransactionToken(child.name.substring(prefix.length())))
                continue;
            try {
                provider.delete(child.uri);
            } catch (IOException | RuntimeException ignored) {
                // Record removal is the commit point. Orphan cleanup cannot
                // make a verified live slot unreadable.
            }
        }
    }

    private static boolean isTransactionToken(String token) {
        try {
            return UUID.fromString(token).toString().equals(token);
        } catch (IllegalArgumentException invalid) {
            return false;
        }
    }

    private void removeQuietly(String path) {
        if (path == null)
            return;
        try {
            provider.delete(path);
        } catch (IOException | RuntimeException ignored) {
        }
    }

    private static final class Marker {
        final boolean ready;
        final boolean hadPrevious;
        final String backupName;
        Marker(boolean ready, boolean hadPrevious, String backupName) {
            this.ready = ready;
            this.hadPrevious = hadPrevious;
            this.backupName = backupName;
        }
    }

    private static final class MarkerDocument {
        final String uri;
        final Marker state;
        MarkerDocument(String uri, Marker state) {
            this.uri = uri;
            this.state = state;
        }
    }

    static String temporaryName(String destinationName) {
        return temporaryPrefix(destinationName) + UUID.randomUUID();
    }

    static String markerName(String name, char phase, String token) {
        return markerPrefix(name) + phase + "-" + token;
    }

    static String markerPrefix(String name) {
        return artifactStem(name) + MARKER_SUFFIX;
    }

    static String backupName(String name, String token) {
        return backupPrefix(name) + token;
    }

    private static String backupPrefix(String name) {
        return artifactStem(name) + BACKUP_SUFFIX;
    }

    private static String temporaryPrefix(String name) {
        return artifactStem(name) + TEMP_SUFFIX;
    }

    private static String artifactStem(String name) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(name.getBytes(StandardCharsets.UTF_8));
            StringBuilder hex = new StringBuilder(".hcs-");
            for (int i = 0; i < 12; i++)
                hex.append(String.format("%02x", digest[i] & 0xff));
            return hex.toString();
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }
}
