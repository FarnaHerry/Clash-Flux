package dev.farna.clashflux;

public final class RuntimeModeReconcilerTest {
    private interface ThrowingRunnable {
        void run() throws Exception;
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void requireFailure(ThrowingRunnable action, String message) throws Exception {
        try {
            action.run();
            throw new AssertionError(message);
        } catch (IllegalStateException | IllegalArgumentException expected) {
            // Expected rejected or unconfirmed mode transition.
        }
    }

    public static void main(String[] args) throws Exception {
        RuntimeModeReconciler matching = new RuntimeModeReconciler();
        matching.update("rule");
        final int[] matchingWrites = {0};
        String matched = matching.reconcile("rule", mode -> ++matchingWrites[0], 100);
        require("rule".equals(matched) && matchingWrites[0] == 0,
                "an already matching cached mode must not be rewritten");

        RuntimeModeReconciler restored = new RuntimeModeReconciler();
        restored.update("direct");
        final int[] restoredWrites = {0};
        String reconciled = restored.reconcile("rule", mode -> {
            ++restoredWrites[0];
            restored.update(mode);
        }, 100);
        require("rule".equals(reconciled) && restoredWrites[0] == 1,
                "cached direct must be changed to the configured rule mode");
        require("rule".equals(restored.actualMode()),
                "the confirmed runtime mode must be published to observers");

        RuntimeModeReconciler asynchronous = new RuntimeModeReconciler();
        asynchronous.update("direct");
        final int[] asynchronousWrites = {0};
        String asynchronouslyConfirmed = asynchronous.reconcile("global", mode -> {
            ++asynchronousWrites[0];
            new Thread(() -> asynchronous.update(mode)).start();
        }, 1000);
        require("global".equals(asynchronouslyConfirmed) && asynchronousWrites[0] == 1,
                "reconciliation must wait for the libbox update callback");

        RuntimeModeReconciler latest = new RuntimeModeReconciler();
        latest.update("direct");
        final String[] latestTarget = {"rule"};
        final String[] latestWrites = {"", ""};
        final int[] latestWriteCount = {0};
        String latestConfirmed = latest.reconcileLatest(() -> latestTarget[0], mode -> {
            latestWrites[latestWriteCount[0]++] = mode;
            latest.update(mode);
            if ("rule".equals(mode)) latestTarget[0] = "global";
        }, 100);
        require("global".equals(latestConfirmed) && latestWriteCount[0] == 2
                        && "rule".equals(latestWrites[0]) && "global".equals(latestWrites[1]),
                "startup must reconcile again when the target changes during confirmation");

        RuntimeModeReconciler unconfirmed = new RuntimeModeReconciler();
        unconfirmed.update("direct");
        requireFailure(() -> unconfirmed.reconcile("rule", mode -> {}, 10),
                "a mode setter without a confirming callback must fail");
        requireFailure(() -> unconfirmed.reconcile("invalid", mode -> {}, 10),
                "unsupported modes must be rejected before writing");

        RuntimeModeReconciler reset = new RuntimeModeReconciler();
        reset.update("direct");
        requireFailure(() -> reset.reconcile("rule", mode -> reset.reset(), 1000),
                "a replaced control connection must cancel stale confirmation");
        require("".equals(reset.actualMode()), "a disconnected runtime has no observed mode");

        System.out.println("RuntimeModeReconcilerTest: ok");
    }
}
