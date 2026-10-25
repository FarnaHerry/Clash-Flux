package dev.farna.clashflux;

public final class VpnUptimeTest {
    static void require(boolean value) { if (!value) throw new AssertionError("VPN session uptime"); }
    public static void main(String[] args) {
        VpnUptime clock = new VpnUptime();
        require(clock.snapshot(1000).elapsedMs == 0); // core-only / no VPN
        clock.update(1, 2000);
        require(clock.snapshot(9000).elapsedMs == 0); // consent/startup does not count
        clock.update(2, 10000);
        require(clock.snapshot(10000).elapsedMs == 0);
        require(clock.snapshot(15000).elapsedMs == 5000);
        // New Binder/UI observers read the same background-owned session.
        require(clock.snapshot(3610000).elapsedMs == 3600000); // includes screen-off time
        clock.update(2, 3611000); // repeated connected / rejected stop, still connected
        require(clock.snapshot(3612000).elapsedMs == 3602000);
        clock.update(0, 3613000);
        require(clock.snapshot(3614000).state == 0 && clock.snapshot(3614000).elapsedMs == 0);
        clock.update(1, 3615000);
        clock.update(3, 3616000);
        require(clock.snapshot(3617000).elapsedMs == 0); // failed/cancelled startup
        clock.update(2, 4000000);
        require(clock.snapshot(4000500).elapsedMs == 500); // new session
        require(clock.snapshot(3999999).elapsedMs == 0); // never a negative duration
        require(new VpnUptime().snapshot(5000000).elapsedMs == 0); // real service recreation
        System.out.println("VpnUptimeTest: ok");
    }
}
