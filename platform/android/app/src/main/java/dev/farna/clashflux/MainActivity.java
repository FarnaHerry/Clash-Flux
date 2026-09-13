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


public final class MainActivity extends HuxerUIActivity {
    private static final String TAG = "ClashFlux";
    private static final int REQUEST_VPN_CONSENT = 4001;
    static {
        Log.i(TAG, "Loading application native library: " + BuildConfig.HUXERUI_APP_LIBRARY);
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
        Log.i(TAG, "Application native library loaded");
    }

    private static MainActivity current;
    private static volatile Context applicationContext;

    private static native void nativeInit(String filesDirectory, String nativeLibraryDirectory);
    private static native void nativeSetSystemDark(boolean dark);
    private static native void nativeStartCore();
    private static native void nativeVpnStartCancelled();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.i(TAG, "MainActivity.onCreate entered");
        current = this;
        applicationContext = getApplicationContext();
        // The data plane is loaded in-process as libclash.so.  There is no
        // executable extraction/fork path: that path cannot receive
        // VpnService.protect(fd) callbacks and is the source of VPN blackholes.
        try {
            io.github.oviron.libmihomo.Clash.INSTANCE.load(
                    getApplicationInfo().nativeLibraryDir);
            io.github.oviron.libmihomo.Clash.INSTANCE.assertReady();
            Log.i(TAG, "Embedded mihomo bridge loaded");
        } catch (RuntimeException error) {
            Log.e(TAG, "Unable to load embedded mihomo", error);
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
        // CMFA-style resident notification from launch: the foreground
        // service pins a persistent notification for the process lifetime.
        try {
            startForegroundService(new Intent(this, CoreService.class));
        } catch (RuntimeException error) {
            Log.e(TAG, "Unable to start the core notification service", error);
        }
        Log.i(TAG, "Native mihomo startup requested");
    }

    private static boolean isSystemDarkMode() {
        MainActivity activity = current;
        if (activity == null) return false;
        final int nightMode = activity.getResources().getConfiguration().uiMode
                & Configuration.UI_MODE_NIGHT_MASK;
        return nightMode == Configuration.UI_MODE_NIGHT_YES;
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
        applicationContext = context.getApplicationContext();
        nativeInit(context.getFilesDir().getAbsolutePath(),
                context.getApplicationInfo().nativeLibraryDir);
    }

    /** Called by the C++ CoreProcess Android backend on its worker thread. */
    public static boolean startEmbeddedCore(String homeDirectory) {
        Context context = applicationContext;
        if (context == null) return false;
        return MihomoRuntime.start(context, homeDirectory);
    }

    /** Called by the C++ CoreProcess Android backend during orderly shutdown. */
    public static void stopEmbeddedCore() {
        MihomoRuntime.stop();
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
        // JNI 可能从 HuxerUI 的渲染线程进入；Activity 跳转必须在 Android
        // 主线程执行，否则部分设备会无提示地忽略这次请求。
        activity.runOnUiThread(() -> {
            PowerManager power =
                    (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
            if (power == null
                    || power.isIgnoringBatteryOptimizations(activity.getPackageName())) {
                return;
            }
            try {
                activity.startActivity(new Intent(
                        Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                        Uri.parse("package:" + activity.getPackageName())));
            } catch (ActivityNotFoundException error) {
                Log.w(TAG, "Battery optimization settings activity is unavailable", error);
            }
        });
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
        if (requestCode == REQUEST_VPN_CONSENT) {
            if (resultCode == RESULT_OK) {
                startVpnService();
            } else {
                // 用户取消系统授权时回滚 C++ 持久化状态，设置页的受控开关会
                // 在下一次状态校准中恢复为关闭。
                nativeVpnStartCancelled();
            }
        }
    }
}
