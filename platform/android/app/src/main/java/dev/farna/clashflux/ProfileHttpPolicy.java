package dev.farna.clashflux;

final class ProfileHttpPolicy {
    static final String DEFAULT_USER_AGENT = "clash-flux/0.1";
    static final String CLASH_META_USER_AGENT = "clash.meta";

    private ProfileHttpPolicy() {}

    static boolean shouldRetryWithClashMeta(int status, long maxBytes, String[] requestHeaders) {
        return status == 403 && maxBytes == 0 &&
                requestHeaders != null && requestHeaders.length == 0;
    }
}
