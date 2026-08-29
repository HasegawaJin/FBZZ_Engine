/// @file    DependencyViewPanel.cpp
/// @brief   選択アセットへの参照元を一覧表示する依存関係ビューパネル。
/// @author  Hasegawa Jin
/// @date    2026-06-16
///
/// WHY 探索キーが guid か (不具合修正):
/// 以前はファイル名とステムの部分一致で探していた。ステム一致は "Player" が
/// "PlayerHUD.mat" や UIText の文言にも当たるため誤検出し、逆に参照が guid:
/// でエンコードされているファイルはファイル名を含まないので取りこぼしていた。
/// AssetDatabase が guid ⇄ パスの索引を持っているのだから、32 桁 hex を直接
/// 探すのが正確でも速くもある。guid 化前の生パス参照は、ステムではなく
/// 「プロジェクト相対パス丸ごと」の一致で拾う。
#include <Editor/Panels/DependencyViewPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <string_view>

namespace fbzz::editor {

namespace {

// 参照を書き込みうるファイル種別。ここに無い拡張子は中身を読まない。
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

// 絶対パスを "Assets/..." 形式へ落とす。プロジェクト外なら空文字列。
std::string ToProjectRelative(const std::string& absPath)
{
    const std::string root = util::StringUtils::ToLower(asset::AssetDatabase::ProjectRoot());
    if (root.empty()) return {};
    const std::string lower = util::StringUtils::ToLower(
        util::FileSystem::NormalizePathSeparators(absPath));
    if (lower.rfind(root, 0) != 0) return {};
    return lower.substr(root.size());
}

} // namespace

void DependencyViewPanel::Scan(const std::string& assetPath, const std::string& rootPath)
{
    m_scannedPath = assetPath;
    m_scannedGuid.clear();
    m_results.clear();
    m_scanning = false;
    if (assetPath.empty() || rootPath.empty()) return;

    // WHY 解決を挟むか: baked サブアセット (.anim/.mat) は Asset Browser 上では
    //     論理パス "Assets/.../Foo/Foo.anim" で選ばれるが、実体は Library/Baked にあり
    //     guid もそちらに索引されている。生パスのまま引くと必ず空振りする。
    const std::string resolved = asset::AssetManager::ResolveAssetPath(assetPath);
    const std::string target   = resolved.empty() ? assetPath : resolved;

    // 参照系なので .meta を新規発行しない。未 Import の FBX には guid が無く、
    // その場合はパス一致だけで探す。
    m_scannedGuid = util::StringUtils::ToLower(
        asset::AssetDatabase::TryGetGuidFromPath(target));
    const std::string relative = ToProjectRelative(target);
    if (m_scannedGuid.empty() && relative.empty()) return;

    const std::string selfKey = util::StringUtils::ToLower(
        util::FileSystem::NormalizePathSeparators(assetPath));

    for (const auto& p : util::FileSystem::ListFilesRecursive(
             util::FileSystem::PathFromUtf8(rootPath))) {
        const std::string scanPath = util::FileSystem::NormalizePathSeparators(
            util::FileSystem::PathToUtf8(p));
        const std::string ext = util::StringUtils::ToLower(
            util::FileSystem::GetExtension(scanPath));
        if (!IsReferrerExt(ext)) continue;
        // 自分自身は参照元に数えない (.mat が自分の guid をヒントに持つ等)。
        if (util::StringUtils::ToLower(scanPath) == selfKey) continue;

        std::string content;
        if (!util::FileSystem::ReadText(scanPath, content)) continue;
        const std::string lower = util::StringUtils::ToLower(content);

        // guid が最優先。32 桁 hex はファイル内で他の意味を持たないので誤検出しない。
        if (!m_scannedGuid.empty() && lower.find(m_scannedGuid) != std::string::npos) {
            m_results.push_back({ scanPath, true });
            continue;
        }
        // 未エンコードの生パス参照。相対パス丸ごとで照合する
        // (ステム一致は "Player" が別アセット名や UI 文言にも当たるため使わない)。
        if (!relative.empty() && lower.find(relative) != std::string::npos)
            m_results.push_back({ scanPath, false });
    }
}

void DependencyViewPanel::OnRenderContent(EditorContext& ctx)
{
    const std::string& cur = ctx.selectedAssetPath;
    if (!cur.empty() && cur != m_scannedPath) {
        m_scannedPath = cur;
        m_scanning    = true;
    }

    const std::string assetsRoot =
        ctx.projectRoot.empty() ? "Assets" : ctx.projectRoot + "/Assets";
    if (m_scanning)
        Scan(cur, assetsRoot);

    if (m_scannedPath.empty()) {
        ImGui::TextDisabled("Select an asset in the Asset Browser to view its references.");
        return;
    }

    const std::string name = util::FileSystem::GetFilename(m_scannedPath);
    ImGui::TextUnformatted(name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu ref(s))", m_results.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh"))
        Scan(m_scannedPath, assetsRoot);

    // guid が無いアセットは「移動したら参照が切れる」状態。黙って劣化させず明示する。
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
        // パス参照は移動・リネームで切れる。一覧の中で見分けられるようにする。
        if (!hit.byGuid) {
            ImGui::SameLine();
            ImGui::TextColored({ 0.95f, 0.7f, 0.25f, 1.0f }, "(path)");
        }
    }
    ImGui::EndChild();
}

} // namespace fbzz::editor
