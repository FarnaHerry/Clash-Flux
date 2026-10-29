package dev.farna.clashflux;

import android.app.Activity;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import org.huxerui.HuxerUIPlatformChannel;
import org.huxerui.HuxerUIPlatformModule;
import org.huxerui.PlatformPayload;
import java.io.File;
import java.io.IOException;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/** Android-only verification and user-confirmed installation; HTTP stays in HuxerUI. */
public final class AppUpdateModule implements HuxerUIPlatformModule.Factory {
    @Override public HuxerUIPlatformModule create(Context context, PlatformPayload options,
                                                 HuxerUIPlatformChannel.Events events) {
        return new Installer(context.getApplicationContext());
    }

    private static final class Installer implements HuxerUIPlatformModule {
        private final Context context;
        private final Handler main = new Handler(Looper.getMainLooper());
        private final ExecutorService worker = Executors.newSingleThreadExecutor();
        private final AtomicBoolean disposed = new AtomicBoolean();
        private final Set<Operation> operations = ConcurrentHashMap.newKeySet();

        Installer(Context context) { this.context = context; }

        private final class Operation {
            final HuxerUIPlatformChannel.Result result;
            final AtomicBoolean done = new AtomicBoolean();
            Operation(HuxerUIPlatformChannel.Result result) { this.result = result; operations.add(this); }
            boolean active() { return !disposed.get() && !done.get(); }
            void complete(PlatformPayload value) {
                if (done.compareAndSet(false, true)) { operations.remove(this); result.complete(value); }
            }
            void fail(Throwable error) {
                if (done.compareAndSet(false, true)) {
                    operations.remove(this);
                    result.fail("update/failed", error.getMessage() == null ? "Android update failed" : error.getMessage(),
                            PlatformPayload.nullValue());
                }
            }
            void cancel() {
                if (done.compareAndSet(false, true)) {
                    operations.remove(this);
                    result.close();
                    main.post(() -> {
                        MainActivity activity = MainActivity.updateActivity();
                        if (activity != null) activity.cancelUpdateRequest(this);
                    });
                }
            }
        }

        @Override public HuxerUIPlatformChannel.Cancellation invoke(
                String method, PlatformPayload arguments, HuxerUIPlatformChannel.Result result) {
            Operation operation = new Operation(result);
            if (disposed.get()) { operation.fail(new IOException("Android updater is closed")); return null; }
            try {
                if ("permission".equals(method)) {
                    main.post(() -> requestPermission(operation));
                } else if ("verify".equals(method) || "install".equals(method)) {
                    // Decode owning metadata before dispatching any blocking inspection.
                    String path = arguments.requireField("path").requireString();
                    long size = arguments.requireField("size").requireInt64();
                    String hash = arguments.requireField("sha256").requireString();
                    String version = arguments.requireField("version").requireString();
                    worker.execute(() -> {
                        if (!operation.active()) return;
                        try {
                            File apk = verify(path, size, hash, version);
                            if (!operation.active()) return;
                            if ("verify".equals(method)) operation.complete(PlatformPayload.nullValue());
                            else main.post(() -> launchInstaller(operation, apk));
                        } catch (Exception error) { operation.fail(error); }
                    });
                } else operation.fail(new IOException("Unknown Android update operation"));
            } catch (RuntimeException error) { operation.fail(error); }
            return operation::cancel;
        }

        private boolean allowed() {
            return Build.VERSION.SDK_INT < 26 || context.getPackageManager().canRequestPackageInstalls();
        }

        private void requestPermission(Operation operation) {
            if (!operation.active()) return;
            try {
                if (allowed()) { operation.complete(PlatformPayload.booleanValue(true)); return; }
                MainActivity activity = requireActivity();
                Intent intent = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                        Uri.parse("package:" + context.getPackageName()));
                activity.beginUpdateRequest(MainActivity.REQUEST_UPDATE_SOURCE, operation, intent,
                        ignored -> operation.complete(PlatformPayload.booleanValue(allowed())));
            } catch (RuntimeException | IOException error) { operation.fail(error); }
        }

        @SuppressWarnings("deprecation")
        private void launchInstaller(Operation operation, File apk) {
            if (!operation.active()) return;
            try {
                if (!allowed()) throw new IOException("Allow installation from Clash-Flux before retrying");
                MainActivity activity = requireActivity();
                Uri uri = UpdateApkProvider.uri(context, apk);
                // The system owns installation and process replacement. Keep the
                // result callback so cancellation restores the same live session.
                Intent install = new Intent(Intent.ACTION_INSTALL_PACKAGE).setDataAndType(uri, UpdateApkProvider.MIME)
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        .putExtra(Intent.EXTRA_RETURN_RESULT, true);
                install.setClipData(ClipData.newRawUri("Clash-Flux update", uri));
                activity.beginUpdateRequest(MainActivity.REQUEST_UPDATE_INSTALL, operation, install,
                        code -> {
                            if (code == Activity.RESULT_OK) operation.complete(PlatformPayload.booleanValue(true));
                            else if (code == Activity.RESULT_CANCELED) operation.complete(PlatformPayload.booleanValue(false));
                            else operation.fail(new IOException("System installer failed with result " + code));
                        });
            } catch (RuntimeException | IOException error) { operation.fail(error); }
        }

        private MainActivity requireActivity() throws IOException {
            MainActivity activity = MainActivity.updateActivity();
            if (activity == null) throw new IOException("Return to Clash-Flux before installing the update");
            return activity;
        }

        @SuppressWarnings("deprecation")
        private File verify(String path, long size, String hash, String version) throws Exception {
            File apk = ApkUpdatePolicy.verify(context.getCacheDir(), path, size, hash);
            PackageManager packages = context.getPackageManager();
            int flags = Build.VERSION.SDK_INT >= 28 ? PackageManager.GET_SIGNING_CERTIFICATES : PackageManager.GET_SIGNATURES;
            PackageInfo current = packages.getPackageInfo(context.getPackageName(), flags);
            PackageInfo candidate = packages.getPackageArchiveInfo(apk.getPath(), flags);
            if (candidate == null) throw new IOException("APK metadata or signing certificate could not be read");
            if (candidate.applicationInfo != null && candidate.applicationInfo.minSdkVersion > Build.VERSION.SDK_INT)
                throw new IOException("This update requires a newer Android version");
            long currentCode = Build.VERSION.SDK_INT >= 28 ? current.getLongVersionCode() : current.versionCode;
            long candidateCode = Build.VERSION.SDK_INT >= 28 ? candidate.getLongVersionCode() : candidate.versionCode;
            ApkUpdatePolicy.checkIdentity(current.packageName, currentCode, signers(current),
                    candidate.packageName, candidateCode, candidate.versionName, signers(candidate), version);
            return apk;
        }

        @SuppressWarnings("deprecation")
        private Set<String> signers(PackageInfo info) throws Exception {
            Signature[] certificates = Build.VERSION.SDK_INT >= 28 && info.signingInfo != null
                    ? info.signingInfo.getApkContentsSigners() : info.signatures;
            Set<String> result = new HashSet<>();
            if (certificates != null) for (Signature certificate : certificates)
                result.add(ApkUpdatePolicy.certificate(certificate.toByteArray()));
            return result;
        }

        @Override public void dispose() {
            if (disposed.compareAndSet(false, true)) {
                for (Operation operation : operations) operation.cancel();
                worker.shutdownNow();
            }
        }
    }
}
