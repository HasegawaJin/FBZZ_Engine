// FBZZ Engine
// AssetBrowserImport.cpp | fbzz::editor
// AssetBrowser の未変換アセット検出とバックグラウンドインポート
#include "AssetBrowserCommon.hpp"
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <toml++/toml.hpp>
#include <sstream>
#include <string_view>

namespace fbzz::editor {

bool AssetBrowserPanel::IsImportableRaw(const std::string& ext)
{
    // WHY: 画像は PNG / JPG 等を直接 GPU リソースとして読み込めるため、
    //      Asset Browser の変換パイプラインはモデル形式だけを対象にする。
    return ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb";
}

bool AssetBrowserPanel::IsTextureRaw(const std::string& ext)
{
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".tga" || ext == ".dds" || ext == ".bmp" ||
           ext == ".hdr" || ext == ".exr";
}

namespace {

// ── Import Preset ヘルパー ────────────────────────────────────────────────────

struct ImportPreset {
    std::string      name;
    FbxImportOptions options;
};

std::string GetPresetsDir(const std::string& rootPath)
{
    return util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(rootPath) / ".import_presets");
}

std::vector<ImportPreset> LoadPresetsFromDir(const std::string& presetsDir)
{
    std::vector<ImportPreset> result;
    namespace fs = std::filesystem;
    const fs::path dir = util::FileSystem::PathFromUtf8(presetsDir);
    if (!util::FileSystem::Exists(dir)) return result;
    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = util::StringUtils::ToLower(
                util::FileSystem::PathToUtf8(entry.path().extension()));
            if (ext != ".toml") continue;
            std::string text;
            if (!util::FileSystem::ReadText(util::FileSystem::PathToUtf8(entry.path()), text)) continue;
            std::istringstream ss(text);
            const auto parsed = toml::parse(ss);
            if (!parsed) continue;
            ImportPreset p;
            p.name = entry.path().stem().string();
            const auto& tbl = parsed.table();
            if (auto v = tbl["options"]["flip_green_channel"].value<bool>())
                p.options.flipGreenChannel = *v;
            result.push_back(std::move(p));
        }
    } catch (...) {}
    std::sort(result.begin(), result.end(),
              [](const ImportPreset& a, const ImportPreset& b) { return a.name < b.name; });
    return result;
}

bool SavePreset(const std::string& presetsDir, const std::string& name, const FbxImportOptions& opts)
{
    util::FileSystem::EnsureDirectory(presetsDir);
    toml::table optTbl;
    optTbl.insert("flip_green_channel", opts.flipGreenChannel);
    toml::table root;
    root.insert("options", std::move(optTbl));
    std::ostringstream ss;
    ss << root;
    const std::string path = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(presetsDir) / (name + ".toml"));
    return util::FileSystem::WriteText(path, ss.str());
}

// ── テクスチャメタデータ (stem.asset) ─────────────────────────────────────────
// WHY: モデルインポートも stem.asset を生成するため、テクスチャ設定も同じ拡張子に統合する。
//      [texture] セクションの有無でモデル asset と区別する。

static constexpr const char* kCompressNames[] = { "None (Raw)", "BC1 (RGB)", "BC3 (RGBA)", "BC5 (RG)", "BC7 (High Quality)" };
static constexpr int kCompressCount = 5;

std::string GetTexAssetPath(const std::string& texAbsPath)
{
    const std::filesystem::path p = util::FileSystem::PathFromUtf8(texAbsPath);
    const std::string stem = util::FileSystem::PathToUtf8(p.stem());
    return util::FileSystem::PathToUtf8(p.parent_path() / (stem + ".asset"));
}

bool LoadTexMeta(const std::string& texAbsPath, bool& srgb, bool& mipmaps, int& compress, bool& flipGreen)
{
    const std::string assetPath = GetTexAssetPath(texAbsPath);
    std::string text;
    if (!util::FileSystem::ReadText(assetPath, text)) return false;
    std::istringstream iss(text);
    const auto parsed = toml::parse(iss);
    if (!parsed) return false;
    const auto& tbl = parsed.table();
    if (!tbl["texture"]) return false; // モデル .asset と区別
    if (auto v = tbl["texture"]["srgb"].value<bool>())              srgb      = *v;
    if (auto v = tbl["texture"]["generate_mipmaps"].value<bool>())  mipmaps   = *v;
    if (auto v = tbl["texture"]["flip_green_channel"].value<bool>()) flipGreen = *v;
    if (auto v = tbl["texture"]["compression"].value<std::string>()) {
        for (int i = 0; i < kCompressCount; ++i)
            if (*v == kCompressNames[i]) { compress = i; break; }
    }
    return true;
}

void SaveTexMeta(const std::string& texAbsPath, bool srgb, bool mipmaps, int compress, bool flipGreen)
{
    toml::table texTbl;
    texTbl.insert("srgb",               srgb);
    texTbl.insert("generate_mipmaps",   mipmaps);
    texTbl.insert("flip_green_channel", flipGreen);
    texTbl.insert("compression",        std::string(kCompressNames[compress]));
    toml::table root;
    root.insert("type",    std::string("texture_meta"));
    root.insert("texture", std::move(texTbl));
    std::ostringstream ss;
    ss << root;
    util::FileSystem::WriteText(GetTexAssetPath(texAbsPath), ss.str());
}

// ── 除外パターンヘルパー ──────────────────────────────────────────────────────

bool IsExcludedByPattern(const std::string& absPath)
{
    const std::string stem = util::StringUtils::ToLower(
        util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(absPath).stem()));
    static constexpr const char* kExcludeSuffixes[] = {
        "_backup", "_old", "_wip", "_ref", "_tmp", "_test", "_unused", "_bak"
    };
    for (const auto* suffix : kExcludeSuffixes) {
        const size_t slen = std::strlen(suffix);
        if (stem.size() >= slen && stem.compare(stem.size() - slen, slen, suffix) == 0)
            return true;
    }
    return false;
}
} // namespace

bool AssetBrowserPanel::IsOutdated(const std::string& absPath)
{
    namespace fs = std::filesystem;
    const fs::path p = util::FileSystem::PathFromUtf8(absPath);
    const std::string stem = util::FileSystem::PathToUtf8(p.stem());
    const fs::path fzasset = p.parent_path() / (stem + ".asset");
    if (!util::FileSystem::Exists(fzasset)) return false;
    std::error_code ec;
    const auto srcTime    = fs::last_write_time(p,       ec); if (ec) return false;
    const auto assetTime  = fs::last_write_time(fzasset, ec); if (ec) return false;
    return srcTime > assetTime;
}

bool AssetBrowserPanel::IsAlreadyImported(const std::string& absPath)
{
    namespace fs = std::filesystem;
    // WHY: 新インポート形式では stem.asset が FBX と同じディレクトリに置かれる。
    const fs::path p = util::FileSystem::PathFromUtf8(absPath);
    const std::string stem = util::FileSystem::PathToUtf8(p.stem());
    const fs::path check = p.parent_path() / (stem + ".asset");
    return util::FileSystem::Exists(check);
}

void AssetBrowserPanel::TryQueuePendingImport(const std::string& relPath)
{
    const std::string ext = util::StringUtils::ToLower(
        util::FileSystem::GetExtension(relPath));
    if (!IsImportableRaw(ext)) return;

    namespace fs = std::filesystem;
    const std::string absPath = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(m_rootPath) / util::FileSystem::PathFromUtf8(relPath));

    if (IsAlreadyImported(absPath)) return;
    if (IsExcludedByPattern(absPath)) return;

    // 重複チェック
    for (const auto& p : m_pendingImports)
        if (p.path == absPath) return;
    for (const auto& p : m_pendingConfirmImports)
        if (p == absPath) return;

    // ウォッチャー経由の新規ファイルはインポート設定確認キューへ積む（UE 同様の動線）
    m_pendingConfirmImports.push_back(absPath);
}

void AssetBrowserPanel::ScanAndQueueUnimported(const std::string& dirAbsPath)
{
    FBZZ_LOG_INFO("AssetBrowserPanel: scanning for unimported assets in [%s]",
                  dirAbsPath.c_str());
    m_outdatedPaths.clear();
    for (const auto& path : util::FileSystem::ListFilesRecursive(util::FileSystem::PathFromUtf8(dirAbsPath)))
    {
        const std::string absPath = util::FileSystem::PathToUtf8(path);
        const std::string ext = util::StringUtils::ToLower(
            util::FileSystem::GetExtension(absPath));
        if (!IsImportableRaw(ext)) continue;

        // 重複チェック
        bool found = false;
        for (const auto& p : m_pendingImports)
            if (p.path == absPath) { found = true; break; }
        if (found) continue;

        if (IsExcludedByPattern(absPath)) continue;

        if (IsAlreadyImported(absPath)) {
            if (IsOutdated(absPath))
                m_outdatedPaths.insert(absPath);
            continue;
        }

        m_pendingImports.push_back({ absPath });
    }

    FBZZ_LOG_INFO("AssetBrowserPanel: scan complete — %zu file(s) queued for import",
                  m_pendingImports.size());
    if (!m_pendingImports.empty())
        m_importAllRequested = true;
}

// ─── インポートバッジバー ─────────────────────────────────────────────────────

void AssetBrowserPanel::DrawPendingImportBar(EditorContext&)
{
    // 初回スキャンで積まれた m_pendingImports のみ表示（ウォッチャー経由はモーダル経由）
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
//      OnBeforeBegin (毎フレーム確実に呼ばれる) でウォッチャーとインポートを処理する。

void AssetBrowserPanel::OnBeforeBegin(EditorContext&)
{
    // ── ファイルシステム監視 ──────────────────────────────────────────────
    // WHY: Poll() を OnBeforeBegin に置くことで、パネルが collapsed / 非表示でも
    //      イベントを取りこぼさず、追加ファイルのインポートとツリー更新が即座に走る。
    for (const auto& ev : m_watcher.Poll())
    {
        const std::string absPath = util::FileSystem::NormalizePathSeparators(
            m_rootPath + ev.path);

        if (ev.type == AssetFileWatcher::EventType::Added   ||
            ev.type == AssetFileWatcher::EventType::Removed ||
            ev.type == AssetFileWatcher::EventType::Renamed)
        {
            // 変更が起きたディレクトリのツリーキャッシュを無効化
            InvalidateTreeCache(util::FileSystem::GetDirectory(absPath));

            // カレントディレクトリ以下の変化ならグリッドも再スキャン
            if (util::FileSystem::IsChildPathText(absPath, m_currentPath))
                RefreshDirectory();
        }

        if (ev.type == AssetFileWatcher::EventType::Added)
            TryQueuePendingImport(ev.path);
    }

    // ── スレッド完了チェック ──────────────────────────────────────────────
    if (m_importThreadDone.load()) {
        m_importThreadDone.store(false);
        m_isImporting.store(false);
        m_importFuture = {};
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

    m_importFuture = fbzz::TaskSystem::Submit([this, imports = std::move(imports)]() mutable {
        try {
            for (const auto& imp : imports) {
                FBZZ_LOG_INFO("AssetBrowserPanel: importing [%s]", imp.path.c_str());
                {
                    std::lock_guard<std::mutex> lock(m_importStatusMtx);
                    m_importStatusStr = util::FileSystem::GetFilename(imp.path);
                }

                namespace fs = std::filesystem;
                const fs::path srcPath = util::FileSystem::PathFromUtf8(imp.path);
                const std::string outDir =
                    util::FileSystem::PathToUtf8(srcPath.parent_path() / srcPath.stem());
                FBZZ_LOG_INFO("AssetBrowserPanel: model outDir = [%s]", outDir.c_str());
                const bool ok = FbxImportTool::Import(imp.path, outDir, imp.path, imp.options);

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

void AssetBrowserPanel::DrawImportSettingsModal(EditorContext& ctx)
{
    // Inspector からの Reimport リクエスト（優先度高）
    if (!ctx.requestOpenImportModal.empty() && !ImGui::IsPopupOpen("Import Settings")) {
        const std::string reqExt = util::StringUtils::ToLower(
            util::FileSystem::GetExtension(ctx.requestOpenImportModal));
        m_importSettings.path        = ctx.requestOpenImportModal;
        m_importSettings.options     = ctx.defaultImportOptions;
        m_importSettings.open        = true;
        m_importSettings.fromWatcher = false;
        m_importSettings.isTexture   = IsTextureRaw(reqExt);
        ctx.requestOpenImportModal.clear();
    }

    // ウォッチャー確認キューが溜まっていて、モーダルが閉じているなら自動オープン
    if (!m_pendingConfirmImports.empty()
        && !m_importSettings.open
        && !ImGui::IsPopupOpen("Import Settings")) {
        m_importSettings.path        = m_pendingConfirmImports.front();
        m_importSettings.options     = ctx.defaultImportOptions;
        m_importSettings.open        = true;
        m_importSettings.fromWatcher = true;
    }

    if (m_importSettings.open) {
        ImGui::OpenPopup("Import Settings");
        m_importSettings.open = false;
    }

    // 複数ファイルモード: watcher 経由で 2 件以上
    const bool isMulti = m_importSettings.fromWatcher && m_pendingConfirmImports.size() > 1;
    const float modalW = isMulti ? 480.0f : 380.0f;
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, { 0.5f, 0.5f });
    ImGui::SetNextWindowSize({ modalW, 0.0f }, ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("Import Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    // ── テクスチャ専用 UI ─────────────────────────────────────────────────────
    if (m_importSettings.isTexture) {
        ImGui::TextUnformatted(util::FileSystem::GetFilename(m_importSettings.path).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m_importSettings.path.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::IsWindowAppearing())
            LoadTexMeta(m_importSettings.path,
                        m_importSettings.texSRGB,
                        m_importSettings.texMipmaps,
                        m_importSettings.texCompress,
                        m_importSettings.texFlipGreen);

        ImGui::SeparatorText("Texture Options");
        ImGui::Checkbox("sRGB (Color Texture)", &m_importSettings.texSRGB);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("カラーテクスチャ（Albedo 等）は ON。\n法線マップ・ラフネス等リニアデータは OFF。");
        ImGui::Checkbox("Generate Mipmaps", &m_importSettings.texMipmaps);
        ImGui::Checkbox("Flip Green Channel (OpenGL Normal Map)", &m_importSettings.texFlipGreen);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("法線マップの Y 成分を反転します。\nBlender / Maya がデフォルト出力する OpenGL 形式の場合にチェック。");
        ImGui::Spacing();
        ImGui::TextDisabled("Compression");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::BeginCombo("##texcomp", kCompressNames[m_importSettings.texCompress])) {
            for (int i = 0; i < kCompressCount; ++i) {
                const bool sel = (m_importSettings.texCompress == i);
                if (ImGui::Selectable(kCompressNames[i], sel))
                    m_importSettings.texCompress = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Settings saved as  %s.asset",
            util::FileSystem::PathToUtf8(
                util::FileSystem::PathFromUtf8(m_importSettings.path).stem()).c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Apply", { 90.0f, 0.0f })) {
            SaveTexMeta(m_importSettings.path,
                        m_importSettings.texSRGB,
                        m_importSettings.texMipmaps,
                        m_importSettings.texCompress,
                        m_importSettings.texFlipGreen);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", { 90.0f, 0.0f }))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
        return;
    }

    // ── 初期化（ポップアップが開くたびに実行） ─────────────────────────────
    const std::string presetsDir = GetPresetsDir(m_rootPath);
    static std::vector<ImportPreset> s_presets;
    static std::size_t               s_loadRevision = static_cast<std::size_t>(-1);
    static FbxScanResult             s_scan;
    static int                       s_presetSel = -1;
    static char                      s_presetNameBuf[64] = {};
    if (ImGui::IsWindowAppearing()) {
        s_presets      = LoadPresetsFromDir(presetsDir);
        s_loadRevision = 0;
        s_presetSel    = -1;
        s_presetNameBuf[0] = '\0';
        if (!isMulti) {
            s_scan = FbxImportTool::Scan(m_importSettings.path);
            m_importSettings.options.selectedMeshNames = s_scan.meshNames;
            m_importSettings.options.selectedAnimNames = s_scan.animNames;
        }
        // 複数ファイル: チェック状態を初期化（全選択）
        m_pendingConfirmIncludes.assign(m_pendingConfirmImports.size(), true);
    }

    // ── ヘッダー ──────────────────────────────────────────────────────────
    if (isMulti) {
        // ── 複数ファイルリスト ───────────────────────────────────────────
        const int total = static_cast<int>(m_pendingConfirmImports.size());
        int checkedCount = 0;
        for (bool b : m_pendingConfirmIncludes) if (b) ++checkedCount;

        ImGui::TextColored({ 0.95f, 0.75f, 0.25f, 1.0f },
            "%d file(s) detected", total);
        ImGui::SameLine();
        ImGui::TextDisabled("(%d selected)", checkedCount);

        // All / None ボタンを右端に配置
        const float btnW = 38.0f;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - btnW * 2.0f - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::SmallButton("All##chk"))
            std::fill(m_pendingConfirmIncludes.begin(), m_pendingConfirmIncludes.end(), true);
        ImGui::SameLine();
        if (ImGui::SmallButton("None##chk"))
            std::fill(m_pendingConfirmIncludes.begin(), m_pendingConfirmIncludes.end(), false);

        // スクロール可能なファイルリスト
        const float listH = std::min(static_cast<float>(total) * ImGui::GetTextLineHeightWithSpacing() + 8.0f, 160.0f);
        ImGui::BeginChild("##confirm_list", { 0.0f, listH }, true);
        for (int i = 0; i < total; ++i) {
            if (i >= static_cast<int>(m_pendingConfirmIncludes.size()))
                m_pendingConfirmIncludes.push_back(true);
            bool inc = m_pendingConfirmIncludes[i];
            ImGui::PushID(i);
            if (ImGui::Checkbox("##inc", &inc))
                m_pendingConfirmIncludes[i] = inc;
            ImGui::SameLine();

            // 拡張子バッジ（色付き）
            const std::string rawExt = util::FileSystem::GetExtension(m_pendingConfirmImports[i]);
            std::string badge = rawExt.size() > 1 ? rawExt.substr(1) : rawExt;
            for (char& c : badge) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            ImGui::TextDisabled("[%s]", badge.c_str());
            ImGui::SameLine();

            const std::string fname = util::FileSystem::GetFilename(m_pendingConfirmImports[i]);
            if (!inc) ImGui::PushStyleColor(ImGuiCol_Text, { 0.45f, 0.45f, 0.45f, 1.0f });
            ImGui::TextUnformatted(fname.c_str());
            if (!inc) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", m_pendingConfirmImports[i].c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::TextDisabled("Settings below apply to all checked files.");
    } else {
        // ── 単一ファイルヘッダー ─────────────────────────────────────────
        ImGui::TextUnformatted(util::FileSystem::GetFilename(m_importSettings.path).c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", m_importSettings.path.c_str());
    }

    ImGui::Separator();
    ImGui::Spacing();

    // ── プリセット ────────────────────────────────────────────────────────
    if (!s_presets.empty()) {
        ImGui::TextDisabled("Preset");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        const char* previewName = (s_presetSel >= 0 && s_presetSel < (int)s_presets.size())
            ? s_presets[s_presetSel].name.c_str() : "(select)";
        if (ImGui::BeginCombo("##preset", previewName)) {
            for (int i = 0; i < (int)s_presets.size(); ++i) {
                const bool sel = (s_presetSel == i);
                if (ImGui::Selectable(s_presets[i].name.c_str(), sel))
                    s_presetSel = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const bool canLoad = (s_presetSel >= 0 && s_presetSel < (int)s_presets.size());
        if (!canLoad) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Load"))
            m_importSettings.options = s_presets[s_presetSel].options;
        if (!canLoad) ImGui::EndDisabled();
        ImGui::SameLine();
        if (canLoad && ImGui::SmallButton("Delete")) {
            const std::string delPath = util::FileSystem::PathToUtf8(
                util::FileSystem::PathFromUtf8(presetsDir)
                / (s_presets[s_presetSel].name + ".toml"));
            util::FileSystem::RemoveAll(util::FileSystem::PathFromUtf8(delPath));
            s_presets = LoadPresetsFromDir(presetsDir);
            s_presetSel = -1;
        }
        ImGui::Spacing();
    }

    // ── オプション ────────────────────────────────────────────────────────
    ImGui::SeparatorText("Options");
    ImGui::Checkbox("Flip Green Channel (OpenGL Normal Map)",
                    &m_importSettings.options.flipGreenChannel);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip(
            "法線マップの Y 成分を反転します。\n"
            "Blender / Maya がデフォルト出力する OpenGL 形式の場合にチェック。");

    // ── 選択的インポート（単一ファイルのみ） ──────────────────────────────
    if (!isMulti && s_scan.valid && (!s_scan.meshNames.empty() || !s_scan.animNames.empty())) {
        ImGui::Spacing();
        ImGui::SeparatorText("Contents");

        auto drawSelectList = [](const char* label,
                                 const std::vector<std::string>& all,
                                 std::vector<std::string>& selected)
        {
            if (all.empty()) return;
            ImGui::TextDisabled("%s", label);
            ImGui::SameLine();
            if (ImGui::SmallButton("All##sel_all")) selected = all;
            ImGui::SameLine();
            if (ImGui::SmallButton("None##sel_none")) selected.clear();
            ImGui::Spacing();
            for (const auto& name : all) {
                bool checked = false;
                for (const auto& s : selected) if (s == name) { checked = true; break; }
                if (ImGui::Checkbox(name.c_str(), &checked)) {
                    if (checked) {
                        selected.push_back(name);
                    } else {
                        selected.erase(
                            std::remove(selected.begin(), selected.end(), name),
                            selected.end());
                    }
                }
            }
        };
        drawSelectList("Meshes", s_scan.meshNames,
                       m_importSettings.options.selectedMeshNames);
        if (!s_scan.animNames.empty()) {
            ImGui::Spacing();
            drawSelectList("Animations", s_scan.animNames,
                           m_importSettings.options.selectedAnimNames);
        }
    }
    if (isMulti) {
        ImGui::Spacing();
        ImGui::TextDisabled("(Contents selection is available in single-file import)");
    }

    ImGui::Spacing();

    // ── プリセット保存 ────────────────────────────────────────────────────
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##preset_name", "Preset name...", s_presetNameBuf, sizeof(s_presetNameBuf));
    ImGui::SameLine();
    const bool hasName = (s_presetNameBuf[0] != '\0');
    if (!hasName) ImGui::BeginDisabled();
    if (ImGui::SmallButton("Save Preset")) {
        if (SavePreset(presetsDir, s_presetNameBuf, m_importSettings.options)) {
            s_presets = LoadPresetsFromDir(presetsDir);
            s_presetNameBuf[0] = '\0';
        }
    }
    if (!hasName) ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── ボタン行 ──────────────────────────────────────────────────────────
    if (isMulti) {
        // ─ 複数ファイルモード ─
        int checkedCount = 0;
        for (bool b : m_pendingConfirmIncludes) if (b) ++checkedCount;

        // "Import (N)" ボタン
        char importBtnLabel[40];
        std::snprintf(importBtnLabel, sizeof(importBtnLabel), "Import (%d)", checkedCount);
        if (checkedCount == 0) ImGui::BeginDisabled();
        if (ImGui::Button(importBtnLabel, { 120.0f, 0.0f })) {
            for (int i = 0; i < (int)m_pendingConfirmImports.size(); ++i) {
                const bool inc = (i < (int)m_pendingConfirmIncludes.size()) && m_pendingConfirmIncludes[i];
                if (inc)
                    m_pendingImports.push_back({ m_pendingConfirmImports[i], m_importSettings.options });
            }
            m_pendingConfirmImports.clear();
            m_importSettings.fromWatcher = false;
            m_importAllRequested = true;
            ImGui::CloseCurrentPopup();
        }
        if (checkedCount == 0) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("チェックされたファイルをインポート");

        ImGui::SameLine();
        if (ImGui::Button("Import All", { 95.0f, 0.0f })) {
            for (const auto& p : m_pendingConfirmImports)
                m_pendingImports.push_back({ p, m_importSettings.options });
            m_pendingConfirmImports.clear();
            m_importSettings.fromWatcher = false;
            m_importAllRequested = true;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("全 %zu 件をインポート", m_pendingConfirmImports.size());

        ImGui::SameLine();
        if (ImGui::Button("Cancel", { 75.0f, 0.0f })) {
            m_pendingConfirmImports.clear();
            m_importSettings.fromWatcher = false;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("全件スキップ");
    } else {
        // ─ 単一ファイルモード ─
        auto enqueueCurrentFile = [&]() {
            bool found = false;
            for (auto& p : m_pendingImports) {
                if (p.path == m_importSettings.path) {
                    p.options = m_importSettings.options;
                    found = true;
                    break;
                }
            }
            if (!found)
                m_pendingImports.push_back({ m_importSettings.path, m_importSettings.options });
        };
        auto advanceConfirmQueue = [&]() {
            if (!m_pendingConfirmImports.empty()
                && m_pendingConfirmImports.front() == m_importSettings.path) {
                m_pendingConfirmImports.erase(m_pendingConfirmImports.begin());
            }
            if (!m_pendingConfirmImports.empty()) {
                m_importSettings.path        = m_pendingConfirmImports.front();
                m_importSettings.options     = ctx.defaultImportOptions;
                m_importSettings.open        = true;
                m_importSettings.fromWatcher = true;
            } else {
                m_importSettings.fromWatcher = false;
            }
        };

        if (ImGui::Button("Import", { 90.0f, 0.0f })) {
            enqueueCurrentFile();
            m_importAllRequested = true;
            ImGui::CloseCurrentPopup();
            advanceConfirmQueue();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("インポートして次へ");

        ImGui::SameLine();
        if (ImGui::Button("Cancel", { 90.0f, 0.0f })) {
            ImGui::CloseCurrentPopup();
            advanceConfirmQueue();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("スキップ");
    }

    ImGui::EndPopup();
}

} // namespace fbzz::editor
