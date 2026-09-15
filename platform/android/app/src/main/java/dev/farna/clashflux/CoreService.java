package dev.farna.clashflux;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * CMFA-style resident notification: a foreground service started when the
 * app launches that pins a persistent keep-alive notification for the whole
 * process lifetime. Keeping a foreground service alive from launch
 * also raises the process priority so OEM background killers are less
 * likely to reap the proxy core.
 */
public final class CoreService extends Service {
    private static final String CHANNEL_ID = "clashflux_core";
    private static final int NOTIFICATION_ID = 2;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        MainActivity.appLog("常驻保活服务已创建", false);
        startForegroundWithNotification();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Sticky: the service (and its notification) survives process death
        // and is recreated alongside the process.
        return START_STICKY;
    }

    private void startForegroundWithNotification() {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null
                && Build.VERSION.SDK_INT >= 26
                && manager.getNotificationChannel(CHANNEL_ID) == null) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "内核状态", NotificationManager.IMPORTANCE_MIN);
            channel.setDescription("Clash-Flux 后台保活状态");
            manager.createNotificationChannel(channel);
        }

        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        Notification notification = builder
                .setContentTitle("Clash-Flux")
                .setContentText("后台保活已启用，VPN 状态见 VPN 通知")
                .setSmallIcon(android.R.drawable.stat_notify_sync_noanim)
                .setOngoing(true)
                .build();

        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
        MainActivity.appLog("常驻保活通知已建立", false);
    }
}
