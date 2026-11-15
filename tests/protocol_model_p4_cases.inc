void RunP4() {
    using namespace protocol;
    NodeSpec base;
    base.origin = {"source-p4", "typed-p4"};
    base.server = "example.test";
    base.port = 443;
    auto secure = base;
    secure.tls.enabled = true;
    const TargetCapabilities mobile{false, false};

    auto hy1 = secure;
    Hysteria1Options oldQuic;
    oldQuic.bandwidth = {20, 100};
    oldQuic.auth = HysteriaBytesAuth{"YQ==\r\n"};
    oldQuic.streamReceiveWindow = 1048576;
    oldQuic.connectionReceiveWindow = 4194304;
    oldQuic.disableMtuDiscovery = false;
    oldQuic.hopping = PortHopping{{{443, 443}, {5000, 5002}}, 10, 10};
    hy1.options = oldQuic;
    hy1.port.reset();
    Diagnostics h1Notes{hy1.origin, {}};
    const auto h1 = ValidateNode(hy1, mobile, h1Notes);
    check(bool(h1) && h1Notes.entries.empty(), "typed HY1 accepts ports-only endpoints and explicit effective windows");
    if (h1) {
        const auto out = EmitSingBox(*h1);
        check(!out.contains("server_port") && out["server_ports"] == json::array({"443:443", "5000:5002"}) &&
            out["hop_interval"] == "10s" && !out.contains("hop_interval_max") && out["auth"] == "YQ==\r\n" &&
            !out.contains("auth_str") && out["stream_receive_window"] == 1048576 && out["connection_receive_window"] == 4194304 &&
            out["disable_path_mtu_discovery"] == false && !out["tls"].contains("utls") && !out["tls"].contains("insecure"),
            "HY1 preserves byte-auth encoding, window axes, false flags, port ranges and verified QUIC TLS");
    }
    auto hy2 = secure;
    Hysteria2Options newQuic;
    newQuic.password = " exact HY2 key +%40 ";
    newQuic.obfs = GeckoObfs{" exact obfs ", 0, 0};
    newQuic.hopping = PortHopping{{{5000, 5002}}, 30, 45};
    hy2.options = newQuic;
    hy2.port.reset();
    Diagnostics h2Notes{hy2.origin, {}};
    const auto h2 = ValidateNode(hy2, mobile, h2Notes);
    check(bool(h2) && h2Notes.entries.empty(), "typed HY2 accepts gecko defaults and interval ranges without bandwidth");
    if (h2) {
        const auto out = EmitSingBox(*h2);
        check(out["password"] == " exact HY2 key +%40 " && out["obfs"]["password"] == " exact obfs " &&
            out["obfs"]["min_packet_size"] == 0 && out["obfs"]["max_packet_size"] == 0 &&
            out["hop_interval"] == "30s" && out["hop_interval_max"] == "45s" && !out.contains("server_port") &&
            !out.contains("up_mbps") && !out["tls"].contains("utls"), "HY2 keeps explicit zero sizes, ranges and its own credential/QUIC fields");
    }
    auto tuic = secure;
    tuic.options = TuicOptions{"11111111-2222-3333-4444-555555555555", std::string{}, std::string{"bbr"}, false};
    tuic.tls.disableServerName = true;
    tuic.udp = false;
    Diagnostics tuicNotes{tuic.origin, {}};
    const auto t = ValidateNode(tuic, mobile, tuicNotes);
    check(bool(t), "typed TUIC supports its native SNI and UDP intent");
    if (t) {
        const auto out = EmitSingBox(*t);
        check(out["tls"]["disable_sni"] == true && !out["tls"].contains("utls") && out["password"] == "" &&
            out["congestion_control"] == "bbr" && !out.contains("zero_rtt_handshake") && out["network"] == "tcp",
            "TUIC keeps empty passwords and omits inactive zero-RTT without losing TLS fields");
    }
    auto any = secure;
    any.tls.fingerprint = "firefox";
    any.options = AnyTlsOptions{" exact AnyTLS key ", 0, 0, 0, std::string{}};
    any.udp = false;
    any.dial.detour = "DIRECT";
    any.dial.interfaceName = "interface-test";
    any.dial.routingMark = 9;
    Diagnostics anyNotes{any.origin, {}};
    NoteAnyTlsUdp(any, anyNotes);
    const auto a = ValidateNode(any, mobile, anyNotes);
    check(bool(a) && anyNotes.entries.size() == 2 && anyNotes.entries[0].field == "udp" &&
        anyNotes.entries[1].field == "dialer-proxy" && anyNotes.entries[0].note.sourceId == "source-p4",
        "AnyTLS capability warning precedes dial notes and is not duplicated by staged validation");
    if (a) {
        const auto out = EmitSingBox(*a);
        check(out["idle_session_check_interval"] == "0s" && out["idle_session_timeout"] == "0s" &&
            out["min_idle_session"] == 0 && out["client_metadata"] == "" && !out.contains("network") &&
            out["tls"]["utls"]["fingerprint"] == "firefox", "AnyTLS retains zero seconds/counts, empty metadata and native UDP capability");
    }
    auto snell = base;
    snell.options = Snell4Options{" exact psk ", false, SnellObfsOptions{SnellObfsOptions::Mode::Tls, "edge.test"}};
    snell.udp = false;
    Diagnostics snellNotes{snell.origin, {}};
    const auto n = ValidateNode(snell, mobile, snellNotes);
    check(bool(n), "typed Snell is explicitly v4 and supports its own obfuscation");
    if (n) {
        const auto out = EmitSingBox(*n);
        check(out["version"] == 4 && out["psk"] == " exact psk " && out["reuse"] == false &&
            out["obfs_mode"] == "tls" && out["obfs_host"] == "edge.test" && !out.contains("tls") && out["network"] == "tcp",
            "Snell does not turn disguise into TLS or substitute a different version");
    }
    auto ssh = base;
    ssh.options = SshOptions{" exact user ", std::string{},
        SshPrivateKey{"-----BEGIN OPENSSH PRIVATE KEY-----\npublic-fixture\n-----END OPENSSH PRIVATE KEY-----\n", std::string{}},
        std::vector<std::string>{"public-key-1", "public-key-2"}, std::vector<std::string>{"ssh-ed25519", "rsa-sha2-512"}};
    ssh.udp = true;
    ssh.dial.multiPathTcp = true;
    Diagnostics sshNotes{ssh.origin, {}};
    const auto s = ValidateNode(ssh, mobile, sshNotes);
    check(bool(s) && sshNotes.entries.size() == 2 && sshNotes.entries[0].field == "mptcp" && sshNotes.entries[1].field == "udp",
        "SSH UDP warning follows native dialer notes");
    if (s) {
        const auto out = EmitSingBox(*s);
        check(out["user"] == " exact user " && out["password"] == "" && out["private_key_passphrase"] == "" &&
            out["host_key"] == json::array({"public-key-1", "public-key-2"}) &&
            out["host_key_algorithms"] == json::array({"ssh-ed25519", "rsa-sha2-512"}) &&
            !out.contains("private_key_path") && !out.contains("network") && !out.contains("tls"),
            "SSH retains inline credentials and preference order without file paths or UDP/TLS fields");
    }
    for (int invalid = 0; invalid < 13; ++invalid) {
        auto bad = hy1;
        if (invalid == 0) bad.tls.fingerprint = "chrome";
        if (invalid == 1) bad.tls.enabled = false;
        if (invalid == 2) bad.tls.alpn = std::vector<std::string>{};
        if (invalid == 3) std::get<Hysteria1Options>(bad.options).streamReceiveWindow = 0;
        if (invalid == 4) std::get<Hysteria1Options>(bad.options).auth = HysteriaBytesAuth{"invalid"};
        if (invalid == 5) std::get<Hysteria1Options>(bad.options).hopping->minimumSeconds = 3;
        if (invalid == 6) std::get<Hysteria1Options>(bad.options).hopping->maximumSeconds = 15;
        if (invalid == 7) { bad = hy2; std::get<GeckoObfs>(std::get<Hysteria2Options>(bad.options).obfs).minimumPacketSize = 1300; }
        if (invalid == 8) { bad = any; std::get<AnyTlsOptions>(bad.options).password.clear(); }
        if (invalid == 9) { bad = snell; std::get<Snell4Options>(bad.options).psk.clear(); }
        if (invalid == 10) { bad = ssh; std::get<SshOptions>(bad.options).privateKey->pem = "id_ed25519"; }
        if (invalid == 11) { bad = ssh; std::get<SshOptions>(bad.options).hostKeyAlgorithms = std::vector<std::string>{}; }
        if (invalid == 12) { bad = ssh; std::get<SshOptions>(bad.options).hostKeyAlgorithms = std::vector<std::string>{"ssh-ed25519", "unknown"}; }
        Diagnostics notes{bad.origin, {}};
        check(!ValidateNode(bad, mobile, notes) && !notes.entries.empty(), "P4 model rejects unsupported combinations without a format adapter");
    }
}
