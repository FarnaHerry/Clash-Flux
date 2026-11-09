package dev.farna.clashflux;

import android.app.Application;

// HuxerUI 0.4 links the framework statically into the application library, so
// it must be loaded before any HuxerUI class is initialized. Loading it from
// Application also covers the :background process (VpnService/RuntimeControl).
public final class MainApplication extends Application {
    static {
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }
}
