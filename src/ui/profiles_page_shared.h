#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <huxerui/huxerui.h>

namespace clashflux::ui {

inline constexpr float kCardWidth = 340.0F;
inline constexpr float kCardHeight = 180.0F;
inline constexpr float kCardGap = 8.0F;
inline constexpr float kDialogFormHeight = 340.0F;

enum class ProfileGridItemKind { GroupHeader, Profile, Footer };

struct ProfileGridItem {
    ProfileGridItemKind kind = ProfileGridItemKind::Profile;
    std::string type;
    std::size_t profileIndex = 0;
    std::size_t profileCount = 0;
};

struct NativeProfileSupport {
    bool pptp = false;
    bool openvpn = false;
};

using ProfileEditLoader =
    std::function<void(std::int64_t, std::function<void()>)>;
using ProfileEditDialog = std::function<void(std::int64_t)>;

struct ProfileCreateFields {
    huxerui::State<huxerui::TextEditingValue> name;
    huxerui::State<huxerui::TextEditingValue> url;
    huxerui::State<huxerui::TextEditingValue> qr_content;
    huxerui::State<huxerui::TextEditingValue> config_content;
    huxerui::State<std::size_t> type_index;
    huxerui::State<huxerui::TextEditingValue> desc;
    huxerui::State<huxerui::TextEditingValue> timeout;
    huxerui::State<huxerui::TextEditingValue> interval;
    huxerui::State<bool> auto_update;
    huxerui::State<bool> system_proxy;
    huxerui::State<bool> core_proxy;
    huxerui::State<bool> invalid_cert;
    huxerui::State<huxerui::TextEditingValue> pptp_server;
    huxerui::State<huxerui::TextEditingValue> pptp_username;
    huxerui::State<huxerui::TextEditingValue> pptp_password;
    huxerui::State<huxerui::TextEditingValue> pptp_timeout;
    huxerui::State<huxerui::TextEditingValue> pptp_routes;
    huxerui::State<bool> pptp_mppe;
    huxerui::State<huxerui::TextEditingValue> openvpn_config;
    huxerui::State<huxerui::TextEditingValue> openvpn_routes;
    huxerui::State<std::string> picked_path;
    huxerui::State<bool> importing;
};

enum class ProfileAddMethod { Qr, File, Url, Direct };

struct ProfileImportRequest {
    bool remote = false;
    bool local = false;
    bool pptp = false;
    bool openvpn = false;
    std::string name;
    std::string url;
    std::string picked_path;
    db::Profile options;
};

using ProfileImportResult = std::pair<std::int64_t, std::string>;

struct ProfileEditFields {
    huxerui::State<huxerui::TextEditingValue> name;
    huxerui::State<huxerui::TextEditingValue> url;
    huxerui::State<std::string> type;
    huxerui::State<huxerui::TextEditingValue> desc;
    huxerui::State<huxerui::TextEditingValue> timeout;
    huxerui::State<huxerui::TextEditingValue> interval;
    huxerui::State<bool> auto_update;
    huxerui::State<bool> system_proxy;
    huxerui::State<bool> core_proxy;
    huxerui::State<bool> invalid_cert;
    huxerui::State<huxerui::TextEditingValue> pptp_server;
    huxerui::State<huxerui::TextEditingValue> pptp_username;
    huxerui::State<huxerui::TextEditingValue> pptp_password;
    huxerui::State<huxerui::TextEditingValue> pptp_timeout;
    huxerui::State<huxerui::TextEditingValue> pptp_routes;
    huxerui::State<bool> pptp_mppe;
    huxerui::State<huxerui::TextEditingValue> openvpn_config;
    huxerui::State<huxerui::TextEditingValue> openvpn_routes;
};

namespace profile_detail {

std::string profileTypeLabel(std::string_view type);
bool isNativeVpnType(std::string_view type);
NativeProfileSupport profileSupport();

void OpenProfileEditInfo(std::int64_t id, bool compact,
                         const ProfileEditLoader& load_edit_info,
                         const ProfileEditDialog& show_edit_info,
                         huxerui::State<std::int64_t> edit_page_id);
std::string truncateOneLine(const std::string& value, std::size_t maxCodePoints);
huxerui::CanvasPainter QrPainter(const std::string& text);
int parseNumber(const huxerui::TextEditingValue& value, int fallback);
std::vector<std::string> parseRouteField(std::string_view text);
std::string joinRoutes(const std::vector<std::string>& routes);
std::optional<std::string> makePptpConfig(
    const huxerui::TextEditingValue& server,
    const huxerui::TextEditingValue& username,
    const huxerui::TextEditingValue& password,
    const huxerui::TextEditingValue& timeout, bool requireMppe,
    std::string& error);
std::string pptpStateText(const store::PptpState& state);
std::string openVpnStateText(const store::OpenVpnState& state);

std::size_t ResponsiveProfilePageIndex(
    bool compact, huxerui::State<std::int64_t> edit_page_id,
    huxerui::State<bool> create_page_open,
    huxerui::State<std::int64_t> file_page_id,
    huxerui::State<std::int64_t> rules_page_id);
void OpenProfileCreate(bool compact, huxerui::State<bool> page,
                       huxerui::TaskScope tasks,
                       const std::function<void()>& dialog);

huxerui::Task<store::FetchedProfile> AndroidFetchProfile(
    std::shared_ptr<huxerui::HttpClient> http, std::string url,
    int timeoutSecs);
huxerui::Task<std::int64_t> AndroidImportRemote(
    std::shared_ptr<huxerui::HttpClient> http, const std::string& name,
    const std::string& url, db::Profile options);
huxerui::Task<std::string> AndroidRefreshRemote(
    std::shared_ptr<huxerui::HttpClient> http, std::int64_t id);

using ProfileRefreshAction = std::function<void(std::int64_t)>;
void RefreshProfileForPlatform(std::int64_t id,
                               const ProfileRefreshAction& http_refresh,
                               const ProfileRefreshAction& desktop_refresh);
huxerui::Task<ProfileImportResult> ImportProfileForPlatform(
    std::shared_ptr<huxerui::HttpClient> http, ProfileImportRequest request);

} // namespace profile_detail

using profile_detail::AndroidFetchProfile;
using profile_detail::AndroidImportRemote;
using profile_detail::AndroidRefreshRemote;
using profile_detail::ImportProfileForPlatform;
using profile_detail::OpenProfileCreate;
using profile_detail::OpenProfileEditInfo;
using profile_detail::isNativeVpnType;
using profile_detail::parseNumber;
using profile_detail::parseRouteField;
using profile_detail::profileTypeLabel;
using profile_detail::ProfileRefreshAction;
using profile_detail::QrPainter;
using profile_detail::joinRoutes;
using profile_detail::makePptpConfig;
using profile_detail::openVpnStateText;
using profile_detail::pptpStateText;
using profile_detail::RefreshProfileForPlatform;
using profile_detail::ResponsiveProfilePageIndex;
using profile_detail::truncateOneLine;

huxerui::View ProfileOptionsForm(
    huxerui::State<huxerui::TextEditingValue> desc,
    huxerui::State<huxerui::TextEditingValue> timeout,
    huxerui::State<huxerui::TextEditingValue> interval,
    huxerui::State<bool> auto_update, huxerui::State<bool> system_proxy,
    huxerui::State<bool> core_proxy, huxerui::State<bool> invalid_cert);
huxerui::View PptpOptionsForm(
    huxerui::State<huxerui::TextEditingValue> server,
    huxerui::State<huxerui::TextEditingValue> username,
    huxerui::State<huxerui::TextEditingValue> password,
    huxerui::State<huxerui::TextEditingValue> timeout,
    huxerui::State<huxerui::TextEditingValue> routes,
    huxerui::State<bool> require_mppe);
huxerui::View OpenVpnOptionsForm(
    huxerui::State<huxerui::TextEditingValue> config,
    huxerui::State<huxerui::TextEditingValue> routes);

huxerui::View ProfileCard(
    const db::Profile& profile, bool compact, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::shared_ptr<huxerui::HttpClient> http,
    std::function<void()> reload,
    huxerui::State<std::optional<std::int64_t>> optimisticSelected,
    const store::PptpState& pptpState,
    const store::OpenVpnState& openVpnState, bool connectionSelected,
    const std::function<void(std::int64_t, bool)>& toggleConnection,
    const std::function<void(std::int64_t)>& openEditInfo,
    const std::function<void(std::int64_t)>& openEditRules,
    const std::function<void(std::int64_t)>& openEditFile,
    const std::function<void(std::int64_t)>& openQr,
    huxerui::DialogHandle dialog);

huxerui::View ResponsiveProfileEditSurface(
    std::int64_t id, ProfileEditFields fields, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::function<void()> on_back);
huxerui::View ResponsiveProfileCreateSurface(
    bool open, ProfileCreateFields fields, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<huxerui::HttpClient> http, bool pptp_supported,
    bool openvpn_supported, std::function<void()> on_back);
huxerui::View ProfileAddMethodPage(
    ProfileCreateFields fields, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<huxerui::HttpClient> http, bool pptp_supported,
    bool openvpn_supported,
    huxerui::NavigationController navigation);
huxerui::View ProfileCreateMethodPage(
    ProfileAddMethod method, ProfileCreateFields fields,
    huxerui::TaskScope tasks, huxerui::ToastHandle toast,
    std::shared_ptr<huxerui::FilePicker> picker,
    std::shared_ptr<huxerui::HttpClient> http, bool pptp_supported,
    bool openvpn_supported,
    huxerui::NavigationController navigation, std::function<void()> on_back,
    std::function<void()> on_complete);
huxerui::View ProfileEditRoutePage(
    std::int64_t id, ProfileEditFields fields, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::function<void()> on_back);
huxerui::View ProfileFilePage(
    std::int64_t id, huxerui::State<std::shared_ptr<std::string>> content,
    huxerui::State<bool> loading, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::function<void()> on_back);
huxerui::View ProfileRulesPage(
    std::int64_t id, huxerui::State<std::string> yaml,
    huxerui::StateList<std::string> rules,
    huxerui::State<huxerui::TextEditingValue> input,
    huxerui::State<bool> dirty, huxerui::TaskScope tasks,
    huxerui::ToastHandle toast, std::function<void()> on_back);

} // namespace clashflux::ui
