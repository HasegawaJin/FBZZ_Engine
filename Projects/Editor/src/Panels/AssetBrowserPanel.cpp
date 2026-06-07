// FBZZ Engine
// AssetBrowserPanel.cpp | fbzz::editor
// AssetBrowserPanel の2ペインレイアウトとメイン描画
#include "AssetBrowser/AssetBrowserCommon.hpp"

namespace fbzz::editor {

void AssetBrowserPanel::OnRenderContent(EditorContext& ctx)
{
    DrawImportResultBar(ctx);

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

    // ナビゲーションバー
    if (!util::FileSystem::SamePathText(m_currentPath, m_rootPath)) {
        if (ImGui::SmallButton(" ^ ")) {
            m_currentPath = ParentPath();
            RefreshDirectory();
        }
        ImGui::SameLine();
    }
    const std::string rel = DisplayPath();
    ImGui::TextDisabled("%s", rel.c_str());

    // 未変換ファイルがあれば警告バーを表示
    DrawPendingImportBar(ctx);

    // 検索 + アイコンサイズスライダー + 作成 + 更新
    ImGui::SetNextItemWidth(-260.0f);
    ImGui::InputText("##search", m_searchBuf.data(), m_searchBuf.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderFloat("##sz", &m_iconSize, 40.0f, 120.0f, "%.0f");
    ImGui::SameLine();
    if (ImGui::SmallButton("Create")) {
        ImGui::OpenPopup("##content_ctx");
    }
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
        m_currentPath = util::FileSystem::NormalizePathSeparators(std::move(m_pendingNavigate));
        m_pendingNavigate.clear();
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

} // namespace fbzz::editor
