package dev.farna.clashflux;

import android.os.Bundle;
import android.os.SystemClock;

import java.util.ArrayDeque;
import java.util.ArrayList;

/** Short log relay so runtime-process diagnostics reach the UI process. */
final class RuntimeLogStore {
    private static final int MAX_ENTRIES = 500;
    private static final int MAX_ENTRY_CHARS = 4096;
    private static final int MAX_BATCH_ENTRIES = 32;
    private static final int MAX_BATCH_CHARS = 96 * 1024;
    private static final ArrayDeque<String> entries = new ArrayDeque<>();
    private static long nextSequence = SystemClock.elapsedRealtime() * 1000L + 1;

    private RuntimeLogStore() {}

    static synchronized void append(String stream, int level, String message) {
        if (message == null || message.isEmpty()) return;
        if (message.length() > MAX_ENTRY_CHARS) {
            message = message.substring(0, MAX_ENTRY_CHARS) + "…（日志已截断）";
        }
        entries.addLast(nextSequence++ + "|" + stream + "|" + level + "|" + message);
        while (entries.size() > MAX_ENTRIES) entries.removeFirst();
    }

    static synchronized Bundle after(long sequence) {
        Bundle result = new Bundle();
        ArrayList<String> updates = new ArrayList<>();
        long deliveredSequence = sequence;
        int batchChars = 0;
        for (String entry : entries) {
            int separator = entry.indexOf('|');
            if (separator <= 0) continue;
            try {
                long entrySequence = Long.parseLong(entry.substring(0, separator));
                if (entrySequence <= sequence) continue;
                if (updates.size() >= MAX_BATCH_ENTRIES
                        || batchChars + entry.length() > MAX_BATCH_CHARS) break;
                updates.add(entry);
                batchChars += entry.length();
                deliveredSequence = entrySequence;
            } catch (NumberFormatException ignored) {
            }
        }
        result.putLong("sequence", deliveredSequence);
        result.putStringArrayList("entries", updates);
        return result;
    }
}
