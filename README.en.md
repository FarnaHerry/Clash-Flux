# Clash-Flux

See the [v0.3.18 release notes](docs/releases/v0.3.18.md). A `v*` tag triggers CI; after the release gates pass and packages are collected, it publishes the Release using `docs/releases/<tag>.md` when available.

[简体中文](README.md) | English

Clash-Flux is a cross-platform proxy client built with C++23 and HuxerUI, bringing
Clash Verge Rev's core experience to sing-box. Desktop builds launch the official
sing-box binary and communicate through the `clash_api` REST API and WebSocket
streams. Android runs libbox in a background process and uses the official libbox
CommandClient. Both the UI and the core integration are implemented in C++.

Configurations can be imported as Clash YAML, which the built-in compiler converts
to sing-box configuration, or as native sing-box JSON, which is merged with
application-managed settings before being passed to sing-box.

## Features

- Subscription and profile management: cards with context menus, double-click
  activation and refresh actions; URL import, update, activation, deletion and rule
  editing; local storage; Clash YAML and native sing-box JSON file import.
- Browser subscription links: accept sing-box and FlClash links and open an import
  form with the name and URL filled in. Pasting and QR import accept these links too;
  see [formats and platform registration](docs/profile-links.md).
- Subscription downloads: choose the sing-box core, system environment proxy or a
  direct connection for each subscription, with an explicit option to allow invalid
  certificates. On Windows, HTTPS downloads use the Windows certificate store for
  validation, matching the system browser's trust decisions.
- Proxies: horizontally scrollable group tabs with a primary-color underline;
  switch groups by clicking, choosing from the tab menu or swiping the content.
  The underline and tab strip follow content swipes, keeping the destination tab near
  the center. Content rebounds when canceled; returning to a group preserves its scroll
  position. Switch nodes and test a whole group's latency, with colored delay values and
  a test button at the bottom right.
- Rules, connections and logs: view each subscription's rules; assign domains,
  IP addresses and CIDRs to different connections through global routing rules;
  inspect connection snapshots and close individual or all connections. Core and
  application logs are stored separately and restored for viewing.
- Native VPN profiles: PPTP uses system dialing and privileged service integration.
  Windows uses RAS with MS-CHAPv2; enabling MPPE-128 requires encryption, while
  disabling that option lets the server negotiate its requirements. OpenVPN
  `.ovpn` profiles use sing-box 1.14's `openvpn-client` endpoint and support multiple
  active connections. Private-network CIDRs route directly to these endpoints;
  only PPTP installs system routes through the platform backend.
- Core controls: start the core with the application, using a minimal configuration
  when no subscription is selected. Restore TUN and system proxy settings according
  to saved preferences. Select rule, global or direct outbound mode; configure the
  mixed port, LAN access and log level. App-managed desktop GEOIP/GEOSITE caches
  check SRS, version and zlib headers before replacement, refresh weekly on startup,
  and retain the old file on failure. Native rule sets remain managed by the core;
  Android China rule sets remain pinned build assets. The core performs full parsing.
- Subscription conversion: compile Clash YAML to sing-box JSON, including
  ss/vmess/vless/trojan/hysteria2/tuic/AnyTLS/Snell v4 protocols, policy groups,
  domain/IP/GEOIP/GEOSITE rules, source addresses, port lists/ranges and TCP/UDP
  matching. Test tolerance and long test intervals are preserved within documented
  core limits. Undeclared legacy entries `RULE-SET,cn` and `RULE-SET,cn-ip` map
  approximately to the bundled China rule sets. Unsupported entries are reported explicitly. Native
  sing-box JSON subscriptions pass through the native configuration path.
- DNS conversion: UDP/TCP/DoT/DoH/DoQ, HTTPS `#h3=true`, the system resolver
  and DNS outbound binding. `nameserver-policy` and node-specific
  `proxy-server-nameserver-policy` support exact domains and
  `+.domain` suffixes, IP bootstrap and node resolvers, with DNS/proxy dependency
  cycle checks. Direct resolver timing, multiple-server, fallback and unmapped semantics appear in
  the fidelity report; full Clash DNS policy compatibility is not claimed.
- Proxy chains: `dialer-proxy` maps to native `detour`, with forward references
  to nodes/groups and compile-time rejection of missing targets and cycles.
  Selectors support `default-selected`; unmapped group fields and the current
  `no-resolve` semantic gap appear in the fidelity report.
- Advanced mappings: desktop interface binding, Linux uint32 routing marks and
  TCP Fast Open/Multi Path. Ignored options with detour and MPTCP's IPv6 limitation
  are reported. AND/OR/NOT and arbitrary inline rule providers support a defined
  domain/regex/CIDR/port/network and platform process/package subset. Route rules also
  support IP version and Linux UID. Explicit MATCH targets and REJECT-DROP are preserved;
  external provider downloads and an app picker remain unfinished. See the fidelity contract
  for schema and platform limits.
- JSON codec: pinned Glaze 9.0.0 handles runtime snapshots, API messages and internal
  routing policies. Pages consume ordinary C++ data; connection decoding runs on
  worker threads. Clash YAML and native configuration DOM keep their existing
  adapters. See the [migration record](docs/glaze-migration.md).
- System proxy support for Windows, macOS, KDE and GNOME, plus sing-box TUN mode.
  Changing TUN restarts the core to apply the setting. Windows background proxy
  operations use system APIs without opening command windows.
- Optional service mode: one root systemd service manages sing-box and Linux PPTP,
  avoiding repeated authorization for TUN, PPTP dialing and native routes. OpenVPN
  follows the sing-box lifecycle and does not require a system OpenVPN CLI. Without
  the service, the application first tries to adopt an external instance, then
  falls back to launching the core directly.
- A full CLI in the same executable: `core`, `mode`, `tun`, `proxy`, `profile` and
  `service` commands. Launch without arguments to open the GUI.
- Light and dark themes with system theme detection, an island-style interface,
  custom window title bars, a system tray and responsive layouts for narrow windows.
  Windows tray menus follow the application theme.
- A dashboard pie chart shows cumulative upload/download shares, totals and
  percentages.
- Android: sing-box libbox uses the same version as the desktop core and runs in a
  separate `:background` process. VpnService creates the TUN interface, while the UI
  process owns the configuration database. Binder controls the runtime; atomic
  snapshots share outbound and connection data. The foreground service restores
  the persisted core or TUN mode after system reclamation. Network changes and
  device wakeups update the default interface and reconnect existing connections.
  Notification permission affects notification visibility and does not block
  service startup. A Quick Settings tile toggles TUN.

The project is under development; interfaces and data structures may change.

## Build requirements

- CMake 3.30 or newer.
- A compiler supporting C++23 modules and `import std` (GCC 16 on the development
  machine).
- Ninja, recommended.
- Building HuxerUI from source on Linux requires development packages for GTK
  ≥4.14, libepoxy ≥1.5 and libsoup ≥3.0. If unavailable, the build falls back to the
  installed or offline 0.3.0 SDK.
- The project bundles sing-box: configuration automatically downloads the official
  release for the host platform and architecture, with pinned hashes. Desktop and
  Android libbox both use 1.14.2. No manual installation is needed; disable bundling
  with `-DCLASHFLUX_BUNDLE_SINGBOX=OFF`. Desktop caches are isolated by version
  and asset SHA256 to prevent reuse of an older core.

On Fedora:

```bash
sudo dnf install cmake ninja-build gcc-c++ gtk4-devel libepoxy-devel libsoup3-devel
```

The Linux PPTP root service also requires system dialing tools. The service invokes
these tools as root; they are not bundled with the application:

```bash
# Fedora
sudo dnf install ppp pptp iproute
# Debian/Ubuntu
sudo apt install ppp pptp iproute2
```

OpenVPN profiles are stored as native `.ovpn` text. To let sing-box manage the
connection at runtime, use inline `<ca>`, `<cert>`, `<key>`, `<tls-auth>` and
`<auth-user-pass>` blocks. The compiler passes these to sing-box. External file
paths, scripts and system routing directives are not executed silently.

## Build and run

```bash
cmake -B build -G Ninja
cmake --build build -j
./run.sh
```

### Required development validation

After every change, including code, UI, scripts and configuration, rebuild locally
before delivering or validating functionality:

```bash
cmake --build build --target clash-flux
./run.sh --version
```

Section content uses the framework Pager for direct dragging, rebound, and retained scroll positions.
Source builds automatically apply the Pager retargeting and hidden virtual page layout patches; all CI platforms apply them as well.
See [UI development notes](docs/ui-development.md).

By default, `run.sh` starts the existing `build/clash-flux` executable and does not
build it implicitly. A failed build must not be reported as complete. For another
build directory, explicitly select its executable with
`CLASHFLUX_BIN=/absolute/path/clash-flux ./run.sh --version`. Run `./run.sh` afterward
when GUI startup or interaction needs validation.

Alternatively, use the HuxerUI CLI:

```bash
huxerui build linux
huxerui run linux
```

Android builds require Android SDK/NDK, Gradle and `HUXERUI_HOME` pointing to the
HuxerUI SDK:

```bash
export HUXERUI_HOME=/path/to/huxerui-sdk
huxerui build android --profile release
```

Android uses a compatibility compilation path and does not require C++ module
support in the NDK. The data plane is a sing-box libbox AAR built from source for
arm64-v8a, matching the desktop core version, running in a separate background
process and providing TUN through VpnService.

When `third_party/huxerui` is available, both the Android Java and native C++
modules build from that same source revision. Otherwise, both use the installed
SDK under `HUXERUI_HOME`. See [Android build and runtime version consistency](docs/android-build.md).
Gradle generates and bundles `geoip-cn.srs` and `geosite-cn.srs` using pinned
upstream revisions and SHA256 hashes. The first build needs network access;
runtime China routing does not require downloading these rule sets.
See [Android background service lifecycle](docs/android-background-lifecycle.md)
for process ownership, Binder control, service recovery and network changes.

GitHub Release APKs use a stable Android release signing key. CI requires four
repository secrets: `CLASHFLUX_ANDROID_KEYSTORE_BASE64`,
`CLASHFLUX_ANDROID_KEYSTORE_PASSWORD`, `CLASHFLUX_ANDROID_KEY_ALIAS` and
`CLASHFLUX_ANDROID_KEY_PASSWORD`. APKs include both v1 (JAR) and v2 signatures, and
CI verifies each. Local builds without a release key use Gradle's debug signing.

## Architecture and configuration fidelity

The core follows sing-box, input follows the Clash ecosystem, and the product uses
Clash terminology while exposing capabilities the core actually provides. The
built-in compiler handles Clash YAML, native sing-box JSON and native PPTP/OpenVPN
connections. Each mapping is recorded as `exact`, `approx` or `unsupported` in a
fidelity ledger. Degradation is reported, and the UI does not claim unsupported
core capabilities.

See [sing-box layers and Clash-to-sing-box fidelity](docs/singbox-layers-and-fidelity.md)
for responsibilities, decision rules, protocol/endpoint/rule coverage and known
limitations.

See the [stable core upgrade record](docs/singbox-stable-upgrade.md) for the
version pin, platform hashes and libbox artifact metadata requirement.

See the [core capability audit](docs/singbox-capability-audit.md) for integration
gaps and field limits, and the [Clash YAML example](docs/examples/kernel-capabilities.yaml)
for the newly mapped capabilities.

See the [L1 / L2 / L3 development review](docs/l1-l2-l3-status.md) for the
2026-10-02 workspace snapshot, remaining fidelity defects and validation scope.
It distinguishes implementation from device validation and does not describe
every capability of the published packages.
The first L2 batch fixes literal credentials, malformed TLS/transport values and
global fingerprint handling, with permanent regressions. Protocol type coverage
remains 10/15; further field and provider adaptation remains in the review backlog.

Desktop now implements [one main source with rule-driven secondary sources](docs/desktop-subscription-orchestration.md).
Clash YAML secondary sources participate only through enabled rules, with isolated
group and node names. Fixed tiers order user overrides, source policies, main rules
and the main fallback; priority applies within a tier. Unavailable targets default
to rejection, with explicit main-default or direct alternatives. The main source
can be Clash YAML or native JSON; native JSON secondary sources and cross-source
proxy chains remain unsupported. Mobile keeps one active ordinary proxy source.
See the [official GUI review](docs/singbox-official-gui-review.md) for interaction
references and implementation priorities.

## CLI

The same executable provides the GUI and CLI. Without arguments it opens the GUI;
with arguments it runs a command:

```bash
clash-flux version                    # Show version
clash-flux core start|stop|restart|status
clash-flux mode [rule|global|direct]   # Show or change outbound mode
clash-flux tun on|off                 # TUN; requires service mode or root
clash-flux proxy on|off|status        # System proxy
clash-flux profile list|import <url> [name]|use <id>|update <id>|remove <id>
clash-flux profile check [<id>]       # Check fidelity; exit 1 for unmappable entries
clash-flux service install|uninstall|status|run
```

`service install` requires root; the GUI settings page invokes it through `pkexec`.
It installs `clash-flux.service`, after which one root daemon manages sing-box and
Linux PPTP. OpenVPN endpoints are created by sing-box and do not require the
OpenVPN CLI.

At the next core startup, an installed service checks its version and synchronizes
Clash-Flux and sing-box from the current client. Upgrade files are written to
temporary files in the same directory and atomically replaced, allowing systemd
to restart into the new version even while older processes are running. For the
first installation, or an older service without self-upgrade support, run
`sudo clash-flux service install`. Root safety checks require the client to be in
a root-managed installation directory that ordinary users cannot write to; the
default package location is `/opt/clash-flux`.

The GUI sends fixed-protocol requests through the restricted Unix socket
`/run/clash-flux/service.sock`. The service executes `pppd` and `ip route`. During
installation it records the user UID before privilege elevation; only that user
and root can access the socket. Linux TUN/PPTP requires the service and prompts
for its installation when absent. OpenVPN uses sing-box userspace endpoints.

## Cross-platform CI

`.github/workflows/build.yml` runs the full matrix for pushes to `main`, pull
requests and `v*` tags. The repository is public, so standard GitHub-hosted runner
usage is free; Windows/macOS billing multipliers apply to private repository
quotas. Concurrency and fair-use limits still apply. Tag pushes additionally run
`publish-release`, which collects platform artifacts into a GitHub Release,
including platform archives and the Windows installer.

Jobs follow the `build-<os>-<arch>` naming convention:

| Job | Runner | Status |
|-----|--------|--------|
| build-linux-x86_64 | Ubuntu container + clang-21/libc++ | Supported |
| build-linux-arm64 | Native ubuntu-24.04-arm | Experimental |
| build-windows-x86_64 | MSVC + preinstalled runner OpenSSL + HuxerUI installer | Supported |
| build-macos-arm64 | macos-15 + Homebrew LLVM | Release gate |
| build-android | HuxerUI CLI arm64 APK build: GUI/native shell + sing-box libbox | Release gate |
| build-ios-simulator-arm64 | macOS + iOS Simulator SDK; unsigned app/Packet Tunnel and pinned sing-box device/Simulator Libbox.xcframework | TODO / deferred; experimental diagnostics, does not block releases |

The Windows installer has a separate `build-windows-installer` gate. There are
currently no Windows arm64 or macOS x86_64 jobs; Linux arm64 is non-blocking.
These statuses describe workflow gates, not a fresh check of hosted CI results
or completed device validation.

The iOS diagnostic job builds `clash-flux_huxerui_ios_core` for the arm64 Simulator
from the project's CMake configuration using a pinned HuxerUI iOS source revision.
It builds `Libbox.xcframework` for iOS devices and Simulator from the same sing-box
revision as Android. The job checks compilation of the app, Packet Tunnel and
engine. It generates no IPA, uploads no release assets and is not a release gate.
iOS subscriptions use native URLSession for TLS, validating system certificates
by default with an explicit per-subscription option to allow invalid certificates.

**iOS support is currently deferred as a TODO. It is not a supported platform, and
there is no TestFlight or App Store release commitment.** Current work validates
Simulator compilation only; distributable signed device packages and VPN lifecycle
validation on physical devices remain unfinished. Restoring support requires a
future project decision. See [iOS builds and Apple signing](docs/ios-build.md).

Linux RPM/DEB packages include `/usr/bin/clash-flux`, desktop menu integration
through a `.desktop` file and hicolor icons. The application is installed under
`/opt/clash-flux`.

The official desktop core asset table contains six pinned mappings; client build
coverage follows the actual CI matrix above.
Android libbox builds verify the official release tag against the pinned revision;
Gradle validates version, revision, ABI and AAR SHA256 metadata before packaging.
Core asset SHA256 hashes are pinned in `cmake/singbox_bundle.cmake`, with automatic
downloads during configuration. Desktop jobs build HuxerUI from a pinned upstream
source checkout. Linux, Windows, macOS, Android and iOS Simulator all use HuxerUI
revision `0c5126235d43c2b703166bcc00781b850f2d1c39` and apply the remaining local
patches under `cmake/patches/` for drag previews, Linux frame lifecycle and macOS/iOS
aggregate initialization. Before building, CMake also adapts the pinned Lib-Camera
revision to the HuxerUI `ApplicationContext` API.

When upgrading HuxerUI, review every patch against upstream. Remove fixes already
included upstream; retained patches must pass `git apply --check --unidiff-zero`
for every applicable platform.

Windows packaging with `huxerui package windows` creates a `setup.exe` with an
installation wizard: Burn bundles an MSI and a HuxerUI installer interface with
destination selection, desktop shortcuts, repair and uninstall. The installer
supports Simplified Chinese, Traditional Chinese and English. GitHub Release
Windows archives include both `clash-flux-Setup-<version>.exe` and portable files.
