package dev.farna.clashflux;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.Set;
import java.util.stream.Stream;

public final class ApkUpdatePolicyTest {
    private static final String HASH = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    private static final Set<String> SIGNER = Collections.singleton(ApkUpdatePolicy.RELEASE_CERTIFICATE);
    private interface Checked { void run() throws Exception; }
    private static void require(boolean condition) { if (!condition) throw new AssertionError("APK update policy"); }
    private static void refused(Checked operation) throws Exception {
        try { operation.run(); } catch (java.io.IOException expected) { return; }
        throw new AssertionError("Unsafe APK update was accepted");
    }
    private static void identity(String id, long code, String version, Set<String> signer) throws Exception {
        ApkUpdatePolicy.checkIdentity("dev.farna.clashflux", 154, SIGNER, id, code, version, signer, "0.3.77");
    }

    public static void main(String[] args) throws Exception {
        identity("dev.farna.clashflux", 155, "0.3.77", SIGNER);
        refused(() -> identity("another.application", 155, "0.3.77", SIGNER));
        refused(() -> identity("dev.farna.clashflux", 154, "0.3.77", SIGNER));
        refused(() -> identity("dev.farna.clashflux", 153, "0.3.77", SIGNER));
        refused(() -> identity("dev.farna.clashflux", 155, "0.3.76", SIGNER));
        refused(() -> identity("dev.farna.clashflux", 155, "0.3.77", Collections.emptySet()));
        refused(() -> identity("dev.farna.clashflux", 155, "0.3.77", Collections.singleton("debug")));
        refused(() -> identity("dev.farna.clashflux", 155, "0.3.77", new HashSet<>(Arrays.asList(
                ApkUpdatePolicy.RELEASE_CERTIFICATE, "unexpected-second-signer"))));
        refused(() -> ApkUpdatePolicy.checkIdentity("dev.farna.clashflux", 154, Collections.singleton("debug"),
                "dev.farna.clashflux", 155, "0.3.77", SIGNER, "0.3.77"));
        Path root = Files.createTempDirectory("clashflux-apk-policy-");
        try {
            File cache = Files.createDirectories(root.resolve("cache")).toFile();
            Path stage = Files.createDirectories(cache.toPath().resolve("app-updates/pending-1-2"));
            Path partial = stage.resolve("package.part.apk");
            Files.write(partial, "abc".getBytes(StandardCharsets.UTF_8));
            require(ApkUpdatePolicy.verify(cache, partial.toString(), 3, HASH).equals(partial.toFile().getCanonicalFile()));
            refused(() -> ApkUpdatePolicy.resolve(cache, partial.toString(), false));
            refused(() -> ApkUpdatePolicy.verify(cache, partial.toString(), 2, HASH));
            refused(() -> ApkUpdatePolicy.verify(cache, partial.toString(), 4, HASH));
            refused(() -> ApkUpdatePolicy.verify(cache, partial.toString(), 3, HASH.toUpperCase(java.util.Locale.ROOT)));
            refused(() -> ApkUpdatePolicy.verify(cache, partial.toString(), ApkUpdatePolicy.MAX_BYTES + 1, HASH));
            Files.write(partial, "abd".getBytes(StandardCharsets.UTF_8));
            refused(() -> ApkUpdatePolicy.verify(cache, partial.toString(), 3, HASH));
            Path outside = root.resolve("package.apk");
            Files.write(outside, "user profile".getBytes(StandardCharsets.UTF_8));
            refused(() -> ApkUpdatePolicy.resolve(cache, outside.toString(), false));
            refused(() -> ApkUpdatePolicy.resolve(cache, stage.resolve("../../../package.apk").toString(), false));
            Path finalApk = stage.resolve("package.apk");
            Files.move(partial, finalApk);
            require(ApkUpdatePolicy.resolve(cache, finalApk.toString(), false).equals(finalApk.toFile().getCanonicalFile()));
            require(new String(Files.readAllBytes(outside), StandardCharsets.UTF_8).equals("user profile"));
        } finally {
            // Only the exclusively created test tree; walk does not follow links.
            try (Stream<Path> files = Files.walk(root)) {
                Path[] paths = files.sorted(Comparator.reverseOrder()).toArray(Path[]::new);
                for (Path path : paths) Files.delete(path);
            }
        }
        System.out.println("ApkUpdatePolicyTest: identity, signer, version, SHA-256 and private-cache boundaries passed");
    }
}
