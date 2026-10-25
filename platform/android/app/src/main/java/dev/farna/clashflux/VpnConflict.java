package dev.farna.clashflux;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.VpnService;
import android.os.Build;
import android.os.Process;
import android.util.Log;

/** Read-only, per-user VPN evidence. Persisted preferences are never ownership proof. */
final class VpnConflict {
    private VpnConflict() {}

    static boolean isOtherVpn(boolean vpn, int ownerUid, int ourUid, boolean prepared) {
        if (!vpn) return false;
        return ownerUid >= 0 ? ownerUid != ourUid : !prepared;
    }

    static String find(Context context) {
        try {
            ConnectivityManager manager = context.getSystemService(ConnectivityManager.class);
            if (manager == null) return ""; // Unknown: leave the normal system consent flow intact.
            for (Network network : manager.getAllNetworks()) {
                NetworkCapabilities capabilities = manager.getNetworkCapabilities(network);
                if (capabilities == null || !capabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) continue;
                int owner = Build.VERSION.SDK_INT >= 29 ? capabilities.getOwnerUid() : -1;
                // Older/privacy-limited systems expose no owner UID. prepare(context)
                // is authoritative current authorization, unlike stale vpn-active.txt.
                boolean prepared = owner < 0 && VpnService.prepare(context) == null;
                if (!isOtherVpn(true, owner, Process.myUid(), prepared)) continue;
                LinkProperties properties = manager.getLinkProperties(network);
                String name = properties == null ? "VPN" : properties.getInterfaceName();
                return context.getString(R.string.vpn_conflict, name == null ? "VPN" : name);
            }
        } catch (RuntimeException error) {
            Log.w("ClashFlux", "VPN conflict evidence unavailable", error);
        }
        return "";
    }
}
