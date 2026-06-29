// FBZZ Engine
// AssetBrowserPanel.cpp | fbzz::editor
// AssetBrowserPanel のフォルダツリーとアセットグリッド描画
#include "AssetBrowser/AssetBrowserCommon.hpp"

namespace fbzz::editor {

void AssetBrowserPanel::OnRenderContent(EditorContext& ctx)
{
    DrawImportSettingsModal(ctx);

    // テクスチャ遅延ロードキューを処理 (3件/フレームに分散)
    DrainTexLoadQueue(ctx);

    UpdateMounts(ctx);

    if (ctx.requestAssetBrowserRefresh) {
        RefreshDirectory();
        ctx.requestAssetBrowserRefresh = false;
    }

    // ── 左ペイン: フォルダツリー ─────────────────────────────────────────
    // セクション見出し (FAVORITES / FOLDERS) を控えめなラベルで描く小ヘルパー。
    const auto sectionHeader = [](const char* label) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::Separator();
    };

    // 高さを確保し、ツリー / スプリッター / コンテンツを同じ高さで並べる。
    const float paneH = ImGui::GetContentRegionAvail().y;

    ImGui::BeginChild("##tree", { m_treeWidth, paneH }, true);

    // Favorites セクション
    if (!ctx.assetBrowserBookmarks.empty()) {
        sectionHeader("FAVORITES");
        for (const auto& bk : ctx.assetBrowserBookmarks) {
            const std::string label = util::FileSystem::GetFilename(bk).empty()
                ? bk : util::FileSystem::GetFilename(bk);
            const bool sel = util::FileSystem::SamePathText(m_currentPath, bk);
            // 先頭に星を付けてお気に入りであることを示す。
            ImGui::PushStyleColor(ImGuiCol_Text, sel
                ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                : ImVec4(1.0f, 0.82f, 0.28f, 1.0f));
            const std::string row = "\xe2\x98\x85 " + label;
            if (ImGui::Selectable(row.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) {
                if (util::FileSystem::Exists(bk)) {
                    m_currentPath = bk;
                    RefreshDirectory();
                }
            }
            ImGui::PopStyleColor();
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Remove from Favorites")) {
                    auto& bks = ctx.assetBrowserBookmarks;
                    bks.erase(std::remove(bks.begin(), bks.end(), bk), bks.end());
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    break; // iterator invalidated
                }
                ImGui::EndPopup();
            }
        }
    }

    sectionHeader("FOLDERS");

    ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_SpanAvailWidth
                                 | ImGuiTreeNodeFlags_DefaultOpen;
    const bool rootIsCurrent = util::FileSystem::SamePathText(m_currentPath, m_rootPath);
    if (rootIsCurrent) rootFlags |= ImGuiTreeNodeFlags_Selected;

    // WHY: 現在フォルダはアクセント色の塗りで強調する (既定の薄い選択色より目立たせる)。
    if (rootIsCurrent)
        ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    bool rootOpen = ImGui::TreeNodeEx("##root", rootFlags, "Assets");
    if (rootIsCurrent)
        ImGui::PopStyleColor();
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

    // ── EXTERNAL: マウント (外部ソースフォルダ) を専用セクションに分離 ──
    // WHY: 以前は Assets ツリーの子に混ぜていたが、由来が異なる外部フォルダは見出しで分けた方が分かりやすい。
    if (!m_mounts.empty()) {
        sectionHeader("EXTERNAL");
        for (const AssetMount& mount : m_mounts) {
            ImGui::PushID(mount.path.c_str());
            ImGuiTreeNodeFlags mflags = ImGuiTreeNodeFlags_OpenOnArrow
                                      | ImGuiTreeNodeFlags_SpanAvailWidth;
            const bool mCurrent = util::FileSystem::SamePathText(m_currentPath, mount.path);
            if (mCurrent) mflags |= ImGuiTreeNodeFlags_Selected;
            if (mCurrent)
                ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            // 表示名は Assets 側の仮想名、ID は実パスにして同名マウントでも衝突しない。
            const bool mOpen = ImGui::TreeNodeEx(mount.path.c_str(), mflags, "%s", mount.name.c_str());
            if (mCurrent)
                ImGui::PopStyleColor();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
                m_currentPath = mount.path;
                RefreshDirectory();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n\nExternal source folder mounted under Assets", mount.path.c_str());
            // 外部フォルダへのドロップ → そのフォルダへ Prefab 保存
            if (ImGui::BeginDragDropTarget()) {
                if (SaveHierarchyPayloadAsPrefab(
                        ImGui::AcceptDragDropPayload("FBZZ_HIERARCHY_ENTITY"), ctx, mount.path)) {
                    RefreshDirectory();
                }
                ImGui::EndDragDropTarget();
            }
            if (mOpen) {
                DrawFolderTree(mount.path, ctx);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    ImGui::EndChild();

    // ── スプリッター: 左ツリーの幅をドラッグで可変にする ───────────────────
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("##tree_splitter", { 6.0f, paneH });
    const bool splitActive = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() || splitActive)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (splitActive) {
        m_treeWidth = std::clamp(m_treeWidth + ImGui::GetIO().MouseDelta.x, 140.0f, 420.0f);
        ctx.assetBrowserTreeWidth = m_treeWidth; // EditorSettings 経由で永続化される
    }
    {
        // ホバー / ドラッグ中だけアクセント色の縦線を見せる。
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetItemRectMin();
        const ImVec2 p1 = ImGui::GetItemRectMax();
        const float  cx = (p0.x + p1.x) * 0.5f;
        const ImU32  col = (ImGui::IsItemHovered() || splitActive)
            ? ImGui::GetColorU32(ImGuiCol_SeparatorHovered)
            : ImGui::GetColorU32(ImGuiCol_Separator);
        dl->AddLine({ cx, p0.y + 2.0f }, { cx, p1.y - 2.0f }, col, 1.5f);
    }
    ImGui::SameLine(0.0f, 0.0f);

    // ── 右ペイン: コンテンツエリア ───────────────────────────────────────
    ImGui::BeginChild("##content", { 0.0f, paneH }, false);
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

    // ── ツールバー: 検索(伸縮) | Type / Sort / Size | Save / Create / Refresh / View | 件数 ──
    // WHY: 旧実装は検索欄を固定 -470px で予約していたが、フォントサイズ変更でズレるため、
    //      右側コントロール群の幅を実測して検索欄を動的に伸縮させ、どの DPI/フォントでも揃える。
    static constexpr const char* kTypeLabels[] = {
        "All", "Scene", "Material", "Script", "Texture", "Audio",
        "Mesh", "Shader", "Prefab", "Animation", "Skeleton", "Asset" };
    static constexpr const char* kSortLabels[] = { "Name ^", "Name v", "Type", "Modified" };
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float sp = st.ItemSpacing.x;
        const auto  btnW = [&](const char* s) {
            return ImGui::CalcTextSize(s).x + st.FramePadding.x * 2.0f;
        };

        constexpr float kComboW  = 78.0f;
        constexpr float kSliderW = 84.0f;

        const bool dirty = AssetDirtyRegistry::HasAny();
        char saveLabel[32] = {};
        if (dirty)
            std::snprintf(saveLabel, sizeof(saveLabel), "Save* (%d)",
                          static_cast<int>(AssetDirtyRegistry::GetAll().size()));

        const char* viewLabel = (m_viewMode == ViewMode::Grid) ? "List" : "Grid";

        int files = 0, dirs = 0;
        for (const auto& e : m_entries) { if (e.isDir) ++dirs; else ++files; }
        char countStr[48];
        std::snprintf(countStr, sizeof(countStr), "%d files, %d dirs", files, dirs);

        // 右クラスタの合計幅を実測して検索欄の幅を決める。
        float rightW = kComboW + sp + kComboW + sp + kSliderW + sp;          // Type / Sort / Size
        if (dirty) rightW += btnW(saveLabel) + sp;
        rightW += btnW("Create") + sp + btnW("Refresh") + sp + btnW(viewLabel) + sp;
        rightW += ImGui::CalcTextSize(countStr).x;

        const float avail   = ImGui::GetContentRegionAvail().x;
        const float searchW = std::max(120.0f, avail - rightW - sp);

        ImGui::SetNextItemWidth(searchW);
        ImGui::InputTextWithHint("##search", "Search assets...",
                                 m_searchBuf.data(), m_searchBuf.size());
        ImGui::SameLine(0.0f, sp);

        ImGui::SetNextItemWidth(kComboW);
        { int tf = static_cast<int>(m_typeFilter);
          if (ImGui::Combo("##type", &tf, kTypeLabels, 12)) m_typeFilter = static_cast<TypeFilter>(tf); }
        ImGui::SameLine(0.0f, sp);

        ImGui::SetNextItemWidth(kComboW);
        { int sm = static_cast<int>(m_sortMode);
          if (ImGui::Combo("##sort", &sm, kSortLabels, 4)) { m_sortMode = static_cast<SortMode>(sm); RefreshDirectory(); } }
        ImGui::SameLine(0.0f, sp);

        ImGui::SetNextItemWidth(kSliderW);
        if (ImGui::SliderFloat("##sz", &m_iconSize, 56.0f, 132.0f, "%.0f"))
            ctx.assetBrowserIconSize = m_iconSize;
        ImGui::SameLine(0.0f, sp);

        if (dirty) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.38f, 0.05f, 1.0f));
            if (ImGui::Button(saveLabel)) {
                m_showSaveModifiedDialog = true;
                m_saveModifiedSelected.assign(AssetDirtyRegistry::GetAll().size(), true);
            }
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, sp);
        }

        if (ImGui::Button("Create")) ImGui::OpenPopup("##content_ctx");
        ImGui::SameLine(0.0f, sp);
        if (ImGui::Button("Refresh")) RefreshDirectory();
        ImGui::SameLine(0.0f, sp);
        if (ImGui::Button(viewLabel))
            m_viewMode = (m_viewMode == ViewMode::Grid) ? ViewMode::List : ViewMode::Grid;
        ImGui::SameLine(0.0f, sp);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", countStr);
    }

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
    if (m_assetExpandDirty) {
        m_assetExpandDirty = false;
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
    case TypeFilter::Scene:    return e.ext == ".scene";
    case TypeFilter::Material: return e.ext == ".mat";
    case TypeFilter::Script:   return e.ext == ".hpp" || e.ext == ".cpp" || e.ext == ".h"
                                   || e.ext == ".c"   || e.ext == ".cc"  || e.ext == ".cxx"
                                   || e.ext == ".py"  || e.ext == ".lua" || e.ext == ".cs";
    case TypeFilter::Texture:  return e.ext == ".png" || e.ext == ".jpg" || e.ext == ".jpeg"
                                   || e.ext == ".dds" || e.ext == ".bmp" || e.ext == ".tga"
                                   || e.ext == ".fnt" || e.ext == ".ttf" || e.ext == ".otf";
    case TypeFilter::Audio:    return e.ext == ".wav" || e.ext == ".mp3" || e.ext == ".ogg"
                                   || e.ext == ".flac";
    case TypeFilter::Mesh:      return e.ext == ".fbx"    || e.ext == ".obj"    || e.ext == ".gltf"
                                    || e.ext == ".glb"    || e.ext == ".mesh"   || e.ext == ".fzasset";
    case TypeFilter::Shader:    return e.ext == ".hlsl" || e.ext == ".hlsli";
    case TypeFilter::Prefab:    return e.ext == ".prefab";
    case TypeFilter::Animation: return e.ext == ".anim";
    case TypeFilter::Skeleton:  return e.ext == ".skel";
    case TypeFilter::Asset:     return e.ext == ".asset" || e.ext == ".fzasset";
    default:                    return true;
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
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable;
    if (!ImGui::BeginTable("##list", 4, kTableFlags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("Type",     ImGuiTableColumnFlags_WidthFixed, 70.0f);
    ImGui::TableSetupColumn("Size",     ImGuiTableColumnFlags_WidthFixed, 62.0f);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 116.0f);
    ImGui::TableHeadersRow();

    // 列ヘッダクリックでソート。フォルダは常に先頭に固定し、その中で指定列順に並べる。
    // WHY: サイズ/更新日時のソートだけ stat が要るため、この並べ替えの間だけローカルにキャッシュする。
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
        const int  col = specs->Specs[0].ColumnIndex;
        const bool asc = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;

        std::unordered_map<size_t, uintmax_t>     sizeCache;
        std::unordered_map<size_t, long long>     timeCache;
        auto fileSizeOf = [&](size_t idx) -> uintmax_t {
            if (auto it = sizeCache.find(idx); it != sizeCache.end()) return it->second;
            std::error_code ec;
            const uintmax_t v = m_entries[idx].isDir ? 0
                : std::filesystem::file_size(util::FileSystem::PathFromUtf8(m_entries[idx].path), ec);
            return sizeCache[idx] = (ec ? 0 : v);
        };
        auto fileTimeOf = [&](size_t idx) -> long long {
            if (auto it = timeCache.find(idx); it != timeCache.end()) return it->second;
            std::error_code ec;
            const auto ft = std::filesystem::last_write_time(
                util::FileSystem::PathFromUtf8(m_entries[idx].path), ec);
            return timeCache[idx] = (ec ? 0 : static_cast<long long>(ft.time_since_epoch().count()));
        };

        // 大文字小文字を無視した比較 (プラットフォーム拡張に依存しない)。
        auto ciCmp = [](const std::string& a, const std::string& b) -> int {
            const size_t n = std::min(a.size(), b.size());
            for (size_t i = 0; i < n; ++i) {
                const int ca = std::tolower(static_cast<unsigned char>(a[i]));
                const int cb = std::tolower(static_cast<unsigned char>(b[i]));
                if (ca != cb) return ca < cb ? -1 : 1;
            }
            return (a.size() == b.size()) ? 0 : (a.size() < b.size() ? -1 : 1);
        };

        std::stable_sort(visIndices.begin(), visIndices.end(), [&](size_t a, size_t b) {
            const Entry& ea = m_entries[a];
            const Entry& eb = m_entries[b];
            if (ea.isDir != eb.isDir) return ea.isDir; // フォルダ先頭固定 (昇降に関わらず)
            int cmp = 0;
            switch (col) {
            case 1:  cmp = ciCmp(ea.ext, eb.ext); break;
            case 2:  { const auto sa = fileSizeOf(a), sb = fileSizeOf(b);
                       cmp = (sa < sb) ? -1 : (sa > sb) ? 1 : 0; } break;
            case 3:  { const auto ta = fileTimeOf(a), tb = fileTimeOf(b);
                       cmp = (ta < tb) ? -1 : (ta > tb) ? 1 : 0; } break;
            default: cmp = ciCmp(ea.name, eb.name); break;
            }
            if (cmp == 0) cmp = ciCmp(ea.name, eb.name);
            return asc ? (cmp < 0) : (cmp > 0);
        });
    }

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
