package dev.farna.clashflux;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Collections;
import java.util.Set;

/** Platform-independent checks shared by APK verification and the read-only provider. */
final class ApkUpdatePolicy {
    static final long MAX_BYTES = 512L * 1024 * 1024;
    static final String RELEASE_CERTIFICATE =
            "a4a0bc09a3dc76d7ac37f92043d2be6e153bda1347ce344013828e15472419f4";

    static File resolve(File cache, String path, boolean partialAllowed) throws IOException {
        File root = new File(cache, "app-updates").getCanonicalFile();
        File file = new File(path).getCanonicalFile();
        File directory = file.getParentFile();
        if (directory == null || !root.equals(directory.getParentFile())
                || !directory.getName().matches("pending-[0-9]+-[0-9]+")
                || !("package.apk".equals(file.getName())
                     || (partialAllowed && "package.part.apk".equals(file.getName())))
                || !file.isFile()) {
            throw new IOException("APK is outside the private update cache");
        }
        return file;
    }

    static File verify(File cache, String path, long expectedSize, String expectedHash) throws Exception {
        if (expectedSize <= 0 || expectedSize > MAX_BYTES || expectedHash == null
                || !expectedHash.matches("[0-9a-f]{64}")) {
            throw new IOException("Invalid APK size or SHA-256 metadata");
        }
        File file = resolve(cache, path, true);
        if (file.length() != expectedSize) throw new IOException("APK size mismatch");
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        long bytes = 0;
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[64 * 1024];
            for (int count; (count = input.read(buffer)) != -1;) {
                if (Thread.currentThread().isInterrupted()) throw new IOException("APK verification cancelled");
                bytes += count;
                if (bytes > expectedSize) throw new IOException("APK exceeds its declared size");
                digest.update(buffer, 0, count);
            }
        }
        if (bytes != expectedSize || !hex(digest.digest()).equals(expectedHash))
            throw new IOException("APK size or SHA-256 mismatch");
        return file;
    }

    static void checkIdentity(String installedId, long installedCode, Set<String> installedSigners,
                              String candidateId, long candidateCode, String candidateVersion,
                              Set<String> candidateSigners, String expectedVersion) throws IOException {
        if (!"dev.farna.clashflux".equals(installedId) || !installedId.equals(candidateId))
            throw new IOException("APK package name does not match Clash-Flux");
        if (installedCode < 0 || candidateCode <= installedCode)
            throw new IOException("APK versionCode must be newer than the installed application");
        if (expectedVersion == null || !expectedVersion.matches("[0-9]+\\.[0-9]+\\.[0-9]+")
                || !expectedVersion.equals(candidateVersion))
            throw new IOException("APK version does not match the selected release");
        Set<String> expected = Collections.singleton(RELEASE_CERTIFICATE);
        if (!expected.equals(installedSigners) || !expected.equals(candidateSigners))
            throw new IOException("APK must use the same official signing certificate as the installed application");
    }

    static String certificate(byte[] encoded) throws NoSuchAlgorithmException {
        return hex(MessageDigest.getInstance("SHA-256").digest(encoded));
    }

    private static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            result.append("0123456789abcdef".charAt((value & 255) >>> 4));
            result.append("0123456789abcdef".charAt(value & 15));
        }
        return result.toString();
    }
}
