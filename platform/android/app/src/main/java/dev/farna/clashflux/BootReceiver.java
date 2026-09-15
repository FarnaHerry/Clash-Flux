package dev.farna.clashflux;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/**
 * Restores the VPN tunnel after boot or an app update. ClashVpnService
 * records whether the tunnel was up in a shared preference; on shutdown the
 * process may die without callbacks, which is exactly the case we want to
 * restore, so a stale "true" is the desired outcome.
 */
public final class BootReceiver extends BroadcastReceiver {
    private static final String PREFS = "clashflux";
    private static final String KEY_VPN_ACTIVE = "vpn_active";

    static void setVpnActive(Context context, boolean active) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit()
                .putBoolean(KEY_VPN_ACTIVE, active)
                .apply();
        VpnTileService.syncState(context, active);
    }

    static boolean isVpnActive(Context context) {
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
        final boolean wasActive = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getBoolean(KEY_VPN_ACTIVE, false);
        if (!wasActive) {
            return;
        }
        // Initialize the native data-directory override before recording the
        // restore attempt; boot receivers run before MainActivity exists.
        MainActivity.bootstrapNative(context);
        MainActivity.appLog("系统启动广播触发 VPN 恢复", false);
        try {
            context.startForegroundService(new Intent(context, ClashVpnService.class));
        } catch (RuntimeException error) {
            MainActivity.appLog("系统启动后恢复 VPN 失败：" + error.getMessage(), true);
        }
    }
}
