package dev.farna.clashflux;

import android.app.*;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.ConnectivityManager;
import android.net.DnsResolver;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.RouteInfo;
import android.net.VpnService;
import android.os.Build;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.system.ErrnoException;
import android.util.Log;
import io.nekohasekai.libbox.*;
import java.io.File;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.InetSocketAddress;
import java.net.UnknownHostException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Iterator;
import java.util.List;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONArray;
import org.json.JSONObject;

/** sing-box owns the data plane; Android owns TUN creation and protect(fd). */
public final class ClashVpnService extends VpnService implements PlatformInterface, CommandServerHandler, CommandClientHandler {
    private static final String TAG = "ClashFlux", CHANNEL_ID = "clashflux_vpn";
    private static final int NOTIFICATION_ID = 1;
    private static final Object LIBBOX_SETUP_LOCK = new Object();
    private static volatile boolean libboxSetup;
    private CommandServer server;
    private CommandClient client;
    private ParcelFileDescriptor tunnel;
    private boolean started;
    private boolean foregroundReady;
    private boolean libboxReady;
    private boolean failureReported;
    private volatile boolean starting;
    private volatile boolean startRequested;
    private final AtomicBoolean stopRequested = new AtomicBoolean();
    private volatile Network defaultNetwork;
    private final Object clientLock = new Object();
    private static volatile ClashVpnService current;
    private static volatile String outboundGroupsJson = "{\"proxies\":{}}";
    private static final Executor DNS_EXECUTOR = Executors.newCachedThreadPool(runnable -> {
        Thread thread = new Thread(runnable, "clashflux-dns");
        thread.setDaemon(true);
        return thread;
    });
    static { System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY); }
    private static native void nativeVpnState(int state, String message);
    private static native void nativeVpnStats(long uploadRate, long downloadRate,
                                              long uploadTotal, long downloadTotal,
                                              int connections);

    @Override public void onCreate() {
        super.onCreate();
        current = this;
        // Android starts the foreground-service deadline before onCreate.
        // Promote the service before JNI setup, file access, or any libbox
        // initialization so a slow device cannot kill the VPN process first.
        try {
            foreground();
            foregroundReady = true;
            MainActivity.appLog("VPN 前台服务已就绪", false);
        } catch (RuntimeException error) {
            fail("无法启动 VPN 前台服务：" + error.getMessage());
            return;
        }
        try {
            MainActivity.appLog("VPN 服务已创建，开始初始化 libbox", false);
            MainActivity.bootstrapNative(this);
            defaultNetwork = findPhysicalNetwork();
            setup();
        } catch (Throwable error) {
            Log.e(TAG, "VPN service initialization", error);
            fail("VPN 服务初始化失败：" + error.getMessage());
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
        try {
            SetupOptions o = new SetupOptions();
            o.setBasePath(getFilesDir().getPath());
            o.setWorkingPath(getFilesDir().getPath());
            o.setTempPath(getCacheDir().getPath());
            // libbox can call Java platform callbacks from a Go goroutine.
            // This workaround prevents the Android callback stack-splitting
            // failure seen on some ARM64/older Android runtimes.
            o.setFixAndroidStack(true);
            o.setLogMaxLines(3000);
            o.setDebug(BuildConfig.DEBUG);
            o.setCrashReportSource("ClashFlux");
            o.setAppVersion(String.valueOf(BuildConfig.VERSION_CODE));
            o.setAppMarketingVersion(BuildConfig.VERSION_NAME);
            synchronized (LIBBOX_SETUP_LOCK) {
                if (libboxSetup) {
                    Libbox.reloadSetupOptions(o);
                } else {
                    Libbox.setup(o);
                    libboxSetup = true;
                }
            }
            libboxReady = true;
            MainActivity.appLog("libbox 初始化完成", false);
        } catch (Throwable e) {
            Log.e(TAG, "libbox setup", e);
            MainActivity.appLog("libbox 初始化失败：" + e.getMessage(), true);
        }
    }
    private void startDataPlane() {
        MainActivity.appLog("开始创建 sing-box VPN 数据面", false);
        nativeVpnState(1, "正在启动 sing-box VPN 数据面");
        try {
            if (!libboxReady) throw new IllegalStateException("libbox 尚未初始化完成");
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
            MainActivity.appLog("已找到运行配置，准备校验 libbox 配置", false);
            String configContent = new String(
                    Files.readAllBytes(config.toPath()), StandardCharsets.UTF_8);
            Libbox.checkConfig(configContent);
            MainActivity.appLog("运行配置校验通过", false);
            MainActivity.appLog("正在创建 libbox CommandServer", false);
            server = new CommandServer(this, this);
            MainActivity.appLog("libbox CommandServer 已创建，正在监听本地命令通道", false);
            server.start();
            MainActivity.appLog("libbox CommandServer 已启动", false);
            if (!startRequested) {
                close("VPN 启动已取消");
                return;
            }
            MainActivity.appLog("正在启动 libbox 服务数据面", false);
            server.startOrReloadService(
                configContent, new OverrideOptions());
            MainActivity.appLog("libbox 服务数据面启动完成", false);
            if (!startRequested) {
                close("VPN 启动已取消");
                return;
            }
            started = true; BootReceiver.setVpnActive(this, true);
            nativeVpnState(2, "sing-box 已附着 TUN；socket protect 已启用");
            MainActivity.appLog("sing-box 已成功附着 Android TUN，控制通道按需连接", false);
        } catch (Throwable e) {
            if (startRequested) fail("sing-box 启动失败: " + e.getMessage());
        } finally {
            starting = false;
        }
    }
    @Override public int openTun(TunOptions o) throws Exception {
        try {
            if (VpnService.prepare(this) != null) {
                throw new SecurityException("Android VPN 尚未获得系统授权");
            }
            Builder b = new Builder().setSession("Clash-Flux")
                    .setMtu(o.getMTU()).setBlocking(false);
            // The app must keep a physical-network escape path for importing
            // or refreshing subscriptions while its own TUN is active.  The
            // VPN data plane still captures other applications; without this
            // exclusion HuxerUI HttpClient is routed back into the TUN before
            // a usable profile exists and subscription HTTP requests fail.
            b.addDisallowedApplication(getPackageName());
            MainActivity.appLog("已排除 Clash-Flux 自身流量，订阅请求保持直连", false);
            addAddresses(b, o.getInet4Address());
            addAddresses(b, o.getInet6Address());
            if (Build.VERSION.SDK_INT >= 29) b.setMetered(false);
            if (o.getAutoRoute()) {
                addRoutes(b, o.getInet4RouteRange());
                addRoutes(b, o.getInet6RouteRange());
                StringBox dnsMode = o.getDNSMode();
                if (dnsMode != null && !Libbox.DNSModeDisabled.equals(dnsMode.getValue())) {
                    addDnsServers(b, o.getDNSServerAddress());
                }
            }
            b.setConfigureIntent(PendingIntent.getActivity(
                    this, 0, new Intent(this, MainActivity.class),
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE));
            tunnel = b.establish();
            if (tunnel == null) throw new IllegalStateException("系统拒绝创建 VPN 接口");
            MainActivity.appLog("Android VPN 接口已创建", false);
            // Keep the PFD owned by the service until libbox is stopped.  The
            // official Android client returns pfd.fd and closes the same PFD
            // from onDestroy; detachFd() makes the Java lifetime unrelated to
            // the descriptor handed to libbox and can leave a stale tunnel
            // across service recreation.
            return tunnel.getFd();
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

    private static void addDnsServers(Builder b, StringIterator i) {
        if (i == null) return;
        while (i.hasNext()) {
            String address = i.next();
            if (address != null && !address.isEmpty()) b.addDnsServer(address);
        }
    }

    private static final class StringArray implements StringIterator {
        private final List<String> values;
        private int index;

        StringArray(List<String> values) {
            this.values = values;
        }

        @Override public boolean hasNext() { return index < values.size(); }
        // Keep the same contract as sing-box's Android StringArray: core
        // consumes the iterator, and does not use len() for preallocation.
        @Override public int len() { return 0; }
        @Override public String next() { return values.get(index++); }
    }

    private static final class NetworkInterfaceArray implements NetworkInterfaceIterator {
        private final Iterator<io.nekohasekai.libbox.NetworkInterface> values;

        NetworkInterfaceArray(List<io.nekohasekai.libbox.NetworkInterface> values) {
            this.values = values.iterator();
        }

        @Override public boolean hasNext() { return values.hasNext(); }
        @Override public io.nekohasekai.libbox.NetworkInterface next() {
            return values.next();
        }
    }

    private static final LocalDNSTransport SYSTEM_DNS = new LocalDNSTransport() {
        private Network physicalNetwork() {
            ClashVpnService service = current;
            Network physical = service == null ? null : service.defaultNetwork;
            if (physical == null && service != null) {
                physical = service.findPhysicalNetwork();
                service.defaultNetwork = physical;
            }
            return physical;
        }

        @Override public boolean raw() {
            return Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q;
        }

        @Override public void exchange(ExchangeContext context, byte[] message) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
                context.errorCode(2);
                return;
            }
            final Network physical = physicalNetwork();
            if (physical == null) {
                context.errorCode(2);
                return;
            }
            final CountDownLatch completed = new CountDownLatch(1);
            final CancellationSignal cancellation = new CancellationSignal();
            try {
                context.onCancel(() -> {
                    cancellation.cancel();
                    completed.countDown();
                });
                DnsResolver.getInstance().rawQuery(
                        physical, message, DnsResolver.FLAG_NO_RETRY, DNS_EXECUTOR,
                        cancellation, new DnsResolver.Callback<byte[]>() {
                            @Override public void onAnswer(byte[] answer, int rcode) {
                                try {
                                    if (rcode == 0) context.rawSuccess(answer);
                                    else context.errorCode(rcode);
                                } finally {
                                    completed.countDown();
                                }
                            }

                            @Override public void onError(DnsResolver.DnsException error) {
                                try {
                                    if (error.getCause() instanceof ErrnoException) {
                                        context.errnoCode(((ErrnoException) error.getCause()).errno);
                                    } else {
                                        context.errorCode(2);
                                    }
                                } finally {
                                    completed.countDown();
                                }
                            }
                        });
                await(completed, cancellation);
            } catch (RuntimeException error) {
                cancellation.cancel();
                context.errorCode(2);
            }
        }

        @Override public void lookup(ExchangeContext context, String network,
                                     String domain) {
            final Network physical = physicalNetwork();
            if (physical == null) {
                context.errorCode(2);
                return;
            }
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
                try {
                    InetAddress[] addresses = physical.getAllByName(domain);
                    StringBuilder result = new StringBuilder();
                    for (InetAddress address : addresses) {
                        if (address == null) continue;
                        if (result.length() > 0) result.append('\n');
                        result.append(address.getHostAddress());
                    }
                    if (result.length() == 0) context.errorCode(3);
                    else context.success(result.toString());
                } catch (UnknownHostException error) {
                    context.errorCode(3);
                } catch (RuntimeException error) {
                    context.errorCode(2);
                }
                return;
            }

            final CountDownLatch completed = new CountDownLatch(1);
            final CancellationSignal cancellation = new CancellationSignal();
            try {
                context.onCancel(() -> {
                    cancellation.cancel();
                    completed.countDown();
                });
                int type = network != null && network.endsWith("4")
                        ? DnsResolver.TYPE_A
                        : network != null && network.endsWith("6")
                        ? DnsResolver.TYPE_AAAA : 0;
                DnsResolver.Callback<Collection<InetAddress>> callback =
                        new DnsResolver.Callback<Collection<InetAddress>>() {
                            @Override public void onAnswer(Collection<InetAddress> addresses,
                                                            int rcode) {
                                try {
                                    if (rcode != 0) {
                                        context.errorCode(rcode);
                                        return;
                                    }
                                    StringBuilder result = new StringBuilder();
                                    if (addresses != null) {
                                        for (InetAddress address : addresses) {
                                            if (address == null) continue;
                                            if (result.length() > 0) result.append('\n');
                                            result.append(address.getHostAddress());
                                        }
                                    }
                                    if (result.length() == 0) context.errorCode(3);
                                    else context.success(result.toString());
                                } finally {
                                    completed.countDown();
                                }
                            }

                            @Override public void onError(DnsResolver.DnsException error) {
                                try {
                                    if (error.getCause() instanceof ErrnoException) {
                                        context.errnoCode(((ErrnoException) error.getCause()).errno);
                                    } else {
                                        context.errorCode(2);
                                    }
                                } finally {
                                    completed.countDown();
                                }
                            }
                        };
                if (type == DnsResolver.TYPE_A || type == DnsResolver.TYPE_AAAA) {
                    DnsResolver.getInstance().query(
                            physical, domain, type, DnsResolver.FLAG_NO_RETRY,
                            DNS_EXECUTOR, cancellation, callback);
                } else {
                    DnsResolver.getInstance().query(
                            physical, domain, DnsResolver.FLAG_NO_RETRY,
                            DNS_EXECUTOR, cancellation, callback);
                }
                await(completed, cancellation);
            } catch (RuntimeException error) {
                cancellation.cancel();
                context.errorCode(2);
            }
        }

        private void await(CountDownLatch completed, CancellationSignal cancellation) {
            try {
                if (!completed.await(15, TimeUnit.SECONDS)) {
                    cancellation.cancel();
                }
            } catch (InterruptedException error) {
                cancellation.cancel();
                Thread.currentThread().interrupt();
            }
        }
    };

    private Network findPhysicalNetwork() {
        ConnectivityManager manager =
                (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        if (manager == null) return null;
        Network active = manager.getActiveNetwork();
        if (isPhysicalNetwork(manager, active)) return active;
        try {
            for (Network network : manager.getAllNetworks()) {
                if (isPhysicalNetwork(manager, network)) return network;
            }
        } catch (RuntimeException error) {
            MainActivity.appLog("读取 Android 默认网络失败：" + error.getMessage(), true);
        }
        return null;
    }

    private static boolean isPhysicalNetwork(ConnectivityManager manager,
                                             Network network) {
        if (network == null) return false;
        NetworkCapabilities capabilities = manager.getNetworkCapabilities(network);
        return capabilities != null
                && !capabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN);
    }

    @Override public NetworkInterfaceIterator getInterfaces() {
        List<io.nekohasekai.libbox.NetworkInterface> result = new ArrayList<>();
        ConnectivityManager manager =
                (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        if (manager == null) return new NetworkInterfaceArray(result);
        try {
            // Build the list from Android ConnectivityManager, not from every
            // Linux interface. This avoids exposing the VPN's own tun device
            // as a candidate physical default interface.
            for (Network network : manager.getAllNetworks()) {
                LinkProperties link = manager.getLinkProperties(network);
                NetworkCapabilities capabilities = manager.getNetworkCapabilities(network);
                if (link == null || capabilities == null
                        || link.getInterfaceName() == null) continue;
                java.net.NetworkInterface source;
                try {
                    source = java.net.NetworkInterface.getByName(link.getInterfaceName());
                } catch (Exception ignored) {
                    continue;
                }
                if (source == null) continue;
                io.nekohasekai.libbox.NetworkInterface target =
                        new io.nekohasekai.libbox.NetworkInterface();
                target.setName(link.getInterfaceName());
                target.setIndex(source.getIndex());
                try {
                    int mtu = source.getMTU();
                    target.setMTU(mtu > 0 ? mtu : 1500);
                } catch (Exception ignored) {
                    target.setMTU(1500);
                }

                List<String> addresses = new ArrayList<>();
                for (InterfaceAddress address : source.getInterfaceAddresses()) {
                    if (address == null || address.getAddress() == null) continue;
                    addresses.add(stripAddressScope(address.getAddress()) + "/"
                            + address.getNetworkPrefixLength());
                }
                target.setAddresses(new StringArray(addresses));
                List<String> dnsServers = new ArrayList<>();
                for (InetAddress dns : link.getDnsServers()) {
                    if (dns != null) dnsServers.add(stripAddressScope(dns));
                }
                target.setDNSServer(new StringArray(dnsServers));
                List<String> gateways = new ArrayList<>();
                for (RouteInfo route : link.getRoutes()) {
                    if (route == null || route.getDestination() == null
                            || route.getDestination().getPrefixLength() != 0) continue;
                    InetAddress gateway = route.getGateway();
                    if (gateway != null && !gateway.isAnyLocalAddress()) {
                        gateways.add(stripAddressScope(gateway));
                    }
                }
                target.setGateway(new StringArray(gateways));
                if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                    target.setType(Libbox.InterfaceTypeWIFI);
                } else if (capabilities.hasTransport(
                        NetworkCapabilities.TRANSPORT_CELLULAR)) {
                    target.setType(Libbox.InterfaceTypeCellular);
                } else if (capabilities.hasTransport(
                        NetworkCapabilities.TRANSPORT_ETHERNET)) {
                    target.setType(Libbox.InterfaceTypeEthernet);
                } else {
                    target.setType(Libbox.InterfaceTypeOther);
                }

                int flags = 0;
                try {
                    if (capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)) {
                        flags |= 0x1 | 0x40;
                    }
                    if (source.isLoopback()) flags |= 0x8;
                    if (source.isPointToPoint()) flags |= 0x10;
                    if (source.supportsMulticast()) flags |= 0x1000;
                } catch (Exception ignored) {
                    // Interface metadata is advisory; keep the entry usable.
                }
                target.setFlags(flags);
                target.setMetered(!capabilities.hasCapability(
                        NetworkCapabilities.NET_CAPABILITY_NOT_METERED));
                result.add(target);
            }
        } catch (Exception error) {
            MainActivity.appLog("读取 Android 网络接口失败：" + error.getMessage(), true);
        }
        return new NetworkInterfaceArray(result);
    }

    private static String stripAddressScope(InetAddress address) {
        String value = address.getHostAddress();
        int scope = value == null ? -1 : value.indexOf('%');
        return scope < 0 ? value : value.substring(0, scope);
    }

    @Override public LocalDNSTransport localDNSTransport() { return SYSTEM_DNS; }

    private CommandClient controlClient() throws Exception {
        synchronized (clientLock) {
            if (client != null) return client;
            if (!startRequested) throw new IllegalStateException("VPN 未在运行");
            MainActivity.appLog("线路切换：按需连接 libbox 控制通道", false);
            CommandClientOptions options = new CommandClientOptions();
            options.setStatusInterval(0);
            CommandClient candidate = new CommandClient(this, options);
            candidate.connect();
            client = candidate;
            MainActivity.appLog("libbox 控制通道已连接", false);
            return candidate;
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
        CommandClient activeClient;
        synchronized (clientLock) {
            activeClient = client;
            client = null;
        }
        try { if (activeClient != null) activeClient.disconnect(); } catch (Exception ignored) {}
        try { if (server != null) server.closeService(); } catch (Exception ignored) {}
        try { if (server != null) server.close(); } catch (Exception ignored) {}
        server = null;
        try { if (tunnel != null) tunnel.close(); } catch (Exception ignored) {}
        tunnel = null;
        nativeVpnStats(0, 0, 0, 0, 0);
        if (reportStopped && !failureReported) nativeVpnState(0, msg);
    }

    /** Stops the current data plane before asking Android to destroy the service. */
    public static boolean stopCurrent() {
        ClashVpnService service = current;
        if (service == null) return false;
        if (!service.stopRequested.compareAndSet(false, true)) return true;
        service.startRequested = false;
        new Thread(() -> {
            service.close("用户请求关闭 Android VPN");
            service.stopForeground(STOP_FOREGROUND_REMOVE);
            service.stopSelf();
        }, "clashflux-vpn-stop").start();
        return true;
    }

    public static boolean selectOutbound(String group, String name) {
        ClashVpnService service = current;
        if (service == null || group == null || group.isEmpty() || name == null || name.isEmpty()) {
            return false;
        }
        try {
            service.controlClient().selectOutbound(group, name);
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
    @Override public boolean useProcFS(){return Build.VERSION.SDK_INT < Build.VERSION_CODES.Q;} @Override public boolean includeAllNetworks(){return false;} @Override public boolean underNetworkExtension(){return false;}
    @Override public boolean usePlatformBridge(){return false;} @Override public boolean usePlatformShell(){return false;}
    @Override public void clearDNSCache(){}
    // libbox dereferences the returned owner while preparing TUN DNS metadata;
    // returning null here causes a Go-side SIGSEGV. Android 10+ exposes the
    // exact socket-to-UID lookup, while older releases use libbox procfs mode.
    @Override public ConnectionOwner findConnectionOwner(int protocol, String sourceAddress,
                                                         int sourcePort, String destinationAddress,
                                                         int destinationPort) throws Exception {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            throw new UnsupportedOperationException("Android connection owner lookup requires API 29");
        }
        ConnectivityManager manager =
                (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        if (manager == null) throw new IllegalStateException("Android 网络服务不可用");
        int uid = manager.getConnectionOwnerUid(
                protocol,
                new InetSocketAddress(sourceAddress, sourcePort),
                new InetSocketAddress(destinationAddress, destinationPort));
        if (uid == Process.INVALID_UID) {
            throw new IllegalStateException("Android 未找到连接所属应用");
        }
        String[] packages = getPackageManager().getPackagesForUid(uid);
        List<String> packageNames = new ArrayList<>();
        if (packages != null) {
            for (String packageName : packages) {
                if (packageName != null && !packageName.isEmpty()) packageNames.add(packageName);
            }
        }
        ConnectionOwner owner = new ConnectionOwner();
        owner.setUserId(uid);
        owner.setUserName(packageNames.isEmpty() ? "" : packageNames.get(0));
        owner.setAndroidPackageNames(new StringArray(packageNames));
        return owner;
    }
    @Override public WIFIState readWIFIState(){return null;}
    @Override public void startDefaultInterfaceMonitor(InterfaceUpdateListener l){} @Override public void closeDefaultInterfaceMonitor(InterfaceUpdateListener l){} @Override public void startNeighborMonitor(NeighborUpdateListener l){} @Override public void closeNeighborMonitor(NeighborUpdateListener l){} @Override public void registerMyInterface(String n){} @Override public void checkPlatformShell(){}
    @Override public BridgeSession createBridge(BridgeOptions o){return null;} @Override public PlatformUser lookupUser(String n){return null;} @Override public ShellSession openShellSession(PlatformUser u,String c,StringIterator e,String d,int p,int q){return null;} @Override public String lookupSFTPServer(){return "";} @Override public String readSystemSSHHostKey(){return "";} @Override public String tailscaleHostname(){return "";}
    @Override public void sendNotification(io.nekohasekai.libbox.Notification n){} @Override public void cancelNotification(String i,int t){} @Override public int connectSSHAgent(){return -1;} @Override public SystemProxyStatus getSystemProxyStatus(){return null;} @Override public void serviceReload(){} @Override public void serviceStop(){close("sing-box 已停止");} @Override public void setSystemProxyEnabled(boolean e){} @Override public void triggerNativeCrash(){} @Override public void writeDebugMessage(String m){Log.d(TAG,m); MainActivity.appLog("libbox 调试信息："+m,false);}
    @Override public void clearLogs(){} @Override public void connected(){Log.i(TAG,"sing-box status stream connected"); MainActivity.appLog("libbox 状态通道已连接",false);} @Override public void disconnected(String message){Log.w(TAG,"sing-box status stream disconnected: "+message); MainActivity.appLog("libbox 状态通道断开："+message,true);} @Override public void initializeClashMode(StringIterator modes,String current){} @Override public void setDefaultLogLevel(int level){} @Override public void updateClashMode(String mode){} @Override public void writeConnectionEvents(ConnectionEvents events){} @Override public void writeLogs(LogIterator logs){} @Override public void writeOutbounds(OutboundGroupItemIterator outbounds){} @Override public void writeStatus(StatusMessage status){
        if (status == null) return;
        try {
            nativeVpnStats(status.getUplink(),status.getDownlink(),status.getUplinkTotal(),status.getDownlinkTotal(),status.getConnectionsIn()+status.getConnectionsOut());
        } catch (Throwable error) {
            Log.w(TAG, "Unable to publish sing-box status", error);
            MainActivity.appLog("发布 libbox 状态失败：" + error.getMessage(), true);
        }
    }
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
        } catch (Throwable error) {
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
