package dev.farna.clashflux;

import android.Manifest;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.net.Uri;
import android.net.VpnService;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.Log;

import org.huxerui.HuxerUIActivity;

import java.io.File;
import java.io.IOException;

public final class MainActivity extends HuxerUIActivity {
    private static final String TAG = "ClashFlux";
    private static final int REQUEST_VPN_CONSENT = 4001;
    static {
        Log.i(TAG, "Loading application native library: " + BuildConfig.HUXERUI_APP_LIBRARY);
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
        Log.i(TAG, "Application native library loaded");
    }

    private static MainActivity current;

    private static native void nativeInit(String filesDirectory, String nativeLibraryDirectory);
    private static native void nativeSetSystemDark(boolean dark);
    private static native void nativeStartCore();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.i(TAG, "MainActivity.onCreate entered");
        current = this;
        try {
            ensureMihomoBinary();
        } catch (IOException | RuntimeException error) {
            // Keep the UI available so the failure is visible in the core status
            // card and in logcat instead of turning a missing packaged asset into
            // an unrelated Activity crash.
            Log.e(TAG, "Unable to extract the bundled mihomo Android engine", error);
        }
        Log.i(TAG, "Calling HuxerUIActivity.onCreate");
        super.onCreate(savedInstanceState);
        Log.i(TAG, "HuxerUIActivity.onCreate returned");
        // Pass the private files directory before starting the native worker so
        // the bundled engine does not depend on HuxerUI's first composable frame
        // to discover its data directory.
        nativeInit(getFilesDir().getAbsolutePath(), getApplicationInfo().nativeLibraryDir);
        Log.i(TAG, "Native bridge initialized");
        // System dark/light toggles recreate the Activity (uiMode is not in
        // configChanges), so this per-onCreate report is always fresh for the
        // "follow system" theme option.
        nativeSetSystemDark(isSystemDarkMode());
        nativeStartCore();
        Log.i(TAG, "Native mihomo startup requested");
    }

    private static boolean isSystemDarkMode() {
        MainActivity activity = current;
        if (activity == null) return false;
        final int nightMode = activity.getResources().getConfiguration().uiMode
                & Configuration.UI_MODE_NIGHT_MASK;
        return nightMode == Configuration.UI_MODE_NIGHT_YES;
    }

    private void ensureMihomoBinary() throws IOException {
        String abi = Build.SUPPORTED_ABIS.length == 0 ? "unknown" : Build.SUPPORTED_ABIS[0];
        File binary = new File(getApplicationInfo().nativeLibraryDir, "libmihomo.so");
        if (!binary.isFile() || !binary.canExecute()) {
            throw new IOException("Bundled mihomo JNI library is unavailable for ABI " + abi +
                    ": " + binary);
        }
        Log.i(TAG, "Bundled mihomo ready (nativeLibraryDir): " + binary.getAbsolutePath());
    }

    @Override
    protected void onDestroy() {
        if (current == this) {
            current = null;
        }
        super.onDestroy();
    }

    public static void openUrl(String url) {
        MainActivity activity = current;
        if (activity == null || url == null || url.isEmpty()) {
            return;
        }
        activity.runOnUiThread(() -> {
            try {
                activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
            } catch (ActivityNotFoundException ignored) {
                // No browser is installed; the native caller has no recovery action.
            }
        });
    }

    // ---- VPN（C++ 设置页 VPN 开关经 JNI 调用）----

    private static final int REQUEST_NOTIFICATIONS = 4002;
    private static boolean pendingVpnStart = false;

    /** Initializes the native store when no Activity UI ran (boot restore). */
    public static void bootstrapNative(Context context) {
        nativeInit(context.getFilesDir().getAbsolutePath(),
                context.getApplicationInfo().nativeLibraryDir);
    }

    public static void startVpn() {
        MainActivity activity = current;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(activity::beginVpnStart);
    }

    public static void stopVpn() {
        MainActivity activity = current;
        if (activity == null) {
            return;
        }
        activity.runOnUiThread(() ->
                activity.stopService(new Intent(activity, ClashVpnService.class)));
    }

    public static boolean isIgnoringBatteryOptimizations() {
        MainActivity activity = current;
        if (activity == null) return true;
        PowerManager power = (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
        return power == null
                || power.isIgnoringBatteryOptimizations(activity.getPackageName());
    }

    public static void requestIgnoreBatteryOptimizations() {
        MainActivity activity = current;
        if (activity == null) return;
        PowerManager power = (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
        if (power == null || power.isIgnoringBatteryOptimizations(activity.getPackageName())) {
            return;
        }
        activity.startActivity(new Intent(
                Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                Uri.parse("package:" + activity.getPackageName())));
    }

    private void beginVpnStart() {
        // The sticky FGS notification is part of the keep-alive story: ask
        // for the (denied-by-default) notification permission first, then
        // continue with the VPN consent dialog once the answer is in.
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(
                Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
            pendingVpnStart = true;
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS},
                    REQUEST_NOTIFICATIONS);
            return;
        }
        continueVpnStart();
    }

    private void continueVpnStart() {
        Intent consent = VpnService.prepare(this);
        if (consent != null) {
            startActivityForResult(consent, REQUEST_VPN_CONSENT);
        } else {
            startVpnService();
        }
    }

    private void startVpnService() {
        try {
            startForegroundService(new Intent(this, ClashVpnService.class));
        } catch (RuntimeException error) {
            Log.e(TAG, "Unable to start the VPN service", error);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_NOTIFICATIONS && pendingVpnStart) {
            pendingVpnStart = false;
            continueVpnStart();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_VPN_CONSENT && resultCode == RESULT_OK) {
            startVpnService();
        }
    }
}
