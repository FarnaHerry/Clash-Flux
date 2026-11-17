package dev.farna.clashflux;

final class ProfileHttpPolicy {
    static final String[] SUBSCRIPTION_USER_AGENTS = {"sing-box", "clash.meta", "clash-flux"};
    static final String RESOURCE_USER_AGENT = "clash-flux/0.1";

    private ProfileHttpPolicy() {}

    static boolean shouldTryNextUserAgent(int status, boolean subscriptionRequest, String[] requestHeaders) {
        if (status != 403 || !subscriptionRequest || requestHeaders == null || requestHeaders.length % 2 != 0)
            return false;
        for (int i = 0; i < requestHeaders.length; i += 2) {
            String name = requestHeaders[i];
            if (!"if-none-match".equalsIgnoreCase(name) && !"if-modified-since".equalsIgnoreCase(name)) return false;
        }
        return true;
    }
}
