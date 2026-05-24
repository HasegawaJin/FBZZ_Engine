// FBZZ Engine
// AssetBrowserPanel.cpp | fbzz::editor
// Unity スタイルの2ペインアセットブラウザ
#include <Editor/Panels/AssetBrowserPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/SceneSerializer.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <imgui.h>
#include <algorithm>

namespace fbzz::editor {

namespace {

ImVec4 Lighten(ImVec4 c) {
    return { std::min(c.x + 0.15f, 1.0f), std::min(c.y + 0.15f, 1.0f),
             std::min(c.z + 0.15f, 1.0f), c.w };
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

AssetBrowserPanel::AssetBrowserPanel(const std::string& rootPath)
    : m_rootPath(rootPath), m_currentPath(rootPath) {}

void AssetBrowserPanel::OnInit(EditorContext&) { RefreshDirectory(); }

void AssetBrowserPanel::RefreshDirectory()
{
    m_entries.clear();
    for (const auto& p : util::FileSystem::ListAll(m_currentPath)) {
        Entry e;
        e.path  = p;
        e.name  = util::FileSystem::GetFilename(p);
        e.ext   = util::StringUtils::ToLower(util::FileSystem::GetExtension(p));
        e.isDir = util::FileSystem::IsDirectory(p);
        m_entries.push_back(std::move(e));
    }
    std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir; // dirs first
        return a.name < b.name;
    });
}

ImVec4 AssetBrowserPanel::EntryColor(const Entry& e)
{
    if (e.isDir)                                          return { 0.80f, 0.60f, 0.10f, 1.0f };
    if (e.ext == ".hlsl" || e.ext == ".hlsli")            return { 0.15f, 0.65f, 0.25f, 1.0f };
    if (e.ext == ".png"  || e.ext == ".jpg" ||
        e.ext == ".dds"  || e.ext == ".bmp" || e.ext == ".tga")
                                                          return { 0.15f, 0.40f, 0.80f, 1.0f };
    if (e.ext == ".fbx"  || e.ext == ".obj" ||
        e.ext == ".gltf" || e.ext == ".glb")              return { 0.80f, 0.45f, 0.10f, 1.0f };
    if (e.ext == ".fbzz")                                 return { 0.60f, 0.15f, 0.70f, 1.0f };
    if (e.ext == ".toml" || e.ext == ".json")             return { 0.65f, 0.65f, 0.10f, 1.0f };
    if (e.ext == ".wav"  || e.ext == ".mp3" || e.ext == ".ogg")
                                                          return { 0.70f, 0.20f, 0.50f, 1.0f };
    return { 0.38f, 0.38f, 0.38f, 1.0f };
}

const char* AssetBrowserPanel::EntryLabel(const Entry& e)
{
    if (e.isDir)                                          return "DIR";
    if (e.ext == ".hlsl" || e.ext == ".hlsli")            return "HLSL";
    if (e.ext == ".png"  || e.ext == ".jpg" ||
        e.ext == ".dds"  || e.ext == ".bmp" || e.ext == ".tga")
                                                          return "TEX";
    if (e.ext == ".fbx"  || e.ext == ".obj" ||
        e.ext == ".gltf" || e.ext == ".glb")              return "MESH";
    if (e.ext == ".fbzz")                                 return "SCENE";
    if (e.ext == ".toml")                                 return "TOML";
    if (e.ext == ".wav"  || e.ext == ".mp3" || e.ext == ".ogg")
                                                          return "SFX";
    return "FILE";
}

// ─── フォルダツリー (左ペイン) ───────────────────────────────────────────────

void AssetBrowserPanel::DrawFolderTree(const std::string& dirPath)
{
    for (const auto& p : util::FileSystem::ListAll(dirPath)) {
        if (!util::FileSystem::IsDirectory(p)) continue;

        std::string name = util::FileSystem::GetFilename(p);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (p == m_currentPath) flags |= ImGuiTreeNodeFlags_Selected;

        bool open = ImGui::TreeNodeEx(p.c_str(), flags, "%s", name.c_str());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
            m_currentPath = p;
            RefreshDirectory();
        }
        if (open) {
            DrawFolderTree(p);
            ImGui::TreePop();
        }
    }
}

// ─── アイコン1個 (右ペイン) ──────────────────────────────────────────────────

void AssetBrowserPanel::DrawEntry(const Entry& e, EditorContext& ctx)
{
    ImGui::PushID(e.path.c_str());

    const ImVec4 base    = EntryColor(e);
    const ImU32  cFill   = ImGui::ColorConvertFloat4ToU32(base);
    const ImU32  cHov    = ImGui::ColorConvertFloat4ToU32(Lighten(base));
    const ImU32  cDark   = ImGui::ColorConvertFloat4ToU32(
        { base.x * 0.50f, base.y * 0.50f, base.z * 0.50f, 1.0f });

    const ImVec2 origin  = ImGui::GetCursorScreenPos();
    const float  sz      = m_iconSize;

    ImGui::InvisibleButton("##icon", { sz, sz });
    const bool hov = ImGui::IsItemHovered();
    const ImU32 fill = hov ? cHov : cFill;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (e.isDir) {
        // フォルダ形状: タブ (左上) + 本体
        const float tabW  = sz * 0.48f;
        const float tabH  = sz * 0.14f;
        const float bodyY = origin.y + tabH;
        const float bodyH = sz * 0.82f;
        const float r     = sz * 0.07f;

        dl->AddRectFilled({ origin.x, origin.y }, { origin.x + tabW, bodyY + r }, fill, r);
        dl->AddRectFilled({ origin.x, bodyY },    { origin.x + sz,   origin.y + bodyH }, fill, r);
        dl->AddRect(      { origin.x, bodyY },    { origin.x + sz,   origin.y + bodyH }, cDark, r, 0, 1.0f);
    } else {
        // ファイル形状: 右上に折り角
        const float bodyH = sz * 0.85f;
        const float dog   = sz * 0.22f;

        ImVec2 pts[5] = {
            { origin.x,        origin.y       },
            { origin.x+sz-dog, origin.y       },
            { origin.x+sz,     origin.y+dog   },
            { origin.x+sz,     origin.y+bodyH },
            { origin.x,        origin.y+bodyH },
        };
        dl->AddConvexPolyFilled(pts, 5, fill);
        dl->AddPolyline(pts, 5, cDark, ImDrawFlags_Closed, 1.0f);

        // 折り角の三角形 (影)
        ImVec2 tri[3] = {
            { origin.x+sz-dog, origin.y     },
            { origin.x+sz,     origin.y+dog },
            { origin.x+sz-dog, origin.y+dog },
        };
        dl->AddConvexPolyFilled(tri, 3, cDark);

        // 拡張子ラベルをアイコン中央に表示
        const char* lbl  = EntryLabel(e);
        const ImVec2 tsz = ImGui::CalcTextSize(lbl);
        dl->AddText({ origin.x + (sz - tsz.x) * 0.5f,
                      origin.y + bodyH * 0.52f - tsz.y * 0.5f },
                    IM_COL32(255, 255, 255, 220), lbl);
    }

    // 選択ハイライト (FBX のみ)
    const bool isFbxSelected = (e.path == m_selectedFbxPath);
    if (isFbxSelected) {
        ImGui::GetWindowDrawList()->AddRect(
            origin, { origin.x + sz, origin.y + sz * 0.85f },
            IM_COL32(255, 200, 80, 220), 3.0f, 0, 2.0f);
    }

    // ドラッグソース (ファイルのみ)
    if (!e.isDir && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("ASSET_PATH", e.path.c_str(), e.path.size() + 1);
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::EndDragDropSource();
    }

    if (hov) ImGui::SetTooltip("%s", e.path.c_str());

    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const bool isMesh = (e.ext == ".fbx" || e.ext == ".obj" ||
                             e.ext == ".gltf" || e.ext == ".glb");
        if (!e.isDir && isMesh) {
            // FBX をシングルクリック → 内容を非同期ロード
            if (m_selectedFbxPath != e.path) {
                m_selectedFbxPath = e.path;
                m_selectedModel.reset();
                if (auto* res = renderer::ResourceManager::Active())
                    m_selectedModel = asset::AssetManager::Load<asset::Model>(e.path);
            }
        }
    }

    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (e.isDir) {
            m_pendingNavigate = e.path;
        } else if (e.ext == ".fbzz" && ctx.activeScene) {
            if (SceneSerializer::Load(*ctx.activeScene, e.path)) {
                ctx.selectedEntities.clear();
                FBZZ_LOG_INFO("Opened scene: %s", e.path.c_str());
            } else {
                FBZZ_LOG_ERROR("Failed to open scene: %s", e.path.c_str());
            }
        }
    }

    // ファイル名 (アイコン幅に収まるよう末尾省略、中央揃え)
    std::string display = e.name;
    while (display.size() > 2 &&
           ImGui::CalcTextSize(display.c_str()).x + ImGui::CalcTextSize("..").x > m_iconSize)
        display.pop_back();
    if (display.size() < e.name.size()) display += "..";

    float indent = (m_iconSize - ImGui::CalcTextSize(display.c_str()).x) * 0.5f;
    if (indent > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
    ImGui::TextUnformatted(display.c_str());

    ImGui::PopID();
}

// ─── FBX 内容プレビュー (サブアセットアイコン) ────────────────────────────────

void AssetBrowserPanel::DrawFbxContents()
{
    if (m_selectedFbxPath.empty()) return;

    ImGui::Separator();
    ImGui::TextDisabled("  %s", util::FileSystem::GetFilename(m_selectedFbxPath).c_str());
    ImGui::Spacing();

    if (!m_selectedModel) {
        ImGui::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "Failed to load");
        return;
    }

    const auto& model = *m_selectedModel;
    const bool hasMesh = !model.meshes.empty();
    const bool hasClip = !model.clips.empty();
    if (!hasMesh && !hasClip) {
        ImGui::TextDisabled("  (no meshes or clips)");
        return;
    }

    // アイコン描画ラムダ (DrawEntry と同じスタイル)
    const float padding = 8.0f;
    const float avail   = ImGui::GetContentRegionAvail().x;
    const int   cols    = std::max(1, (int)(avail / (m_iconSize + padding)));
    int col     = 0;
    int iconIdx = 0;

    auto drawSubIcon = [&](const char* label, const char* displayName,
                           const char* tooltip, ImVec4 color,
                           const std::string& payload)
    {
        if (col > 0 && (col % cols) != 0) ImGui::SameLine(0.0f, padding);

        char uid[64];
        std::snprintf(uid, sizeof(uid), "##sub%d", iconIdx++);

        ImGui::BeginGroup();
        ImGui::PushID(uid);

        const ImU32 cFill = ImGui::ColorConvertFloat4ToU32(color);
        const ImU32 cHov  = ImGui::ColorConvertFloat4ToU32(Lighten(color));
        const ImU32 cDark = ImGui::ColorConvertFloat4ToU32(
            { color.x * 0.5f, color.y * 0.5f, color.z * 0.5f, 1.0f });

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float  sz     = m_iconSize;

        ImGui::InvisibleButton("##icon", { sz, sz });
        const bool   hov  = ImGui::IsItemHovered();
        const ImU32  fill = hov ? cHov : cFill;

        ImDrawList* dl    = ImGui::GetWindowDrawList();
        const float bodyH = sz * 0.85f;
        const float dog   = sz * 0.22f;

        ImVec2 pts[5] = {
            { origin.x,        origin.y       },
            { origin.x+sz-dog, origin.y       },
            { origin.x+sz,     origin.y+dog   },
            { origin.x+sz,     origin.y+bodyH },
            { origin.x,        origin.y+bodyH },
        };
        dl->AddConvexPolyFilled(pts, 5, fill);
        dl->AddPolyline(pts, 5, cDark, ImDrawFlags_Closed, 1.0f);

        ImVec2 tri[3] = {
            { origin.x+sz-dog, origin.y     },
            { origin.x+sz,     origin.y+dog },
            { origin.x+sz-dog, origin.y+dog },
        };
        dl->AddConvexPolyFilled(tri, 3, cDark);

        const ImVec2 tsz = ImGui::CalcTextSize(label);
        dl->AddText({ origin.x + (sz - tsz.x) * 0.5f,
                      origin.y + bodyH * 0.52f - tsz.y * 0.5f },
                    IM_COL32(255, 255, 255, 220), label);

        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("ASSET_PATH", payload.c_str(), payload.size() + 1);
            ImGui::Text("%s: %s", label,
                util::FileSystem::GetFilename(payload).c_str());
            ImGui::EndDragDropSource();
        }

        if (hov) ImGui::SetTooltip("%s", tooltip);

        // 名前テキスト (省略、中央揃え)
        std::string disp(displayName);
        const std::string full(displayName);
        while (disp.size() > 2 &&
               ImGui::CalcTextSize(disp.c_str()).x + ImGui::CalcTextSize("..").x > sz)
            disp.pop_back();
        if (disp.size() < full.size()) disp += "..";

        const float ind = (sz - ImGui::CalcTextSize(disp.c_str()).x) * 0.5f;
        if (ind > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ind);
        ImGui::TextUnformatted(disp.c_str());

        ImGui::PopID();
        ImGui::EndGroup();
        ++col;
    };

    // ── MESH アイコン ──
    if (hasMesh) {
        char tip[256];
        std::snprintf(tip, sizeof(tip), "%zu mesh(es)%s\nDrag → Skinned Mesh Renderer",
            model.meshes.size(),
            model.skeleton
                ? (std::string(" + skeleton (") +
                   std::to_string(model.skeleton->bones.size()) + " bones)").c_str()
                : "");
        drawSubIcon("MESH", "Mesh", tip, { 0.80f, 0.45f, 0.10f, 1.0f }, m_selectedFbxPath);
    }

    // ── ANIM アイコン (クリップ1件につき1個) ──
    for (const auto& clip : model.clips) {
        const float dur = static_cast<float>(clip.durationTicks /
            (clip.ticksPerSecond > 0.0 ? clip.ticksPerSecond : 30.0));
        char tip[256];
        std::snprintf(tip, sizeof(tip), "%s  (%.2fs)\nDrag → Animator",
            clip.name.c_str(), dur);
        drawSubIcon("ANIM", clip.name.c_str(), tip,
                    { 0.20f, 0.70f, 0.30f, 1.0f }, m_selectedFbxPath);
    }
}

// ─── メインレイアウト ─────────────────────────────────────────────────────────

void AssetBrowserPanel::OnRenderContent(EditorContext& ctx)
{
    // ── 左ペイン: フォルダツリー ─────────────────────────────────────────
    ImGui::BeginChild("##tree", { 150.0f, 0.0f }, true);

    ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_SpanAvailWidth
                                 | ImGuiTreeNodeFlags_DefaultOpen;
    if (m_currentPath == m_rootPath) rootFlags |= ImGuiTreeNodeFlags_Selected;

    bool rootOpen = ImGui::TreeNodeEx("##root", rootFlags, "Assets");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        m_currentPath = m_rootPath;
        RefreshDirectory();
    }
    if (rootOpen) {
        DrawFolderTree(m_rootPath);
        ImGui::TreePop();
    }

    ImGui::EndChild();
    ImGui::SameLine();

    // ── 右ペイン: コンテンツエリア ───────────────────────────────────────
    ImGui::BeginChild("##content", { 0.0f, 0.0f }, false);

    // ナビゲーションバー
    if (m_currentPath != m_rootPath) {
        if (ImGui::SmallButton(" ^ ")) {
            size_t pos = m_currentPath.find_last_of("/\\");
            if (pos != std::string::npos) m_currentPath = m_currentPath.substr(0, pos);
            RefreshDirectory();
        }
        ImGui::SameLine();
    }
    std::string rel = m_currentPath.size() > m_rootPath.size()
        ? m_currentPath.substr(m_rootPath.size() + 1) : "Assets";
    ImGui::TextDisabled("%s", rel.c_str());

    // 検索 + アイコンサイズスライダー + 更新
    ImGui::SetNextItemWidth(-200.0f);
    ImGui::InputText("##search", m_searchBuf.data(), m_searchBuf.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderFloat("##sz", &m_iconSize, 40.0f, 120.0f, "%.0f");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) RefreshDirectory();

    ImGui::Separator();

    // グリッド表示
    std::string filter(m_searchBuf.data());
    const float padding  = 8.0f;
    const float avail    = ImGui::GetContentRegionAvail().x;
    const int   cols     = std::max(1, (int)(avail / (m_iconSize + padding)));

    int col = 0;
    for (const auto& e : m_entries) {
        if (!filter.empty() && !util::StringUtils::ContainsCI(e.name, filter)) continue;

        if (col > 0 && (col % cols) != 0) ImGui::SameLine(0.0f, padding);
        ImGui::BeginGroup();
        DrawEntry(e, ctx);
        ImGui::EndGroup();
        ++col;
    }

    // ループ外でナビゲートを処理
    if (!m_pendingNavigate.empty()) {
        m_currentPath = std::move(m_pendingNavigate);
        m_pendingNavigate.clear();
        RefreshDirectory();
    }

    // 選択 FBX の内容プレビュー
    DrawFbxContents();

    ImGui::EndChild();
}

} // namespace fbzz::editor
