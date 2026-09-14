package dev.farna.clashflux;

import android.app.*;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.VpnService;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import io.nekohasekai.libbox.*;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONArray;
import org.json.JSONObject;

/** sing-box owns the data plane; Android owns TUN creation and protect(fd). */
public final class ClashVpnService extends VpnService implements PlatformInterface, CommandServerHandler, CommandClientHandler {
    private static final String TAG = "ClashFlux", CHANNEL_ID = "clashflux_vpn";
    private static final int NOTIFICATION_ID = 1;
    private CommandServer server;
    private CommandClient client;
    private ParcelFileDescriptor tunnel;
    private boolean started;
    private boolean foregroundReady;
    private boolean failureReported;
    private volatile boolean starting;
    private volatile boolean startRequested;
    private static volatile ClashVpnService current;
    private static volatile String outboundGroupsJson = "{\"proxies\":{}}";
    static { System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY); }
    private static native void nativeVpnState(int state, String message);
    private static native void nativeVpnStats(long uploadRate, long downloadRate,
                                              long uploadTotal, long downloadTotal,
                                              int connections);

    @Override public void onCreate() {
        super.onCreate();
        current = this;
        MainActivity.appLog("VPN 服务已创建，开始初始化 libbox", false);
        MainActivity.bootstrapNative(this);
        setup();
        try {
            foreground();
            foregroundReady = true;
            MainActivity.appLog("VPN 前台服务已就绪", false);
        } catch (RuntimeException error) {
            fail("无法启动 VPN 前台服务：" + error.getMessage());
        }
    }
    @Override public int onStartCommand(Intent i, int f, int id) {
        if (!foregroundReady) {
            MainActivity.appLog("VPN 服务未完成前台初始化，拒绝启动数据面", true);
            return START_NOT_STICKY;
        }
        if (!started && !starting) {
            starting = true;
            startRequested = true;
            MainActivity.appLog("VPN 数据面启动线程已开始", false);
            new Thread(this::startDataPlane, "clashflux-vpn-start").start();
        }
        return START_STICKY;
    }
    @Override public void onRevoke() {
        MainActivity.appLog("系统撤销了 VPN 授权", true);
        close("系统撤销了 VPN");
        stopSelf();
    }
    @Override public void onDestroy() {
        MainActivity.appLog("VPN 服务正在销毁", false);
        close("VPN 已关闭");
        if (current == this) current = null;
        super.onDestroy();
    }
    private void setup() {
        try { SetupOptions o = new SetupOptions(); o.setBasePath(getFilesDir().getPath()); o.setWorkingPath(getFilesDir().getPath()); o.setTempPath(getCacheDir().getPath()); o.setAppVersion(String.valueOf(BuildConfig.VERSION_CODE)); o.setAppMarketingVersion(BuildConfig.VERSION_NAME); Libbox.setup(o); }
        catch (Exception e) {
            Log.e(TAG, "libbox setup", e);
            MainActivity.appLog("libbox 初始化失败：" + e.getMessage(), true);
        }
    }
    private void startDataPlane() {
        MainActivity.appLog("开始创建 sing-box VPN 数据面", false);
        nativeVpnState(1, "正在启动 sing-box VPN 数据面");
        try {
            // The native store compiles the selected profile into sing-box
            // JSON (clashflux.singbox). Java only hands the file content to
            // libbox — no second converter on this side.
            File config = new File(getFilesDir(), "clash-flux/core/config.json");
            for (int attempt = 0; !config.isFile() && attempt < 50; ++attempt) {
                if (!startRequested) return;
                Thread.sleep(200);
            }
            if (!startRequested) return;
            if (!config.isFile()) {
                throw new IllegalStateException("未找到启用订阅的运行配置");
            }
            MainActivity.appLog("已找到运行配置，启动 libbox CommandServer", false);
            String configContent = new String(
                    Files.readAllBytes(config.toPath()), StandardCharsets.UTF_8);
            Libbox.checkConfig(configContent);
            MainActivity.appLog("运行配置校验通过", false);
            server = new CommandServer(this, this);
            server.start();
            if (!startRequested) {
                close("VPN 启动已取消");
                return;
            }
            server.startOrReloadService(
                configContent, new OverrideOptions());
            if (!startRequested) {
                close("VPN 启动已取消");
                return;
            }
            startStatusClient();
            started = true; BootReceiver.setVpnActive(this, true);
            nativeVpnState(2, "sing-box 已附着 TUN；socket protect 已启用");
            MainActivity.appLog("sing-box 已成功附着 Android TUN", false);
        } catch (Exception e) {
            if (startRequested) fail("sing-box 启动失败: " + e.getMessage());
        } finally {
            starting = false;
        }
    }
    @Override public int openTun(TunOptions o) throws Exception {
        try {
            Builder b = new Builder().setSession("Clash-Flux")
                    .setMtu(o.getMTU()).setBlocking(false);
            addAddresses(b, o.getInet4Address());
            addAddresses(b, o.getInet6Address());
            if (o.getAutoRoute()) {
                addRoutes(b, o.getInet4RouteRange());
                addRoutes(b, o.getInet6RouteRange());
                if (Build.VERSION.SDK_INT >= 29) b.setMetered(false);
            }
            b.setConfigureIntent(PendingIntent.getActivity(
                    this, 0, new Intent(this, MainActivity.class),
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE));
            tunnel = b.establish();
            if (tunnel == null) throw new IllegalStateException("系统拒绝创建 VPN 接口");
            MainActivity.appLog("Android VPN 接口已创建", false);
            return tunnel.detachFd();
        } catch (Exception error) {
            MainActivity.appLog("Android VPN 接口创建失败：" + error.getMessage(), true);
            throw error;
        }
    }
    private static void addAddresses(Builder b, RoutePrefixIterator i) {
        if (i == null) return;
        while (i.hasNext()) {
            RoutePrefix p = i.next();
            if (p != null) b.addAddress(p.address(), p.prefix());
        }
    }
    private static void addRoutes(Builder b, RoutePrefixIterator i) {
        if (i == null) return;
        while (i.hasNext()) {
            RoutePrefix p = i.next();
            if (p != null) b.addRoute(p.address(), p.prefix());
        }
    }
    private void startStatusClient() {
        try {
            CommandClientOptions options = new CommandClientOptions();
            options.setStatusInterval(1000);
            options.addCommand(Libbox.CommandStatus);
            client = new CommandClient(this, options);
            client.connect();
        } catch (Exception error) {
            // The VPN data plane is independent of the optional UI client.
            Log.w(TAG, "Unable to attach sing-box status stream", error);
            MainActivity.appLog("libbox 状态通道连接失败：" + error.getMessage(), true);
        }
    }
    private void close(String msg) { close(msg, true); }
    private void close(String msg, boolean reportStopped) {
        startRequested = false;
        MainActivity.appLog("VPN 数据面关闭：" + msg, false);
        if (!started && tunnel == null && server == null) {
            if (reportStopped && !failureReported) nativeVpnState(0, msg);
            return;
        }
        started = false;
        BootReceiver.setVpnActive(this, false);
        outboundGroupsJson = "{\"proxies\":{}}";
        try { if (client != null) client.disconnect(); } catch (Exception ignored) {}
        client = null;
        try { if (server != null) server.closeService(); } catch (Exception ignored) {}
        try { if (server != null) server.close(); } catch (Exception ignored) {}
        server = null;
        try { if (tunnel != null) tunnel.close(); } catch (Exception ignored) {}
        tunnel = null;
        nativeVpnStats(0, 0, 0, 0, 0);
        if (reportStopped && !failureReported) nativeVpnState(0, msg);
    }

    public static boolean selectOutbound(String group, String name) {
        ClashVpnService service = current;
        if (service == null || group == null || group.isEmpty() || name == null || name.isEmpty()) {
            return false;
        }
        try {
            CommandClient activeClient = service.client;
            if (activeClient == null) return false;
            activeClient.selectOutbound(group, name);
            return true;
        } catch (Exception error) {
            Log.w(TAG, "Unable to select outbound " + group + " -> " + name, error);
            MainActivity.appLog("线路切换失败：" + error.getMessage(), true);
            return false;
        }
    }

    public static String proxyGroups() {
        return outboundGroupsJson;
    }
    // Preserve Failed for the native/UI state machine.  Previously close() sent
    // a second "stopped" callback immediately, hiding the actual libbox error.
    private void fail(String msg) {
        failureReported = true;
        Log.e(TAG, msg);
        MainActivity.appLog(msg, true);
        close(msg, false);
        nativeVpnState(3, msg);
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }

    @Override public boolean usePlatformAutoDetectInterfaceControl(){return true;}
    // Present in newer libbox builds but deliberately not annotated: v1.14 does
    // not declare it, while an unannotated method remains harmless there.
    public boolean usePlatformAutoRedirect(){return false;}
    @Override public void autoDetectInterfaceControl(int fd){protect(fd);}
    @Override public boolean useProcFS(){return false;} @Override public boolean includeAllNetworks(){return false;} @Override public boolean underNetworkExtension(){return false;}
    @Override public boolean usePlatformBridge(){return false;} @Override public boolean usePlatformShell(){return false;}
    @Override public void clearDNSCache(){} @Override public LocalDNSTransport localDNSTransport(){return null;} @Override public ConnectionOwner findConnectionOwner(int a,String b,int c,String d,int e){return null;} @Override public NetworkInterfaceIterator getInterfaces(){return null;} @Override public WIFIState readWIFIState(){return null;}
    @Override public void startDefaultInterfaceMonitor(InterfaceUpdateListener l){} @Override public void closeDefaultInterfaceMonitor(InterfaceUpdateListener l){} @Override public void startNeighborMonitor(NeighborUpdateListener l){} @Override public void closeNeighborMonitor(NeighborUpdateListener l){} @Override public void registerMyInterface(String n){} @Override public void checkPlatformShell(){}
    @Override public BridgeSession createBridge(BridgeOptions o){return null;} @Override public PlatformUser lookupUser(String n){return null;} @Override public ShellSession openShellSession(PlatformUser u,String c,StringIterator e,String d,int p,int q){return null;} @Override public String lookupSFTPServer(){return "";} @Override public String readSystemSSHHostKey(){return "";} @Override public String tailscaleHostname(){return "";}
    @Override public void sendNotification(io.nekohasekai.libbox.Notification n){} @Override public void cancelNotification(String i,int t){} @Override public int connectSSHAgent(){return -1;} @Override public SystemProxyStatus getSystemProxyStatus(){return null;} @Override public void serviceReload(){} @Override public void serviceStop(){close("sing-box 已停止");} @Override public void setSystemProxyEnabled(boolean e){} @Override public void triggerNativeCrash(){} @Override public void writeDebugMessage(String m){Log.d(TAG,m); MainActivity.appLog("libbox 调试信息："+m,false);}
    @Override public void clearLogs(){} @Override public void connected(){Log.i(TAG,"sing-box status stream connected"); MainActivity.appLog("libbox 状态通道已连接",false);} @Override public void disconnected(String message){Log.w(TAG,"sing-box status stream disconnected: "+message); MainActivity.appLog("libbox 状态通道断开："+message,true);} @Override public void initializeClashMode(StringIterator modes,String current){} @Override public void setDefaultLogLevel(int level){} @Override public void updateClashMode(String mode){} @Override public void writeConnectionEvents(ConnectionEvents events){} @Override public void writeLogs(LogIterator logs){} @Override public void writeOutbounds(OutboundGroupItemIterator outbounds){} @Override public void writeStatus(StatusMessage status){nativeVpnStats(status.getUplink(),status.getDownlink(),status.getUplinkTotal(),status.getDownlinkTotal(),status.getConnectionsIn()+status.getConnectionsOut());}
    @Override public void writeGroups(OutboundGroupIterator groups) {
        try {
            JSONObject proxies = new JSONObject();
            while (groups != null && groups.hasNext()) {
                OutboundGroup group = groups.next();
                if (group == null || group.getTag() == null || group.getTag().isEmpty()) continue;
                JSONObject value = new JSONObject();
                value.put("type", group.getType());
                value.put("now", group.getSelected());
                JSONArray all = new JSONArray();
                OutboundGroupItemIterator items = group.getItems();
                while (items != null && items.hasNext()) {
                    OutboundGroupItem item = items.next();
                    if (item != null && item.getTag() != null) all.put(item.getTag());
                }
                value.put("all", all);
                proxies.put(group.getTag(), value);
            }
            outboundGroupsJson = new JSONObject().put("proxies", proxies).toString();
        } catch (Exception error) {
            Log.w(TAG, "Unable to snapshot outbound groups", error);
            MainActivity.appLog("读取出站线路失败：" + error.getMessage(), true);
        }
    }
    private void foreground() {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null && Build.VERSION.SDK_INT >= 26
                && manager.getNotificationChannel(CHANNEL_ID) == null) {
            manager.createNotificationChannel(new NotificationChannel(
                    CHANNEL_ID, "VPN 状态", NotificationManager.IMPORTANCE_LOW));
        }
        android.app.Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new android.app.Notification.Builder(this, CHANNEL_ID)
                : new android.app.Notification.Builder(this);
        android.app.Notification notification = builder
                .setContentTitle("Clash-Flux")
                .setContentText("正在启动 sing-box VPN 隧道")
                .setSmallIcon(android.R.drawable.stat_notify_sync_noanim)
                .setCategory(android.app.Notification.CATEGORY_SERVICE)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
    }
}
