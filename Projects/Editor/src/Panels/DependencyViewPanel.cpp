/// @file    DependencyViewPanel.cpp
/// @brief   選択アセットへの参照元を一覧表示する依存関係ビューパネル。
/// @author  Hasegawa Jin
/// @date    2026-06-16
#include <Editor/Panels/DependencyViewPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/EditorIcons.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <chrono>
#include <filesystem>
#include <string_view>

namespace fbzz::editor {

namespace {

/// @note 参照を書き込みうるファイル種別。ここに無い拡張子は中身を読まない。
constexpr std::string_view kReferrerExts[] = {
    ".scene", ".prefab", ".mat", ".animcontroller", ".animctrl",
    ".vfx", ".sequence", ".behaviortree", ".terrain", ".fzdata", ".ibl",
};

bool IsReferrerExt(std::string_view ext)
{
    for (const auto e : kReferrerExts)
        if (ext == e) return true;
    return false;
}

/// @note 絶対パスを "Assets/..." 形式へ落とす。プロジェクト外なら空文字列。
std::string ToProjectRelative(const std::string& absPath)
{
    const std::string root = util::StringUtils::ToLower(asset::AssetDatabase::ProjectRoot());
    if (root.empty()) return {};
    const std::string lower = util::StringUtils::ToLower(
        util::FileSystem::NormalizePathSeparators(absPath));
    if (lower.rfind(root, 0) != 0) return {};
    return lower.substr(root.size());
}

} /// @note namespace

DependencyViewPanel::~DependencyViewPanel()
{
    if (m_cancel) m_cancel->store(true, std::memory_order_relaxed);
}

void DependencyViewPanel::RequestScan(const std::string& assetPath, const std::string& rootPath)
{
    if (m_cancel) m_cancel->store(true, std::memory_order_relaxed);
    m_scannedPath = assetPath;
    m_rootPath = rootPath;
    m_scannedGuid.clear();
    m_results.clear();
    m_pending = !assetPath.empty() && !rootPath.empty();
}

void DependencyViewPanel::UpdateScan()
{
    if (m_scan.valid()) {
        if (m_scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto hits = m_scan.get();
        if (!m_cancel->load(std::memory_order_relaxed)) m_results = std::move(hits);
        m_cancel.reset();
    }
    if (!m_pending) return;
    m_pending = false;

    /// @note baked サブアセット (.anim/.mat) は論理パスで選ばれるが実体は Library/Baked にあり、
    /// @note guid もそちらに索引される。AssetDatabase へのアクセスは UI スレッドで完結する。
    const std::string resolved = asset::AssetManager::ResolveAssetPath(m_scannedPath);
    const std::string target   = resolved.empty() ? m_scannedPath : resolved;

    /// @note 参照系なので .meta を新規発行しない。未 Import の FBX には guid が無く、
    /// @note その場合はパス一致だけで探す。
    m_scannedGuid = util::StringUtils::ToLower(
        asset::AssetDatabase::TryGetGuidFromPath(target));
    const std::string relative = ToProjectRelative(target);
    if (m_scannedGuid.empty() && relative.empty()) return;

    m_cancel = std::make_shared<std::atomic_bool>(false);
    const std::string selfKey = util::StringUtils::ToLower(
        util::FileSystem::NormalizePathSeparators(m_scannedPath));
    /// @note パネルを捕捉せず、一度に一件だけ走らせる。選択切替で古い結果は公開しない。
    m_scan = TaskSystem::Submit([root = m_rootPath, guid = m_scannedGuid, relative, selfKey, cancel = m_cancel] {
        std::vector<RefHit> results;
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::recursive_directory_iterator it(util::FileSystem::PathFromUtf8(root),
            fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end && !cancel->load(std::memory_order_relaxed); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string path = util::FileSystem::NormalizePathSeparators(util::FileSystem::PathToUtf8(it->path()));
            const auto extension = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));
            if (!IsReferrerExt(extension) || util::StringUtils::ToLower(path) == selfKey) continue;
            std::string content;
            if (!util::FileSystem::ReadText(path, content)) continue;
            const auto lower = util::StringUtils::ToLower(content);
            const bool byGuid = !guid.empty() && lower.find(guid) != std::string::npos;
            if (byGuid || (!relative.empty() && lower.find(relative) != std::string::npos))
                results.push_back({path, byGuid});
        }
        return results;
    });
}

void DependencyViewPanel::OnRenderContent(EditorContext& ctx)
{
    const std::string& cur = ctx.selectedAssetPath;
    const std::string assetsRoot =
        ctx.projectRoot.empty() ? "Assets" : ctx.projectRoot + "/Assets";
    if (cur != m_scannedPath || assetsRoot != m_rootPath) RequestScan(cur, assetsRoot);
    UpdateScan();

    if (m_scannedPath.empty()) {
        widgets::EmptyState(icons::Or(icons::kLink, nullptr),
                            LOCT("No asset selected"),
                            LOCT("Pick an asset in the Asset Browser to see what points at it."));
        return;
    }

    const std::string name = util::FileSystem::GetFilename(m_scannedPath);
    ImGui::TextUnformatted(name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu ref(s))", m_results.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) RequestScan(cur, assetsRoot);
    if (m_pending || m_scan.valid()) {
        ImGui::TextDisabled("Scanning saved references...");
        return;
    }

    /// @note guid が無いアセットは「移動したら参照が切れる」状態。黙って劣化させず明示する。
    if (m_scannedGuid.empty())
        ImGui::TextColored({ 0.95f, 0.7f, 0.25f, 1.0f },
                           "No GUID yet - matched by path only.");
    else
        ImGui::TextDisabled("guid: %s", m_scannedGuid.c_str());

    ImGui::Separator();

    if (m_results.empty()) {
        ImGui::TextDisabled("No references found in scenes, prefabs, materials, "
                            "animation controllers, sequences, VFX graphs, or terrains.");
        return;
    }

    ImGui::BeginChild("##depview_list", { 0.0f, ImGui::GetContentRegionAvail().y }, false);
    for (const auto& hit : m_results) {
        const std::string label = util::FileSystem::GetFilename(hit.path);
        const std::string ext   = util::StringUtils::ToLower(
            util::FileSystem::GetExtension(hit.path));

        const char* badge      = "?";
        ImVec4      badgeColor = { 0.5f, 0.5f, 0.5f, 1.0f };
        if      (ext == ".scene")          { badge = "SC"; badgeColor = { 0.3f,  0.6f,  1.0f,  1.0f }; }
        else if (ext == ".mat")            { badge = "MT"; badgeColor = { 0.5f,  0.9f,  0.4f,  1.0f }; }
        else if (ext == ".prefab")         { badge = "PF"; badgeColor = { 0.9f,  0.7f,  0.3f,  1.0f }; }
        else if (ext == ".animcontroller"
              || ext == ".animctrl")       { badge = "AN"; badgeColor = { 0.8f,  0.4f,  0.9f,  1.0f }; }
        else if (ext == ".vfx")            { badge = "VX"; badgeColor = { 0.95f, 0.35f, 0.55f, 1.0f }; }
        else if (ext == ".sequence")       { badge = "SQ"; badgeColor = { 0.85f, 0.6f,  0.3f,  1.0f }; }
        else if (ext == ".behaviortree")   { badge = "AI"; badgeColor = { 0.45f, 0.8f,  0.65f, 1.0f }; }
        else if (ext == ".terrain")        { badge = "TR"; badgeColor = { 0.35f, 0.7f,  0.3f,  1.0f }; }

        ImGui::TextColored(badgeColor, "%s", badge);
        ImGui::SameLine();
        ImGui::PushID(hit.path.c_str());
        if (ImGui::Selectable(label.c_str()))
            SelectAsset(ctx, hit.path);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s", hit.path.c_str(),
                              hit.byGuid ? "matched by guid"
                                         : "matched by path (not GUID-encoded yet)");
        ImGui::PopID();
        /// @note パス参照は移動・リネームで切れる。一覧の中で見分けられるようにする。
        if (!hit.byGuid) {
            ImGui::SameLine();
            ImGui::TextColored({ 0.95f, 0.7f, 0.25f, 1.0f }, "(path)");
        }
    }
    ImGui::EndChild();
}

} /// @note namespace fbzz::editor
