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

    private static volatile MainActivity current;
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
        // The core resident service is a real foreground service, not only a
        // label in the settings page. Android 13+ asks for notification access
        // before the service is started so the keep-alive state is observable.
        requestNotificationAction(PENDING_CORE_SERVICE);
        Log.i(TAG, "Android shell initialized; sing-box waits for VPN consent");
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

    /** Initializes the native store when no Activity UI ran (boot restore). */
    public static void bootstrapNative(Context context) {
        applicationContext = context.getApplicationContext();
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

    public static boolean selectOutbound(String group, String name) {
        return ClashVpnService.selectOutbound(group, name);
    }

    public static String proxyGroups() {
        return ClashVpnService.proxyGroups();
    }

    public static boolean isIgnoringBatteryOptimizations() {
        MainActivity activity = current;
        Context context = activity != null ? activity : applicationContext;
        if (context == null) return false;
        PowerManager power = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
        return power != null
                && power.isIgnoringBatteryOptimizations(context.getPackageName());
    }

    public static void requestIgnoreBatteryOptimizations() {
        MainActivity activity = current;
        if (activity == null) return;
        // JNI 可能从 HuxerUI 的渲染线程进入；Activity 跳转必须在 Android
        // 主线程执行，否则部分设备会无提示地忽略这次请求。
        activity.runOnUiThread(() -> {
            activity.openBatteryOptimizationSettings();
        });
    }

    // ---- 后台保活 ----------------------------------------------------------

    public static void requestBackgroundKeepAlive() {
        MainActivity activity = current;
        if (activity == null) return;
        activity.runOnUiThread(() ->
                activity.requestNotificationAction(PENDING_CORE_SERVICE | PENDING_BATTERY));
    }

    private static final int PENDING_CORE_SERVICE = 1;
    private static final int PENDING_VPN = 2;
    private static final int PENDING_BATTERY = 4;
    private static int pendingNotificationActions = 0;
    private static boolean notificationRequestInFlight = false;

    private void requestNotificationAction(int actions) {
        pendingNotificationActions |= actions;
        // The sticky FGS notification is part of the keep-alive story: ask
        // for the (denied-by-default) notification permission first, then
        // continue with the requested operation once the answer is in.
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(
                Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
            if (!notificationRequestInFlight) {
                notificationRequestInFlight = true;
                requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS},
                        REQUEST_NOTIFICATIONS);
            }
            return;
        }
        drainNotificationActions();
    }

    private void drainNotificationActions() {
        final int actions = pendingNotificationActions;
        pendingNotificationActions = 0;
        if ((actions & (PENDING_CORE_SERVICE | PENDING_BATTERY)) != 0) {
            startCoreService();
        }
        if ((actions & PENDING_BATTERY) != 0) {
            openBatteryOptimizationSettings();
        }
        if ((actions & PENDING_VPN) != 0) {
            continueVpnStart();
        }
    }

    private void startCoreService() {
        try {
            Intent intent = new Intent(this, CoreService.class);
            if (Build.VERSION.SDK_INT >= 26) {
                startForegroundService(intent);
            } else {
                startService(intent);
            }
        } catch (RuntimeException error) {
            Log.e(TAG, "Unable to start the core keep-alive service", error);
        }
    }

    private void openBatteryOptimizationSettings() {
        PowerManager power =
                (PowerManager) getSystemService(Context.POWER_SERVICE);
        if (power == null || power.isIgnoringBatteryOptimizations(getPackageName())) {
            return;
        }
        try {
            startActivity(new Intent(
                    Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                    Uri.parse("package:" + getPackageName())));
        } catch (ActivityNotFoundException error) {
            // Some OEM ROMs do not expose the package-specific action; still
            // open the generic list so the user has a real system action.
            try {
                startActivity(new Intent(
                        Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS));
            } catch (ActivityNotFoundException fallbackError) {
                Log.w(TAG, "Battery optimization settings unavailable",
                        fallbackError);
            }
        }
    }

    // ---- VPN（VpnService.prepare 授权 + 前台服务）--------------------------

    private void beginVpnStart() {
        requestNotificationAction(PENDING_CORE_SERVICE | PENDING_VPN);
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
            Intent intent = new Intent(this, ClashVpnService.class);
            if (Build.VERSION.SDK_INT >= 26) {
                startForegroundService(intent);
            } else {
                startService(intent);
            }
        } catch (RuntimeException error) {
            Log.e(TAG, "Unable to start the VPN service", error);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_NOTIFICATIONS) {
            notificationRequestInFlight = false;
            drainNotificationActions();
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
