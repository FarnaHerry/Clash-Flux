package dev.farna.clashflux;

import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Binder facade for commands that cross from the UI process into :background. */
public final class RuntimeControlService extends Service {
    private static final String TAG = "ClashFlux";
    private static final String ACTION_STOP = "dev.farna.clashflux.action.STOP_RUNTIME";
    private static final ExecutorService COMMANDS = Executors.newSingleThreadExecutor(runnable -> {
        Thread thread = new Thread(runnable, "clashflux-runtime-control");
        thread.setDaemon(true);
        return thread;
    });

    private final IClashRuntime.Stub binder = new IClashRuntime.Stub() {
        @Override public Bundle snapshot() {
            Bundle snapshot = ClashVpnService.runtimeSnapshot();
            snapshot.putBoolean("tunResetPending", getSharedPreferences(
                    "clashflux", MODE_PRIVATE).getBoolean(
                    "tun_preference_reset_pending", false));
            return snapshot;
        }

        @Override public Bundle logsAfter(long sequence) {
            return RuntimeLogStore.after(sequence);
        }

        @Override public boolean acknowledgeTunReset() {
            return getSharedPreferences("clashflux", MODE_PRIVATE).edit()
                    .putBoolean("tun_preference_reset_pending", false).commit();
        }

        @Override public boolean selectOutbound(String group, String name) {
            return ClashVpnService.selectOutbound(group, name);
        }

        @Override public boolean urlTest(String group) {
            return ClashVpnService.urlTest(group);
        }

        @Override public boolean setClashMode(String mode) {
            return ClashVpnService.setClashMode(mode);
        }

        @Override public boolean refreshProxyGroups() {
            ClashVpnService.proxyGroups();
            return true;
        }

        @Override public boolean closeConnection(String id) {
            return ClashVpnService.closeConnection(id);
        }

        @Override public boolean closeAllConnections() {
            return ClashVpnService.closeAllConnections();
        }

        @Override public boolean stopRuntime() {
            return stopRuntimeService();
        }
    };

    @Override public IBinder onBind(Intent intent) {
        return binder;
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            COMMANDS.execute(() -> {
                stopRuntimeService();
                stopSelf(startId);
            });
        }
        return START_NOT_STICKY;
    }

    static void requestStop(Context context) {
        Intent intent = new Intent(context, RuntimeControlService.class).setAction(ACTION_STOP);
        try {
            context.startService(intent);
        } catch (RuntimeException error) {
            Log.e(TAG, "Unable to queue sing-box runtime stop", error);
            ClashVpnService.stopServiceFallback(context);
        }
    }

    private boolean stopRuntimeService() {
        if (ClashVpnService.stopCurrent()) return true;
        ClashVpnService.stopServiceFallback(this);
        return true;
    }
}
