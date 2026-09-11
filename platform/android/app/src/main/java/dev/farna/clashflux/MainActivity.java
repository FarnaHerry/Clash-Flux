package dev.farna.clashflux;

import android.content.ActivityNotFoundException;
import android.content.res.AssetManager;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;

import org.huxerui.HuxerUIActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

public final class MainActivity extends HuxerUIActivity {
    private static final String TAG = "ClashFlux";
    private static final String MIHOMO_VERSION = "v1.19.30";

    static {
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }

    private static MainActivity current;

    private static native void nativeInit();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        current = this;
        try {
            ensureMihomoBinary();
        } catch (IOException | RuntimeException error) {
            // Keep the UI available so the failure is visible in the core status
            // card and in logcat instead of turning a missing packaged asset into
            // an unrelated Activity crash.
            Log.e(TAG, "Unable to extract the bundled mihomo Android engine", error);
        }
        super.onCreate(savedInstanceState);
        // HuxerUI must finish creating its Activity/View first. The native bridge
        // now only retains the VM/class for URL callbacks; application directories
        // are obtained from HuxerUI's ApplicationHandle inside the native runtime.
        nativeInit();
    }

    private void ensureMihomoBinary() throws IOException {
        String abi = Build.SUPPORTED_ABIS.length == 0 ? null : Build.SUPPORTED_ABIS[0];
        if (abi == null || !(abi.equals("arm64-v8a") || abi.equals("x86_64") ||
                abi.equals("armeabi-v7a") || abi.equals("x86"))) {
            throw new IOException("Unsupported Android ABI: " + abi);
        }

        File dataRoot = new File(getFilesDir(), "clash-flux");
        File engineDirectory = new File(dataRoot, "engines");
        if (!engineDirectory.isDirectory() && !engineDirectory.mkdirs()) {
            throw new IOException("Unable to create engine directory: " + engineDirectory);
        }

        File binary = new File(engineDirectory, "mihomo");
        File marker = new File(engineDirectory, "mihomo.version");
        if (binary.isFile() && binary.canExecute() && MIHOMO_VERSION.equals(readMarker(marker))) {
            Log.i(TAG, "Bundled mihomo ready (cached): " + binary.getAbsolutePath());
            return;
        }

        String assetPath = "engines/" + abi + "/mihomo";
        File temporary = new File(engineDirectory, "mihomo.tmp");
        AssetManager assets = getAssets();
        try (InputStream input = assets.open(assetPath, AssetManager.ACCESS_STREAMING);
             OutputStream output = new FileOutputStream(temporary, false)) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                output.write(buffer, 0, count);
            }
            output.flush();
        }

        if (!temporary.setExecutable(true, false)) {
            temporary.delete();
            throw new IOException("Unable to mark mihomo executable: " + temporary);
        }
        if (binary.exists() && !binary.delete()) {
            temporary.delete();
            throw new IOException("Unable to replace old mihomo engine: " + binary);
        }
        if (!temporary.renameTo(binary)) {
            temporary.delete();
            throw new IOException("Unable to install mihomo engine: " + binary);
        }
        try (FileOutputStream output = new FileOutputStream(marker, false)) {
            output.write(MIHOMO_VERSION.getBytes(StandardCharsets.UTF_8));
        }
        if (!binary.setExecutable(true, false)) {
            throw new IOException("Installed mihomo engine is not executable: " + binary);
        }
        Log.i(TAG, "Bundled mihomo ready (extracted): " + binary.getAbsolutePath());
    }

    private static String readMarker(File marker) throws IOException {
        if (!marker.isFile()) {
            return "";
        }
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(marker), StandardCharsets.UTF_8))) {
            return reader.readLine();
        }
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
}
