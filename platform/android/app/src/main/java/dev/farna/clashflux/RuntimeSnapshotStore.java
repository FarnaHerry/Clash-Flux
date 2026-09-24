package dev.farna.clashflux;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Atomic file snapshots shared by the UI and runtime processes. */
final class RuntimeSnapshotStore {
    private static final String TAG = "ClashFlux";
    private static final ExecutorService WRITER = Executors.newSingleThreadExecutor(runnable -> {
        Thread thread = new Thread(runnable, "clashflux-runtime-snapshot");
        thread.setDaemon(true);
        return thread;
    });

    private RuntimeSnapshotStore() {}

    static void writeAsync(Context context, String name, String value) {
        if (context == null || value == null) return;
        final File directory = new File(context.getFilesDir(), "clash-flux/android-runtime");
        final File target = new File(directory, name);
        final File temporary = new File(directory, name + ".tmp");
        final byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        WRITER.execute(() -> writeNow(directory, target, temporary, bytes, name));
    }

    static void writeSync(Context context, String name, String value) {
        if (context == null || value == null) return;
        File directory = new File(context.getFilesDir(), "clash-flux/android-runtime");
        writeNow(directory, new File(directory, name),
                new File(directory, name + ".tmp"), value.getBytes(StandardCharsets.UTF_8), name);
    }

    private static void writeNow(File directory, File target, File temporary,
                                 byte[] bytes, String name) {
        try {
            if (!directory.isDirectory() && !directory.mkdirs()) {
                throw new IllegalStateException("无法创建 Android 运行时快照目录");
            }
            try (FileOutputStream output = new FileOutputStream(temporary, false)) {
                output.write(bytes);
                output.getFD().sync();
            }
            if (!temporary.renameTo(target)) {
                if (target.exists() && !target.delete()) {
                    throw new IllegalStateException("无法替换运行时快照 " + name);
                }
                if (!temporary.renameTo(target)) {
                    throw new IllegalStateException("无法提交运行时快照 " + name);
                }
            }
        } catch (Exception error) {
            temporary.delete();
            Log.w(TAG, "Unable to write runtime snapshot " + name, error);
        }
    }

    static String read(Context context, String name, String fallback) {
        if (context == null) return fallback;
        File file = new File(new File(context.getFilesDir(), "clash-flux/android-runtime"), name);
        if (!file.isFile()) return fallback;
        try (FileInputStream input = new FileInputStream(file);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            for (int count = input.read(buffer); count >= 0; count = input.read(buffer)) {
                if (count > 0) output.write(buffer, 0, count);
            }
            return output.toString(StandardCharsets.UTF_8.name());
        } catch (Exception error) {
            Log.w(TAG, "Unable to read runtime snapshot " + name, error);
            return fallback;
        }
    }
}
