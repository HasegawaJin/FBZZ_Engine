/// @file    PixCapture.hpp
/// @brief   Opt-in PIX startup options and verified early-loading status.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace fbzz::core {

inline constexpr std::wstring_view PIX_CAPTURE_STABLE_VERSION = L"2603.25";

struct PixCaptureOptions {
    bool requested = false;
    /// @note Empty selects the pinned stable release beneath Program Files; explicit paths must be absolute directories.
    std::filesystem::path installedDirectory;
};

struct PixCaptureStatus {
    bool requested = false;
    /// @note True only after the startup loader verifies the selected module, not merely its presence on disk.
    bool ready = false;
    std::string reason;
    /// @note The resolved capture DLL directory; it can be nonempty on a failed request without implying readiness.
    std::filesystem::path installedDirectory;
};

/// @brief Parse already-tokenized PIX arguments without filesystem access or module loading.
/// @note The caller consumes operands of its other options before passing PIX arguments here; unrelated arguments are ignored.
/// @return False leaves out unchanged and provides a reason; success clears reason.
[[nodiscard]] bool ParsePixCaptureOptions(std::span<const std::wstring_view> arguments,
                                         PixCaptureOptions& out, std::string& reason);

/// @brief Resolve the opt-in directory lexically without probing disk, changing process state or loading a module.
/// @note An unrequested option resolves to an empty path even if Program Files is unavailable.
/// @return False leaves out unchanged and provides a reason; success clears reason.
[[nodiscard]] bool ResolvePixCaptureDirectory(const PixCaptureOptions& options,
                                             const std::filesystem::path& programFilesDirectory,
                                             std::filesystem::path& out, std::string& reason);

/// @brief Load only the selected capturer and retain its module reference until process termination.
/// @pre Called on the startup thread before any D3D12 API; the launcher must first select the supported DX12 backend.
/// @note Startup options are immutable after the first call; ordinary startup never loads PIX, and failed startup is not retried.
/// @return False logs the failure and never reports readiness; callers must stop startup before creating a renderer.
/// @see https://devblogs.microsoft.com/pix/taking-a-capture/ Matching WinPixGpuCapturer.dll must precede D3D12 initialization.
[[nodiscard]] bool InitializePixCapture(const PixCaptureOptions& options);

/// @brief Read the process startup proof; no probing, loading, injection or restart is performed.
/// @note Read on the main thread after startup initialization; the referenced status lives until process termination.
[[nodiscard]] const PixCaptureStatus& GetPixCaptureStatus();

} /// @note namespace fbzz::core
