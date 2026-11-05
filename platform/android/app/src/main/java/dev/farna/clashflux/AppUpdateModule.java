package dev.farna.clashflux;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Build;
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

/** Application-scoped APK inspection; presentation belongs to AppUpdatePresentationModule. */
public final class AppUpdateModule implements HuxerUIPlatformModule.Factory {
    @Override public HuxerUIPlatformModule create(Context context, PlatformPayload options,
                                                 HuxerUIPlatformChannel.Events events) {
        return new Verifier(context.getApplicationContext());
    }

    private static final class Verifier implements HuxerUIPlatformModule {
        private final Context context;
        private final ExecutorService worker = Executors.newSingleThreadExecutor();
        private final AtomicBoolean disposed = new AtomicBoolean();
        private final Set<Operation> operations = ConcurrentHashMap.newKeySet();

        Verifier(Context context) { this.context = context; }

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
                }
            }
        }

        @Override public HuxerUIPlatformChannel.Cancellation invoke(
                String method, PlatformPayload arguments, HuxerUIPlatformChannel.Result result) {
            Operation operation = new Operation(result);
            if (disposed.get()) { operation.fail(new IOException("Android updater is closed")); return null; }
            try {
                if ("verify".equals(method)) {
                    // Decode owning metadata before dispatching any blocking inspection.
                    String path = arguments.requireField("path").requireString();
                    long size = arguments.requireField("size").requireInt64();
                    String hash = arguments.requireField("sha256").requireString();
                    String version = arguments.requireField("version").requireString();
                    worker.execute(() -> {
                        if (!operation.active()) return;
                        try {
                            verify(path, size, hash, version);
                            if (!operation.active()) return;
                            operation.complete(PlatformPayload.nullValue());
                        } catch (Exception error) { operation.fail(error); }
                    });
                } else operation.fail(new IOException("Unknown Android update operation"));
            } catch (RuntimeException error) { operation.fail(error); }
            return operation::cancel;
        }

        @SuppressWarnings("deprecation")
        private void verify(String path, long size, String hash, String version) throws Exception {
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
