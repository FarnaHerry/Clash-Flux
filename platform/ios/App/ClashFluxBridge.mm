#import <Foundation/Foundation.h>
#import <CFNetwork/CFNetwork.h>
#import <NetworkExtension/NetworkExtension.h>
#import <UIKit/UIKit.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::mutex gManagerMutex;
__strong NEPacketTunnelProviderManager* gManager = nil;
std::atomic_bool gTunnelRunning{false};

NSString* appGroupIdentifier() {
    id value = NSBundle.mainBundle.infoDictionary[@"ClashFluxAppGroup"];
    return [value isKindOfClass:NSString.class] ? value : nil;
}

NSString* extensionBundleIdentifier() {
    id value = NSBundle.mainBundle.infoDictionary[@"ClashFluxPacketTunnelBundleIdentifier"];
    return [value isKindOfClass:NSString.class] ? value : nil;
}

NSURL* appGroupContainer() {
    NSString* identifier = appGroupIdentifier();
    if (identifier.length == 0) return nil;
    return [[NSFileManager defaultManager]
        containerURLForSecurityApplicationGroupIdentifier:identifier];
}

void setError(char* output, std::size_t capacity, NSString* message) {
    if (output == nullptr || capacity == 0) return;
    const char* text = message.UTF8String;
    if (text == nullptr) text = "Unknown iOS Network Extension error";
    const std::size_t length = std::min(std::strlen(text), capacity - 1);
    std::memcpy(output, text, length);
    output[length] = '\0';
}

bool waitFor(dispatch_semaphore_t semaphore, double timeoutSeconds) {
    return dispatch_semaphore_wait(
               semaphore,
               dispatch_time(DISPATCH_TIME_NOW,
                             static_cast<int64_t>(timeoutSeconds * NSEC_PER_SEC))) == 0;
}

bool copyString(char* output, std::size_t capacity, NSString* value) {
    if (output == nullptr || capacity == 0) return false;
    const char* text = value.UTF8String;
    if (text == nullptr) text = "";
    const std::size_t length = std::strlen(text);
    if (length >= capacity) {
        output[0] = '\0';
        return false;
    }
    std::memcpy(output, text, length);
    output[length] = '\0';
    return true;
}

NEPacketTunnelProviderManager* findManager(NSError** error) {
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    __block NSArray<NEPacketTunnelProviderManager*>* managers = nil;
    __block NSError* loadError = nil;
    [NEPacketTunnelProviderManager loadAllFromPreferencesWithCompletionHandler:
        ^(NSArray<NEPacketTunnelProviderManager*>* loaded, NSError* resultError) {
            managers = loaded;
            loadError = resultError;
            dispatch_semaphore_signal(semaphore);
        }];
    if (!waitFor(semaphore, 20.0)) {
        if (error) {
            *error = [NSError errorWithDomain:@"ClashFluxNetworkExtension"
                                         code:1
                                     userInfo:@{NSLocalizedDescriptionKey:
                                         @"等待读取 VPN 配置超时"}];
        }
        return nil;
    }
    if (loadError != nil) {
        if (error) *error = loadError;
        return nil;
    }

    NSString* expectedIdentifier = extensionBundleIdentifier();
    for (NEPacketTunnelProviderManager* manager in managers) {
        NETunnelProviderProtocol* protocol =
            [manager.protocolConfiguration isKindOfClass:NETunnelProviderProtocol.class]
                ? (NETunnelProviderProtocol*)manager.protocolConfiguration
                : nil;
        if ([protocol.providerBundleIdentifier isEqualToString:expectedIdentifier]) {
            return manager;
        }
    }
    return [NEPacketTunnelProviderManager new];
}

bool saveManager(NEPacketTunnelProviderManager* manager, NSError** error) {
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    __block NSError* saveError = nil;
    [manager saveToPreferencesWithCompletionHandler:^(NSError* resultError) {
        saveError = resultError;
        dispatch_semaphore_signal(semaphore);
    }];
    if (!waitFor(semaphore, 20.0)) {
        if (error) {
            *error = [NSError errorWithDomain:@"ClashFluxNetworkExtension"
                                         code:2
                                     userInfo:@{NSLocalizedDescriptionKey:
                                         @"等待保存 VPN 配置超时"}];
        }
        return false;
    }
    if (saveError != nil) {
        if (error) *error = saveError;
        return false;
    }
    return true;
}

} // namespace

@interface ClashFluxDownloadDelegate : NSObject <NSURLSessionDelegate>
@property(nonatomic, assign) BOOL allowInvalidCertificate;
@end

@implementation ClashFluxDownloadDelegate
- (void)URLSession:(NSURLSession*)session
    didReceiveChallenge:(NSURLAuthenticationChallenge*)challenge
      completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition,
                                  NSURLCredential*))completionHandler {
    static_cast<void>(session);
    if (self.allowInvalidCertificate &&
        [challenge.protectionSpace.authenticationMethod
            isEqualToString:NSURLAuthenticationMethodServerTrust] &&
        challenge.protectionSpace.serverTrust != nullptr) {
        completionHandler(
            NSURLSessionAuthChallengeUseCredential,
            [NSURLCredential credentialForTrust:challenge.protectionSpace.serverTrust]);
        return;
    }
    completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
}
@end

extern "C" const char* clashflux_ios_core_work_dir() noexcept {
    static std::string path;
    static std::once_flag once;
    try {
        std::call_once(once, [] {
            @autoreleasepool {
                NSURL* container = appGroupContainer();
                if (container == nil) return;
                path = container.path.UTF8String ?: "";
                if (path.empty()) return;
                NSString* corePath = [container.path stringByAppendingPathComponent:@"core"];
                NSError* error = nil;
                if (![[NSFileManager defaultManager] createDirectoryAtPath:corePath
                                               withIntermediateDirectories:YES
                                                                attributes:nil
                                                                     error:&error]) {
                    path.clear();
                }
            }
        });
    } catch (...) {
        return nullptr;
    }
    return path.empty() ? nullptr : path.c_str();
}

extern "C" bool clashflux_ios_start_tunnel(const char* configPath,
                                            char* errorBuffer,
                                            std::size_t errorBufferSize) noexcept {
    try {
        @autoreleasepool {
            NSString* groupID = appGroupIdentifier();
            NSString* extensionID = extensionBundleIdentifier();
            NSURL* groupURL = appGroupContainer();
            if (groupID.length == 0 || extensionID.length == 0 || groupURL == nil) {
                setError(errorBuffer, errorBufferSize,
                         @"缺少可用的 App Group 或 Packet Tunnel bundle 配置");
                return false;
            }
            if (configPath == nullptr || configPath[0] == '\0') {
                setError(errorBuffer, errorBufferSize, @"sing-box 配置文件路径为空");
                return false;
            }
            NSString* path = [NSString stringWithUTF8String:configPath];
            NSString* expectedPath = [groupURL.path stringByAppendingPathComponent:@"core/config.json"];
            if (![path.stringByStandardizingPath isEqualToString:
                      expectedPath.stringByStandardizingPath] ||
                ![[NSFileManager defaultManager] fileExistsAtPath:expectedPath]) {
                setError(errorBuffer, errorBufferSize,
                         @"sing-box 配置不在 Packet Tunnel 可访问的 App Group 中");
                return false;
            }

            NSError* error = nil;
            NEPacketTunnelProviderManager* manager = findManager(&error);
            if (manager == nil) {
                setError(errorBuffer, errorBufferSize,
                         error.localizedDescription ?: @"读取 VPN 配置失败");
                return false;
            }
            NETunnelProviderProtocol* tunnelProtocol = [NETunnelProviderProtocol new];
            tunnelProtocol.providerBundleIdentifier = extensionID;
            tunnelProtocol.serverAddress = @"Clash-Flux";
            tunnelProtocol.providerConfiguration = @{
                @"appGroupIdentifier": groupID,
                @"configRelativePath": @"core/config.json"
            };
            manager.protocolConfiguration = tunnelProtocol;
            manager.localizedDescription = @"Clash-Flux";
            manager.enabled = YES;
            if (!saveManager(manager, &error)) {
                setError(errorBuffer, errorBufferSize,
                         error.localizedDescription ?: @"保存 VPN 配置失败");
                return false;
            }

            dispatch_semaphore_t reloadSemaphore = dispatch_semaphore_create(0);
            __block NSError* reloadError = nil;
            [manager loadFromPreferencesWithCompletionHandler:^(NSError* resultError) {
                reloadError = resultError;
                dispatch_semaphore_signal(reloadSemaphore);
            }];
            if (!waitFor(reloadSemaphore, 20.0) || reloadError != nil) {
                setError(errorBuffer, errorBufferSize,
                         reloadError.localizedDescription ?: @"重新读取 VPN 配置失败");
                return false;
            }

            NETunnelProviderSession* session = (NETunnelProviderSession*)manager.connection;
            if (session.status != NEVPNStatusConnected &&
                session.status != NEVPNStatusConnecting) {
                NSError* startError = nil;
                if (![session startTunnelWithOptions:nil andReturnError:&startError]) {
                    setError(errorBuffer, errorBufferSize,
                             startError.localizedDescription ?: @"系统拒绝启动 Packet Tunnel");
                    return false;
                }
            }

            {
                std::lock_guard lock(gManagerMutex);
                gManager = manager;
            }
            bool sawConnecting = session.status == NEVPNStatusConnecting;
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(45);
            while (std::chrono::steady_clock::now() < deadline) {
                const NEVPNStatus status = session.status;
                if (status == NEVPNStatusConnected) {
                    gTunnelRunning.store(true);
                    return true;
                }
                if (status == NEVPNStatusConnecting) sawConnecting = true;
                if (status == NEVPNStatusInvalid ||
                    (sawConnecting && status == NEVPNStatusDisconnected)) {
                    gTunnelRunning.store(false);
                    setError(errorBuffer, errorBufferSize,
                             @"Packet Tunnel 启动失败，请检查系统 VPN 授权和扩展日志");
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            setError(errorBuffer, errorBufferSize, @"等待 Packet Tunnel 建立超时");
            return false;
        }
    } catch (...) {
        setError(errorBuffer, errorBufferSize, @"启动 Packet Tunnel 时发生原生异常");
        return false;
    }
}

extern "C" void clashflux_ios_stop_tunnel() noexcept {
    try {
        @autoreleasepool {
            NEPacketTunnelProviderManager* manager = nil;
            {
                std::lock_guard lock(gManagerMutex);
                manager = gManager;
            }
            if (manager == nil) {
                NSError* error = nil;
                manager = findManager(&error);
            }
            if (manager == nil) {
                gTunnelRunning.store(false);
                return;
            }
            NETunnelProviderSession* session = (NETunnelProviderSession*)manager.connection;
            if (session.status != NEVPNStatusDisconnected &&
                session.status != NEVPNStatusInvalid) {
                [session stopTunnel];
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(15);
                while (std::chrono::steady_clock::now() < deadline &&
                       session.status != NEVPNStatusDisconnected &&
                       session.status != NEVPNStatusInvalid) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            gTunnelRunning.store(false);
        }
    } catch (...) {
        gTunnelRunning.store(false);
    }
}

extern "C" bool clashflux_ios_tunnel_running() noexcept {
    NEPacketTunnelProviderManager* manager = nil;
    {
        std::lock_guard lock(gManagerMutex);
        manager = gManager;
    }
    if (manager == nil) return gTunnelRunning.load();
    const NEVPNStatus status = manager.connection.status;
    const bool active = status == NEVPNStatusConnected ||
                        status == NEVPNStatusConnecting ||
                        status == NEVPNStatusReasserting;
    gTunnelRunning.store(active);
    return active;
}

extern "C" void clashflux_ios_open_url(const char* url) noexcept {
    if (url == nullptr || url[0] == '\0') return;
    @autoreleasepool {
        NSString* value = [NSString stringWithUTF8String:url];
        NSURL* target = value == nil ? nil : [NSURL URLWithString:value];
        if (target == nil) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            [[UIApplication sharedApplication] openURL:target
                                               options:@{}
                                     completionHandler:nil];
        });
    }
}

extern "C" bool clashflux_ios_download_to_file(
    const char* url, const char* tempPath, long timeoutSeconds,
    bool allowInvalidCertificate, const char* proxyUrl,
    bool allowSystemProxy, long* httpStatus, long long* expectedBytes,
    char* responseHeadersJson, std::size_t responseHeadersCapacity,
    char* errorBuffer, std::size_t errorBufferCapacity) noexcept {
    if (httpStatus != nullptr) *httpStatus = 0;
    if (expectedBytes != nullptr) *expectedBytes = -1;
    if (responseHeadersJson != nullptr && responseHeadersCapacity > 0) {
        responseHeadersJson[0] = '\0';
    }
    try {
        @autoreleasepool {
            if (url == nullptr || tempPath == nullptr || url[0] == '\0' ||
                tempPath[0] == '\0') {
                setError(errorBuffer, errorBufferCapacity, @"下载 URL 或目标文件为空");
                return false;
            }
            NSString* urlText = [NSString stringWithUTF8String:url];
            NSURL* sourceURL = urlText == nil ? nil : [NSURL URLWithString:urlText];
            NSString* scheme = sourceURL.scheme.lowercaseString;
            if (sourceURL == nil ||
                (![scheme isEqualToString:@"http"] &&
                 ![scheme isEqualToString:@"https"])) {
                setError(errorBuffer, errorBufferCapacity,
                         @"订阅下载仅支持 HTTP 或 HTTPS URL");
                return false;
            }

            NSURLSessionConfiguration* configuration =
                [NSURLSessionConfiguration ephemeralSessionConfiguration];
            const long timeout = std::max(timeoutSeconds, 1L);
            configuration.timeoutIntervalForRequest = static_cast<double>(timeout);
            configuration.timeoutIntervalForResource = static_cast<double>(timeout);
            configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
            configuration.URLCache = nil;
            configuration.HTTPMaximumConnectionsPerHost = 4;

            const char* proxyText = proxyUrl == nullptr ? "" : proxyUrl;
            if (proxyText[0] != '\0') {
                NSString* proxyString = [NSString stringWithUTF8String:proxyText];
                NSURLComponents* components = proxyString == nil
                    ? nil
                    : [NSURLComponents componentsWithString:proxyString];
                NSString* proxyScheme = components.scheme.lowercaseString;
                const NSInteger port = components.port == nil
                    ? ([proxyScheme isEqualToString:@"https"] ? 443 : 80)
                    : components.port.integerValue;
                if (components.host.length == 0 ||
                    (![proxyScheme isEqualToString:@"http"] &&
                     ![proxyScheme isEqualToString:@"https"]) ||
                    port <= 0 || port > 65535 || components.user.length > 0 ||
                    components.password.length > 0) {
                    setError(errorBuffer, errorBufferCapacity,
                             @"iOS 订阅下载仅支持无认证的 HTTP/HTTPS 代理");
                    return false;
                }
                NSNumber* proxyPort = @(port);
                NSString* proxyHost = components.host;
                configuration.connectionProxyDictionary = @{
                    (__bridge NSString*)kCFNetworkProxiesHTTPEnable: @YES,
                    (__bridge NSString*)kCFNetworkProxiesHTTPProxy: proxyHost,
                    (__bridge NSString*)kCFNetworkProxiesHTTPPort: proxyPort,
                    (__bridge NSString*)kCFNetworkProxiesHTTPSEnable: @YES,
                    (__bridge NSString*)kCFNetworkProxiesHTTPSProxy: proxyHost,
                    (__bridge NSString*)kCFNetworkProxiesHTTPSPort: proxyPort,
                };
            } else if (!allowSystemProxy) {
                configuration.connectionProxyDictionary = @{
                    (__bridge NSString*)kCFNetworkProxiesHTTPEnable: @NO,
                    (__bridge NSString*)kCFNetworkProxiesHTTPSEnable: @NO,
                };
            }

            NSMutableURLRequest* request = [NSMutableURLRequest
                requestWithURL:sourceURL
                   cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
               timeoutInterval:static_cast<double>(timeout)];
            [request setValue:@"clash-flux/0.1" forHTTPHeaderField:@"User-Agent"];
            [request setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];

            NSString* destinationPath = [NSString stringWithUTF8String:tempPath];
            if (destinationPath == nil) {
                setError(errorBuffer, errorBufferCapacity, @"下载目标路径不是有效 UTF-8");
                return false;
            }
            NSURL* destinationURL = [NSURL fileURLWithPath:destinationPath];
            ClashFluxDownloadDelegate* delegate = [ClashFluxDownloadDelegate new];
            delegate.allowInvalidCertificate = allowInvalidCertificate;
            NSURLSession* session = [NSURLSession
                sessionWithConfiguration:configuration
                                delegate:delegate
                           delegateQueue:nil];
            dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
            __block NSError* transferError = nil;
            __block NSDictionary<NSString*, id>* responseHeaders = @{};
            __block long responseStatus = 0;
            __block long long responseLength = -1;
            __block bool completed = false;
            NSURLSessionDownloadTask* task = [session
                downloadTaskWithRequest:request
                      completionHandler:^(NSURL* location,
                                          NSURLResponse* response,
                                          NSError* error) {
                if (error != nil) {
                    transferError = error;
                    dispatch_semaphore_signal(semaphore);
                    return;
                }
                if (![response isKindOfClass:NSHTTPURLResponse.class]) {
                    transferError = [NSError errorWithDomain:@"ClashFluxDownload"
                                                        code:1
                                                    userInfo:@{
                        NSLocalizedDescriptionKey: @"服务器没有返回 HTTP 响应"
                    }];
                    dispatch_semaphore_signal(semaphore);
                    return;
                }
                NSHTTPURLResponse* http = (NSHTTPURLResponse*)response;
                responseStatus = static_cast<long>(http.statusCode);
                NSMutableDictionary<NSString*, id>* normalizedHeaders =
                    [NSMutableDictionary dictionary];
                for (id key in http.allHeaderFields) {
                    id value = http.allHeaderFields[key];
                    if (![key isKindOfClass:NSString.class] ||
                        ![value isKindOfClass:NSString.class]) continue;
                    normalizedHeaders[((NSString*)key).lowercaseString] = value;
                }
                responseHeaders = normalizedHeaders;
                NSString* contentEncoding = responseHeaders[@"content-encoding"];
                const bool encoded = contentEncoding.length > 0 &&
                    ![contentEncoding.lowercaseString isEqualToString:@"identity"];
                if (!encoded && http.expectedContentLength > 0) {
                    responseLength = http.expectedContentLength;
                }

                if (responseStatus >= 200 && responseStatus < 300) {
                    NSFileManager* fileManager = [NSFileManager defaultManager];
                    NSError* removeError = nil;
                    [fileManager removeItemAtURL:destinationURL error:&removeError];
                    NSError* moveError = nil;
                    if (location == nil ||
                        ![fileManager moveItemAtURL:location
                                              toURL:destinationURL
                                              error:&moveError]) {
                        transferError = moveError ?: [NSError errorWithDomain:@"ClashFluxDownload"
                                                                          code:2
                                                                      userInfo:@{
                            NSLocalizedDescriptionKey: @"无法保存订阅下载文件"
                        }];
                        dispatch_semaphore_signal(semaphore);
                        return;
                    }
                }
                completed = true;
                dispatch_semaphore_signal(semaphore);
            }];
            [task resume];
            const double waitSeconds = static_cast<double>(timeout) + 15.0;
            const dispatch_time_t deadline = dispatch_time(
                DISPATCH_TIME_NOW,
                static_cast<int64_t>(waitSeconds * static_cast<double>(NSEC_PER_SEC)));
            if (dispatch_semaphore_wait(semaphore, deadline) != 0) {
                [task cancel];
                [session invalidateAndCancel];
                setError(errorBuffer, errorBufferCapacity, @"iOS URLSession 下载超时");
                return false;
            }
            [session finishTasksAndInvalidate];
            if (!completed) {
                setError(errorBuffer, errorBufferCapacity,
                         transferError.localizedDescription ?: @"iOS URLSession 下载失败");
                return false;
            }

            if (httpStatus != nullptr) *httpStatus = responseStatus;
            if (expectedBytes != nullptr) *expectedBytes = responseLength;
            NSError* jsonError = nil;
            NSData* jsonData = [NSJSONSerialization
                dataWithJSONObject:responseHeaders options:0 error:&jsonError];
            NSString* jsonText = jsonData == nil
                ? @"{}"
                : [[NSString alloc] initWithData:jsonData
                                         encoding:NSUTF8StringEncoding];
            if (jsonText == nil ||
                !copyString(responseHeadersJson, responseHeadersCapacity, jsonText)) {
                setError(errorBuffer, errorBufferCapacity,
                         @"服务器响应头超过 iOS 下载接口缓冲区");
                return false;
            }
            return true;
        }
    } catch (...) {
        setError(errorBuffer, errorBufferCapacity, @"iOS URLSession 下载时发生原生异常");
        return false;
    }
}
