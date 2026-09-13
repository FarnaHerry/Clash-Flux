package dev.farna.clashflux;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.InetAddresses;
import android.net.VpnService;
import android.os.Build;
import android.util.Log;

import io.github.oviron.libmihomo.Clash;
import io.github.oviron.libmihomo.TunInterface;

import java.net.InetSocketAddress;

/**
 * Android-owned TUN endpoint for the embedded mihomo C-shared core.  The
 * core calls protect(fd) for every physical outbound socket, which is the
 * essential difference from the old child-process/fd-inheritance design.
 */
public final class ClashVpnService extends VpnService {
    private static final String TAG = "ClashFlux";
    private static final String CHANNEL_ID = "clashflux_vpn";
    private static final int NOTIFICATION_ID = 1;

    private boolean established = false;
    private final TunInterface tunCallbacks = new TunInterface() {
        @Override public void protect(int fd) {
            // VpnService.protect returns whether Android exempted this socket;
            // mihomo's callback contract is notification-only.
            ClashVpnService.this.protect(fd);
        }

        @Override public String resolverProcess(int protocol, String source,
                                                String target, int uid) {
            return ClashVpnService.this.resolverProcess(protocol, source, target, uid);
        }
    };

    static {
        // Same native library MainActivity loads; declared here so the JNI
        // entry points resolve even if the service is the first component
        // touched after a process restart.
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }

    private static native void nativeVpnState(int state, String message);

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
        closeTunnel("系统撤销了 VPN");
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    @Override
    public void onDestroy() {
        closeTunnel("VPN 已关闭");
        super.onDestroy();
    }

    private void establishVpn() {
        nativeVpnState(1, "正在建立系统 VPN");
        // Boot restore has no Activity/core worker. Reapply the persisted
        // generated config before the TUN is handed to mihomo.
        if (!MihomoRuntime.start(this,
                new java.io.File(getFilesDir(), "clash-flux/core").getAbsolutePath())) {
            failTunnel("内嵌 mihomo 未能加载运行配置");
            return;
        }
        int fd;
        try {
            // Builder parameters mirror ClashMetaForAndroid's TunService.
            VpnService.Builder builder = new VpnService.Builder()
                    .setSession("Clash-Flux")
                    // 与 core.cpp 的 TUN 配置一致；1400 避免移动网络 MTU
                    // 黑洞。198.18.0.0/16 留给 mihomo fake-IP，不能拿作接口地址。
                    .setMtu(1400)
                    .setBlocking(false)
                    .addAddress("172.19.0.1", 30)
                    .addDnsServer("172.19.0.2")
                    .addRoute("0.0.0.0", 0);

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
            failTunnel("建立 VPN 失败: " + error.getMessage());
            return;
        }
        if (fd < 0) {
            failTunnel("系统拒绝创建 VPN 接口");
            return;
        }
        try {
            Clash.INSTANCE.startTUN(fd, tunCallbacks, "clash-flux", "system",
                    "172.19.0.1/30", "172.19.0.2", 1400);
            nativeVpnState(2, "TUN 已附着；socket protect 已启用");
        } catch (RuntimeException error) {
            failTunnel("内嵌 mihomo 未能附着 TUN: " + error.getMessage());
        }
    }

    private String resolverProcess(int protocol, String source, String target, int uid) {
        int owner = uid;
        if (owner < 0 && Build.VERSION.SDK_INT >= 29) {
            try {
                ConnectivityManager connectivity =
                        (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
                if (connectivity != null) {
                    owner = connectivity.getConnectionOwnerUid(protocol,
                            parseEndpoint(source), parseEndpoint(target));
                }
            } catch (RuntimeException ignored) {
                owner = -1;
            }
        }
        if (owner < 0) return "";
        String[] packages = getPackageManager().getPackagesForUid(owner);
        String name = packages != null && packages.length > 0 ? packages[0] : "";
        return owner + "\n" + name;
    }

    private static InetSocketAddress parseEndpoint(String value) {
        int separator = value.lastIndexOf(':');
        if (separator <= 0) throw new IllegalArgumentException("Bad endpoint: " + value);
        String host = value.substring(0, separator).replace("[", "").replace("]", "");
        return new InetSocketAddress(InetAddresses.parseNumericAddress(host),
                Integer.parseInt(value.substring(separator + 1)));
    }

    private void closeTunnel(String message) {
        if (!established) return;
        established = false;
        BootReceiver.setVpnActive(this, false);
        try {
            Clash.INSTANCE.stopTun();
        } catch (RuntimeException error) {
            Log.w(TAG, "Unable to stop embedded TUN", error);
        }
        nativeVpnState(0, message);
    }

    private void failTunnel(String message) {
        Log.e(TAG, message);
        established = false;
        BootReceiver.setVpnActive(this, false);
        try { Clash.INSTANCE.stopTun(); } catch (RuntimeException ignored) { }
        nativeVpnState(3, message);
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
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
