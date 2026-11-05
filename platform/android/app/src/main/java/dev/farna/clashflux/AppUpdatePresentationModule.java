package dev.farna.clashflux;

import android.app.Activity;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import org.huxerui.HuxerUIActivityResultLauncher;
import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.HuxerUIView;
import org.huxerui.PlatformPayload;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Set;

/** Captures one original window's result queue; never looks up a replacement Activity. */
public final class AppUpdatePresentationModule implements HuxerUIPlatformModule.WindowFactory {
    @Override public HuxerUIPlatformModule create(HuxerUIView window, PlatformPayload options,
                                                 HuxerUIPlatformChannel.Events events) {
        return new Presentation(window.getContext().getApplicationContext(), window.getActivityResultLauncher());
    }

    private static final class Presentation implements HuxerUIPlatformModule {
        private final Context context;
        private final HuxerUIActivityResultLauncher launcher;
        private final Set<Operation> operations = new HashSet<>();
        private boolean disposed;

        Presentation(Context context, HuxerUIActivityResultLauncher launcher) {
            this.context = context;
            this.launcher = launcher;
        }

        private final class Operation implements HuxerUIPlatformChannel.Cancellation {
            final HuxerUIPlatformChannel.Result result;
            HuxerUIPlatformChannel.Cancellation launch;
            boolean done;

            Operation(HuxerUIPlatformChannel.Result result) { this.result = result; operations.add(this); }
            void complete(boolean value) {
                if (done) return;
                done = true; operations.remove(this); launch = null;
                result.complete(PlatformPayload.booleanValue(value));
            }
            void fail(Exception error) {
                if (done) return;
                done = true; operations.remove(this); launch = null;
                result.fail("update/presentation-failed", error.getMessage() == null
                        ? "Android update presentation failed" : error.getMessage(), PlatformPayload.nullValue());
            }
            @Override public void cancel() {
                if (done) return;
                done = true; operations.remove(this);
                HuxerUIPlatformChannel.Cancellation request = launch;
                launch = null;
                result.close();
                if (request != null) request.cancel();
            }
        }

        private boolean allowed() {
            return Build.VERSION.SDK_INT < 26 || context.getPackageManager().canRequestPackageInstalls();
        }

        @Override @SuppressWarnings("deprecation")
        public HuxerUIPlatformChannel.Cancellation invoke(String method, PlatformPayload arguments,
                                                         HuxerUIPlatformChannel.Result result) {
            Operation operation = new Operation(result);
            try {
                if (disposed) throw new IOException("Android update window is closed");
                final boolean permission = "permission".equals(method);
                final Intent intent;
                if (permission) {
                    if (allowed()) { operation.complete(true); return operation; }
                    intent = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                            Uri.parse("package:" + context.getPackageName()));
                } else if ("install".equals(method)) {
                    if (!allowed()) throw new IOException("Allow installation from Clash-Flux before retrying");
                    // The application verifier rechecks the APK before this invocation. The provider
                    // independently enforces its private read-only path and temporary grant boundary.
                    Uri uri = UpdateApkProvider.uri(context, new File(arguments.requireString()));
                    intent = new Intent(Intent.ACTION_INSTALL_PACKAGE).setDataAndType(uri, UpdateApkProvider.MIME)
                            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                            .putExtra(Intent.EXTRA_RETURN_RESULT, true);
                    intent.setClipData(ClipData.newRawUri("Clash-Flux update", uri));
                } else throw new IOException("Unknown Android update presentation");
                operation.launch = launcher.launch(intent, new HuxerUIActivityResultLauncher.Callback() {
                    @Override public void onResult(int code, Intent data) {
                        try {
                            if (permission) operation.complete(allowed());
                            else if (code == Activity.RESULT_OK) operation.complete(true);
                            else if (code == Activity.RESULT_CANCELED) operation.complete(false);
                            else operation.fail(new IOException("System installer failed with result " + code));
                        } catch (RuntimeException error) { operation.fail(error); }
                    }
                    @Override public void onFailure(RuntimeException error) { operation.fail(error); }
                });
            } catch (RuntimeException | IOException error) { operation.fail(error); }
            return operation;
        }

        @Override public void dispose() {
            if (disposed) return;
            disposed = true;
            for (Operation operation : new ArrayList<>(operations)) operation.cancel();
        }
    }
}
