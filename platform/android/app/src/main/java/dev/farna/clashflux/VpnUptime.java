package dev.farna.clashflux;

/** One background-owned VPN session clock. All times are elapsedRealtime ms. */
final class VpnUptime {
    static final class Snapshot {
        final int state;
        final long elapsedMs;
        Snapshot(int state, long elapsedMs) { this.state = state; this.elapsedMs = elapsedMs; }
    }

    private int state;
    private long connectedAt = -1;

    synchronized void update(int next, long now) {
        if (next == 2 && state != 2) connectedAt = now;
        else if (next != 2) connectedAt = -1;
        state = next;
    }

    synchronized Snapshot snapshot(long now) {
        return new Snapshot(state, state == 2 && connectedAt >= 0 ? Math.max(0, now - connectedAt) : 0);
    }
}
