package dev.farna.clashflux;

public final class VpnConflictTest {
    private static void check(boolean value) {
        if (!value) throw new AssertionError("Android VPN conflict evidence");
    }
    public static void main(String[] args) {
        check(!VpnConflict.isOtherVpn(false, 2000, 1000, false));
        check(!VpnConflict.isOtherVpn(true, 1000, 1000, false));
        check(VpnConflict.isOtherVpn(true, 2000, 1000, false));
        check(VpnConflict.isOtherVpn(true, 2000, 1000, true)); // stale preparation cannot override known foreign owner
        check(!VpnConflict.isOtherVpn(true, -1, 1000, true));
        check(VpnConflict.isOtherVpn(true, -1, 1000, false));
        System.out.println("Android VPN ownership/conflict cases passed");
    }
}
