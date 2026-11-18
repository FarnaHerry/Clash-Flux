package dev.farna.clashflux;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.util.Log;

/** Isolates the launcher shortcuts' task-clearing flags from MainActivity. */
public final class LauncherShortcutActivity extends Activity {
    private static final String SCAN = "dev.farna.clashflux.action.SHORTCUT_SCAN";
    private static final String TOGGLE_VPN = "dev.farna.clashflux.action.SHORTCUT_TOGGLE_VPN";
    private static final long RUNTIME_QUERY_TIMEOUT_MS = 1500;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable queryTimeout = this::fallbackToPersistedVpnState;
    private final ServiceConnection runtimeConnection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            if (toggleResolved) return;
            try {
                Bundle snapshot = IClashRuntime.Stub.asInterface(service).snapshot();
                resolveToggle(RuntimeTogglePolicy.shouldStopVpn(
                        snapshot.getInt("vpnState", 0),
                        snapshot.getBoolean("runtimeOwner", false)));
            } catch (RemoteException | RuntimeException error) {
                Log.w("ClashFlux", "Unable to read live VPN state for shortcut", error);
                fallbackToPersistedVpnState();
            }
        }

        @Override public void onServiceDisconnected(ComponentName name) {
            fallbackToPersistedVpnState();
        }
    };

    private boolean runtimeBound;
    private boolean toggleResolved;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        String action = getIntent().getAction();
        if (SCAN.equals(action)) {
            Intent scan = new Intent(Intent.ACTION_VIEW, Uri.parse("clash-flux://app/scan-qr"),
                    this, MainActivity.class);
            startActivity(scan.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            toggleResolved = true;
            finish();
        } else if (TOGGLE_VPN.equals(action)) {
            queryRuntimeAndToggle();
        } else {
            toggleResolved = true;
            finish();
        }
    }

    private void queryRuntimeAndToggle() {
        Intent service = new Intent(this, RuntimeControlService.class);
        try {
            runtimeBound = bindService(service, runtimeConnection, Context.BIND_AUTO_CREATE);
        } catch (RuntimeException error) {
            Log.w("ClashFlux", "Unable to bind runtime for shortcut", error);
            runtimeBound = false;
        }
        if (!runtimeBound) {
            fallbackToPersistedVpnState();
            return;
        }
        handler.postDelayed(queryTimeout, RUNTIME_QUERY_TIMEOUT_MS);
    }

    private void fallbackToPersistedVpnState() {
        if (toggleResolved) return;
        MainActivity.appLog("无法读取实时 VPN 状态，快捷开关使用最近恢复状态", true);
        resolveToggle(BootReceiver.isVpnActive(this));
    }

    private void resolveToggle(boolean stop) {
        if (toggleResolved) return;
        toggleResolved = true;
        handler.removeCallbacks(queryTimeout);
        unbindRuntime();
        if (stop) {
            RuntimeControlService.requestStop(this);
        } else {
            startActivity(new Intent(this, VpnTileConsentActivity.class)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        }
        finish();
    }

    private void unbindRuntime() {
        if (!runtimeBound) return;
        runtimeBound = false;
        try {
            unbindService(runtimeConnection);
        } catch (IllegalArgumentException ignored) {
            // The system may already have dropped the binding with the process.
        }
    }

    @Override
    protected void onDestroy() {
        toggleResolved = true;
        handler.removeCallbacks(queryTimeout);
        unbindRuntime();
        super.onDestroy();
    }
}
