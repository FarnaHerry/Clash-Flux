// win32_raii.h — Win32 句柄与注册表键的最小 RAII 包装（仅 _WIN32 参与编译）。
//
// 这些资源的原生 API 都是「获取 + 显式 CloseHandle / RegCloseKey」：只要中途
// 多一条 return 或抛一次异常就会泄漏。包装后由析构统一释放，调用方继续用
// get()/put() 把原生类型传给 Win32 API。只在 Windows 分支 include；非 Windows
// 平台展开为空。
#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <utility>  // std::exchange

namespace clashflux::win32 {

// CloseHandle 所有权；nullptr 与 INVALID_HANDLE_VALUE 都视为「无句柄」。
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) reset(std::exchange(other.handle_, nullptr));
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    // 交出所有权（调用方负责 CloseHandle）。
    HANDLE release() noexcept { return std::exchange(handle_, nullptr); }
    void reset(HANDLE handle = nullptr) noexcept {
        if (valid()) ::CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

// RegCloseKey 所有权；put() 供 RegOpenKeyEx / RegCreateKeyEx 的输出参数使用。
class UniqueHkey {
public:
    UniqueHkey() = default;
    ~UniqueHkey() { reset(); }
    UniqueHkey(const UniqueHkey&) = delete;
    UniqueHkey& operator=(const UniqueHkey&) = delete;
    [[nodiscard]] HKEY* put() noexcept { return &key_; }
    [[nodiscard]] HKEY get() const noexcept { return key_; }
    [[nodiscard]] bool valid() const noexcept { return key_ != nullptr; }
    void reset() noexcept {
        if (key_ != nullptr) {
            ::RegCloseKey(key_);
            key_ = nullptr;
        }
    }

private:
    HKEY key_ = nullptr;
};

} // namespace clashflux::win32

#endif  // _WIN32
