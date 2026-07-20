// FBZZ Engine
// SearchEverythingPanel.cpp | fbzz::editor
// シーン + アセット横断検索パネルの実装
#include <Editor/Panels/SearchEverythingPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <unordered_set>

namespace fbzz::editor {

void SearchEverythingPanel::RebuildAssetIndex(const std::string& projectRoot)
{
    m_assetIndex.clear();
    m_indexedRoot = projectRoot;
    if (projectRoot.empty()) return;

    const std::filesystem::path assetsDir =
        util::FileSystem::PathFromUtf8(projectRoot) / L"Assets";
    if (!util::FileSystem::Exists(assetsDir)) return;

    // 「開きたい」主要アセットのみ索引する (中間/生成物はノイズなので除外)。
    static const std::unordered_set<std::string> kIncludeExt = {
        ".scene", ".prefab", ".mat", ".fbx", ".obj", ".gltf", ".glb",
        ".png", ".jpg", ".jpeg", ".tga", ".dds", ".hdr",
        ".hlsl", ".hlsli", ".hpp", ".cpp", ".h", ".cs", ".lua",
        ".anim", ".animcontroller", ".vfx", ".skel",
        ".wav", ".mp3", ".ogg", ".terrain", ".asset", ".fzdata",
    };
    for (const auto& p : util::FileSystem::ListFilesRecursive(assetsDir)) {
        const std::string ext = util::StringUtils::ToLower(
            util::FileSystem::PathToUtf8(p.extension()));
        if (kIncludeExt.find(ext) == kIncludeExt.end()) continue;
        m_assetIndex.push_back(util::FileSystem::PathToUtf8(p));
        if (m_assetIndex.size() >= 5000) break; // 暴走防止の上限
    }
}

void SearchEverythingPanel::OnBeforeBegin(EditorContext&)
{
    if (m_requestFocus) {
        ImGui::SetNextWindowFocus();
        m_requestFocus = false;
    }
}

void SearchEverythingPanel::OnRenderContent(EditorContext& ctx)
{
    // 初回、または projectRoot が変わったら索引を作り直す。手動更新も可能。
    if (m_indexedRoot != ctx.projectRoot)
        RebuildAssetIndex(ctx.projectRoot);

    // ── 検索欄 + 更新ボタン ─────────────────────────────────────────────────
    if (m_refocusInput) {
        ImGui::SetKeyboardFocusHere();
        m_refocusInput = false;
    }
    const float btnW = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##search_all", "Search objects and assets...",
                             m_query, sizeof(m_query));
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        RebuildAssetIndex(ctx.projectRoot);

    const std::string query = m_query;
    if (query.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Type to search across the scene and assets.");
        ImGui::TextDisabled("Objects -> select & focus,  assets -> open / inspect.");
        return;
    }

    ImGui::Separator();
    ImGui::BeginChild("##results");

    // ── GameObject 検索 (名前一致) ──────────────────────────────────────────
    if (ctx.activeScene) {
        std::vector<scene::GameObject*> hits;
        for (auto& go : ctx.activeScene->GameObjects()) {
            if (util::StringUtils::ContainsCI(go.name, query))
                hits.push_back(&go);
            if (hits.size() >= 100) break;
        }
        if (!hits.empty()) {
            ImGui::SeparatorText("Objects");
            for (scene::GameObject* go : hits) {
                ImGui::PushID(go);
                if (ImGui::Selectable(go->name.c_str())) {
                    ctx.selectedEntities       = { go->GetID() };
                    ctx.focusTargetPosition    = go->transform.worldPosition;
                    ctx.focusTargetRadius      = 0.0f;
                    ctx.requestFocusOnSelected = true;
                }
                ImGui::PopID();
            }
        }
    }

    // ── アセット検索 (ファイル名一致) ───────────────────────────────────────
    {
        int shown = 0;
        bool headerDrawn = false;
        for (const std::string& path : m_assetIndex) {
            const std::string name = util::FileSystem::GetFilename(path);
            if (!util::StringUtils::ContainsCI(name, query)) continue;
            if (!headerDrawn) { ImGui::SeparatorText("Assets"); headerDrawn = true; }

            ImGui::PushID(path.c_str());
            if (ImGui::Selectable(name.c_str())) {
                const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));
                if (ext == ".scene") {
                    if (ctx.requestOpenScene) ctx.requestOpenScene(path);
                } else if (ext == ".animcontroller") {
                    ctx.selectedAssetPath = path;
                    ctx.requestOpenAnimationGraph = true;
                } else if (ext == ".vfx") {
                    ctx.selectedAssetPath = path;
                    ctx.requestOpenVFXEditor = true;
                } else {
                    ctx.selectedAssetPath = path; // Inspector にアセットを表示
                }
            }
            // フルパスをツールチップで示し、同名ファイルの区別を助ける。
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", path.c_str());
            ImGui::PopID();

            if (++shown >= 200) {
                ImGui::TextDisabled("... more results hidden, refine your query");
                break;
            }
        }
    }

    ImGui::EndChild();
}

} // namespace fbzz::editor
