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

/** sing-box owns the data plane; Android owns TUN creation and protect(fd). */
public final class ClashVpnService extends VpnService implements PlatformInterface, CommandServerHandler, CommandClientHandler {
    private static final String TAG = "ClashFlux", CHANNEL_ID = "clashflux_vpn";
    private static final int NOTIFICATION_ID = 1;
    private CommandServer server;
    private CommandClient client;
    private ParcelFileDescriptor tunnel;
    private boolean started;
    static { System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY); }
    private static native void nativeVpnState(int state, String message);
    private static native void nativeVpnStats(long uploadRate, long downloadRate,
                                              long uploadTotal, long downloadTotal,
                                              int connections);

    @Override public void onCreate() { super.onCreate(); MainActivity.bootstrapNative(this); setup(); foreground(); }
    @Override public int onStartCommand(Intent i, int f, int id) { if (!started) start(); return START_STICKY; }
    @Override public void onRevoke() { close("系统撤销了 VPN"); stopSelf(); }
    @Override public void onDestroy() { close("VPN 已关闭"); super.onDestroy(); }
    private void setup() {
        try { SetupOptions o = new SetupOptions(); o.setBasePath(getFilesDir().getPath()); o.setWorkingPath(getFilesDir().getPath()); o.setTempPath(getCacheDir().getPath()); o.setAppVersion(String.valueOf(BuildConfig.VERSION_CODE)); o.setAppMarketingVersion(BuildConfig.VERSION_NAME); Libbox.setup(o); }
        catch (Exception e) { Log.e(TAG, "libbox setup", e); }
    }
    private void start() {
        nativeVpnState(1, "正在启动 sing-box VPN 数据面");
        try {
            File yaml = new File(getFilesDir(), "clash-flux/core/config.yaml");
            if (!yaml.isFile()) throw new IllegalStateException("未找到启用订阅的运行配置");
            server = new CommandServer(this, this); server.start();
            server.startOrReloadService(SingBoxConfig.compile(yaml, "rule"), new OverrideOptions());
            startStatusClient();
            started = true; BootReceiver.setVpnActive(this, true);
            nativeVpnState(2, "sing-box 已附着 TUN；socket protect 已启用");
        } catch (Exception e) { fail("sing-box 启动失败: " + e.getMessage()); }
    }
    @Override public int openTun(TunOptions o) throws Exception {
        Builder b = new Builder().setSession("Clash-Flux").setMtu(o.getMTU()).setBlocking(false);
        addAddresses(b, o.getInet4Address()); addAddresses(b, o.getInet6Address());
        if (o.getAutoRoute()) { addRoutes(b, o.getInet4RouteRange()); addRoutes(b, o.getInet6RouteRange()); if (Build.VERSION.SDK_INT >= 29) b.setMetered(false); }
        b.setConfigureIntent(PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE));
        tunnel = b.establish(); if (tunnel == null) throw new IllegalStateException("系统拒绝创建 VPN 接口"); return tunnel.detachFd();
    }
    private static void addAddresses(Builder b, RoutePrefixIterator i) { while (i.hasNext()) { RoutePrefix p=i.next(); b.addAddress(p.address(),p.prefix()); } }
    private static void addRoutes(Builder b, RoutePrefixIterator i) { while (i.hasNext()) { RoutePrefix p=i.next(); b.addRoute(p.address(),p.prefix()); } }
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
        }
    }
    private void close(String msg) { if (!started && tunnel == null) return; started=false; BootReceiver.setVpnActive(this,false); try { if(client!=null)client.disconnect(); } catch(Exception ignored){} client=null; try { if(server!=null) server.closeService(); } catch(Exception ignored){} try { if(server!=null) server.close(); } catch(Exception ignored){} server=null; try { if(tunnel!=null)tunnel.close(); }catch(Exception ignored){} tunnel=null; nativeVpnStats(0,0,0,0,0); nativeVpnState(0,msg); }
    private void fail(String msg) { Log.e(TAG,msg); nativeVpnState(3,msg); close(msg); stopForeground(STOP_FOREGROUND_REMOVE); stopSelf(); }

    @Override public boolean usePlatformAutoDetectInterfaceControl(){return true;}
    @Override public void autoDetectInterfaceControl(int fd){protect(fd);}
    @Override public boolean useProcFS(){return false;} @Override public boolean includeAllNetworks(){return false;} @Override public boolean underNetworkExtension(){return false;}
    @Override public boolean usePlatformAutoRedirect(){return false;} @Override public boolean usePlatformBridge(){return false;} @Override public boolean usePlatformShell(){return false;}
    @Override public void clearDNSCache(){} @Override public LocalDNSTransport localDNSTransport(){return null;} @Override public ConnectionOwner findConnectionOwner(int a,String b,int c,String d,int e){return null;} @Override public NetworkInterfaceIterator getInterfaces(){return null;} @Override public WIFIState readWIFIState(){return null;}
    @Override public void startDefaultInterfaceMonitor(InterfaceUpdateListener l){} @Override public void closeDefaultInterfaceMonitor(InterfaceUpdateListener l){} @Override public void startNeighborMonitor(NeighborUpdateListener l){} @Override public void closeNeighborMonitor(NeighborUpdateListener l){} @Override public void registerMyInterface(String n){} @Override public void checkPlatformShell(){}
    @Override public AutoRedirectSession createAutoRedirect(byte[] o,AutoRedirectHandler h){return null;} @Override public BridgeSession createBridge(BridgeOptions o){return null;} @Override public PlatformUser lookupUser(String n){return null;} @Override public ShellSession openShellSession(PlatformUser u,String c,StringIterator e,String d,int p,int q){return null;} @Override public String lookupSFTPServer(){return "";} @Override public String readSystemSSHHostKey(){return "";} @Override public String tailscaleHostname(){return "";}
    @Override public void sendNotification(io.nekohasekai.libbox.Notification n){} @Override public void cancelNotification(String i,int t){} @Override public int connectSSHAgent(){return -1;} @Override public SystemProxyStatus getSystemProxyStatus(){return null;} @Override public void serviceReload(){} @Override public void serviceStop(){close("sing-box 已停止");} @Override public void setSystemProxyEnabled(boolean e){} @Override public void triggerNativeCrash(){} @Override public void writeDebugMessage(String m){Log.d(TAG,m);}
    @Override public void clearLogs(){} @Override public void connected(){Log.i(TAG,"sing-box status stream connected");} @Override public void disconnected(String message){Log.w(TAG,"sing-box status stream disconnected: "+message);} @Override public void initializeClashMode(StringIterator modes,String current){} @Override public void setDefaultLogLevel(int level){} @Override public void updateClashMode(String mode){} @Override public void writeConnectionEvents(ConnectionEvents events){} @Override public void writeGroups(OutboundGroupIterator groups){} @Override public void writeLogs(LogIterator logs){} @Override public void writeOutbounds(OutboundGroupItemIterator outbounds){} @Override public void writeStatus(StatusMessage status){nativeVpnStats(status.getUplink(),status.getDownlink(),status.getUplinkTotal(),status.getDownlinkTotal(),status.getConnectionsIn()+status.getConnectionsOut());}
    private void foreground(){ NotificationManager m=getSystemService(NotificationManager.class); if(m!=null&&Build.VERSION.SDK_INT>=26&&m.getNotificationChannel(CHANNEL_ID)==null)m.createNotificationChannel(new NotificationChannel(CHANNEL_ID,"VPN 状态",NotificationManager.IMPORTANCE_LOW)); android.app.Notification n=new android.app.Notification.Builder(this,CHANNEL_ID).setContentTitle("Clash-Flux").setContentText("sing-box VPN 隧道运行中").setSmallIcon(android.R.drawable.stat_notify_sync_noanim).setOngoing(true).build(); if(Build.VERSION.SDK_INT>=35)startForeground(NOTIFICATION_ID,n,ServiceInfo.FOREGROUND_SERVICE_TYPE_SYSTEM_EXEMPTED);else startForeground(NOTIFICATION_ID,n); }
}
