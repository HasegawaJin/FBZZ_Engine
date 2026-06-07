// FBZZ Engine
// AssetBrowserImport.cpp | fbzz::editor
// AssetBrowser の未変換アセット検出とバックグラウンドインポート
#include "AssetBrowserCommon.hpp"

namespace fbzz::editor {

bool AssetBrowserPanel::IsImportableRaw(const std::string& ext)
{
    return ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb";
}

bool AssetBrowserPanel::IsAlreadyImported(const std::string& absPath)
{
    namespace fs = std::filesystem;
    const fs::path p(absPath.begin(), absPath.end());
    const std::string stem = p.stem().string();
    const fs::path check = p.parent_path() / stem / (stem + ".fzasset");
    std::error_code ec;
    return fs::exists(check, ec);
}

void AssetBrowserPanel::TryQueuePendingImport(const std::string& relPath)
{
    const std::string ext = util::StringUtils::ToLower(
        util::FileSystem::GetExtension(relPath));
    if (!IsImportableRaw(ext)) return;

    // パス結合 (m_rootPath が trailing slash を持つかどうかに依らず正しく結合)
    namespace fs = std::filesystem;
    const std::string absPath = (fs::path(m_rootPath.begin(), m_rootPath.end())
                                 / fs::path(relPath.begin(), relPath.end())).string();

    // 重複チェック
    for (const auto& p : m_pendingImports)
        if (p.path == absPath) return;

    if (IsAlreadyImported(absPath)) return;

    m_pendingImports.push_back({ absPath, PendingImport::Kind::Fbx });
    m_importAllRequested = true;
}

void AssetBrowserPanel::ScanAndQueueUnimported(const std::string& dirAbsPath)
{
    FBZZ_LOG_INFO("AssetBrowserPanel: scanning for unimported assets in [%s]",
                  dirAbsPath.c_str());
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(
             fs::path(dirAbsPath.begin(), dirAbsPath.end()),
             fs::directory_options::skip_permission_denied, ec))
    {
        if (!entry.is_regular_file(ec)) continue;

        const std::string absPath = entry.path().string();
        const std::string ext = util::StringUtils::ToLower(
            util::FileSystem::GetExtension(absPath));
        if (!IsImportableRaw(ext)) continue;

        // 重複チェック
        bool found = false;
        for (const auto& p : m_pendingImports)
            if (p.path == absPath) { found = true; break; }
        if (found) continue;

        if (IsAlreadyImported(absPath)) continue;

        m_pendingImports.push_back({ absPath, PendingImport::Kind::Fbx });
    }

    FBZZ_LOG_INFO("AssetBrowserPanel: scan complete — %zu file(s) queued for import",
                  m_pendingImports.size());
    if (!m_pendingImports.empty())
        m_importAllRequested = true;
}

// ─── インポートバッジバー ─────────────────────────────────────────────────────

void AssetBrowserPanel::DrawPendingImportBar(EditorContext&)
{
    if (m_pendingImports.empty()) return;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.30f, 0.18f, 0.05f, 1.0f));
    ImGui::BeginChild("##pending_bar", { 0.0f, 36.0f }, false);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
    ImGui::Text("  ! 未変換ファイル %zu 件", m_pendingImports.size());
    ImGui::PopStyleColor();

    ImGui::SameLine();

    if (ImGui::SmallButton("Import All"))
        m_importAllRequested = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Dismiss"))
        m_pendingImports.clear();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Separator();
}

// ─── インポート結果バー ───────────────────────────────────────────────────────

void AssetBrowserPanel::DrawImportResultBar(EditorContext&)
{
    if (!m_showImportResults || m_importResults.empty()) return;

    size_t failed = 0;
    for (const auto& r : m_importResults) if (!r.ok) ++failed;
    const size_t succeeded = m_importResults.size() - failed;

    // 背景色付きの矩形を描いてからテキストを重ねる
    const ImVec4 bgColor = (failed > 0)
        ? ImVec4(0.45f, 0.10f, 0.10f, 0.85f)
        : ImVec4(0.10f, 0.35f, 0.10f, 0.85f);
    const float lineH  = ImGui::GetTextLineHeightWithSpacing();
    const float rows   = static_cast<float>(1 + failed); // サマリー1行 + 失敗行
    const float height = rows * lineH + ImGui::GetStyle().FramePadding.y * 2.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1 = { p0.x + ImGui::GetContentRegionAvail().x, p0.y + height };
    ImGui::GetWindowDrawList()->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(bgColor), 3.0f);

    ImGui::SetCursorScreenPos({ p0.x + 6.0f, p0.y + ImGui::GetStyle().FramePadding.y });

    if (failed > 0)
        ImGui::TextColored({ 1.0f, 0.5f, 0.5f, 1.0f },
            "[!] Import: %zu OK  %zu FAILED", succeeded, failed);
    else
        ImGui::TextColored({ 0.5f, 1.0f, 0.5f, 1.0f },
            "[v] Import: %zu file(s) OK", succeeded);

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
    if (ImGui::SmallButton("Dismiss"))
        m_showImportResults = false;

    for (const auto& r : m_importResults) {
        if (!r.ok) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
            ImGui::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "  x  %s", r.filename.c_str());
        }
    }

    ImGui::Dummy({ 0.0f, 2.0f });
    ImGui::Separator();
}

// ─── インポートキュー処理 ─────────────────────────────────────────────────────
// WHY: OnRenderContent はウィンドウが collapsed のとき呼ばれないため
//      OnBeforeBegin (毎フレーム確実に呼ばれる) でインポートを処理する。

void AssetBrowserPanel::OnBeforeBegin(EditorContext&)
{
    // ── スレッド完了チェック ──────────────────────────────────────────────
    if (m_importThreadDone.load()) {
        m_importThreadDone.store(false);
        m_isImporting.store(false);
        if (m_importThread.joinable()) m_importThread.join();
        EditorTaskOverlay::End();
        FBZZ_LOG_INFO("AssetBrowserPanel: all imports done, flushing asset cache");
        asset::AssetManager::FlushFailed();
        m_showImportResults = true;
        RefreshDirectory();
        return;
    }

    // ── インポート中: 毎フレーム進捗をオーバーレイに反映 ──────────────────
    if (m_isImporting.load()) {
        const size_t total = m_importTotal.load();
        if (total > 0) {
            EditorTaskOverlay::SetProgress(
                static_cast<float>(m_importDone.load()) / static_cast<float>(total));
        }
        {
            std::lock_guard<std::mutex> lock(m_importStatusMtx);
            EditorTaskOverlay::SetStatus(m_importStatusStr.c_str());
        }
        return;
    }

    // ── インポート開始 ────────────────────────────────────────────────────
    if (!m_importAllRequested || m_pendingImports.empty()) {
        m_importAllRequested = false;
        return;
    }
    m_importAllRequested = false;

    const size_t total = m_pendingImports.size();
    m_importTotal.store(total);
    m_importDone.store(0);
    m_isImporting.store(true);
    m_importThreadDone.store(false);
    m_showImportResults = false;
    m_importResults.clear();
    FBZZ_LOG_INFO("AssetBrowserPanel: starting import of %zu file(s)", total);
    EditorTaskOverlay::Begin("Importing Assets");
    EditorTaskOverlay::SetProgress(0.0f);

    auto imports = std::move(m_pendingImports);

    m_importThread = std::thread([this, imports = std::move(imports)]() mutable {
        try {
            for (const auto& imp : imports) {
                FBZZ_LOG_INFO("AssetBrowserPanel: importing [%s]", imp.path.c_str());
                {
                    std::lock_guard<std::mutex> lock(m_importStatusMtx);
                    m_importStatusStr = util::FileSystem::GetFilename(imp.path);
                }

                namespace fs = std::filesystem;
                const fs::path srcPath(imp.path.begin(), imp.path.end());
                const std::string outDir =
                    (srcPath.parent_path() / srcPath.stem()).string();
                FBZZ_LOG_INFO("AssetBrowserPanel: FBX outDir = [%s]", outDir.c_str());
                const bool ok = FbxImportTool::Import(imp.path, outDir, imp.path);

                if (ok)
                    FBZZ_LOG_INFO("AssetBrowserPanel: import OK [%s]", imp.path.c_str());
                else
                    FBZZ_LOG_ERROR("AssetBrowserPanel: import FAILED [%s]", imp.path.c_str());

                {
                    std::lock_guard<std::mutex> lk(m_importStatusMtx);
                    m_importResults.push_back({
                        util::FileSystem::GetFilename(imp.path), ok });
                }
                m_importDone.fetch_add(1);
            }
        } catch (const std::exception& e) {
            FBZZ_LOG_ERROR("AssetBrowserPanel: import thread exception: %s", e.what());
        } catch (...) {
            FBZZ_LOG_ERROR("AssetBrowserPanel: import thread unknown exception");
        }
        m_importThreadDone.store(true);
    });
}

} // namespace fbzz::editor
