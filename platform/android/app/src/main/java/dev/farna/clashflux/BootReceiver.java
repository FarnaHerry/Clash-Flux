package dev.farna.clashflux;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/**
 * Restores the VPN tunnel after boot or an app update. The runtime process
 * owns the persisted mode and can bootstrap itself without constructing the
 * HuxerUI activity or opening the UI process's native store.
 */
public final class BootReceiver extends BroadcastReceiver {
    private static final String PREFS = "clashflux";
    private static final String KEY_VPN_ACTIVE = "vpn_active";

    static void setVpnActive(Context context, boolean active) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit()
                .putBoolean(KEY_VPN_ACTIVE, active)
                .commit();
        RuntimeSnapshotStore.writeSync(context, "vpn-active.txt", active ? "1" : "0");
        VpnTileService.syncState(context, active);
    }

    static boolean isVpnActive(Context context) {
        String snapshot = RuntimeSnapshotStore.read(context, "vpn-active.txt", "");
        if (!snapshot.isEmpty()) return "1".equals(snapshot);
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getBoolean(KEY_VPN_ACTIVE, false);
    }

    @Override
    public void onReceive(Context context, Intent intent) {
        final String action = intent.getAction();
        if (!Intent.ACTION_BOOT_COMPLETED.equals(action)
                && !Intent.ACTION_MY_PACKAGE_REPLACED.equals(action)) {
            return;
        }
        final boolean wasActive = isVpnActive(context);
        if (!wasActive) {
            return;
        }
        Log.i("ClashFlux", "系统启动广播触发 VPN 恢复");
        try {
            context.startForegroundService(new Intent(context, ClashVpnService.class));
        } catch (RuntimeException error) {
            Log.e("ClashFlux", "系统启动后恢复 VPN 失败", error);
        }
    }
}
