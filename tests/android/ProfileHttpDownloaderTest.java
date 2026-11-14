package dev.farna.clashflux;

import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

final class ProfileHttpDownloaderTest {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    static void run() throws Exception {
        Path root = Files.createTempDirectory("clashflux-http-");
        Path staging = root.resolve("exclusive-staging");
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        server.createContext("/", exchange -> {
            String path = exchange.getRequestURI().getPath();
            byte[] body = ("/large".equals(path) ? new String(new char[128 * 1024]).replace('\0', 'a') : "source body")
                    .getBytes(StandardCharsets.UTF_8);
            if ("/redirect".equals(path)) {
                exchange.getResponseHeaders().set("Location", "/good");
                exchange.sendResponseHeaders(302, -1);
            } else if ("/conditional".equals(path) && "test-etag".equals(exchange.getRequestHeaders().getFirst("If-None-Match"))) {
                exchange.sendResponseHeaders(304, -1);
            } else {
                int status = "/identity".equals(path) && !"clash.meta".equals(exchange.getRequestHeaders().getFirst("User-Agent")) ? 403 : 200;
                exchange.getResponseHeaders().set("ETag", "test-etag");
                exchange.sendResponseHeaders(status, body.length + ("/short".equals(path) ? 1 : 0));
                exchange.getResponseBody().write(body);
            }
            exchange.close();
        });
        server.start();
        String base = "http://127.0.0.1:" + server.getAddress().getPort();
        try {
            ProfileHttpDownloader.Result good = ProfileHttpDownloader.fetch(base + "/large", 3, 256 * 1024,
                    true, new String[0], false, staging.toString());
            require(good.error.isEmpty() && good.bytes == 128 * 1024 && Files.size(staging) == good.bytes,
                    "large resources stream completely into the exclusive staging file");
            byte[] accepted = Files.readAllBytes(staging);
            ProfileHttpDownloader.Result conditional = ProfileHttpDownloader.fetch(base + "/conditional", 3, 1024,
                    true, new String[]{"If-None-Match", "test-etag"}, true, staging.toString());
            require(conditional.status == 304 && conditional.bytes == 0 && Arrays.equals(accepted, Files.readAllBytes(staging)),
                    "304 must not truncate a source or create a representation body");
            ProfileHttpDownloader.Result oversized = ProfileHttpDownloader.fetch(base + "/large", 3, 32,
                    true, new String[0], false, staging.toString());
            require(!oversized.error.isEmpty() && Files.size(staging) <= 32 && !oversized.retryable,
                    "decoded size limit stops writes and is not retried");
            ProfileHttpDownloader.Result shortBody = ProfileHttpDownloader.fetch(base + "/short", 3, 1024,
                    true, new String[0], false, staging.toString());
            require(!shortBody.error.isEmpty() && shortBody.retryable, "truncated bodies remain explicit retryable failures");
            ProfileHttpDownloader.Result identity = ProfileHttpDownloader.fetch(base + "/identity", 3, 1024,
                    true, new String[0], true, staging.toString());
            require(identity.status == 200 && identity.attempts == 2 && Files.size(staging) == "source body".length(),
                    "bounded subscriptions retry identity and truncate only their own staging file");
            ProfileHttpDownloader.Result resource = ProfileHttpDownloader.fetch(base + "/identity", 3, 1024,
                    true, new String[0], false, staging.toString());
            require(resource.status == 403 && resource.attempts == 1, "providers never change their user agent implicitly");
            ProfileHttpDownloader.Result redirect = ProfileHttpDownloader.fetch(base + "/redirect", 3, 1024,
                    true, new String[]{"Authorization", "test-secret"}, false, staging.toString());
            require(redirect.status == 302 && !redirect.error.isEmpty(), "credential-bearing redirects remain refused");
            require(ProfileHttpDownloader.fetch(base + "/redirect", 3, 1024,
                    true, new String[0], false, staging.toString()).status == 200, "plain redirects retain existing behavior");
            verifyUntrustedTls(staging);
        } finally {
            server.stop(0); Files.deleteIfExists(staging); Files.deleteIfExists(root);
        }
        System.out.println("ProfileHttpDownloaderTest: file streaming, bounds, truncation, 304, identity and redirects passed");
    }
    private static void verifyUntrustedTls(Path staging) throws Exception {
        Path keys = Files.createTempDirectory("clashflux-tls-");
        Path keystore = keys.resolve("server.p12"), toolLog = keys.resolve("keytool.log");
        String tool = java.nio.file.Paths.get(System.getProperty("java.home"), "bin",
                System.getProperty("os.name").startsWith("Windows") ? "keytool.exe" : "keytool").toString();
        com.sun.net.httpserver.HttpsServer server = null;
        try {
            Process generator = new ProcessBuilder(tool, "-genkeypair", "-alias", "loopback", "-keyalg", "RSA",
                    "-keysize", "2048", "-validity", "1", "-dname", "CN=localhost", "-storetype", "PKCS12",
                    "-keystore", keystore.toString(), "-storepass", "fixture-password", "-keypass", "fixture-password")
                    .redirectErrorStream(true).redirectOutput(toolLog.toFile()).start();
            if (!generator.waitFor(15, java.util.concurrent.TimeUnit.SECONDS)) {
                generator.destroyForcibly(); throw new AssertionError("TLS fixture key generation timed out");
            }
            require(generator.exitValue() == 0, "TLS fixture key generation failed");
            java.security.KeyStore keyStore = java.security.KeyStore.getInstance("PKCS12");
            try (java.io.InputStream input = Files.newInputStream(keystore)) {
                keyStore.load(input, "fixture-password".toCharArray());
            }
            javax.net.ssl.KeyManagerFactory managers = javax.net.ssl.KeyManagerFactory.getInstance(
                    javax.net.ssl.KeyManagerFactory.getDefaultAlgorithm());
            managers.init(keyStore, "fixture-password".toCharArray());
            javax.net.ssl.SSLContext context = javax.net.ssl.SSLContext.getInstance("TLS");
            context.init(managers.getKeyManagers(), null, null);
            server = com.sun.net.httpserver.HttpsServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
            server.setHttpsConfigurator(new com.sun.net.httpserver.HttpsConfigurator(context));
            java.util.concurrent.atomic.AtomicInteger payloads = new java.util.concurrent.atomic.AtomicInteger();
            server.createContext("/", exchange -> { payloads.incrementAndGet(); exchange.sendResponseHeaders(200, -1); exchange.close(); });
            server.start();
            byte[] previous = Files.readAllBytes(staging);
            ProfileHttpDownloader.Result rejected = ProfileHttpDownloader.fetch(
                    "https://127.0.0.1:" + server.getAddress().getPort() + "/", 3, 1024,
                    true, new String[0], false, staging.toString());
            require(!rejected.error.isEmpty() && "tls".equals(rejected.failure) && payloads.get() == 0 &&
                    Arrays.equals(previous, Files.readAllBytes(staging)),
                    "untrusted TLS must reject before HTTP payload or file replacement using unchanged system trust");
        } finally {
            if (server != null) server.stop(0);
            Files.deleteIfExists(keystore); Files.deleteIfExists(toolLog); Files.deleteIfExists(keys);
        }
    }

}
