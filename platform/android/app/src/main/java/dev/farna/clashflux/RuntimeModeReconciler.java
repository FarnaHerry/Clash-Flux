package dev.farna.clashflux;

import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

/** Observes libbox mode callbacks and confirms requested changes before reporting success. */
final class RuntimeModeReconciler {
    interface Writer {
        void write(String mode) throws Exception;
    }

    private final Object operationLock = new Object();
    private final Object stateLock = new Object();
    private String actualMode = "";
    private long generation;

    void reset() {
        synchronized (stateLock) {
            actualMode = "";
            ++generation;
            stateLock.notifyAll();
        }
    }

    void update(String mode) {
        if (mode == null || mode.isEmpty()) return;
        synchronized (stateLock) {
            actualMode = mode;
            stateLock.notifyAll();
        }
    }

    String actualMode() {
        synchronized (stateLock) {
            return actualMode;
        }
    }

    String reconcile(String expectedMode, Writer writer, long timeoutMillis) throws Exception {
        requireSupported(expectedMode);
        synchronized (operationLock) {
            final long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMillis);
            final long expectedGeneration;
            synchronized (stateLock) {
                expectedGeneration = generation;
                while (actualMode.isEmpty()) {
                    await(stateLock, expectedGeneration, deadline);
                }
                if (expectedMode.equals(actualMode)) return actualMode;
            }
            writer.write(expectedMode);
            synchronized (stateLock) {
                while (!expectedMode.equals(actualMode)) {
                    await(stateLock, expectedGeneration, deadline);
                }
                return actualMode;
            }
        }
    }

    String reconcileLatest(Supplier<String> expectedMode, Writer writer,
                           long timeoutMillis) throws Exception {
        for (;;) {
            String expected = expectedMode.get();
            String actual = reconcile(expected, writer, timeoutMillis);
            if (expected.equals(expectedMode.get())) return actual;
        }
    }

    static void requireSupported(String mode) {
        if (!"rule".equals(mode) && !"global".equals(mode) && !"direct".equals(mode)) {
            throw new IllegalArgumentException("Unsupported Clash mode");
        }
    }

    private void await(Object lock, long expectedGeneration, long deadline) throws InterruptedException {
        long remaining = deadline - System.nanoTime();
        if (generation != expectedGeneration) {
            throw new IllegalStateException("Clash mode control connection was reset");
        }
        if (remaining <= 0) throw new IllegalStateException("Timed out confirming Clash mode");
        long millis = TimeUnit.NANOSECONDS.toMillis(remaining);
        int nanos = (int) (remaining - TimeUnit.MILLISECONDS.toNanos(millis));
        lock.wait(millis, nanos);
    }
}
