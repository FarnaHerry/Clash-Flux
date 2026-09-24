package dev.farna.clashflux;

import android.app.PendingIntent;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.graphics.drawable.Icon;
import android.net.VpnService;
import android.os.Build;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/** Quick Settings tile that mirrors and controls the Clash-Flux VPN tunnel. */
public final class VpnTileService extends TileService {
    private static volatile VpnTileService current;

    @Override
    public void onStartListening() {
        super.onStartListening();
        current = this;
        updateTile(BootReceiver.isVpnActive(this));
    }

    @Override
    public void onStopListening() {
        if (current == this) current = null;
        super.onStopListening();
    }

    @Override
    public void onClick() {
        super.onClick();
        if (BootReceiver.isVpnActive(this)) {
            MainActivity.appLog("通知栏快捷开关请求关闭 VPN", false);
            RuntimeControlService.requestStop(this);
            updateTile(false);
            return;
        }

        MainActivity.appLog("通知栏快捷开关请求启动 VPN", false);
        if (VpnService.prepare(this) == null) {
            startVpnService(this);
            return;
        }

        Intent consent = new Intent(this, VpnTileConsentActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (Build.VERSION.SDK_INT >= 34) {
            PendingIntent pending = PendingIntent.getActivity(
                    this, 0, consent,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            startActivityAndCollapse(pending);
        } else {
            startActivityAndCollapse(consent);
        }
    }

    static void startVpnService(Context context) {
        Intent service = new Intent(context, ClashVpnService.class);
        if (Build.VERSION.SDK_INT >= 26) {
            context.startForegroundService(service);
        } else {
            context.startService(service);
        }
    }

    static void syncState(Context context, boolean active) {
        VpnTileService service = current;
        if (service != null) service.updateTile(active);
        if (Build.VERSION.SDK_INT >= 24) {
            TileService.requestListeningState(context,
                    new ComponentName(context, VpnTileService.class));
        }
    }

    private void updateTile(boolean active) {
        Tile tile = getQsTile();
        if (tile == null) return;
        tile.setState(active ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        // SystemUI may fall back to the service's cached manifest icon when
        // the tile is rebound. Push the badge with every state update so the
        // launcher/app icon can never replace the QS mascot after a click.
        tile.setIcon(Icon.createWithResource(this, R.drawable.ic_qs_clash_flux));
        tile.setLabel(getString(R.string.app_name));
        if (Build.VERSION.SDK_INT >= 29) {
            tile.setSubtitle(active ? "隧道已开启" : "隧道已关闭");
        }
        tile.updateTile();
    }
}
