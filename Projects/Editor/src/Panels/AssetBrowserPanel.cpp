// FBZZ Engine
// AssetBrowserPanel.cpp | fbzz::editor
// AssetBrowserPanel のフォルダツリーとアセットグリッド描画
#include "AssetBrowser/AssetBrowserCommon.hpp"

namespace fbzz::editor {

void AssetBrowserPanel::OnRenderContent(EditorContext& ctx)
{
    DrawImportResultBar(ctx);

    // テクスチャ遅延ロードキューを処理 (3件/フレームに分散)
    DrainTexLoadQueue(ctx);

    // ファイルシステムの変化をポーリング
    // WHY: スレッドなしでウォッチャーを動かすため毎フレーム Poll() を呼ぶ。
    //      変化があった場合のみ RefreshDirectory() を実行して描画コストを抑える。
    for (const auto& ev : m_watcher.Poll())
    {
        const bool inCurrentDir = util::FileSystem::IsChildPathText(
            util::FileSystem::NormalizePathSeparators(m_rootPath + ev.path),
            m_currentPath);

        if (ev.type == AssetFileWatcher::EventType::Added   ||
            ev.type == AssetFileWatcher::EventType::Removed ||
            ev.type == AssetFileWatcher::EventType::Renamed)
        {
            if (inCurrentDir) RefreshDirectory();

            // 新規追加ファイルが未変換形式なら PendingImport に積む
            if (ev.type == AssetFileWatcher::EventType::Added)
                TryQueuePendingImport(ev.path);
        }
    }

    UpdateMounts(ctx);

    if (ctx.requestAssetBrowserRefresh) {
        RefreshDirectory();
        ctx.requestAssetBrowserRefresh = false;
    }

    // ── 左ペイン: フォルダツリー ─────────────────────────────────────────
    ImGui::BeginChild("##tree", { 150.0f, 0.0f }, true);

    ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_SpanAvailWidth
                                 | ImGuiTreeNodeFlags_DefaultOpen;
    if (util::FileSystem::SamePathText(m_currentPath, m_rootPath)) rootFlags |= ImGuiTreeNodeFlags_Selected;

    bool rootOpen = ImGui::TreeNodeEx("##root", rootFlags, "Assets");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        m_currentPath = m_rootPath;
        RefreshDirectory();
    }
    // Assets ルートへのドロップ
    if (ImGui::BeginDragDropTarget()) {
        if (SaveHierarchyPayloadAsPrefab(
                ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), ctx, m_rootPath)) {
            RefreshDirectory();
        }
        ImGui::EndDragDropTarget();
    }
    if (rootOpen) {
        DrawFolderTree(m_rootPath, ctx);
        ImGui::TreePop();
    }

    ImGui::EndChild();
    ImGui::SameLine();

    // ── 右ペイン: コンテンツエリア ───────────────────────────────────────
    ImGui::BeginChild("##content", { 0.0f, 0.0f }, false);
    // ディレクトリ移動後にスクロールをトップへ戻す。
    // WHY: 深いディレクトリで下にスクロールした後に親へ戻ると、
    //      前のスクロール位置が残りトップにある Fonts 等が見えなくなる。
    if (m_resetScroll) {
        ImGui::SetScrollY(0.0f);
        m_resetScroll = false;
    }
    const ImVec2 contentMin = ImGui::GetWindowPos();
    const ImVec2 contentMax = {
        contentMin.x + ImGui::GetWindowSize().x,
        contentMin.y + ImGui::GetWindowSize().y
    };
    const ImGuiID contentDropId = ImGui::GetID("##content_drop_target");

    // パンくずリスト
    DrawBreadcrumb(ctx);

    // 未変換ファイルがあれば警告バーを表示
    DrawPendingImportBar(ctx);

    // ツールバー: 検索 / Type フィルタ / Sort / アイコンサイズ / Save Modified / Create / Refresh
    ImGui::SetNextItemWidth(-470.0f);
    ImGui::InputText("##search", m_searchBuf.data(), m_searchBuf.size());
    ImGui::SameLine();

    static constexpr const char* kTypeLabels[] = {
        "All", "Scene", "Material", "Script", "Texture", "Audio", "Mesh", "Shader", "Prefab" };
    ImGui::SetNextItemWidth(80.0f);
    {
        int tf = static_cast<int>(m_typeFilter);
        if (ImGui::Combo("##type", &tf, kTypeLabels, 9))
            m_typeFilter = static_cast<TypeFilter>(tf);
    }
    ImGui::SameLine();

    static constexpr const char* kSortLabels[] = { "Name ^", "Name v", "Type", "Modified" };
    ImGui::SetNextItemWidth(80.0f);
    {
        int sm = static_cast<int>(m_sortMode);
        if (ImGui::Combo("##sort", &sm, kSortLabels, 4)) {
            m_sortMode = static_cast<SortMode>(sm);
            RefreshDirectory();
        }
    }
    ImGui::SameLine();

    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::SliderFloat("##sz", &m_iconSize, 56.0f, 132.0f, "%.0f"))
        ctx.assetBrowserIconSize = m_iconSize;
    ImGui::SameLine();

    if (AssetDirtyRegistry::HasAny()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.38f, 0.05f, 1.0f));
        const int n = static_cast<int>(AssetDirtyRegistry::GetAll().size());
        char btnLabel[32];
        std::snprintf(btnLabel, sizeof(btnLabel), "Save* (%d)", n);
        if (ImGui::SmallButton(btnLabel)) {
            m_showSaveModifiedDialog = true;
            m_saveModifiedSelected.assign(AssetDirtyRegistry::GetAll().size(), true);
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }

    if (ImGui::SmallButton("Create")) ImGui::OpenPopup("##content_ctx");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) RefreshDirectory();
    ImGui::SameLine();
    {
        int files = 0, dirs = 0;
        for (const auto& e : m_entries) { if (e.isDir) ++dirs; else ++files; }
        ImGui::TextDisabled("(%d files, %d dirs)", files, dirs);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(m_viewMode == ViewMode::Grid ? "List" : "Grid"))
        m_viewMode = (m_viewMode == ViewMode::Grid) ? ViewMode::List : ViewMode::Grid;

    DrawSaveModifiedDialog();

    ImGui::Separator();

    std::string filter(m_searchBuf.data());

    if (m_viewMode == ViewMode::List) {
        DrawListView(ctx, filter);
    } else {
        const float padding  = 12.0f;
        const float avail    = ImGui::GetContentRegionAvail().x;
        const int   cols     = std::max(1, (int)(avail / (m_iconSize + padding)));
        const float rowH     = m_iconSize + ImGui::GetTextLineHeightWithSpacing() * 1.6f;

        std::vector<size_t> visIndices;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            const auto& e = m_entries[i];
            if (!filter.empty() && !util::StringUtils::ContainsCI(e.name, filter)) continue;
            if (!PassesTypeFilter(e)) continue;
            visIndices.push_back(i);
        }

        const int totalRows = cols > 0
            ? static_cast<int>((visIndices.size() + cols - 1) / cols)
            : 0;

        ImGuiListClipper clipper;
        clipper.Begin(totalRows, rowH);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int c = 0; c < cols; ++c) {
                    const int idx = row * cols + c;
                    if (idx >= static_cast<int>(visIndices.size())) break;
                    if (c > 0) ImGui::SameLine(0.0f, padding);
                    ImGui::BeginGroup();
                    DrawEntry(m_entries[visIndices[idx]], ctx);
                    ImGui::EndGroup();
                }
            }
        }
        clipper.End();
    }

    // ループ外でナビゲートを処理
    if (!m_pendingNavigate.empty()) {
        m_currentPath = util::FileSystem::NormalizePathSeparators(std::move(m_pendingNavigate));
        m_pendingNavigate.clear();
        RefreshDirectory();
    }

    // fzasset 展開トグル後の遅延 Refresh
    if (m_fzExpandDirty) {
        m_fzExpandDirty = false;
        RefreshDirectory();
    }

    // 右クリック: Create メニュー
    // WHY: BeginPopupContextWindow は OpenPopup と混ぜるとボタン起動が安定しない。
    //      右クリック検出と popup 描画を分け、空白右クリックとツールバー Create を同じ経路にする。
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        !ImGui::IsAnyItemHovered()) {
        ImGui::OpenPopup("##content_ctx");
    }
    if (ImGui::BeginPopup("##content_ctx")) {
        if (ImGui::BeginMenu("Create")) {
            DrawCreateMenu(ctx);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    // ヒエラルキーからエンティティをドロップ → Prefab 化
    if (ImGui::BeginDragDropTargetCustom(ImRect(contentMin, contentMax), contentDropId)) {
        if (SaveHierarchyPayloadAsPrefab(
                ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), ctx, m_currentPath)) {
            RefreshDirectory();
        }
        ImGui::EndDragDropTarget();
    }

    // 選択 FBX の内容プレビュー
    DrawFbxContents(ctx);

    ImGui::EndChild();
}

void AssetBrowserPanel::DrawBreadcrumb(EditorContext&)
{
    // Build list of (label, absPath) from root down to m_currentPath
    struct Crumb { std::string label; std::string path; };
    std::vector<Crumb> crumbs;

    const std::string cur = util::FileSystem::NormalizePathSeparators(m_currentPath);
    const std::string root = util::FileSystem::NormalizePathSeparators(m_rootPath);

    // Walk cur up to root, collecting segments
    std::string walk = cur;
    while (true) {
        if (util::FileSystem::SamePathText(walk, root)) {
            crumbs.push_back({ "Assets", root });
            break;
        }
        // check if walk is a mounted root
        bool isMountRoot = false;
        for (const AssetMount& mount : m_mounts) {
            if (util::FileSystem::SamePathText(walk, mount.path)) {
                crumbs.push_back({ mount.name, walk });
                crumbs.push_back({ "Assets", root });
                isMountRoot = true;
                break;
            }
        }
        if (isMountRoot) break;
        crumbs.push_back({ util::FileSystem::GetFilename(walk), walk });
        const size_t pos = walk.find_last_of('/');
        if (pos == std::string::npos) break;
        walk = walk.substr(0, pos);
    }
    std::reverse(crumbs.begin(), crumbs.end());

    if (!util::FileSystem::SamePathText(cur, root)) {
        if (ImGui::SmallButton(" ^ ")) {
            m_currentPath = ParentPath();
            RefreshDirectory();
        }
        ImGui::SameLine(0.0f, 2.0f);
    }

    for (size_t i = 0; i < crumbs.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool isLast = (i + 1 == crumbs.size());
        if (isLast) {
            ImGui::TextDisabled("%s", crumbs[i].label.c_str());
        } else {
            if (ImGui::SmallButton(crumbs[i].label.c_str())) {
                m_currentPath = crumbs[i].path;
                RefreshDirectory();
            }
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0.0f, 2.0f);
        }
        ImGui::PopID();
    }
}

bool AssetBrowserPanel::PassesTypeFilter(const Entry& e) const
{
    if (e.isSubAsset) return true; // サブアセットは親が表示されていれば常に表示
    if (m_typeFilter == TypeFilter::All || e.isDir) return true;
    switch (m_typeFilter) {
    case TypeFilter::Scene:    return e.ext == ".fbzz";
    case TypeFilter::Material: return e.ext == ".fzmat";
    case TypeFilter::Script:   return e.ext == ".hpp" || e.ext == ".cpp" || e.ext == ".h"
                                   || e.ext == ".c"   || e.ext == ".cc"  || e.ext == ".cxx"
                                   || e.ext == ".py"  || e.ext == ".lua" || e.ext == ".cs";
    case TypeFilter::Texture:  return e.ext == ".png" || e.ext == ".jpg" || e.ext == ".jpeg"
                                   || e.ext == ".dds" || e.ext == ".bmp" || e.ext == ".tga"
                                   || e.ext == ".fnt" || e.ext == ".ttf" || e.ext == ".otf";
    case TypeFilter::Audio:    return e.ext == ".wav" || e.ext == ".mp3" || e.ext == ".ogg"
                                   || e.ext == ".flac";
    case TypeFilter::Mesh:     return e.ext == ".fbx"    || e.ext == ".obj"    || e.ext == ".gltf"
                                   || e.ext == ".glb"    || e.ext == ".fzmesh" || e.ext == ".fzskel"
                                   || e.ext == ".fzasset";
    case TypeFilter::Shader:   return e.ext == ".hlsl" || e.ext == ".hlsli";
    case TypeFilter::Prefab:   return e.ext == ".fbzzprefab";
    default:                   return true;
    }
}

void AssetBrowserPanel::DrawListView(EditorContext& ctx, const std::string& filter)
{
    std::vector<size_t> visIndices;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const auto& e = m_entries[i];
        if (!filter.empty() && !util::StringUtils::ContainsCI(e.name, filter)) continue;
        if (!PassesTypeFilter(e)) continue;
        visIndices.push_back(i);
    }

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg;
    if (!ImGui::BeginTable("##list", 4, kTableFlags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Type",     ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Size",     ImGuiTableColumnFlags_WidthFixed, 62.0f);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 116.0f);
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visIndices.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const Entry& e = m_entries[visIndices[i]];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            const bool selected = m_selectedPaths.count(e.path) > 0;
            if (ImGui::Selectable("##row", selected,
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowOverlap, { 0.0f, 0.0f })) {
                HandleEntryClick(e, ctx, true);
            }
            const bool hov = ImGui::IsItemHovered();
            if (hov && ImGui::IsMouseDoubleClicked(0))
                HandleEntryDoubleClick(e, ctx, true);
            DrawEntryContextMenu(e, ctx);

            ImGui::SameLine();
            const ImVec4 col = EntryColor(e);
            ImGui::TextColored(col, "%s", e.isDir ? "[D]" : EntryLabel(e));
            ImGui::SameLine();
            ImGui::TextUnformatted(e.name.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", e.isDir ? "Folder" : e.ext.c_str());

            ImGui::TableSetColumnIndex(2);
            if (!e.isDir) {
                std::error_code ec;
                const auto fsz = std::filesystem::file_size(
                    util::FileSystem::PathFromUtf8(e.path), ec);
                if (!ec) {
                    if (fsz < 1024)
                        ImGui::TextDisabled("%zu B", fsz);
                    else if (fsz < (1 << 20))
                        ImGui::TextDisabled("%.1f KB", fsz / 1024.0);
                    else
                        ImGui::TextDisabled("%.1f MB", fsz / (1024.0 * 1024.0));
                }
            }

            ImGui::TableSetColumnIndex(3);
            {
                std::error_code ec;
                const auto ft = std::filesystem::last_write_time(
                    util::FileSystem::PathFromUtf8(e.path), ec);
                if (!ec) {
                    const auto sysTp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ft - std::filesystem::file_time_type::clock::now()
                        + std::chrono::system_clock::now());
                    const std::time_t t = std::chrono::system_clock::to_time_t(sysTp);
                    char buf[32];
                    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
                    ImGui::TextDisabled("%s", buf);
                }
            }

            ImGui::PopID();
        }
    }
    clipper.End();
    ImGui::EndTable();
}

void AssetBrowserPanel::DrawSaveModifiedDialog()
{
    if (!m_showSaveModifiedDialog) return;
    ImGui::OpenPopup("##save_modified_dlg");
    m_showSaveModifiedDialog = false;

    const auto& dirty = AssetDirtyRegistry::GetAll();
    if (m_saveModifiedSelected.size() != dirty.size())
        m_saveModifiedSelected.assign(dirty.size(), true);

    ImGui::SetNextWindowSize({ 480.0f, 0.0f }, ImGuiCond_Always);
    if (ImGui::BeginPopupModal("##save_modified_dlg", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
    {
        const int count = static_cast<int>(dirty.size());
        ImGui::TextUnformatted("Unsaved Assets");
        ImGui::SameLine();
        ImGui::TextDisabled("(%d)", count);
        ImGui::Separator();

        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            bool sel = m_saveModifiedSelected[i];
            if (ImGui::Checkbox("##chk", &sel)) m_saveModifiedSelected[i] = sel;
            ImGui::SameLine();
            ImGui::TextColored({ 1.0f, 0.75f, 0.2f, 1.0f }, "[%s]", dirty[i].typeLabel.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(dirty[i].displayPath.c_str());
            ImGui::PopID();
        }

        ImGui::Separator();
        if (ImGui::Button("Save Selected")) {
            std::vector<std::string> toClean;
            for (int i = 0; i < count; ++i)
                if (m_saveModifiedSelected[i] && dirty[i].saveFunc())
                    toClean.push_back(dirty[i].path);
            for (const auto& p : toClean) AssetDirtyRegistry::MarkClean(p);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Save All")) {
            AssetDirtyRegistry::SaveAll();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard All")) {
            AssetDirtyRegistry::DiscardAll();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
}

} // namespace fbzz::editor
