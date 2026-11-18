package dev.farna.clashflux;

/** Live-state decision for the launcher VPN shortcut. */
final class RuntimeTogglePolicy {
    private RuntimeTogglePolicy() {}

    /** state 1 = starting, 2 = connected, 3 = failed with retained owner. */
    static boolean shouldStopVpn(int vpnState, boolean runtimeOwned) {
        return vpnState == 1 || vpnState == 2 || (vpnState == 3 && runtimeOwned);
    }
}
