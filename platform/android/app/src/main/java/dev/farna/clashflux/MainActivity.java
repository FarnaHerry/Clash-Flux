package dev.farna.clashflux;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;

import org.huxerui.HuxerUIActivity;

public final class MainActivity extends HuxerUIActivity {
    static {
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }

    private static MainActivity current;

    private static native void nativeInit(String filesDirectory);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        current = this;
        nativeInit(getFilesDir().getAbsolutePath());
        super.onCreate(savedInstanceState);
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
