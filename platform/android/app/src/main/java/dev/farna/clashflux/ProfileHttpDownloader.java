package dev.farna.clashflux;

import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLConnection;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.TimeUnit;

// Worker-only file transport, shared by subscriptions and bounded resources.
// No Android/UI dependency: the same implementation is exercised on the host JVM.
final class ProfileHttpDownloader {
    static final class Result {
        int status;
        int attempts = 1;
        long bytes;
        boolean retryable;
        String error = "", failure = "none";
        Map<String, String> headers = new java.util.TreeMap<>();
    }

    static Result fetch(String address, int timeoutSeconds,
            long maxBytes, boolean forceDirect, String[] requestHeaders,
            boolean subscriptionRequest, String destination) {
        Result result = fetchProfileWithUserAgent(address, timeoutSeconds,
                maxBytes, forceDirect, requestHeaders, destination,
                ProfileHttpPolicy.DEFAULT_USER_AGENT);
        if (ProfileHttpPolicy.shouldRetryWithClashMeta(
                result.status, subscriptionRequest, requestHeaders)) {
            Result retried = fetchProfileWithUserAgent(address, timeoutSeconds,
                    maxBytes, forceDirect, requestHeaders, destination,
                    ProfileHttpPolicy.CLASH_META_USER_AGENT);
            retried.bytes += result.bytes;
            retried.attempts = 2;
            return retried;
        }
        return result;
    }

    private static Result fetchProfileWithUserAgent(
            String address, int timeoutSeconds,
            long maxBytes, boolean forceDirect, String[] requestHeaders,
            String destination, String userAgent) {
        Result result = new Result();
        HttpURLConnection connection = null;
        try {
            if (requestHeaders == null || requestHeaders.length % 2 != 0) {
                throw new IOException("下载请求头格式无效");
            }
            URL target = new URL(address);
            int timeoutMillis = (int) Math.min(Integer.MAX_VALUE,
                    Math.max(1L, (long) timeoutSeconds) * 1000L);
            long deadlineNanos = System.nanoTime()
                    + TimeUnit.MILLISECONDS.toNanos(timeoutMillis);
            for (int redirects = 0; ; redirects++) {
                String protocol = target.getProtocol().toLowerCase(Locale.ROOT);
                if (!"http".equals(protocol) && !"https".equals(protocol)) {
                    throw new IOException("订阅下载仅支持 HTTP 或 HTTPS URL");
                }
                URLConnection opened = forceDirect ? target.openConnection(java.net.Proxy.NO_PROXY)
                                                  : target.openConnection();
                if (!(opened instanceof HttpURLConnection)) {
                    throw new IOException("订阅 URL 不是 HTTP 连接");
                }
                connection = (HttpURLConnection) opened;
                connection.setInstanceFollowRedirects(false);
                connection.setUseCaches(false);
                connection.setRequestProperty("User-Agent", userAgent);
                // Android negotiates gzip and transparently decodes the input stream.
                // Its decoded response removes Content-Length/Content-Encoding;
                // the existing maxBytes check therefore bounds decoded bytes.
                // Compiler/JNI validate the common subset before this call.
                for (int i = 0; i < requestHeaders.length; i += 2) {
                    connection.setRequestProperty(requestHeaders[i], requestHeaders[i + 1]);
                }
                int remainingMillis = remainingTimeoutMillis(deadlineNanos);
                connection.setConnectTimeout(remainingMillis);
                connection.setReadTimeout(remainingMillis);
                int status = connection.getResponseCode();
                result.status = status;
                String location = connection.getHeaderField("Location");
                if (isRedirect(status) && location != null && !location.isEmpty()) {
                    if (requestHeaders.length != 0) {
                        throw new IOException("带请求头的规则集下载不支持重定向");
                    }
                    if (redirects >= 10) {
                        throw new IOException("订阅下载重定向次数过多");
                    }
                    URL redirected = new URL(target, location);
                    String redirectedProtocol =
                            redirected.getProtocol().toLowerCase(Locale.ROOT);
                    if (!"http".equals(redirectedProtocol)
                            && !"https".equals(redirectedProtocol)) {
                        throw new IOException("订阅重定向仅支持 HTTP 或 HTTPS URL");
                    }
                    connection.disconnect();
                    connection = null;
                    target = redirected;
                    continue;
                }

                result.status = status;
                Map<String, String> headers = new java.util.TreeMap<>();
                for (Map.Entry<String, List<String>> entry
                        : connection.getHeaderFields().entrySet()) {
                    String name = entry.getKey();
                    List<String> values = entry.getValue();
                    if (name == null || values == null || values.isEmpty()) continue;
                    StringBuilder value = new StringBuilder();
                    for (String part : values) {
                        if (part == null) continue;
                        if (value.length() > 0) value.append(", ");
                        value.append(part);
                    }
                    headers.put(name.toLowerCase(Locale.ROOT), value.toString());
                }
                result.headers = headers;

                if (status == HttpURLConnection.HTTP_NOT_MODIFIED) {
                    result.error = "";
                    return result;
                }

                long expectedBodyLength = connection.getContentLengthLong();
                long receivedBytes = 0;
                connection.setReadTimeout(remainingTimeoutMillis(deadlineNanos));
                InputStream input = status >= 400
                        ? connection.getErrorStream() : connection.getInputStream();
                if (input != null) {
                    try (InputStream stream = input; FileOutputStream output = new FileOutputStream(destination)) {
                        byte[] buffer = new byte[16 * 1024];
                        while (true) {
                            int count;
                            try {
                                connection.setReadTimeout(
                                        remainingTimeoutMillis(deadlineNanos));
                                count = stream.read(buffer);
                            } catch (IOException readError) {
                                result.retryable = true;
                                throw readError;
                            }
                            if (count == -1) break;
                            if (maxBytes > 0 && count > maxBytes - receivedBytes) {
                                result.failure = "limit";
                                throw new IOException("下载超过大小上限");
                            }
                            output.write(buffer, 0, count);
                            receivedBytes += count;
                            result.bytes = receivedBytes;
                        }
                    }
                }
                if (expectedBodyLength >= 0 && receivedBytes != expectedBodyLength) {
                    result.retryable = true;
                    throw new IOException("下载不完整（收到 " + receivedBytes
                            + "/" + expectedBodyLength + " 字节）");
                }
                result.error = "";
                return result;
            }
        } catch (Exception error) {
            String message = error.getMessage();
            result.error = message == null || message.isEmpty() ? error.getClass().getSimpleName() : message;
            if ("none".equals(result.failure)) result.failure = error instanceof javax.net.ssl.SSLException ? "tls" :
                    error instanceof java.net.SocketTimeoutException ? "timeout" : "transport";
            return result;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }

    private static int remainingTimeoutMillis(long deadlineNanos)
            throws java.net.SocketTimeoutException {
        long remainingNanos = deadlineNanos - System.nanoTime();
        if (remainingNanos <= 0) {
            throw new java.net.SocketTimeoutException("订阅下载超时");
        }
        long millis = TimeUnit.NANOSECONDS.toMillis(remainingNanos);
        return (int) Math.max(1L, Math.min(Integer.MAX_VALUE, millis));
    }

    private static boolean isRedirect(int status) {
        return status == HttpURLConnection.HTTP_MOVED_PERM
                || status == HttpURLConnection.HTTP_MOVED_TEMP
                || status == HttpURLConnection.HTTP_SEE_OTHER
                || status == 307 || status == 308;
    }

}
