package dev.farna.clashflux;

public final class RuntimeStartPolicyTest {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        require(RuntimeStartPolicy.preserveLiveVpn(true, false, true, false, false, false),
                "reattaching UI must preserve an established VPN");
        require(RuntimeStartPolicy.preserveLiveVpn(false, true, true, false, false, false),
                "reattaching UI must preserve an explicitly starting VPN");
        require(!RuntimeStartPolicy.preserveLiveVpn(false, false, false, false, false, false),
                "a fresh service cannot promote core-only startup from old persisted VPN flags");
        require(!RuntimeStartPolicy.preserveLiveVpn(true, false, false, false, false, false),
                "a cancelled runtime is not a live VPN");
        require(!RuntimeStartPolicy.preserveLiveVpn(true, false, true, true, false, false),
                "a pending stop cannot be reversed by a UI reattachment");
        require(!RuntimeStartPolicy.preserveLiveVpn(true, false, true, false, true, false),
                "a resident core remains core-only");
        require(!RuntimeStartPolicy.preserveLiveVpn(true, false, true, false, false, true),
                "a speed-test runtime cannot become a VPN through UI startup");

        require(RuntimeStartPolicy.mayStart(false, false, false, false, false), "initial start is permitted");
        require(!RuntimeStartPolicy.mayStart(true, true, false, false, false), "wait for a running core to close");
        require(!RuntimeStartPolicy.mayStart(true, false, true, false, false), "wait for a cancelled startup");
        require(!RuntimeStartPolicy.mayStart(true, false, false, true, false), "retain a server after close failure");
        require(!RuntimeStartPolicy.mayStart(true, false, false, false, true), "retain a tunnel after close failure");
        require(RuntimeStartPolicy.mayStart(true, false, false, false, false), "a fully stopped service can be reused");

        require(RuntimeStartPolicy.allowTun(false, false, true, false), "an explicit VPN request may establish TUN");
        require(!RuntimeStartPolicy.allowTun(true, false, true, false), "core-only must reject openTun");
        require(!RuntimeStartPolicy.allowTun(false, true, true, false), "speed tests must reject openTun");
        require(!RuntimeStartPolicy.allowTun(false, false, false, false), "cancelled startup must reject openTun");
        require(!RuntimeStartPolicy.allowTun(false, false, true, true), "stopping runtime must reject openTun");
        require(!RuntimeStartPolicy.retainInbound(true, "tun"), "core-only config must omit TUN in memory");
        require(RuntimeStartPolicy.retainInbound(false, "tun"), "VPN config must retain TUN");
        for (String type : new String[]{"mixed", "socks", "http", "direct", "redirect", "custom"}) {
            require(RuntimeStartPolicy.retainInbound(true, type), "unrelated inbound must survive: " + type);
        }
        System.out.println("RuntimeStartPolicyTest: ok");
    }
}
