package dev.farna.clashflux;

public final class ProfileHttpPolicyTest {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        require(ProfileHttpPolicy.DEFAULT_USER_AGENT.equals("clash-flux/0.1"),
                "ordinary subscriptions keep the v0.3.82 user agent");
        require(ProfileHttpPolicy.shouldRetryWithClashMeta(403, 0, new String[0]),
                "a forbidden plain subscription may retry with the Clash identity");
        require(!ProfileHttpPolicy.shouldRetryWithClashMeta(200, 0, new String[0]) &&
                        !ProfileHttpPolicy.shouldRetryWithClashMeta(401, 0, new String[0]) &&
                        !ProfileHttpPolicy.shouldRetryWithClashMeta(404, 0, new String[0]),
                "only HTTP 403 selects the compatibility retry");
        require(!ProfileHttpPolicy.shouldRetryWithClashMeta(403, 1024, new String[0]),
                "rule-set downloads keep their original user agent");
        require(!ProfileHttpPolicy.shouldRetryWithClashMeta(403, 0,
                        new String[]{"User-Agent", "provider-test/1"}),
                "explicit request headers are never replaced by a fallback");
        require(!ProfileHttpPolicy.shouldRetryWithClashMeta(403, 0, null),
                "invalid request headers are not retried");
        System.out.println("ProfileHttpPolicyTest: ok");
    }
}
