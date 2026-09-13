package dev.farna.clashflux;

import android.content.Context;
import android.util.Log;

import io.github.oviron.libmihomo.Clash;
import io.github.oviron.libmihomo.InvokeInterface;

import java.io.File;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The app-owned lifecycle seam around the embedded mihomo C-shared library.
 *
 * <p>This intentionally contains no VPN policy.  {@link ClashVpnService} owns
 * the Android TUN device and supplies protect(fd); this class only brings the
 * core up against the config.yaml written by the C++ store.</p>
 */
final class MihomoRuntime {
    private static final String TAG = "ClashFlux";
    private static final long SETUP_TIMEOUT_SECONDS = 30;

    private MihomoRuntime() {}

    static synchronized boolean start(Context context, String homeDirectory) {
        try {
            Clash.INSTANCE.load(context.getApplicationInfo().nativeLibraryDir);
            Clash.INSTANCE.assertReady();
            File home = new File(homeDirectory);
            if (!home.isDirectory() && !home.mkdirs()) {
                Log.e(TAG, "Unable to create mihomo home: " + home);
                return false;
            }
            File config = new File(home, "config.yaml");
            if (!config.isFile()) {
                Log.e(TAG, "Missing generated mihomo config: " + config);
                return false;
            }

            CountDownLatch finished = new CountDownLatch(1);
            final String[] failure = new String[1];
            String init = "{\"home-dir\":\"" + jsonEscape(home.getAbsolutePath())
                    + "\",\"version\":" + android.os.Build.VERSION.SDK_INT + "}";
            Clash.INSTANCE.quickSetup(init, "{\"selected-map\":{}}", new InvokeInterface() {
                @Override public void onResult(String result) {
                    failure[0] = result;
                    finished.countDown();
                }
            });
            if (!finished.await(SETUP_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
                Log.e(TAG, "Timed out starting embedded mihomo");
                return false;
            }
            if (failure[0] != null && !failure[0].isEmpty()) {
                Log.e(TAG, "Embedded mihomo setup failed: " + failure[0]);
                return false;
            }
            Log.i(TAG, "Embedded mihomo is ready: " + config);
            return true;
        } catch (Exception error) {
            Log.e(TAG, "Unable to start embedded mihomo", error);
            return false;
        }
    }

    static synchronized void stop() {
        if (!Clash.INSTANCE.isLoaded()) return;
        try {
            // Do not stop TUN here: ClashVpnService owns that lifecycle and
            // may be intentionally alive while a profile is reapplied. Only
            // remove controller/proxy listeners for this core generation.
            Clash.INSTANCE.invokeAction(
                    "{\"id\":\"clashflux-stop\",\"method\":\"stopListener\",\"data\":\"\"}",
                    new InvokeInterface() {
                        @Override public void onResult(String ignored) { }
                    });
        } catch (Exception error) {
            Log.w(TAG, "Unable to stop embedded mihomo cleanly", error);
        }
    }

    private static String jsonEscape(String value) {
        return value.replace("\\", "\\\\").replace("\"", "\\\"");
    }
}
