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
import android.net.IpPrefix;
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
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
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
    static final String EXTRA_SPEED_TEST_ONLY = "speed_test_only";
    private static final int URL_TEST_BATCH_SIZE = 12;
    private static final long URL_TEST_BATCH_TIMEOUT_MS = 16000L;
    private static final long URL_TEST_POLL_MS = 150L;
    private static final Object LIBBOX_SETUP_LOCK = new Object();
    private static final Object URL_TEST_LOCK = new Object();
    private static final AtomicBoolean URL_TEST_RUNNING = new AtomicBoolean();
    private static final Executor URL_TEST_EXECUTOR = Executors.newSingleThreadExecutor(runnable -> {
        Thread thread = new Thread(runnable, "clashflux-url-test");
        thread.setDaemon(true);
        return thread;
    });
    private static volatile boolean libboxSetup;
    private CommandServer server;
    private CommandClient client;
    private ParcelFileDescriptor tunnel;
    private boolean started;
    private boolean foregroundReady;
    private boolean libboxReady;
    private boolean failureReported;
    private volatile boolean speedTestOnly;
    private volatile boolean starting;
    private volatile boolean startRequested;
    private final AtomicBoolean stopRequested = new AtomicBoolean();
    private volatile Network defaultNetwork;
    private volatile InterfaceUpdateListener defaultInterfaceListener;
    private final Object clientLock = new Object();
    private final Object connectionsLock = new Object();
    private Connections connectionSnapshot = Libbox.newConnections();
    private static volatile ClashVpnService current;
    private static volatile String outboundGroupsJson = "{\"proxies\":{}}";
    private static volatile String pendingUrlTestGroup;
    private static volatile String connectionsJson =
            "{\"uploadTotal\":0,\"downloadTotal\":0,\"connections\":[]}";
    private static volatile long uploadTotal;
    private static volatile long downloadTotal;
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
        final boolean requestedSpeedTest = i != null
                && i.getBooleanExtra(EXTRA_SPEED_TEST_ONLY, false);
        if (requestedSpeedTest) {
            speedTestOnly = true;
        } else if (i != null) {
            // A real VPN request promotes an already-running no-TUN speed
            // service back to the normal data plane on the next start.
            if (speedTestOnly && started && !starting) {
                speedTestOnly = false;
                stopRequested.set(false);
                starting = true;
                new Thread(() -> {
                    close("切换到 Android VPN 数据面", false);
                    if (!stopRequested.get()) {
                        startRequested = true;
                        startDataPlane();
                    } else {
                        starting = false;
                    }
                }, "clashflux-vpn-promote").start();
                return START_STICKY;
            }
            speedTestOnly = false;
        }
        // stopCurrent() may have stopped the data plane while Android reused
        // this service instance. A new start command is a fresh lifecycle.
        stopRequested.set(false);
        if (!started && !starting) {
            starting = true;
            startRequested = true;
            MainActivity.appLog("VPN 数据面启动线程已开始", false);
            new Thread(this::startDataPlane, "clashflux-vpn-start").start();
        }
        return speedTestOnly ? START_NOT_STICKY : START_STICKY;
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
        updateForegroundNotification("正在启动 sing-box VPN 隧道");
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
            started = true;
            if (speedTestOnly) {
                BootReceiver.setVpnActive(this, false);
                nativeVpnState(0, "测速内核已启动（未启用 VPN 代理）");
                updateForegroundNotification("sing-box 测速内核运行中（未启用 VPN）");
                MainActivity.appLog("测速内核已启动，未启用 Android VPN/TUN", false);
            } else {
                BootReceiver.setVpnActive(this, true);
                nativeVpnState(2, "sing-box 已附着 TUN；socket protect 已启用");
                updateForegroundNotification("sing-box VPN 隧道运行中");
                MainActivity.appLog("sing-box 已成功附着 Android TUN，控制通道按需连接", false);
            }
            String queuedGroup;
            synchronized (URL_TEST_LOCK) {
                queuedGroup = pendingUrlTestGroup;
                pendingUrlTestGroup = null;
            }
            if (queuedGroup != null && !queuedGroup.isEmpty()) {
                queueUrlTest(queuedGroup);
            }
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
            addAddresses(b, o.getInet4Address());
            addAddresses(b, o.getInet6Address());
            if (Build.VERSION.SDK_INT >= 29) b.setMetered(false);
            if (o.getAutoRoute()) {
                // Android 13+ expects the explicit route-address iterator. The
                // route-range iterator is the legacy API and is empty for the
                // current libbox when the managed config uses auto_route. An
                // empty Builder route only leaves the /30 TUN address route,
                // so other applications never enter the VPN data plane.
                boolean hasIPv4Route = false;
                boolean hasIPv6Route = false;
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    hasIPv4Route = addRoutes(b, o.getInet4RouteAddress());
                    hasIPv6Route = addRoutes(b, o.getInet6RouteAddress());
                    addExcludedRoutes(b, o.getInet4RouteExcludeAddress(),
                            o.getInet6RouteExcludeAddress());
                } else {
                    hasIPv4Route = addRoutes(b, o.getInet4RouteRange());
                    hasIPv6Route = addRoutes(b, o.getInet6RouteRange());
                }
                if (!hasIPv4Route) {
                    b.addRoute("0.0.0.0", 0);
                    MainActivity.appLog("Android VPN 未返回 IPv4 路由，已补充全局路由 0.0.0.0/0", false);
                }
                // Do not install an IPv6 default route unless libbox also
                // supplied an IPv6 TUN address. Some Android 13+ devices
                // reject ::/0 without an IPv6 address on the VPN interface.
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
                        !hasIPv6Route && hasIPv6Address(o)) {
                    b.addRoute("::", 0);
                }
                StringBox dnsMode = o.getDNSMode();
                if (dnsMode != null && !Libbox.DNSModeDisabled.equals(dnsMode.getValue())) {
                    addDnsServers(b, o.getDNSServerAddress());
                }
            }
            addPackageRules(b, o);
            MainActivity.appLog("已排除 Clash-Flux 自身流量，订阅请求保持直连", false);
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
    private static boolean addRoutes(Builder b, RoutePrefixIterator i) {
        if (i == null) return false;
        boolean added = false;
        while (i.hasNext()) {
            RoutePrefix p = i.next();
            if (p != null) {
                b.addRoute(p.address(), p.prefix());
                added = true;
            }
        }
        return added;
    }

    private static void addExcludedRoutes(Builder b, RoutePrefixIterator ipv4,
                                          RoutePrefixIterator ipv6) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return;
        if (ipv4 != null) {
            while (ipv4.hasNext()) {
                RoutePrefix p = ipv4.next();
                if (p != null) excludeRoute(b, p);
            }
        }
        if (ipv6 != null) {
            while (ipv6.hasNext()) {
                RoutePrefix p = ipv6.next();
                if (p != null) excludeRoute(b, p);
            }
        }
    }

    private static void excludeRoute(Builder b, RoutePrefix p) {
        try {
            b.excludeRoute(new IpPrefix(InetAddress.getByName(p.address()), p.prefix()));
        } catch (Exception error) {
            MainActivity.appLog("忽略无效的 Android 排除路由：" + p.address() + "/" + p.prefix(), true);
        }
    }

    private static boolean hasIPv6Address(TunOptions o) {
        RoutePrefixIterator addresses = o.getInet6Address();
        if (addresses == null) return false;
        boolean found = addresses.hasNext();
        while (addresses.hasNext()) addresses.next();
        return found;
    }

    private void addPackageRules(Builder b, TunOptions o) {
        StringIterator include = o.getIncludePackage();
        boolean hasInclude = include != null && include.hasNext();
        if (hasInclude) {
            while (include.hasNext()) {
                String packageName = include.next();
                if (packageName == null || packageName.isEmpty()) continue;
                try {
                    b.addAllowedApplication(packageName);
                } catch (Exception error) {
                    MainActivity.appLog("忽略不存在的 Android 包：" + packageName, true);
                }
            }
            return;
        }
        // Keep the control/subscription app on the physical network. This is
        // also the fallback when the imported profile has no per-app options.
        try {
            b.addDisallowedApplication(getPackageName());
        } catch (Exception error) {
            MainActivity.appLog("无法排除 Clash-Flux 自身流量：" + error.getMessage(), true);
        }
        StringIterator exclude = o.getExcludePackage();
        if (exclude != null) {
            while (exclude.hasNext()) {
                String packageName = exclude.next();
                if (packageName == null || packageName.isEmpty()) continue;
                try {
                    b.addDisallowedApplication(packageName);
                } catch (Exception error) {
                    MainActivity.appLog("忽略不存在的 Android 包：" + packageName, true);
                }
            }
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

    private void notifyDefaultInterface(InterfaceUpdateListener listener) {
        if (listener == null) return;
        try {
            ConnectivityManager manager =
                    (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
            Network physical = defaultNetwork;
            if (manager == null || !isPhysicalNetwork(manager, physical)) {
                physical = findPhysicalNetwork();
                defaultNetwork = physical;
            }
            if (manager == null || physical == null) {
                listener.updateDefaultInterface("", -1, false, false);
                MainActivity.appLog("Android 未找到默认物理网络接口", true);
                return;
            }
            LinkProperties link = manager.getLinkProperties(physical);
            if (link == null || link.getInterfaceName() == null) {
                listener.updateDefaultInterface("", -1, false, false);
                MainActivity.appLog("Android 默认物理网络缺少接口信息", true);
                return;
            }
            java.net.NetworkInterface networkInterface =
                    java.net.NetworkInterface.getByName(link.getInterfaceName());
            if (networkInterface == null) {
                listener.updateDefaultInterface("", -1, false, false);
                MainActivity.appLog("Android 找不到物理接口：" + link.getInterfaceName(), true);
                return;
            }
            NetworkCapabilities capabilities = manager.getNetworkCapabilities(physical);
            boolean metered = capabilities != null && !capabilities.hasCapability(
                    NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
            listener.updateDefaultInterface(link.getInterfaceName(),
                    networkInterface.getIndex(), metered, false);
            MainActivity.appLog("Android 默认物理接口已注册：" + link.getInterfaceName()
                    + " (" + networkInterface.getIndex() + ")", false);
        } catch (Exception error) {
            MainActivity.appLog("注册 Android 默认物理接口失败：" + error.getMessage(), true);
            try {
                listener.updateDefaultInterface("", -1, false, false);
            } catch (RuntimeException ignored) {
            }
        }
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
            // CommandClient 默认不订阅任何数据流。代理页需要 groups，首页/日志
            // 需要 status；不声明这些命令时 writeGroups 永远不会回调，UI 只能看到
            // 空的订阅内容。
            options.addCommand(Libbox.CommandGroup);
            options.addCommand(Libbox.CommandStatus);
            options.addCommand(Libbox.CommandClashMode);
            options.addCommand(Libbox.CommandConnections);
            options.setStatusInterval(1L * 1000L * 1000L * 1000L);
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
        connectionsJson = "{\"uploadTotal\":0,\"downloadTotal\":0,\"connections\":[]}";
        uploadTotal = 0;
        downloadTotal = 0;
        synchronized (connectionsLock) {
            connectionSnapshot = Libbox.newConnections();
        }
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
        // Keep stop and the following profile-restart start command ordered.
        // The old background stop thread could let startVpn() observe
        // started=true, after which the new config was never loaded and the
        // service finally stopped underneath the newly selected subscription.
        service.close("用户请求关闭 Android VPN");
        service.stopForeground(STOP_FOREGROUND_REMOVE);
        service.stopSelf();
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

    public static boolean urlTest(String group) {
        if (group == null || group.isEmpty()) return false;
        synchronized (URL_TEST_LOCK) {
            pendingUrlTestGroup = group;
        }
        ClashVpnService service = current;
        if (service == null || (!service.started && !service.starting)) {
            // The speed-only service uses a no-TUN config. It does not request
            // VPN consent and does not capture application traffic.
            MainActivity.startSpeedTestService();
            return true;
        }
        if (!service.started) {
            // A normal VPN service is still starting; startDataPlane() drains
            // the pending request once its command server is ready.
            return true;
        }
        synchronized (URL_TEST_LOCK) {
            pendingUrlTestGroup = null;
        }
        service.queueUrlTest(group);
        return true;
    }

    private void queueUrlTest(String group) {
        if (!URL_TEST_RUNNING.compareAndSet(false, true)) {
            MainActivity.appLog("测速已在进行中，合并重复请求", false);
            return;
        }
        URL_TEST_EXECUTOR.execute(() -> {
            try {
                runUrlTestWhenReady(group);
            } finally {
                URL_TEST_RUNNING.set(false);
            }
        });
    }

    private void runUrlTestWhenReady(String group) {
        Exception lastError = null;
        for (int attempt = 0; attempt < 80; ++attempt) {
            if (!startRequested || stopRequested.get()) return;
            try {
                CommandClient activeClient = controlClient();
                JSONObject root = new JSONObject(outboundGroupsJson);
                JSONObject proxies = root.optJSONObject("proxies");
                JSONObject target = proxies == null ? null : proxies.optJSONObject(group);
                JSONArray all = target == null ? null : target.optJSONArray("all");
                if (all == null || all.length() == 0) {
                    throw new IllegalStateException("出站组尚未同步");
                }
                List<String> tags = new ArrayList<>();
                for (int index = 0; index < all.length(); ++index) {
                    String tag = all.optString(index, "");
                    if (!tag.isEmpty()) tags.add(tag);
                }
                if (tags.isEmpty()) {
                    throw new IllegalStateException("出站组没有可测速节点");
                }
                MainActivity.appLog("开始测速：" + group + "（" + tags.size()
                        + " 个节点，分批并发 " + URL_TEST_BATCH_SIZE + "）", false);
                int started = 0;
                for (int batchStart = 0; batchStart < tags.size();
                        batchStart += URL_TEST_BATCH_SIZE) {
                    int batchEnd = Math.min(batchStart + URL_TEST_BATCH_SIZE, tags.size());
                    List<String> batch = new ArrayList<>(
                            tags.subList(batchStart, batchEnd));
                    Map<String, Long> baseline = urlTestTimes(batch);
                    for (String tag : batch) {
                        if (stopRequested.get()) return;
                        try {
                            // The speed-only config has URLTest groups lowered
                            // to selectors, so this is the only test sweep.
                            // Keep the batch bounded to avoid saturating the
                            // phone/Wi-Fi path with every node at once.
                            activeClient.urlTest(tag);
                            started++;
                        } catch (Exception itemError) {
                            Log.w(TAG, "Unable to test outbound " + tag, itemError);
                        }
                    }
                    waitForUrlTestBatch(batch, baseline);
                }
                if (started > 0) {
                    return;
                }
                throw new IllegalStateException("出站组没有可测速节点");
            } catch (Exception error) {
                lastError = error;
                try {
                    Thread.sleep(200);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
        }
        if (lastError != null) {
            Log.w(TAG, "Unable to test outbound group " + group, lastError);
            MainActivity.appLog("测速启动失败：" + lastError.getMessage(), true);
        }
    }

    private static Map<String, Long> urlTestTimes(List<String> tags) {
        Map<String, Long> result = new HashMap<>();
        try {
            JSONObject root = new JSONObject(outboundGroupsJson);
            JSONObject proxies = root.optJSONObject("proxies");
            if (proxies == null) return result;
            for (String tag : tags) {
                JSONObject detail = proxies.optJSONObject(tag);
                result.put(tag, detail == null ? 0L : detail.optLong("urlTestTime", 0L));
            }
        } catch (Exception error) {
            Log.w(TAG, "Unable to read URLTest baselines", error);
        }
        return result;
    }

    private static void waitForUrlTestBatch(List<String> tags,
                                            Map<String, Long> baseline) {
        final long deadline = System.currentTimeMillis() + URL_TEST_BATCH_TIMEOUT_MS;
        while (System.currentTimeMillis() < deadline) {
            boolean complete = true;
            Map<String, Long> currentTimes = urlTestTimes(tags);
            for (String tag : tags) {
                long before = baseline.getOrDefault(tag, 0L);
                long after = currentTimes.getOrDefault(tag, 0L);
                if (after <= before) {
                    complete = false;
                    break;
                }
            }
            if (complete) return;
            try {
                Thread.sleep(URL_TEST_POLL_MS);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return;
            }
        }
    }

    public static boolean setClashMode(String mode) {
        ClashVpnService service = current;
        if (service == null || mode == null || mode.isEmpty()) return false;
        try {
            service.controlClient().setClashMode(mode);
            return true;
        } catch (Exception error) {
            Log.w(TAG, "Unable to set clash mode " + mode, error);
            MainActivity.appLog("出站模式切换失败：" + error.getMessage(), true);
            return false;
        }
    }

    public static String proxyGroups() {
        ClashVpnService service = current;
        if (service != null) {
            try {
                // 代理快照按需建立控制通道；首次进入代理页时也必须触发
                // writeGroups，否则 outboundGroupsJson 仍是空对象。
                service.controlClient();
            } catch (Exception error) {
                MainActivity.appLog("读取出站线路失败：" + error.getMessage(), true);
            }
        }
        return outboundGroupsJson;
    }

    public static String connectionsSnapshot() {
        ClashVpnService service = current;
        return service == null ? connectionsJson : service.readConnectionsSnapshot();
    }

    private String readConnectionsSnapshot() {
        synchronized (connectionsLock) {
            return connectionsJson;
        }
    }

    public static boolean closeConnection(String id) {
        ClashVpnService service = current;
        if (service == null || id == null || id.isEmpty()) return false;
        try {
            service.controlClient().closeConnection(id);
            return true;
        } catch (Exception error) {
            MainActivity.appLog("关闭 Android 连接失败：" + error.getMessage(), true);
            return false;
        }
    }

    public static boolean closeAllConnections() {
        ClashVpnService service = current;
        if (service == null) return false;
        try {
            service.controlClient().closeConnections();
            return true;
        } catch (Exception error) {
            MainActivity.appLog("关闭 Android 连接失败：" + error.getMessage(), true);
            return false;
        }
    }
    // Preserve Failed for the native/UI state machine.  Previously close() sent
    // a second "stopped" callback immediately, hiding the actual libbox error.
    private void fail(String msg) {
        failureReported = true;
        Log.e(TAG, msg);
        MainActivity.appLog(msg, true);
        close(msg, false);
        nativeVpnState(speedTestOnly ? 0 : 3, msg);
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
    @Override public void startDefaultInterfaceMonitor(InterfaceUpdateListener listener) {
        defaultInterfaceListener = listener;
        notifyDefaultInterface(listener);
    }
    @Override public void closeDefaultInterfaceMonitor(InterfaceUpdateListener listener) {
        if (defaultInterfaceListener == listener) defaultInterfaceListener = null;
    }
    @Override public void startNeighborMonitor(NeighborUpdateListener l){}
    @Override public void closeNeighborMonitor(NeighborUpdateListener l){}
    @Override public void registerMyInterface(String n){}
    @Override public void checkPlatformShell(){}
    @Override public BridgeSession createBridge(BridgeOptions o){return null;} @Override public PlatformUser lookupUser(String n){return null;} @Override public ShellSession openShellSession(PlatformUser u,String c,StringIterator e,String d,int p,int q){return null;} @Override public String lookupSFTPServer(){return "";} @Override public String readSystemSSHHostKey(){return "";} @Override public String tailscaleHostname(){return "";}
    @Override public void sendNotification(io.nekohasekai.libbox.Notification n){} @Override public void cancelNotification(String i,int t){} @Override public int connectSSHAgent(){return -1;} @Override public SystemProxyStatus getSystemProxyStatus(){return null;} @Override public void serviceReload(){} @Override public void serviceStop(){close("sing-box 已停止");} @Override public void setSystemProxyEnabled(boolean e){} @Override public void triggerNativeCrash(){} @Override public void writeDebugMessage(String m){Log.d(TAG,m); MainActivity.appLog("libbox 调试信息："+m,false);}
    @Override public void clearLogs(){} @Override public void connected(){Log.i(TAG,"sing-box status stream connected"); MainActivity.appLog("libbox 状态通道已连接",false);} @Override public void disconnected(String message){Log.w(TAG,"sing-box status stream disconnected: "+message); MainActivity.appLog("libbox 状态通道断开："+message,true);} @Override public void initializeClashMode(StringIterator modes,String current){} @Override public void setDefaultLogLevel(int level){} @Override public void updateClashMode(String mode){} @Override public void writeConnectionEvents(ConnectionEvents events){
        if (events == null) return;
        try {
            synchronized (connectionsLock) {
                connectionSnapshot.applyEvents(events);
                connectionSnapshot.filterState((int) Libbox.ConnectionStateActive);
                connectionSnapshot.sortByDate();
                connectionsJson = encodeConnectionsLocked();
            }
        } catch (Throwable error) {
            Log.w(TAG, "Unable to snapshot sing-box connections", error);
            MainActivity.appLog("读取 Android 连接失败：" + error.getMessage(), true);
        }
    } @Override public void writeLogs(LogIterator logs){} @Override public void writeOutbounds(OutboundGroupItemIterator outbounds){} @Override public void writeStatus(StatusMessage status){
        if (status == null) return;
        try {
            uploadTotal = status.getUplinkTotal();
            downloadTotal = status.getDownlinkTotal();
            synchronized (connectionsLock) {
                connectionsJson = encodeConnectionsLocked();
            }
            nativeVpnStats(status.getUplink(),status.getDownlink(),status.getUplinkTotal(),status.getDownlinkTotal(),status.getConnectionsIn()+status.getConnectionsOut());
        } catch (Throwable error) {
            Log.w(TAG, "Unable to publish sing-box status", error);
            MainActivity.appLog("发布 libbox 状态失败：" + error.getMessage(), true);
        }
    }

    private String encodeConnectionsLocked() throws Exception {
        JSONObject result = new JSONObject();
        result.put("uploadTotal", uploadTotal);
        result.put("downloadTotal", downloadTotal);
        JSONArray items = new JSONArray();
        ConnectionIterator iterator = connectionSnapshot.iterator();
        while (iterator != null && iterator.hasNext()) {
            Connection connection = iterator.next();
            if (connection == null) continue;
            JSONObject item = new JSONObject();
            item.put("id", safe(connection.getID()));
            item.put("upload", connection.getUplink());
            item.put("download", connection.getDownlink());
            JSONObject metadata = new JSONObject();
            String domain = safe(connection.getDomain());
            String destination = safe(connection.getDestination());
            metadata.put("host", domain.isEmpty() ? destination : domain);
            metadata.put("destination", destination);
            metadata.put("network", safe(connection.getNetwork()));
            metadata.put("destinationPort", destinationPort(destination));
            item.put("metadata", metadata);
            JSONArray chains = new JSONArray();
            StringIterator chain = connection.chain();
            while (chain != null && chain.hasNext()) {
                String hop = chain.next();
                if (hop != null && !hop.isEmpty()) chains.put(hop);
            }
            if (chains.length() > 0) item.put("chains", chains);
            item.put("rule", safe(connection.getRule()));
            items.put(item);
        }
        result.put("connections", items);
        return result.toString();
    }

    private static String safe(String value) { return value == null ? "" : value; }

    private static String destinationPort(String destination) {
        if (destination == null || destination.isEmpty()) return "";
        int colon = destination.lastIndexOf(':');
        return colon < 0 || colon == destination.length() - 1
                ? "" : destination.substring(colon + 1);
    }
    @Override public void writeGroups(OutboundGroupIterator groups) {
        try {
            JSONObject proxies = new JSONObject();
            JSONObject itemDetails = new JSONObject();
            int groupCount = 0;
            int itemCount = 0;
            while (groups != null && groups.hasNext()) {
                OutboundGroup group = groups.next();
                if (group == null || group.getTag() == null || group.getTag().isEmpty()) continue;
                groupCount++;
                JSONObject value = new JSONObject();
                value.put("type", group.getType());
                value.put("now", group.getSelected());
                value.put("selectable", group.getSelectable());
                JSONArray all = new JSONArray();
                OutboundGroupItemIterator items = group.getItems();
                while (items != null && items.hasNext()) {
                    OutboundGroupItem item = items.next();
                    if (item != null && item.getTag() != null) {
                        all.put(item.getTag());
                        JSONObject detail = new JSONObject();
                        detail.put("type", item.getType());
                        detail.put("urlTestTime", item.getURLTestTime());
                        detail.put("urlTestDelay", item.getURLTestDelay());
                        itemDetails.put(item.getTag(), detail);
                        itemCount++;
                    }
                }
                value.put("all", all);
                proxies.put(group.getTag(), value);
            }
            Iterator<String> details = itemDetails.keys();
            while (details.hasNext()) {
                String tag = details.next();
                if (!proxies.has(tag)) proxies.put(tag, itemDetails.get(tag));
            }
            outboundGroupsJson = new JSONObject().put("proxies", proxies).toString();
            Log.i(TAG, "libbox 出站组快照已更新：" + groupCount + " 组，" + itemCount + " 节点");
        } catch (Throwable error) {
            Log.w(TAG, "Unable to snapshot outbound groups", error);
            MainActivity.appLog("读取出站线路失败：" + error.getMessage(), true);
        }
    }
    private android.app.Notification buildForegroundNotification(String text) {
        android.app.Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new android.app.Notification.Builder(this, CHANNEL_ID)
                : new android.app.Notification.Builder(this);
        return builder
                .setContentTitle("Clash-Flux")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_notify_sync_noanim)
                .setCategory(android.app.Notification.CATEGORY_SERVICE)
                .setOngoing(true)
                .build();
    }

    private void updateForegroundNotification(String text) {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) manager.notify(NOTIFICATION_ID, buildForegroundNotification(text));
    }

    private void foreground() {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null && Build.VERSION.SDK_INT >= 26
                && manager.getNotificationChannel(CHANNEL_ID) == null) {
            manager.createNotificationChannel(new NotificationChannel(
                    CHANNEL_ID, "VPN 状态", NotificationManager.IMPORTANCE_LOW));
        }
        android.app.Notification notification =
                buildForegroundNotification("等待启动 sing-box VPN 隧道");
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
    }
}
