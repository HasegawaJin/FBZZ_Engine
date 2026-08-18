// FBZZ Engine
// AssetBrowserPanel.cpp | fbzz::editor
// AssetBrowserPanel のフォルダツリーとアセットグリッド描画
#include "AssetBrowser/AssetBrowserCommon.hpp"
#include <Editor/Util/AssetSearch.hpp>
#include <Editor/Util/VFXEditorLauncher.hpp>

namespace fbzz::editor {

namespace {

// FileSystem::GetDirectory は末尾に '/' を付けて返す ("Assets/Scenes/")。
// パス比較やナビゲート先として使う前に落とす。
std::string DirectoryOf(const std::string& path)
{
    std::string directory = util::FileSystem::GetDirectory(path);
    while (directory.size() > 1 && (directory.back() == '/' || directory.back() == '\\'))
        directory.pop_back();
    return directory;
}

} // namespace

// ── 横断検索 ─────────────────────────────────────────────────────────────────

bool AssetBrowserPanel::IsGlobalSearchActive() const
{
    return m_searchAllFolders && m_searchBuf[0] != '\0';
}

const std::vector<AssetBrowserPanel::Entry>& AssetBrowserPanel::VisibleEntries() const
{
    return IsGlobalSearchActive() ? m_searchResults : m_entries;
}

std::vector<std::string> AssetBrowserPanel::TypeFilterExtensions() const
{
    // WHY PassesTypeFilter を再利用しないか: あちらは Entry を受け取る述語で、
    //     AssetSearch へ渡すのは拡張子の列。同じ分類を二重に書くことになるが、
    //     索引側で先に絞る方が候補を大幅に減らせる (全件を Entry 化してから
    //     捨てるのは無駄)。分類の対応は PassesTypeFilter と揃えること。
    switch (m_typeFilter) {
    case TypeFilter::Scene:     return { ".scene" };
    case TypeFilter::Material:  return { ".mat" };
    case TypeFilter::Script:    return { ".hpp", ".cpp", ".h", ".c", ".cc", ".cxx",
                                         ".py", ".lua", ".cs" };
    case TypeFilter::Texture:   return { ".png", ".jpg", ".jpeg", ".dds", ".bmp", ".tga",
                                         ".fnt", ".ttf", ".otf" };
    case TypeFilter::Audio:     return { ".wav", ".mp3", ".ogg", ".flac" };
    case TypeFilter::Mesh:      return { ".fbx", ".obj", ".gltf", ".glb", ".mesh" };
    case TypeFilter::Shader:    return { ".hlsl", ".hlsli" };
    case TypeFilter::Prefab:    return { ".prefab" };
    case TypeFilter::Animation: return { ".anim", ".animcontroller", ".animctrl", ".mask" };
    case TypeFilter::Skeleton:  return { ".skel" };
    case TypeFilter::Asset:     return { ".asset" };
    case TypeFilter::All:
    default:                    return {};
    }
}

void AssetBrowserPanel::RefreshSearchResults()
{
    const std::string query(m_searchBuf.data());
    const int typeFilter = static_cast<int>(m_typeFilter);

    // 検索語もフィルタも変わっていなければ組み直さない。
    // WHY: 毎フレーム数千件を走査すると、入力していない間もフレーム時間を食う。
    if (query == m_searchResultsQuery && typeFilter == m_searchResultsTypeFilter)
        return;

    m_searchResultsQuery      = query;
    m_searchResultsTypeFilter = typeFilter;
    m_searchResults.clear();

    if (query.empty()) return;

    // 表示件数の上限。これを超えるヒットは絞り込みを促す。
    // WHY: グリッドは ImGuiListClipper で間引くが、Entry の構築自体は全件走るため、
    //      上限がないと曖昧な 1 文字検索で数千件を組み立てることになる。
    constexpr std::size_t MAX_SEARCH_RESULTS = 500;

    const std::vector<std::string> extensions = TypeFilterExtensions();
    const auto hits = AssetSearch::Query(query, extensions, MAX_SEARCH_RESULTS);

    m_searchResults.reserve(hits.size());
    for (const AssetSearchHit& hit : hits) {
        // Asset Browser の表示規則 (生成物・中間物を隠す) は横断検索でも適用する。
        // WHY: 索引は「参照されうるファイル」を広く持つが、ブラウザに出すのは
        //      ユーザーが直接編集・選択する対象だけ、という方針は変えない。
        if (!ShouldDisplayEntry(hit.entry->absolutePath, hit.entry->filename, false))
            continue;

        Entry entry;
        entry.path  = hit.entry->absolutePath;
        entry.name  = hit.entry->filename;
        entry.ext   = hit.entry->extension;
        entry.isDir = false;
        m_searchResults.push_back(std::move(entry));
    }
}

void AssetBrowserPanel::OnRenderContent(EditorContext& ctx)
{
    // HotkeyManager の Scope::AssetBrowser 判定用。以前は誰も立てておらず
    // 該当スコープのショートカットが常に無効化されていた。
    ctx.assetBrowserFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    HandleClipboardShortcuts(ctx);

    DrawImportSettingsModal(ctx);

    // テクスチャ遅延ロードキューを処理 (3件/フレームに分散)
    DrainTexLoadQueue(ctx);

    UpdateMounts(ctx);

    if (ctx.requestAssetBrowserRefresh) {
        RefreshDirectory();
        // 横断検索の結果も作り直す (アセットが増減している可能性があるため)。
        m_searchResultsQuery.clear();
        m_searchResultsTypeFilter = -1;
        ctx.requestAssetBrowserRefresh = false;
    }

    // Inspector 等の参照欄クリック → そのアセットのフォルダへ移動して選択する。
    // WHY Refresh の後か: Reveal は自前で RefreshDirectory を呼ぶため、直後に一括 Refresh が
    //     走ると展開したサブアセットまで組み直され、選択位置の計算がやり直しになる。
    HandleRevealRequest(ctx);

    // 横断検索の索引はプロジェクトルート基準。ルートが変わったときだけ再構築される。
    AssetSearch::SetProjectRoot(ctx.projectRoot);

    // ── 左ペイン: フォルダツリー ─────────────────────────────────────────
    // セクション見出し (FAVORITES / FOLDERS) を控えめなラベルで描く小ヘルパー。
    const auto sectionHeader = [](const char* label) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextMuted));
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
                : EditorTheme::Color(ThemeColor::Warning));
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
    // エクスプローラーからのドロップが Assets ルート行に落ちたらルートを取り込み先にする。
    ConsiderExternalDropTarget(m_rootPath, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
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
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            std::string sourcePath;
            if (ReadAssetDragPayload(p, sourcePath))
                QueueAssetMove(sourcePath, m_rootPath);
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
            ConsiderExternalDropTarget(mount.path, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
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
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string sourcePath;
                    if (ReadAssetDragPayload(p, sourcePath))
                        QueueAssetMove(sourcePath, mount.path);
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

    // Windows Explorerと同じく、通常ホイールは一覧スクロール、Ctrl+ホイールは表示サイズ変更に使う。
    // WHY: Sizeスライダーが検索欄を圧迫していたため、ポインターを一覧から離さず調整できる操作へ移す。
    const ImGuiIO& io = ImGui::GetIO();
    if (m_viewMode == ViewMode::Grid && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
        && io.KeyCtrl && std::fabs(io.MouseWheel) > 0.001f) {
        m_iconSize = std::clamp(m_iconSize + io.MouseWheel * 8.0f, 56.0f, 132.0f);
        ctx.assetBrowserIconSize = m_iconSize;
    }

    // パンくずリスト
    DrawBreadcrumb(ctx);

    // 未変換ファイルがあれば警告バーを表示
    DrawPendingImportBar(ctx);

    // ── ツールバー: 検索(伸縮) | Type / Sort | Save / Create / Refresh / View | 件数 ──
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

        constexpr float kComboW = 78.0f;

        const bool dirty = AssetDirtyRegistry::HasAny();
        char saveLabel[32] = {};
        if (dirty)
            std::snprintf(saveLabel, sizeof(saveLabel), "Save* (%d)",
                          static_cast<int>(AssetDirtyRegistry::GetAll().size()));

        const char* viewLabel = (m_viewMode == ViewMode::Grid) ? "List" : "Grid";

        // 検索範囲トグル。押下状態が一目で分かるようラベル自体を切り替える。
        const char* scopeLabel = m_searchAllFolders ? "All Assets" : "This Folder";

        char countStr[48];
        if (IsGlobalSearchActive()) {
            std::snprintf(countStr, sizeof(countStr), "%d hits",
                          static_cast<int>(m_searchResults.size()));
        } else {
            int files = 0, dirs = 0;
            for (const auto& e : m_entries) { if (e.isDir) ++dirs; else ++files; }
            std::snprintf(countStr, sizeof(countStr), "%d files, %d dirs", files, dirs);
        }

        // 右クラスタの合計幅を実測して検索欄の幅を決める。
        float rightW = btnW(scopeLabel) + sp;          // 検索範囲トグル
        rightW += kComboW + sp + kComboW + sp;         // Type / Sort
        if (dirty) rightW += btnW(saveLabel) + sp;
        rightW += btnW("Create") + sp + btnW("Refresh") + sp + btnW(viewLabel) + sp;
        rightW += ImGui::CalcTextSize(countStr).x;

        const float avail   = ImGui::GetContentRegionAvail().x;
        const float searchW = std::max(120.0f, avail - rightW - sp);

        ImGui::SetNextItemWidth(searchW);
        ImGui::InputTextWithHint("##search",
                                 m_searchAllFolders ? "Search all assets..."
                                                    : "Search this folder...",
                                 m_searchBuf.data(), m_searchBuf.size());
        ImGui::SameLine(0.0f, sp);

        // 検索範囲の切り替え。ON の間はアクセント色で状態を示す。
        if (m_searchAllFolders)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(scopeLabel)) {
            m_searchAllFolders = !m_searchAllFolders;
            // トグル直後に確実に組み直させる。
            m_searchResultsQuery.clear();
            m_searchResultsTypeFilter = -1;
        }
        if (m_searchAllFolders) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "検索範囲を切り替えます。\n"
                "This Folder : 現在のフォルダ内だけを名前で絞り込む\n"
                "All Assets  : プロジェクト全体を横断検索する (略記・部分列も一致)");
        }
        ImGui::SameLine(0.0f, sp);

        ImGui::SetNextItemWidth(kComboW);
        { int tf = static_cast<int>(m_typeFilter);
          if (ImGui::Combo("##type", &tf, kTypeLabels, 12)) m_typeFilter = static_cast<TypeFilter>(tf); }
        ImGui::SameLine(0.0f, sp);

        ImGui::SetNextItemWidth(kComboW);
        { int sm = static_cast<int>(m_sortMode);
          if (ImGui::Combo("##sort", &sm, kSortLabels, 4)) { m_sortMode = static_cast<SortMode>(sm); RefreshDirectory(); } }
        ImGui::SameLine(0.0f, sp);

        if (dirty) {
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::Warning));
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
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Grid size: Ctrl + Mouse Wheel");
        ImGui::SameLine(0.0f, sp);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", countStr);
    }

    DrawSaveModifiedDialog();

    ImGui::Separator();

    // 横断検索が有効なら結果を組み直す (検索語 / フィルタが変わったときのみ実走)。
    RefreshSearchResults();

    const std::string filter(m_searchBuf.data());
    const bool globalSearch = IsGlobalSearchActive();
    const std::vector<Entry>& entries = VisibleEntries();

    if (globalSearch && entries.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("\"%s\" に一致するアセットはありません。", filter.c_str());
        ImGui::TextDisabled("Type フィルタが絞り込みすぎていないか確認してください。");
    }

    if (m_viewMode == ViewMode::List) {
        DrawListView(ctx, filter);
    } else {
        const float padding  = 12.0f;
        const float avail    = ImGui::GetContentRegionAvail().x;
        const int   cols     = std::max(1, (int)(avail / (m_iconSize + padding)));
        const float rowH     = m_iconSize + ImGui::GetTextLineHeightWithSpacing() * 1.6f;

        std::vector<size_t> visIndices;
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            // 横断検索の結果は AssetSearch 側で名前・タイプとも絞り済み。
            // ここで再度フィルタすると、部分列一致でヒットした項目まで落ちてしまう。
            if (!globalSearch) {
                if (!filter.empty() && !util::StringUtils::ContainsCI(e.name, filter)) continue;
                if (!PassesTypeFilter(e)) continue;
            }
            visIndices.push_back(i);
        }

        const int totalRows = cols > 0
            ? static_cast<int>((visIndices.size() + cols - 1) / cols)
            : 0;

        // Reveal 要求の対象を表示範囲へ入れる。
        // WHY 行を自前で計算するか: グリッドは ImGuiListClipper で間引くため、対象タイルが
        //     画面外だとそもそも描かれず SetScrollHereY を呼ぶ機会がない。可視インデックス列から
        //     行番号を割り出し、ビューの中央へ来るようスクロール量を直接指定する。
        if (!m_scrollToPath.empty()) {
            for (size_t visible = 0; visible < visIndices.size(); ++visible) {
                if (entries[visIndices[visible]].path != m_scrollToPath) continue;
                const float targetY = static_cast<float>(visible / static_cast<size_t>(cols)) * rowH;
                const float centered = targetY - (ImGui::GetContentRegionAvail().y - rowH) * 0.5f;
                ImGui::SetScrollY(std::max(0.0f, centered));
                break;
            }
            // 一覧に無かった場合も要求は捨てる (フィルタ・展開状態が合わないだけで、
            // 毎フレーム走査し続ける理由にはならない)。
            m_scrollToPath.clear();
        }

        // 展開した親アセット + 直後に並ぶサブアセット群を 1 本の帯として描くための判定。
        // WHY: サブアセットは RefreshDirectory が親の直後へ挿入するため、帯の範囲は
        //      「展開中の親から、連続する isSubAsset が途切れるまで」という並び順だけで決まる。
        //      パス命名 (FBX の ::mesh:: / materials 配下の .mat 等) には依存させない。
        // 横断検索の結果はサブアセットを展開しないため、帯は常に無効。
        const auto isBandParent = [this, &entries, globalSearch](size_t entryIndex) {
            if (globalSearch) return false;
            const Entry& en = entries[entryIndex];
            return !en.isDir && !en.isSubAsset && en.hasSubAssets &&
                   m_expandedAssets.count(en.path) > 0;
        };
        const auto inBand = [&entries, &isBandParent, globalSearch](size_t entryIndex) {
            if (globalSearch) return false;
            return entries[entryIndex].isSubAsset || isBandParent(entryIndex);
        };

        ImGuiListClipper clipper;
        clipper.Begin(totalRows, rowH);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int c = 0; c < cols; ++c) {
                    const int idx = row * cols + c;
                    if (idx >= static_cast<int>(visIndices.size())) break;
                    if (c > 0) ImGui::SameLine(0.0f, padding);

                    const size_t entryIndex = visIndices[idx];
                    SubAssetBand band;
                    if (inBand(entryIndex)) {
                        band.active   = true;
                        band.isParent = isBandParent(entryIndex);
                        // 帯はセルの中点で接合させる。タイル矩形は左右 4px 張り出しているため、
                        // 中点までの残りは padding/2 - 4px。半透明色なので重ねず「ぴったり」繋ぐ。
                        band.bleed = padding * 0.5f - 4.0f;
                        // 左へ続く = 自分がサブアセットで、直前の可視エントリも同じ帯。
                        const bool continuesLeft = entries[entryIndex].isSubAsset &&
                                                   idx > 0 && inBand(visIndices[idx - 1]);
                        // 右へ続く = 次の可視エントリがサブアセット (= 自分の子か兄弟)。
                        const bool continuesRight = idx + 1 < static_cast<int>(visIndices.size()) &&
                                                    entries[visIndices[idx + 1]].isSubAsset;
                        // 同じ行なら接合、行端なら折り返し。どちらも角は閉じないが、
                        // 矩形を伸ばすのは接合のときだけ (行端に余白を作らない)。
                        band.joinLeft  = continuesLeft  && c > 0;
                        band.wrapLeft  = continuesLeft  && c == 0;
                        band.joinRight = continuesRight && c < cols - 1;
                        band.wrapRight = continuesRight && c == cols - 1;
                    }

                    ImGui::BeginGroup();
                    DrawEntry(entries[entryIndex], ctx, band);
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
        // 空白領域へのドロップは現在フォルダへの移動として扱う。
        // WHY: フォルダが表示されていない検索結果 / List 表示でも、移動先を失わないため。
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            std::string sourcePath;
            if (ReadAssetDragPayload(p, sourcePath))
                QueueAssetMove(sourcePath, m_currentPath);
        }
        ImGui::EndDragDropTarget();
    }

    // 選択 FBX の内容プレビュー
    DrawFbxContents(ctx);

    ImGui::EndChild();

    // ドロップ先フォルダの当たり判定が全て終わった後にコピーを確定する。
    // WHY: ツリー / グリッドの各フォルダ描画で m_externalDrop.targetDir が決まる。
    //      いずれにもヒットしなければ現在フォルダへ取り込まれる。
    if (m_externalDrop.active)
        FinalizeExternalDrop();
    FinalizePendingAssetMove(ctx);
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
                                     || e.ext == ".glb"    || e.ext == ".mesh";
    case TypeFilter::Shader:    return e.ext == ".hlsl" || e.ext == ".hlsli";
    case TypeFilter::Prefab:    return e.ext == ".prefab";
    // Animator Controller と Avatar Mask もアニメーション制作物として一緒に絞り込む。
    // WHY: 上半身レイヤーを組むときは .anim / .animcontroller / .mask を行き来するため、
    //      同じフィルタで一望できないと毎回 All に戻すことになる。
    case TypeFilter::Animation: return e.ext == ".anim" || e.ext == ".animcontroller"
                                     || e.ext == ".animctrl" || e.ext == ".mask";
    case TypeFilter::Skeleton:  return e.ext == ".skel";
    case TypeFilter::Asset:     return e.ext == ".asset";
    default:                    return true;
    }
}

void AssetBrowserPanel::DrawListView(EditorContext& ctx, const std::string& filter)
{
    const bool globalSearch = IsGlobalSearchActive();
    const std::vector<Entry>& entries = VisibleEntries();

    std::vector<size_t> visIndices;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        // 横断検索の結果は AssetSearch 側で絞り済み (部分列一致を落とさない)。
        if (!globalSearch) {
            if (!filter.empty() && !util::StringUtils::ContainsCI(e.name, filter)) continue;
            if (!PassesTypeFilter(e)) continue;
        }
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
    // WHY 横断検索中はソートしないか: AssetSearch がスコア順に並べており、
    //     完全一致・前方一致が上に来るのが検索結果として正しい。
    //     名前順に並べ替えると、その利点が失われる。
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
        !globalSearch && specs && specs->SpecsCount > 0) {
        const int  col = specs->Specs[0].ColumnIndex;
        const bool asc = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;

        std::unordered_map<size_t, uintmax_t>     sizeCache;
        std::unordered_map<size_t, long long>     timeCache;
        auto fileSizeOf = [&](size_t idx) -> uintmax_t {
            if (auto it = sizeCache.find(idx); it != sizeCache.end()) return it->second;
            std::error_code ec;
            const uintmax_t v = entries[idx].isDir ? 0
                : std::filesystem::file_size(util::FileSystem::PathFromUtf8(entries[idx].path), ec);
            return sizeCache[idx] = (ec ? 0 : v);
        };
        auto fileTimeOf = [&](size_t idx) -> long long {
            if (auto it = timeCache.find(idx); it != timeCache.end()) return it->second;
            std::error_code ec;
            const auto ft = std::filesystem::last_write_time(
                util::FileSystem::PathFromUtf8(entries[idx].path), ec);
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
            const Entry& ea = entries[a];
            const Entry& eb = entries[b];
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

    // Reveal 要求の対象行。間引かれていても必ず描かせてからスクロールを合わせる。
    // WHY: ImGuiListClipper は表示範囲外の行を一切描かないため、IncludeItemByIndex で
    //      対象行だけ描画対象へ戻さないと SetScrollHereY を呼ぶ機会が来ない。
    int scrollToRow = -1;
    if (!m_scrollToPath.empty()) {
        for (size_t visible = 0; visible < visIndices.size(); ++visible) {
            if (entries[visIndices[visible]].path != m_scrollToPath) continue;
            scrollToRow = static_cast<int>(visible);
            break;
        }
        m_scrollToPath.clear();
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visIndices.size()));
    if (scrollToRow >= 0) clipper.IncludeItemByIndex(scrollToRow);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const Entry& e = entries[visIndices[i]];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (i == scrollToRow) ImGui::SetScrollHereY(0.5f);

            // 主選択 (Inspector に出ている 1 件) もハイライト対象にする。
            // WHY: グリッドでは主選択が強調されるのに、リストでは Ctrl 選択した
            //      ものしか光らず「選んだはずの行が光らない」状態だった。
            const bool primarySelected = !e.isDir && e.path == ctx.selectedAssetPath;
            const bool selected = primarySelected || m_selectedPaths.count(e.path) > 0;
            const bool emphasized = primarySelected || m_selectedPaths.size() <= 1;
            const bool panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Header,
                                      ImGui::ColorConvertU32ToFloat4(
                                          ui::TileSelectionFill(emphasized, panelFocused)));
            if (ImGui::Selectable("##row", selected,
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowOverlap, { 0.0f, 0.0f })) {
                HandleEntryClick(e, ctx, true);
            }
            if (selected) ImGui::PopStyleColor();
            const bool hov = ImGui::IsItemHovered();
            // グリッドと同じ ASSET_PATH を発行し、List 表示でもファイル / フォルダを整理できるようにする。
            if (!e.isMount && !e.isPackageAsset && ImGui::BeginDragDropSource()) {
                m_entryDragStarted = true;
                const std::string payloadPath = ToAssetDragPayloadPath(e.path, ctx);
                ImGui::SetDragDropPayload("ASSET_PATH", payloadPath.c_str(), payloadPath.size() + 1);
                VFXEditorLauncher::TrackAssetDrag(ctx.projectRoot, payloadPath);
                ImGui::TextUnformatted(e.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (e.isDir && ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string sourcePath;
                    if (ReadAssetDragPayload(p, sourcePath))
                        QueueAssetMove(sourcePath, e.path);
                }
                ImGui::EndDragDropTarget();
            }
            // 主選択の行だけ左端にアクセントバーを立て、複数選択の中の「現在の対象」を示す。
            if (selected && emphasized) {
                ui::DrawSelectionAccent(ImGui::GetWindowDrawList(),
                                        ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                        true, true, 2.0f);
            }
            // エクスプローラーからのドロップがこのフォルダ行に落ちたら取り込み先にする。
            if (e.isDir)
                ConsiderExternalDropTarget(e.path, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (hov && ImGui::IsMouseDoubleClicked(0))
                HandleEntryDoubleClick(e, ctx, true);
            DrawEntryContextMenu(e, ctx);

            ImGui::SameLine();
            // サブアセットは親の下にぶら下がる形で字下げし、罫線で従属関係を示す。
            if (e.isSubAsset) {
                ImGui::Dummy({ 14.0f, 0.0f });
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextDisabled("\xe2\x94\x94");  // └
                ImGui::SameLine();
            }
            const ImVec4 col = EntryColor(e);
            ImGui::TextColored(col, "%s", e.isDir ? "[D]" : EntryLabel(e));
            ImGui::SameLine();
            ImGui::TextUnformatted(e.name.c_str());

            // 横断検索中は所在フォルダを併記する。
            // WHY: 名前だけでは同名アセットを区別できず、「どこの Player.mat か」が分からない。
            if (globalSearch) {
                ImGui::SameLine();
                const std::string folder = DirectoryOf(e.path);
                std::string relative = folder;
                if (util::FileSystem::IsChildPathText(folder, m_rootPath)) {
                    relative = folder.size() > m_rootPath.size()
                        ? "Assets/" + folder.substr(m_rootPath.size() + 1)
                        : "Assets";
                }
                ImGui::TextDisabled("- %s", relative.c_str());
            }

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
