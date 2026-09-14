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
        installCrashLogger();
    }

    private static volatile MainActivity current;
    private static volatile Context applicationContext;

    private static native void nativeInit(String filesDirectory, String nativeLibraryDirectory);
    private static native void nativeSetSystemDark(boolean dark);
    private static native void nativeStartCore();
    private static native void nativeVpnStartCancelled();
    private static native void nativeVpnStartFailed(String message);
    private static native void nativeAppLog(int level, String message);

    private static void installCrashLogger() {
        final Thread.UncaughtExceptionHandler delegate =
                Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            try {
                appLog("线程 " + thread.getName() + " 发生未捕获异常：\n"
                        + Log.getStackTraceString(error), true);
            } catch (Throwable ignored) {
                // The process is already terminating; never mask the original
                // crash with a diagnostic failure.
            }
            if (delegate != null) delegate.uncaughtException(thread, error);
        });
    }

    static void appLog(String message, boolean error) {
        if (message == null || message.isEmpty()) return;
        Log.println(error ? Log.ERROR : Log.INFO, TAG, message);
        try {
            nativeAppLog(error ? 3 : 1, message);
        } catch (RuntimeException | LinkageError ignored) {
            // Diagnostics must not affect permissions or service startup.
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.i(TAG, "MainActivity.onCreate entered");
        current = this;
        applicationContext = getApplicationContext();
        // Initialize the native bridge before HuxerUI composes the first frame.
        // Android settings and VPN callbacks can be queried during composition;
        // waiting until after super.onCreate leaves that first frame without a
        // valid Activity class and data-directory bridge.
        nativeInit(getFilesDir().getAbsolutePath(), getApplicationInfo().nativeLibraryDir);
        Log.i(TAG, "Native bridge initialized");
        appLog("主 Activity 已初始化 native bridge", false);
        // Make the system-theme value available to the first native frame too.
        nativeSetSystemDark(isSystemDarkMode());
        Log.i(TAG, "Calling HuxerUIActivity.onCreate");
        super.onCreate(savedInstanceState);
        Log.i(TAG, "HuxerUIActivity.onCreate returned");
        // System dark/light toggles recreate the Activity (uiMode is not in
        // configChanges), so this per-onCreate report is always fresh for the
        // "follow system" theme option.
        nativeStartCore();
        // The core resident service is a real foreground service, not only a
        // label in the settings page. Android 13+ asks for notification access
        // before the service is started so the keep-alive state is observable.
        requestNotificationAction(PENDING_CORE_SERVICE);
        Log.i(TAG, "Android shell initialized; sing-box waits for VPN consent");
        appLog("Android 外壳已初始化，等待 VPN 系统授权", false);
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

    @Override
    protected void onResume() {
        super.onResume();
        appLog("Activity 已回到前台，刷新系统权限状态", false);
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
            activity.requestBatteryOptimization();
        });
    }

    /** Opens the system management page so the user can revoke the exemption. */
    public static void openBatteryOptimizationSettings() {
        MainActivity activity = current;
        if (activity == null) return;
        activity.runOnUiThread(activity::openBatteryOptimizationManagement);
    }

    // ---- 后台保活 ----------------------------------------------------------

    public static void requestBackgroundKeepAlive() {
        MainActivity activity = current;
        if (activity == null) return;
        appLog("请求后台保活：通知权限、常驻服务和电池优化豁免", false);
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
                appLog("正在请求 Android 通知权限", false);
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
            requestBatteryOptimization();
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
            appLog("后台保活前台服务已请求启动", false);
        } catch (RuntimeException error) {
            Log.e(TAG, "Unable to start the core keep-alive service", error);
            appLog("后台保活服务启动失败：" + error.getMessage(), true);
        }
    }

    private void requestBatteryOptimization() {
        PowerManager power =
                (PowerManager) getSystemService(Context.POWER_SERVICE);
        if (power == null || power.isIgnoringBatteryOptimizations(getPackageName())) {
            appLog("电池优化已处于豁免状态", false);
            return;
        }
        try {
            startActivity(new Intent(
                    Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                    Uri.parse("package:" + getPackageName())));
            appLog("已呼出系统电池优化豁免确认", false);
            return;
        } catch (ActivityNotFoundException | SecurityException error) {
            // Some OEM ROMs do not expose the package-specific action; still
            // open the generic list so the user has a real system action.
            appLog("厂商系统不支持专用电池优化请求，改开管理页面", false);
            openBatteryOptimizationManagement();
        }
    }

    private void openBatteryOptimizationManagement() {
        try {
            startActivity(new Intent(
                    Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS));
            appLog("已打开系统电池优化管理页面", false);
            return;
        } catch (ActivityNotFoundException | SecurityException error) {
            // A few ROMs do not expose the generic list. Opening the app
            // details page is still actionable and avoids a silent no-op.
            try {
                startActivity(new Intent(
                        Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName())));
                appLog("系统电池优化列表不可用，已打开应用详情页", false);
                return;
            } catch (ActivityNotFoundException | SecurityException fallbackError) {
                Log.w(TAG, "Battery optimization settings unavailable", fallbackError);
                appLog("无法打开系统电池优化管理页面：" + fallbackError.getMessage(), true);
            }
            Log.w(TAG, "Battery optimization management unavailable", error);
        }
    }

    // ---- VPN（VpnService.prepare 授权 + 前台服务）--------------------------

    private void beginVpnStart() {
        requestNotificationAction(PENDING_CORE_SERVICE | PENDING_VPN);
    }

    private void continueVpnStart() {
        try {
            appLog("开始请求系统 VPN 授权", false);
            Intent consent = VpnService.prepare(this);
            if (consent != null) {
                appLog("系统 VPN 尚未授权，正在显示授权页面", false);
                startActivityForResult(consent, REQUEST_VPN_CONSENT);
            } else {
                appLog("系统 VPN 已授权，正在启动 VPN 前台服务", false);
                startVpnService();
            }
        } catch (RuntimeException error) {
            reportVpnStartFailure("无法请求系统 VPN 授权：" + error.getMessage(), error);
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
            appLog("VPN 前台服务已请求启动", false);
        } catch (RuntimeException error) {
            reportVpnStartFailure("无法启动 VPN 服务：" + error.getMessage(), error);
        }
    }

    private void reportVpnStartFailure(String message, RuntimeException error) {
        Log.e(TAG, message, error);
        appLog(message, true);
        try {
            nativeVpnStartFailed(message);
        } catch (RuntimeException bridgeError) {
            Log.e(TAG, "Unable to report VPN startup failure", bridgeError);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_NOTIFICATIONS) {
            notificationRequestInFlight = false;
            appLog("通知权限结果：" +
                    (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED
                            ? "已允许" : "未允许"),
                    false);
            drainNotificationActions();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_VPN_CONSENT) {
            if (resultCode == RESULT_OK) {
                appLog("用户已允许系统 VPN，继续启动服务", false);
                startVpnService();
            } else {
                // 用户取消系统授权时回滚 C++ 持久化状态，设置页的受控开关会
                // 在下一次状态校准中恢复为关闭。
                nativeVpnStartCancelled();
                appLog("用户取消系统 VPN 授权", true);
            }
        }
    }
}
