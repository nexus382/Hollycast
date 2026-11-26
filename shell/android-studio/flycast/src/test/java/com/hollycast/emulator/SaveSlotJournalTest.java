package com.hollycast.emulator;

import org.junit.Test;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import static org.junit.Assert.*;

public class SaveSlotJournalTest {
    private static final String ROOT = "opaque-parent-id";
    private static final String SLOT = "pending-save-address";
    private static final byte[] OLD = bytes("known complete previous slot");
    private static final byte[] NEW = bytes("short replacement");

    @Test
    public void everyProviderInterruptionRecoversIdempotently() throws Exception {
        FakeProvider baseline = providerWithOldSlot();
        assertTrue(new SaveSlotJournal(baseline).replace(baseline.tempId, SLOT));
        List<String> transitions = new ArrayList<>(baseline.events);
        assertTrue("must exercise the journal protocol", transitions.size() > 12);

        for (int interruption = 1; interruption <= transitions.size(); interruption++) {
            FakeProvider provider = providerWithOldSlot();
            provider.crashAt = interruption;
            try {
                new SaveSlotJournal(provider).replace(provider.tempId, SLOT);
            } catch (SimulatedCrash expected) {
                // A process interruption bypasses normal error cleanup.
            }
            provider.crashAt = -1;
            SaveSlotJournal restarted = new SaveSlotJournal(provider);
            restarted.recover(SLOT);
            byte[] recovered = provider.contentsOfSlot();
            assertTrue("interruption " + interruption + " left a torn slot",
                    Arrays.equals(OLD, recovered) || Arrays.equals(NEW, recovered));
            restarted.recover(SLOT);
            assertArrayEquals("retry changed the recovered slot", recovered, provider.contentsOfSlot());
            assertArrayEquals(bytes("unrelated user document"), provider.data.get(provider.unrelatedId));
            assertNoJournalArtifacts(provider);
        }
    }

    @Test
    public void opaqueDocumentIdsWorkAndReadOnlyDestinationPreservesOldBytes() throws Exception {
        FakeProvider provider = providerWithOldSlot();
        SaveSlotJournal journal = new SaveSlotJournal(provider);
        assertTrue(journal.replace(provider.tempId, SLOT));
        assertArrayEquals(NEW, provider.contentsOfSlot());

        provider.addTemp("next-stage", bytes("another replacement"));
        byte[] prior = provider.contentsOfSlot();
        provider.readOnlyDestination = true;
        assertFalse(journal.replace("next-stage", SLOT));
        provider.readOnlyDestination = false;
        journal.recover(SLOT);
        assertArrayEquals(prior, provider.contentsOfSlot());
        assertArrayEquals(bytes("unrelated user document"), provider.data.get(provider.unrelatedId));
    }

    @Test
    public void providerQueryErrorIsNotTreatedAsAnAbsentSlot() throws Exception {
        FakeProvider provider = providerWithOldSlot();
        byte[] prior = provider.contentsOfSlot();
        provider.queryFailure = true;
        assertFalse(new SaveSlotJournal(provider).replace(provider.tempId, SLOT));
        provider.queryFailure = false;
        assertArrayEquals(prior, provider.contentsOfSlot());
    }

    @Test
    public void recoveryDoesNotDeleteAnotherActiveStagingAttempt() throws Exception {
        FakeProvider provider = providerWithOldSlot();
        String first = provider.tempId;
        provider.addTemp("second-stage", bytes("parallel staged state"));
        String second = provider.tempId;
        assertTrue(new SaveSlotJournal(provider).replace(first, SLOT));
        assertArrayEquals(bytes("parallel staged state"), provider.data.get(second));
    }

    @Test
    public void unrelatedEmptyDocumentInJournalNamespaceIsPreserved() throws Exception {
        FakeProvider provider = providerWithOldSlot();
        String markerId = "unrelated-marker-id";
        byte[] unrelatedMarker = new byte[0];
        provider.add(ROOT, "file", markerId, SaveSlotJournal.markerPrefix("slot.state") + "user-file", unrelatedMarker);
        assertTrue(new SaveSlotJournal(provider).replace(provider.tempId, SLOT));
        assertArrayEquals(NEW, provider.contentsOfSlot());
        assertArrayEquals(unrelatedMarker, provider.data.get(markerId));
    }

    @Test
    public void firstSaveInterruptionLeavesNoPartialSlot() throws Exception {
        FakeProvider baseline = providerWithoutSlot();
        assertTrue(new SaveSlotJournal(baseline).replace(baseline.tempId, SLOT));
        int transitions = baseline.events.size();
        for (int interruption = 1; interruption <= transitions; interruption++) {
            FakeProvider provider = providerWithoutSlot();
            provider.crashAt = interruption;
            try {
                new SaveSlotJournal(provider).replace(provider.tempId, SLOT);
            } catch (SimulatedCrash expected) {
                // Restart below exercises the production recovery protocol.
            }
            provider.crashAt = -1;
            SaveSlotJournal restarted = new SaveSlotJournal(provider);
            restarted.recover(SLOT);
            byte[] current = provider.contentsOfSlot();
            assertTrue("interruption " + interruption + " left a partial new slot",
                    current == null || Arrays.equals(NEW, current));
            restarted.recover(SLOT);
            assertArrayEquals(current, provider.contentsOfSlot());
            assertArrayEquals(bytes("unrelated user document"), provider.data.get(provider.unrelatedId));
            assertNoJournalArtifacts(provider);
        }
    }

    @Test
    public void overwriteRollbackCanBeInterruptedAtEveryRecoveryTransition() throws Exception {
        FakeProvider baseline = prepareOverwriteRollback();
        new SaveSlotJournal(baseline).recover(SLOT);
        int transitions = baseline.events.size();
        assertTrue(transitions > 8);
        for (int interruption = 1; interruption <= transitions; interruption++) {
            FakeProvider provider = prepareOverwriteRollback();
            provider.crashAt = interruption;
            SaveSlotJournal restarted = new SaveSlotJournal(provider);
            try {
                restarted.recover(SLOT);
            } catch (SimulatedCrash expected) {
                // Retry the same durable rollback record after another restart.
            }
            provider.crashAt = -1;
            restarted.recover(SLOT);
            assertArrayEquals(OLD, provider.contentsOfSlot());
            restarted.recover(SLOT);
            assertArrayEquals(OLD, provider.contentsOfSlot());
            assertNoJournalArtifacts(provider);
            assertArrayEquals(bytes("unrelated user document"), provider.data.get(provider.unrelatedId));
        }
    }

    @Test
    public void firstSaveRollbackCanBeInterruptedAtEveryRecoveryTransition() throws Exception {
        FakeProvider baseline = prepareFirstSaveRollback();
        new SaveSlotJournal(baseline).recover(SLOT);
        int transitions = baseline.events.size();
        assertTrue(transitions > 5);
        for (int interruption = 1; interruption <= transitions; interruption++) {
            FakeProvider provider = prepareFirstSaveRollback();
            provider.crashAt = interruption;
            SaveSlotJournal restarted = new SaveSlotJournal(provider);
            try {
                restarted.recover(SLOT);
            } catch (SimulatedCrash expected) {
                // Retry the rollback after its interrupted cleanup step.
            }
            provider.crashAt = -1;
            restarted.recover(SLOT);
            assertNull(provider.contentsOfSlot());
            restarted.recover(SLOT);
            assertNull(provider.contentsOfSlot());
            assertNoJournalArtifacts(provider);
            assertArrayEquals(bytes("unrelated user document"), provider.data.get(provider.unrelatedId));
        }
    }

    private static FakeProvider prepareOverwriteRollback() {
        FakeProvider provider = providerWithOldSlot();
        provider.crashOnSlotWrite = true;
        try {
            new SaveSlotJournal(provider).replace(provider.tempId, SLOT);
            throw new AssertionError("expected an interruption during destination write");
        } catch (SimulatedCrash expected) {
            provider.crashOnSlotWrite = false;
            provider.crashAt = -1;
            provider.eventCount = 0;
            provider.events.clear();
            return provider;
        }
    }

    private static FakeProvider prepareFirstSaveRollback() {
        FakeProvider provider = providerWithoutSlot();
        provider.crashOnSlotWrite = true;
        try {
            new SaveSlotJournal(provider).replace(provider.tempId, SLOT);
            throw new AssertionError("expected an interruption during first-slot write");
        } catch (SimulatedCrash expected) {
            provider.crashOnSlotWrite = false;
            provider.crashAt = -1;
            provider.eventCount = 0;
            provider.events.clear();
            return provider;
        }
    }

    private static FakeProvider providerWithOldSlot() {
        FakeProvider provider = new FakeProvider();
        provider.add(ROOT, "folder", "state-slot-id-opaque", "slot.state", OLD);
        provider.add("opaque-parent-id", "folder", "unrelated-id-opaque", "notes.txt", bytes("unrelated user document"));
        provider.unrelatedId = "unrelated-id-opaque";
        provider.addTemp("temp-stage-id", NEW);
        return provider;
    }

    private static FakeProvider providerWithoutSlot() {
        FakeProvider provider = new FakeProvider();
        provider.add(ROOT, "folder", "unrelated-id-opaque", "notes.txt", bytes("unrelated user document"));
        provider.unrelatedId = "unrelated-id-opaque";
        provider.addTemp("temp-stage-id", NEW);
        return provider;
    }

    private static byte[] bytes(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }

    private static void assertNoJournalArtifacts(FakeProvider provider) {
        for (Doc doc : provider.docs.values()) {
            assertFalse("left a journal artifact: " + doc.name,
                    doc.name.contains(".hollycast-backup-") || doc.name.contains(".hollycast-transaction-"));
        }
    }

    private static final class Doc {
        final String parent;
        final String name;
        byte[] bytes;
        Doc(String parent, String name, byte[] bytes) {
            this.parent = parent;
            this.name = name;
            this.bytes = bytes;
        }
    }

    private static final class SimulatedCrash extends Error {}

    private static final class FakeProvider implements SaveSlotJournal.Provider {
        final Map<String, Doc> docs = new LinkedHashMap<>();
        final Map<String, byte[]> data = new LinkedHashMap<>();
        final List<String> events = new ArrayList<>();
        int crashAt = -1;
        int eventCount;
        boolean readOnlyDestination;
        boolean queryFailure;
        boolean crashOnSlotWrite;
        String tempId = "temp-stage-id";
        String unrelatedId;

        void add(String parent, String type, String id, String name, byte[] contents) {
            docs.put(id, new Doc(parent, name, contents.clone()));
            data.put(id, contents.clone());
        }

        void addTemp(String id, byte[] contents) {
            tempId = id;
            add(ROOT, "file", id, SaveSlotJournal.temporaryName("slot.state"), contents);
        }

        byte[] contentsOfSlot() {
            try {
                String id = findChild(ROOT, "slot.state");
                return id == null ? null : data.get(id).clone();
            } catch (IOException error) {
                throw new AssertionError(error);
            }
        }

        private void event(String value) {
            events.add(value);
            ++eventCount;
            Doc target = value.startsWith("write:") ? docs.get(value.substring("write:".length())) : null;
            if (eventCount == crashAt || (crashOnSlotWrite && target != null && "slot.state".equals(target.name))) {
                crashAt = -1;
                crashOnSlotWrite = false;
                throw new SimulatedCrash();
            }
        }

        @Override public String parentOf(String document) throws IOException {
            event("parent:" + document);
            if (SLOT.equals(document)) return ROOT;
            Doc doc = docs.get(document);
            if (doc == null) throw new IOException("Unknown opaque URI " + document);
            return doc.parent;
        }

        @Override public String nameOf(String document) throws IOException {
            event("name:" + document);
            if (SLOT.equals(document)) return "slot.state";
            Doc doc = docs.get(document);
            if (doc == null) throw new IOException("Unknown opaque URI " + document);
            return doc.name;
        }

        @Override public String findChild(String parent, String name) throws IOException {
            event("query:" + name);
            if (queryFailure) throw new IOException("Provider query failed");
            String match = null;
            for (Map.Entry<String, Doc> entry : docs.entrySet()) {
                Doc doc = entry.getValue();
                if (parent.equals(doc.parent) && name.equals(doc.name)) {
                    if (match != null) throw new IOException("Ambiguous display name");
                    match = entry.getKey();
                }
            }
            return match;
        }

        @Override public List<SaveSlotJournal.Child> listChildren(String parent) throws IOException {
            event("list:" + parent);
            if (queryFailure) throw new IOException("Provider query failed");
            List<SaveSlotJournal.Child> result = new ArrayList<>();
            for (Map.Entry<String, Doc> entry : docs.entrySet())
                if (parent.equals(entry.getValue().parent))
                    result.add(new SaveSlotJournal.Child(entry.getKey(), entry.getValue().name));
            return result;
        }

        @Override public String createChild(String parent, String name) throws IOException {
            String id = "opaque-created-" + UUID.randomUUID();
            docs.put(id, new Doc(parent, name, new byte[0]));
            data.put(id, new byte[0]);
            event("create:" + name);
            return id;
        }

        @Override public InputStream openInput(String document) throws IOException {
            event("open-read:" + document);
            byte[] contents = data.get(document);
            if (contents == null) throw new IOException("Missing document");
            return new ByteArrayInputStream(contents.clone());
        }

        @Override public OutputStream openOutput(String document) throws IOException {
            if (readOnlyDestination && "slot.state".equals(docs.get(document).name))
                throw new IOException("Document is read only");
            ByteArrayOutputStream output = new ByteArrayOutputStream() {
                @Override public void close() throws IOException {
                    super.close();
                    data.put(document, toByteArray());
                    docs.get(document).bytes = toByteArray();
                    event("close-write:" + document);
                }
            };
            data.put(document, new byte[0]);
            docs.get(document).bytes = new byte[0];
            event("open-write:" + document);
            return new OutputStream() {
                @Override public void write(int value) {
                    output.write(value);
                    data.put(document, output.toByteArray());
                    docs.get(document).bytes = output.toByteArray();
                    event("write:" + document);
                }
                @Override public void write(byte[] bytes, int offset, int length) {
                    int partial = Math.max(1, length / 2);
                    output.write(bytes, offset, partial);
                    data.put(document, output.toByteArray());
                    docs.get(document).bytes = output.toByteArray();
                    event("write:" + document);
                    if (partial < length) {
                        output.write(bytes, offset + partial, length - partial);
                        data.put(document, output.toByteArray());
                        docs.get(document).bytes = output.toByteArray();
                    }
                }
                @Override public void flush() {
                    data.put(document, output.toByteArray());
                    docs.get(document).bytes = output.toByteArray();
                    event("flush:" + document);
                }
                @Override public void close() throws IOException { output.close(); }
            };
        }

        @Override public boolean delete(String document) {
            docs.remove(document);
            data.remove(document);
            event("delete:" + document);
            return true;
        }
    }
}
