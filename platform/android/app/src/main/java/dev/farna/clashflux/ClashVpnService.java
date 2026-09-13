package dev.farna.clashflux;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.VpnService;
import android.os.Build;

/**
 * System VPN tunnel: establishes the TUN device and hands the raw fd to the
 * native side, which spawns mihomo with tun.file-descriptor inheriting it.
 *
 * No protect() callback is needed: the VPN-owning UID (including the mihomo
 * child process) is exempt from VPN routing, and the package is also added
 * via addDisallowedApplication as an explicit belt-and-braces measure.
 */
public final class ClashVpnService extends VpnService {
    private static final String TAG = "ClashFlux";
    private static final String CHANNEL_ID = "clashflux_vpn";
    private static final int NOTIFICATION_ID = 1;

    // Must match core.cpp kAndroidTunFd (fd number used inside the mihomo child).
    static final int MIHOMO_TUN_FD = 3;

    private boolean established = false;

    static {
        // Same native library MainActivity loads; declared here so the JNI
        // entry points resolve even if the service is the first component
        // touched after a process restart.
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }

    private static native void nativeTunEstablished(int fd);
    private static native void nativeTunRevoked();

    @Override
    public void onCreate() {
        super.onCreate();
        // Boot/update restore path: no Activity ran in this process yet, so
        // the native store bootstrap (data directories) happens here first.
        MainActivity.bootstrapNative(this);
        startForegroundWithNotification();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (!established) {
            established = true;
            BootReceiver.setVpnActive(this, true);
            establishVpn();
        }
        return START_STICKY;
    }

    @Override
    public void onRevoke() {
        // System revoked the VPN (user toggled it off, another VPN took over,
        // or the profile was removed). Tear the core's TUN down on our side.
        established = false;
        BootReceiver.setVpnActive(this, false);
        nativeTunRevoked();
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    @Override
    public void onDestroy() {
        if (established) {
            established = false;
            BootReceiver.setVpnActive(this, false);
            nativeTunRevoked();
        }
        super.onDestroy();
    }

    private void establishVpn() {
        int fd;
        try {
            // Builder parameters mirror ClashMetaForAndroid's TunService.
            VpnService.Builder builder = new VpnService.Builder()
                    .setSession("Clash-Flux")
                    .setMtu(9000)
                    .setBlocking(false)
                    .addAddress("198.18.0.1", 30)
                    .addDnsServer("198.18.0.2")
                    .addRoute("0.0.0.0", 0)
                    // The VPN app's own UID must bypass the tunnel so the
                    // spawned mihomo process reaches the physical network
                    // without loops.
                    .addDisallowedApplication(getPackageName());

            PendingIntent configure = PendingIntent.getActivity(
                    this, 0,
                    new Intent(this, MainActivity.class),
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            builder.setConfigureIntent(configure);

            if (Build.VERSION.SDK_INT >= 29) {
                builder.setMetered(false);
            }

            fd = builder.establish().detachFd();
        } catch (Exception error) {
            established = false;
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
            return;
        }
        if (fd < 0) {
            established = false;
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
            return;
        }
        nativeTunEstablished(fd);
    }

    private void startForegroundWithNotification() {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null
                && Build.VERSION.SDK_INT >= 26
                && manager.getNotificationChannel(CHANNEL_ID) == null) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "VPN 状态", NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Clash-Flux VPN 隧道运行状态");
            manager.createNotificationChannel(channel);
        }

        Notification notification = null;
        if (Build.VERSION.SDK_INT >= 26) {
            notification = new Notification.Builder(this, CHANNEL_ID)
                    .setContentTitle("Clash-Flux")
                    .setContentText("VPN 隧道运行中")
                    .setSmallIcon(android.R.drawable.stat_notify_sync_noanim)
                    .setOngoing(true)
                    .build();
        }

        if (Build.VERSION.SDK_INT >= 35) {
            startForeground(NOTIFICATION_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_SYSTEM_EXEMPTED);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
    }
}
