package dev.farna.clashflux;

public final class ProfileHttpPolicyTest {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        require(ProfileHttpPolicy.shouldTryNextUserAgent(403, true, new String[0]),
                "a forbidden plain subscription may try the next identity");
        require(!ProfileHttpPolicy.shouldTryNextUserAgent(200, true, new String[0]) &&
                        !ProfileHttpPolicy.shouldTryNextUserAgent(401, true, new String[0]) &&
                        !ProfileHttpPolicy.shouldTryNextUserAgent(404, true, new String[0]),
                "only HTTP 403 selects the compatibility retry");
        require(!ProfileHttpPolicy.shouldTryNextUserAgent(403, false, new String[0]),
                "rule-set downloads keep their original user agent");
        require(!ProfileHttpPolicy.shouldTryNextUserAgent(403, true,
                        new String[]{"User-Agent", "provider-test/1"}),
                "explicit request headers are never replaced by a fallback");
        require(!ProfileHttpPolicy.shouldTryNextUserAgent(403, true, null),
                "invalid request headers are not retried");
        require(ProfileHttpPolicy.shouldTryNextUserAgent(403, true,
                        new String[]{"If-None-Match", "test-etag"}),
                "a bounded ordinary subscription retains identity fallback during revalidation");
        ProfileHttpDownloaderTest.run();
        System.out.println("ProfileHttpPolicyTest: ok");
    }
}
