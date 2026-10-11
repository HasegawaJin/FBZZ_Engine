/// @file    AssetBrowserExplorer.cpp
/// @brief   AssetBrowser のパス解決と Windows Explorer の起動。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include "AssetBrowserExplorer.hpp"
#include <Editor/Util/Toast.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <system_error>

namespace fbzz::editor {
namespace {

/// @note 既存 apartment は保持し、S_FALSE を含む成功呼び出しの分だけ解除する。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex CoInitializeEx の初期化と解除。
class ExplorerComScope {
public:
    ExplorerComScope() : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ExplorerComScope() { if (SUCCEEDED(m_result)) CoUninitialize(); }
    ExplorerComScope(const ExplorerComScope&) = delete;
    ExplorerComScope& operator=(const ExplorerComScope&) = delete;
    HRESULT Result() const { return m_result; }

private:
    HRESULT m_result;
};

struct ExplorerPidlDeleter {
    /// @note unique_ptr の所有ポインターにも SDK の __unaligned 修飾子を保持する。
    /// @see https://learn.microsoft.com/en-us/cpp/cpp/unaligned __unaligned は参照先データの整列を指定する。
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(pointer item) const { CoTaskMemFree(item); }
};

/// @note Shell 失敗はアクセス拒否や削除競合でも起きるため、通知して操作を中止する。
bool ReportExplorerFailure(std::string_view path, const char* reason, unsigned long code = 0)
{
    FBZZ_LOG_WARN("AssetBrowser: Reveal in Explorer failed [%s]: %s (0x%08lX)",
                  std::string(path).c_str(), reason, code);
    Toast::Error(std::string("Reveal in Explorer: ") + reason);
    return false;
}

} /// @note anonymous namespace

AssetBrowserExplorerTarget ResolveAssetBrowserExplorerTarget(
    std::string_view projectRoot, std::string_view assetPath)
{
    if (assetPath.empty() || assetPath.find('\0') != std::string_view::npos) return {};
    std::string diskPath(assetPath);
    const bool isGuid = asset::AssetDatabase::IsGuidRef(assetPath);
    if (isGuid) {
        diskPath = asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(assetPath));
        if (diskPath.empty()) return {};
    } else {
        for (const std::string_view suffix : { "::mesh::", "::sprite::" }) {
            if (const std::size_t offset = diskPath.find(suffix); offset != std::string::npos)
                diskPath.resize(offset);
        }
    }

    std::filesystem::path path = util::FileSystem::PathFromUtf8(diskPath);
    if (path.empty() || (path.has_root_path() && !path.is_absolute())) return {};
    if (!path.is_absolute()) {
        if (projectRoot.empty()) return {};
        path = util::FileSystem::PathFromUtf8(std::string(projectRoot)) / path;
    }
    std::error_code error;
    path = std::filesystem::absolute(path, error).lexically_normal();
    if (error) return {};
    if (!isGuid && !util::FileSystem::Exists(path)) {
        /// @note 旧論理パスの生成物は既存の import 解決規則で Library/Baked の実体を探す。
        diskPath = asset::AssetManager::ResolveAssetPath(util::FileSystem::PathToUtf8(path));
        if (diskPath.empty()) return {};
        path = util::FileSystem::PathFromUtf8(diskPath);
    }
    const auto status = std::filesystem::status(path, error);
    if (error || (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)))
        return {};
    return { util::FileSystem::PathToUtf8(path), std::filesystem::is_directory(status) };
}

bool RevealAssetBrowserInExplorer(std::string_view projectRoot, std::string_view assetPath)
{
    const AssetBrowserExplorerTarget target = ResolveAssetBrowserExplorerTarget(projectRoot, assetPath);
    if (target.path.empty())
        return ReportExplorerFailure(assetPath, "Target is missing or cannot be resolved");

    const ExplorerComScope com;
    if (FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE)
        return ReportExplorerFailure(target.path, "COM initialization failed", static_cast<unsigned long>(com.Result()));

    const std::wstring path = util::FileSystem::PathFromUtf8(target.path).make_preferred().wstring();
    if (target.isDirectory) {
        /// @see https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecuteexw ShellExecuteExW のフォルダー起動と失敗判定。
        SHELLEXECUTEINFOW info{};
        info.cbSize = static_cast<DWORD>(sizeof(info));
        info.fMask = SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"explore";
        info.lpFile = path.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info))
            return ReportExplorerFailure(target.path, "Could not open folder", GetLastError());
        return true;
    }

    /// @see https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shparsedisplayname SHParseDisplayName の Unicode パスから PIDL への変換。
    PIDLIST_ABSOLUTE rawItem = nullptr;
    HRESULT result = SHParseDisplayName(path.c_str(), nullptr, &rawItem, 0, nullptr);
    const std::unique_ptr<ITEMIDLIST_ABSOLUTE, ExplorerPidlDeleter> item(rawItem);
    if (FAILED(result))
        return ReportExplorerFailure(target.path, "Could not resolve file in Windows Shell", static_cast<unsigned long>(result));
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shopenfolderandselectitems cidl=0 は指定項目の親フォルダーを開いてその項目を選択する。
    result = SHOpenFolderAndSelectItems(item.get(), 0, nullptr, 0);
    if (FAILED(result))
        return ReportExplorerFailure(target.path, "Could not select file in Explorer", static_cast<unsigned long>(result));
    return true;
}

} /// @note namespace fbzz::editor
