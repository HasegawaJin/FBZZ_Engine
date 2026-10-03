/// @file    PixCapture.cpp
/// @brief   Restricted, opt-in loading of a pinned or explicitly selected PIX capturer.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <Engine/Core/PixCapture.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/StringUtils.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <system_error>
#include <utility>
#include <vector>

namespace fbzz::core {
namespace {

constexpr wchar_t CAPTURER_FILENAME[] = L"WinPixGpuCapturer.dll";
constexpr size_t MAX_MODULE_PATH_CHARACTERS = 32768;
PixCaptureStatus g_status;
PixCaptureOptions g_options;
bool g_initialized = false;
bool g_initializationSucceeded = false;

bool ValidOptions(const PixCaptureOptions& options, std::string& reason)
{
    if (!options.requested && !options.installedDirectory.empty()) {
        reason = "--pix-path requires --pix-capture.";
        return false;
    }
    if (!options.installedDirectory.empty() && !options.installedDirectory.is_absolute()) {
        reason = "--pix-path must be an absolute directory.";
        return false;
    }
    reason.clear();
    return true;
}

std::filesystem::path ProgramFilesDirectory()
{
    const DWORD capacity = GetEnvironmentVariableW(L"ProgramFiles", nullptr, 0);
    if (capacity == 0) return {};
    std::vector<wchar_t> value(capacity);
    const DWORD length = GetEnvironmentVariableW(L"ProgramFiles", value.data(), capacity);
    if (length == 0 || length >= capacity) return {};
    return std::filesystem::path(std::wstring(value.data(), length));
}

bool FailInitialization(std::string reason)
{
    g_status.ready = false;
    g_status.reason = std::move(reason);
    FBZZ_LOG_ERROR("PIX capture startup: %s (directory=%s)", g_status.reason.c_str(),
                   util::StringUtils::PathToUtf8(g_status.installedDirectory).c_str());
    return false;
}

bool CanonicalModulePath(HMODULE module, std::filesystem::path& out)
{
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulefilenamew Truncated paths return the buffer capacity.
    for (size_t capacity = 256; capacity <= MAX_MODULE_PATH_CHARACTERS; capacity *= 2) {
        std::vector<wchar_t> value(capacity);
        const DWORD length = GetModuleFileNameW(module, value.data(), static_cast<DWORD>(capacity));
        if (length == 0) return false;
        if (length >= capacity) continue;
        std::error_code error;
        auto path = std::filesystem::canonical(std::filesystem::path(std::wstring(value.data(), length)), error);
        if (error || !path.is_absolute()) return false;
        out = path.make_preferred();
        return true;
    }
    return false;
}

bool SameModulePath(const std::filesystem::path& left, const std::filesystem::path& right)
{
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

} /// @note namespace

bool ParsePixCaptureOptions(std::span<const std::wstring_view> arguments,
                            PixCaptureOptions& out, std::string& reason)
{
    PixCaptureOptions parsed;
    bool directorySpecified = false;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        if (argument == L"--pix-capture") {
            parsed.requested = true;
            continue;
        }
        if (argument.starts_with(L"--pix-capture=")) {
            reason = "--pix-capture does not accept a value.";
            return false;
        }
        if (argument != L"--pix-path" && !argument.starts_with(L"--pix-path=")) continue;
        if (directorySpecified) {
            reason = "--pix-path may only be specified once.";
            return false;
        }
        directorySpecified = true;
        std::wstring_view value;
        if (argument == L"--pix-path") {
            if (index + 1 == arguments.size()) {
                reason = "--pix-path requires an absolute directory.";
                return false;
            }
            value = arguments[++index];
        } else {
            value = argument.substr(std::wstring_view(L"--pix-path=").size());
        }
        if (value.empty()) {
            reason = "--pix-path requires an absolute directory.";
            return false;
        }
        parsed.installedDirectory = std::filesystem::path(value);
    }
    if (!ValidOptions(parsed, reason)) return false;
    out = std::move(parsed);
    return true;
}

bool ResolvePixCaptureDirectory(const PixCaptureOptions& options,
                                const std::filesystem::path& programFilesDirectory,
                                std::filesystem::path& out, std::string& reason)
{
    if (!ValidOptions(options, reason)) return false;
    std::filesystem::path resolved;
    if (options.requested) {
        if (!options.installedDirectory.empty()) {
            resolved = options.installedDirectory;
        } else {
            if (programFilesDirectory.empty() || !programFilesDirectory.is_absolute()) {
                reason = "Program Files is unavailable; select an absolute --pix-path directory.";
                return false;
            }
            resolved = programFilesDirectory / L"Microsoft PIX" / PIX_CAPTURE_STABLE_VERSION;
        }
        resolved = resolved.lexically_normal();
        resolved.make_preferred();
    }
    out = std::move(resolved);
    reason.clear();
    return true;
}

bool InitializePixCapture(const PixCaptureOptions& options)
{
    if (g_initialized) {
        if (options.requested == g_options.requested && options.installedDirectory == g_options.installedDirectory)
            return g_initializationSucceeded;
        FBZZ_LOG_ERROR("PIX capture startup: startup options cannot change after initialization.");
        return false;
    }
    g_initialized = true;
    g_options = options;
    g_status.requested = options.requested;
    std::string reason;
    if (!ResolvePixCaptureDirectory(options, options.requested && options.installedDirectory.empty()
                                                ? ProgramFilesDirectory() : std::filesystem::path{},
                                   g_status.installedDirectory, reason))
        return FailInitialization(std::move(reason));
    if (!options.requested) {
        g_initializationSucceeded = true;
        return true;
    }

    std::error_code error;
    if (!std::filesystem::is_directory(g_status.installedDirectory, error) || error)
        return FailInitialization("The selected PIX directory does not exist or is inaccessible.");
    const auto requestedPath = g_status.installedDirectory / CAPTURER_FILENAME;
    if (!std::filesystem::is_regular_file(requestedPath, error) || error)
        return FailInitialization("The selected directory does not contain WinPixGpuCapturer.dll.");
    auto capturerPath = std::filesystem::canonical(requestedPath, error);
    if (error || !capturerPath.is_absolute())
        return FailInitialization("Cannot resolve the selected capturer to an absolute path.");
    capturerPath.make_preferred();
    g_status.installedDirectory = capturerPath.parent_path();

    if (const HMODULE existing = GetModuleHandleW(CAPTURER_FILENAME)) {
        std::filesystem::path existingPath;
        if (!CanonicalModulePath(existing, existingPath))
            return FailInitialization("Cannot verify the already-loaded PIX capturer path.");
        if (!SameModulePath(existingPath, capturerPath))
            return FailInitialization("A different PIX capturer is already loaded; it will not be replaced.");
    }

    /// @note Retain the LoadLibrary reference for process lifetime, including a matching preloaded module; no unload can race D3D12 teardown.
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw DLL_LOAD_DIR plus SYSTEM32 excludes CWD, PATH and application-directory dependencies.
    const HMODULE module = LoadLibraryExW(capturerPath.c_str(), nullptr,
                                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return FailInitialization("LoadLibraryExW failed (Win32 error " + std::to_string(GetLastError()) + ").");
    std::filesystem::path actualPath;
    if (!CanonicalModulePath(module, actualPath) || !SameModulePath(actualPath, capturerPath))
        return FailInitialization("The loaded PIX capturer path does not match the selected directory.");

    g_status.ready = true;
    g_initializationSucceeded = true;
    FBZZ_LOG_INFO("PIX capture startup: ready before D3D12 initialization (directory=%s)",
                  util::StringUtils::PathToUtf8(g_status.installedDirectory).c_str());
    return true;
}

const PixCaptureStatus& GetPixCaptureStatus()
{
    return g_status;
}

} /// @note namespace fbzz::core
