package dev.farna.clashflux;

import org.json.JSONArray;
import org.json.JSONObject;
import org.snakeyaml.engine.v2.api.Load;
import org.snakeyaml.engine.v2.api.LoadSettings;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;
import java.util.Map;

/** Compiles the Clash subscription format persisted by the shared UI into the
 * sing-box JSON consumed by libbox on Android. Desktop keeps the YAML intact. */
final class SingBoxConfig {
    private SingBoxConfig() { }

    static String compile(File source, String mode) throws Exception {
        String raw = new String(Files.readAllBytes(source.toPath()), StandardCharsets.UTF_8).trim();
        if (raw.startsWith("{")) {
            // A native sing-box profile remains lossless. Add the Android TUN
            // inbound only when the profile does not already declare one.
            JSONObject config = new JSONObject(raw);
            if (!config.has("inbounds")) config.put("inbounds", tunInbound());
            return config.toString();
        }
        Object document = new Load(LoadSettings.builder().setLabel(source.getName()).build())
                .loadFromString(raw);
        if (!(document instanceof Map)) throw new IllegalArgumentException("订阅不是有效 YAML/JSON 配置");
        Map<?, ?> root = (Map<?, ?>) document;
        JSONArray outbounds = new JSONArray();
        outbounds.put(new JSONObject().put("type", "direct").put("tag", "DIRECT"));
        outbounds.put(new JSONObject().put("type", "block").put("tag", "REJECT"));
        Object proxies = root.get("proxies");
        if (proxies instanceof List) for (Object entry : (List<?>) proxies) {
            if (entry instanceof Map) outbounds.put(proxy((Map<?, ?>) entry));
        }
        Object groups = root.get("proxy-groups");
        if (groups instanceof List) for (Object entry : (List<?>) groups) {
            if (entry instanceof Map) outbounds.put(group((Map<?, ?>) entry));
        }
        String fallback = selectFallback(root, outbounds);
        JSONObject route = new JSONObject().put("final", fallback);
        JSONArray rules = rules(root);
        // Preserve the three-mode UX used by the shared UI.  sing-box's
        // Clash API changes clash_mode at runtime; these first two rules
        // override the subscription rules only for Global/Direct.
        JSONArray modeRules = new JSONArray()
                .put(new JSONObject().put("clash_mode", "Direct").put("outbound", "DIRECT"))
                .put(new JSONObject().put("clash_mode", "Global").put("outbound", fallback));
        for (int index = 0; index < rules.length(); index++) modeRules.put(rules.get(index));
        rules = modeRules;
        if (rules.length() != 0) route.put("rules", rules);
        JSONObject clashApi = new JSONObject()
                .put("external_controller", text(root, "external-controller"))
                .put("default_mode", clashMode(mode))
                .put("mode_list", new JSONArray().put("Rule").put("Global").put("Direct"));
        String secret = text(root, "secret");
        if (!secret.isEmpty()) clashApi.put("secret", secret);
        return new JSONObject()
                .put("log", new JSONObject().put("level", "info"))
                .put("inbounds", tunInbound())
                .put("outbounds", outbounds)
                .put("route", route)
                .put("experimental", new JSONObject().put("clash_api", clashApi))
                .toString();
    }

    private static JSONArray tunInbound() throws Exception {
        return new JSONArray().put(new JSONObject()
                .put("type", "tun").put("tag", "tun")
                .put("address", new JSONArray().put("172.19.0.1/30"))
                .put("mtu", 1400).put("auto_route", true)
                .put("strict_route", false).put("sniff", true));
    }

    private static JSONObject proxy(Map<?, ?> item) throws Exception {
        String type = text(item, "type");
        String tag = text(item, "name");
        if (tag.isEmpty() || type.isEmpty()) throw new IllegalArgumentException("订阅包含无名称或类型的节点");
        String singboxType = normalizeType(type);
        JSONObject out = new JSONObject().put("tag", tag).put("type", singboxType);
        copy(item, out, "server", "server"); copy(item, out, "port", "server_port");
        copy(item, out, "uuid", "uuid"); copy(item, out, "password", "password");
        if ("shadowsocks".equals(singboxType)) copy(item, out, "cipher", "method");
        if ("vmess".equals(singboxType)) {
            copy(item, out, "cipher", "security");
            copy(item, out, "alterId", "alter_id");
        }
        copy(item, out, "flow", "flow");
        copy(item, out, "udp", "udp"); copy(item, out, "udp-over-tcp", "udp_over_tcp");
        transport(item, out);
        String sni = text(item, "servername"); if (sni.isEmpty()) sni = text(item, "sni");
        boolean tls = bool(item, "tls") || !sni.isEmpty() || bool(item, "skip-cert-verify");
        if (tls) {
            JSONObject tlsObject = new JSONObject().put("enabled", true);
            if (!sni.isEmpty()) tlsObject.put("server_name", sni);
            if (bool(item, "skip-cert-verify")) tlsObject.put("insecure", true);
            if (!text(item, "client-fingerprint").isEmpty()) tlsObject.put("utls",
                    new JSONObject().put("enabled", true).put("fingerprint",
                            text(item, "client-fingerprint")));
            Object alpn = item.get("alpn");
            if (alpn instanceof List) tlsObject.put("alpn", strings((List<?>) alpn));
            Object reality = item.get("reality-opts");
            if (reality instanceof Map) {
                Map<?, ?> options = (Map<?, ?>) reality;
                JSONObject realityObject = new JSONObject().put("enabled", true);
                copy(options, realityObject, "public-key", "public_key");
                copy(options, realityObject, "short-id", "short_id");
                tlsObject.put("reality", realityObject);
            }
            out.put("tls", tlsObject);
        }
        return out;
    }

    private static JSONObject group(Map<?, ?> item) throws Exception {
        String tag = text(item, "name");
        if (tag.isEmpty()) throw new IllegalArgumentException("订阅包含无名称的代理组");
        String clashType = text(item, "type").toLowerCase();
        String type = ("url-test".equals(clashType) || "fallback".equals(clashType)
                || "load-balance".equals(clashType)) ? "urltest" : "selector";
        JSONObject out = new JSONObject().put("tag", tag).put("type", type);
        JSONArray members = new JSONArray();
        Object values = item.get("proxies");
        if (values instanceof List) for (Object value : (List<?>) values) members.put(String.valueOf(value));
        if (members.length() == 0) members.put("DIRECT");
        out.put("outbounds", members);
        if ("urltest".equals(type)) {
            out.put("url", text(item, "url").isEmpty() ? "https://www.gstatic.com/generate_204" : text(item, "url"));
            Object interval = item.get("interval"); if (interval != null) out.put("interval", String.valueOf(interval));
        }
        return out;
    }

    private static JSONArray rules(Map<?, ?> root) throws Exception {
        JSONArray output = new JSONArray();
        Object items = root.get("rules");
        if (!(items instanceof List)) return output;
        for (Object rawRule : (List<?>) items) {
            String line = String.valueOf(rawRule); String[] parts = line.split(",");
            if (parts.length < 2) continue;
            String kind = parts[0].trim().toUpperCase();
            String target = parts[parts.length - 1].trim();
            JSONObject rule = new JSONObject().put("outbound", target);
            if ("MATCH".equals(kind)) { output.put(rule); continue; }
            if (parts.length < 3) continue;
            String value = parts[1].trim();
            if ("DOMAIN".equals(kind)) rule.put("domain", new JSONArray().put(value));
            else if ("DOMAIN-SUFFIX".equals(kind)) rule.put("domain_suffix", new JSONArray().put(value));
            else if ("DOMAIN-KEYWORD".equals(kind)) rule.put("domain_keyword", new JSONArray().put(value));
            else if ("IP-CIDR".equals(kind) || "IP-CIDR6".equals(kind)) rule.put("ip_cidr", new JSONArray().put(value));
            else if ("GEOIP".equals(kind) && "PRIVATE".equalsIgnoreCase(value)) rule.put("ip_is_private", true);
            else continue;
            output.put(rule);
        }
        return output;
    }

    // Clash stores transports as *-opts while sing-box uses a typed transport
    // object.  Keeping this conversion here avoids silently dialing plain TCP
    // for the most common VMess/VLESS/Trojan subscriptions.
    private static void transport(Map<?, ?> item, JSONObject out) throws Exception {
        String network = text(item, "network").toLowerCase();
        if ("ws".equals(network)) {
            JSONObject transport = new JSONObject().put("type", "websocket");
            Object raw = item.get("ws-opts");
            if (raw instanceof Map) {
                Map<?, ?> options = (Map<?, ?>) raw;
                copy(options, transport, "path", "path");
                Object headers = options.get("headers");
                if (headers instanceof Map) {
                    JSONObject mapped = new JSONObject();
                    for (Map.Entry<?, ?> header : ((Map<?, ?>) headers).entrySet()) {
                        mapped.put(String.valueOf(header.getKey()), String.valueOf(header.getValue()));
                    }
                    transport.put("headers", mapped);
                }
            }
            out.put("transport", transport);
        } else if ("grpc".equals(network)) {
            JSONObject transport = new JSONObject().put("type", "grpc");
            Object raw = item.get("grpc-opts");
            if (raw instanceof Map) {
                Map<?, ?> options = (Map<?, ?>) raw;
                String service = text(options, "grpc-service-name");
                if (service.isEmpty()) service = text(options, "service-name");
                if (!service.isEmpty()) transport.put("service_name", service);
            }
            out.put("transport", transport);
        } else if ("http".equals(network) || "h2".equals(network)) {
            JSONObject transport = new JSONObject().put("type", "http");
            Object raw = item.get("h2-opts");
            if (raw instanceof Map) copy((Map<?, ?>) raw, transport, "path", "path");
            out.put("transport", transport);
        }
    }

    private static String selectFallback(Map<?, ?> root, JSONArray outbounds) {
        Object groups = root.get("proxy-groups");
        if (groups instanceof List && !((List<?>) groups).isEmpty()) {
            Object first = ((List<?>) groups).get(0); if (first instanceof Map) return text((Map<?, ?>) first, "name");
        }
        return outbounds.length() > 2 ? outbounds.optJSONObject(2).optString("tag", "DIRECT") : "DIRECT";
    }
    private static JSONArray strings(List<?> values) { JSONArray array = new JSONArray(); for (Object value : values) array.put(String.valueOf(value)); return array; }
    private static String normalizeType(String type) {
        String lower = type.toLowerCase();
        if ("ss".equals(lower)) return "shadowsocks";
        if ("hysteria2".equals(lower) || "hy2".equals(lower)) return "hysteria2";
        return lower;
    }
    private static String clashMode(String mode) {
        if ("global".equalsIgnoreCase(mode)) return "Global";
        if ("direct".equalsIgnoreCase(mode)) return "Direct";
        return "Rule";
    }
    private static String text(Map<?, ?> item, String key) { Object value = item.get(key); return value == null ? "" : String.valueOf(value); }
    private static boolean bool(Map<?, ?> item, String key) { Object value = item.get(key); return value instanceof Boolean ? (Boolean) value : "true".equalsIgnoreCase(String.valueOf(value)); }
    private static void copy(Map<?, ?> source, JSONObject target, String sourceKey, String targetKey) throws Exception { Object value = source.get(sourceKey); if (value != null) target.put(targetKey, value); }
}
