/// @file    AssetBrowserImport.cpp
/// @brief   AssetBrowser の未変換アセット検出とバックグラウンドインポート。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "AssetBrowserCommon.hpp"
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Editor/Import/ImportSettingsSchema.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Editor/Util/Toast.hpp>
#include <Editor/Util/AssetSearch.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <toml++/toml.hpp>
#include <chrono>
#include <sstream>
#include <string_view>

namespace fbzz::editor {

bool AssetBrowserPanel::IsImportableRaw(const std::string& ext)
{
    // WHY: この関数は .fzasset 生成が必要な raw モデル形式だけを扱う。
    //      テクスチャ形式は .meta sidecar 生成なので IsTextureRaw() と併用する。
    return ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb";
}

bool AssetBrowserPanel::IsTextureRaw(const std::string& ext)
{
    // .dds は IBL ベイク済みキューブマップ等の GPU 直接ロード形式のため除外する。
    // .meta サイドカー経由のインポートパイプラインは通さない。
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".tga" || ext == ".bmp" ||
           ext == ".hdr" || ext == ".exr";
}

namespace {

// ── Import Preset ヘルパー ────────────────────────────────────────────────────

// 中身を差し替えるだけで反映できるアセットか。
//
// WHY 原本 (.fbx / .png) を含めないか: あちらは再インポートで生成物を焼き直す経路
//     (NotifyAssetTouched) が別にあり、GPU 資源の作り直しも伴う。ここはキャッシュ済みの
//     値を入れ替えるだけで済む、軽くて失敗しても巻き戻せる対象に限る。
bool IsHotReloadableAsset(const std::string& absPath)
{
    const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(absPath));
    return ext == ".mat"   || ext == ".anim"   || ext == ".animcontroller" ||
           ext == ".mask"  || ext == ".fzdata" || ext == ".physmat"        ||
           ext == ".synth" || ext == ".terrain";
}

const char* SourceDccLabel(FbxSourceDcc value)
{
    switch (value) {
    case FbxSourceDcc::Auto:    return "Auto";
    case FbxSourceDcc::Maya:    return "Maya / FBX SDK";
    case FbxSourceDcc::Blender: return "Blender";
    }
    return "Auto";
}

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
            if (auto v = tbl["options"]["source_dcc"].value<int64_t>())
                p.options.sourceDcc = static_cast<FbxSourceDcc>(*v);
            if (auto v = tbl["options"]["up_axis"].value<int64_t>())
                p.options.upAxis = static_cast<FbxUpAxis>(*v);
            if (auto v = tbl["options"]["normal_map_convention"].value<int64_t>())
                p.options.normalMapConvention = static_cast<NormalMapConvention>(*v);
            if (auto v = tbl["options"]["unit_scale_multiplier"].value<float>())
                p.options.unitScaleMultiplier = *v;
            if (auto v = tbl["options"]["generate_normals"].value<bool>())
                p.options.generateNormals = *v;
            if (auto v = tbl["options"]["generate_tangents"].value<bool>())
                p.options.generateTangents = *v;
            if (auto v = tbl["options"]["generate_tex_descriptors"].value<bool>())
                p.options.generateTexDescriptors = *v;
            if (auto v = tbl["options"]["default_compression"].value<int64_t>())
                p.options.defaultCompression = static_cast<asset::TextureCompression>(*v);
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
    optTbl.insert("source_dcc",                static_cast<int64_t>(opts.sourceDcc));
    optTbl.insert("up_axis",                   static_cast<int64_t>(opts.upAxis));
    optTbl.insert("normal_map_convention",    static_cast<int64_t>(opts.normalMapConvention));
    optTbl.insert("unit_scale_multiplier",    opts.unitScaleMultiplier);
    optTbl.insert("generate_normals",         opts.generateNormals);
    optTbl.insert("generate_tangents",        opts.generateTangents);
    optTbl.insert("generate_tex_descriptors", opts.generateTexDescriptors);
    optTbl.insert("default_compression",      static_cast<int64_t>(opts.defaultCompression));
    toml::table root;
    root.insert("options", std::move(optTbl));
    std::ostringstream ss;
    ss << root;
    const std::string path = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(presetsDir) / (name + ".toml"));
    return util::FileSystem::WriteText(path, ss.str());
}

// ── テクスチャメタデータ (.tex descriptor) ─────────────────────────────────
// 旧 stem.asset [texture] セクション形式から TexDescSerializer (.meta TOML) に移行。

// テクスチャの隣に置く ".meta" サイドカーのパスを返す ("Foo.png" -> "Foo.png.meta")
// WHY: 二重拡張子で元画像を一意に保持する。replace_extension は末尾拡張子を潰すため使わない。
std::string GetTexDescPath(const std::string& texAbsPath)
{
    return texAbsPath + ".meta";
}

std::filesystem::path GetExistingImportedModelPath(const std::filesystem::path& sourcePath)
{
    const std::string stem = util::FileSystem::PathToUtf8(sourcePath.stem());

    // 正規形式: Foo.fbx → Library/Baked/<fbx-guid>/Foo.fzasset (内部コンテナ)
    // WHY: Browser / Scene の保存パスは原本 .fbx に統一し、再生成可能なバイナリは Library に隔離する。
    const std::filesystem::path bundledModel =
        sourcePath.parent_path() / sourcePath.stem() / (stem + ".fzasset");
    const std::string resolved = asset::AssetManager::ResolveAssetPath(
        util::FileSystem::PathToUtf8(bundledModel));
    if (util::FileSystem::Exists(resolved))
        return util::FileSystem::PathFromUtf8(resolved);

    return {};
}

// .tex を読み込んで TextureImportSettings に展開する。なければ GuessTextureType でデフォルト生成。
bool LoadTexMeta(const std::string& texAbsPath, asset::TextureImportSettings& settings)
{
    asset::TextureAsset texAsset;
    asset::TexDescSerializer ser;
    if (ser.Load(GetTexDescPath(texAbsPath), texAsset)) {
        settings = texAsset.settings;
        return true;
    }
    // fallback: 旧 stem.asset [texture] セクション形式
    const std::filesystem::path p = util::FileSystem::PathFromUtf8(texAbsPath);
    const std::string oldPath = util::FileSystem::PathToUtf8(
        p.parent_path() / (util::FileSystem::PathToUtf8(p.stem()) + ".asset"));
    std::string text;
    if (util::FileSystem::ReadText(oldPath, text)) {
        std::istringstream iss(text);
        const auto parsed = toml::parse(iss);
        if (parsed && parsed.table()["texture"]) {
            const auto& tbl = parsed.table();
            settings = asset::DefaultSettingsForType(asset::GuessTextureType(
                util::FileSystem::GetFilename(texAbsPath)));
            if (auto v = tbl["texture"]["srgb"].value<bool>())              settings.srgb      = *v;
            if (auto v = tbl["texture"]["generate_mipmaps"].value<bool>())  settings.mipmaps   = *v;
            if (auto v = tbl["texture"]["flip_green_channel"].value<bool>()) settings.flipGreen = *v;
            return true;
        }
    }
    // .meta も .asset もない: ファイル名からデフォルトを生成
    settings = asset::DefaultSettingsForType(asset::GuessTextureType(
        util::FileSystem::GetFilename(texAbsPath)));
    return true;
}

void SaveTexMeta(const std::string& texAbsPath, const asset::TextureImportSettings& settings)
{
    asset::TextureAsset texAsset;
    texAsset.sourcePath = util::FileSystem::GetFilename(texAbsPath);
    texAsset.settings   = settings;
    asset::TexDescSerializer ser;
    ser.Save(texAsset, GetTexDescPath(texAbsPath));
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

// 同名衝突を避けたコピー先パスを返す ("Foo.png" が存在すれば "Foo (1).png" …)。
// WHY: エクスプローラーからの取り込みで既存アセットを黙って上書きしないよう、Unity 同様に採番する。
std::filesystem::path MakeUniqueDestPath(const std::filesystem::path& desired)
{
    if (!util::FileSystem::Exists(desired)) return desired;
    const std::filesystem::path dir  = desired.parent_path();
    const std::string           stem = util::FileSystem::PathToUtf8(desired.stem());
    const std::string           ext  = util::FileSystem::PathToUtf8(desired.extension());
    for (int i = 1; i < 10000; ++i) {
        const std::filesystem::path candidate =
            dir / (stem + " (" + std::to_string(i) + ")" + ext);
        if (!util::FileSystem::Exists(candidate)) return candidate;
    }
    return desired; // 事実上到達しない
}
} // namespace

bool AssetBrowserPanel::IsOutdated(const std::string& absPath)
{
    namespace fs = std::filesystem;
    const fs::path p = util::FileSystem::PathFromUtf8(absPath);
    const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(absPath));

    if (IsTextureRaw(ext)) {
        const fs::path texFile = util::FileSystem::PathFromUtf8(GetTexDescPath(absPath));
        if (!util::FileSystem::Exists(texFile)) return false;
        std::error_code ec;
        const auto srcTime = fs::last_write_time(p,       ec); if (ec) return false;
        const auto texTime = fs::last_write_time(texFile, ec); if (ec) return false;
        return srcTime > texTime;
    }

    const fs::path modelFile = GetExistingImportedModelPath(p);
    if (modelFile.empty()) return false;

    // インポータ自体が更新されていたら FBX の更新時刻に関係なく作り直す。
    // WHY: 判定材料が「生成物の有無」と「FBX の更新時刻」だけだと、
    //      インポータのコードを直しても古い生成物が使われ続けてしまう。
    //      FBX を消して入れ直しても source_hash が変わらないため同じ罠にはまる。
    //      (詳細は FbxMetaSerializer::kModelImporterVersion のコメント)
    if (FbxMetaSerializer::LoadImporterVersion(absPath)
        < FbxMetaSerializer::kModelImporterVersion)
        return true;

    // 原本と設定の fingerprint を、前回 import 成功時に .meta へ焼いた値と突き合わせる。
    //
    // WHY mtime 比較をやめたか:
    //   import は「生成物を書く → 原本の .meta を書く」順で走るため、成功直後は必ず
    //   meta の mtime > 生成物の mtime になる。旧実装はこれを「古い」と読んでいたので、
    //   一度 import したモデルは永久に再インポート対象のままだった。結果として
    //   起動のたびに全 FBX が焼き直され、↻ バッジも消えなかった。
    //   fingerprint なら「原本が変わったか」「設定が変わったか」だけを見るので、
    //   .meta を書き直す手順そのものが判定に混ざらない。
    //   Inspector やテキストエディタで .meta の import 設定を触った場合も
    //   settings_hash が動くため、同じ 1 本の判定で拾える。
    const FbxMetaSerializer::CacheInfo cache = FbxMetaSerializer::LoadCacheInfo(absPath);
    if (!cache.sourceHash.empty() && !cache.settingsHash.empty()) {
        FbxImportOptions options{};
        (void)FbxMetaSerializer::LoadOptions(absPath, options);
        return cache.sourceHash   != FbxMetaSerializer::SourceHash(absPath)
            || cache.settingsHash != FbxMetaSerializer::SettingsHash(options);
    }

    // fingerprint 未記録の古い .meta / .meta 自体が無いケースは mtime へフォールバックする。
    // 一度再インポートが走れば [cache] が書かれ、以降は上の経路に乗る。
    std::error_code ec;
    const auto srcTime   = fs::last_write_time(p,         ec); if (ec) return false;
    const auto assetTime = fs::last_write_time(modelFile,  ec); if (ec) return false;
    return srcTime > assetTime;
}

bool AssetBrowserPanel::IsAlreadyImported(const std::string& absPath)
{
    namespace fs = std::filesystem;
    const fs::path p = util::FileSystem::PathFromUtf8(absPath);
    const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(absPath));
    if (IsTextureRaw(ext))
        return util::FileSystem::Exists(util::FileSystem::PathFromUtf8(GetTexDescPath(absPath)));

    return !GetExistingImportedModelPath(p).empty();
}

void AssetBrowserPanel::TryQueuePendingImport(const std::string& relPath)
{
    const std::string ext = util::StringUtils::ToLower(
        util::FileSystem::GetExtension(relPath));
    const bool isModelRaw = IsImportableRaw(ext);
    const bool isTextureRaw = IsTextureRaw(ext);
    if (!isModelRaw && !isTextureRaw) return;

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
    for (const auto& p : m_pendingTextureConfirmImports)
        if (p == absPath) return;

    // ウォッチャー経由の新規ファイルは種類別の確認キューへ積む。
    // WHY: FBX は変換ジョブ、テクスチャは .meta サイドカー保存で責務が違う。
    if (isTextureRaw)
        m_pendingTextureConfirmImports.push_back(absPath);
    else
        m_pendingConfirmImports.push_back(absPath);
}

void AssetBrowserPanel::QueueAutomaticReimport(const std::string& absPath)
{
    const std::string ext = util::StringUtils::ToLower(
        util::FileSystem::GetExtension(absPath));
    if (!IsImportableRaw(ext) || IsExcludedByPattern(absPath) ||
        !IsAlreadyImported(absPath) || !IsOutdated(absPath))
        return;

    // 既にキュー投入済み、または焼き直し中なら二重に積まない。
    // WHY m_pendingImports を見るだけでは足りないか: インポート開始時にキューは
    //     ワーカースレッドへ move されて空になるため、走行中の重複判定に使えない。
    //     完了時に取り除かれる m_outdatedPaths を「処理中」の印として兼用する。
    if (!m_outdatedPaths.insert(absPath).second)
        return;

    // 元の .meta に保存された選択メッシュ・クリップ範囲を維持し、
    // インポータ更新だけを適用する。設定が壊れている場合は既定値で復旧する。
    FbxImportOptions options{};
    (void)FbxMetaSerializer::LoadOptions(absPath, options);
    m_pendingImports.push_back({ absPath, std::move(options) });
    m_importAllRequested = true;
}

void AssetBrowserPanel::NotifyAssetTouched(const std::string& absPath)
{
    std::string source = absPath;

    // "<原本>.meta" への変更は原本の import 設定変更。原本側へ読み替えて判定に回す。
    // WHY: Inspector の Import Settings も、テキストエディタでの直接編集も、
    //      最終的に書き換わるのは .meta だけ。ここを拾わないと「設定を変えたのに
    //      生成物が変わらない」ので、結局 Reimport を手で押す運用に戻ってしまう。
    const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(source));
    if (ext == ".meta")
        source = source.substr(0, source.size() - 5);

    const std::string sourceExt = util::StringUtils::ToLower(
        util::FileSystem::GetExtension(source));
    if (!IsImportableRaw(sourceExt))    return;
    if (IsExcludedByPattern(source))    return;
    // まだ一度も import していない原本は「新規」であり、設定を確認させる経路が別にある。
    if (!IsAlreadyImported(source))     return;

    m_scheduledReimports[source] = std::chrono::steady_clock::now();
}

void AssetBrowserPanel::FlushScheduledReimports()
{
    if (m_scheduledReimports.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_scheduledReimports.begin(); it != m_scheduledReimports.end(); ) {
        if (now - it->second < kAutoReimportQuietTime) {
            ++it;
            continue;
        }
        // 待っている間に消された / 別名になったファイルは黙って落とす。
        if (util::FileSystem::Exists(util::FileSystem::PathFromUtf8(it->first))) {
            // 実際に作り直しが要るかは QueueAutomaticReimport の IsOutdated が決める。
            // 保存し直しただけで中身が同じなら source_hash が変わるので焼き直す。
            QueueAutomaticReimport(it->first);
        }
        it = m_scheduledReimports.erase(it);
    }
}

void AssetBrowserPanel::ScanAndQueueUnimported(const std::string& dirAbsPath)
{
    // WHY ここで m_outdatedPaths を空にしないか: この集合は「バッジ表示用の再計算結果」
    //     から「キュー投入済み / 焼き直し中の印」へ役割が変わった。マウント追加でも
    //     呼ばれるため、走行中のバッチの印まで消すと同じ原本を二重に積んでしまう。
    for (const auto& path : util::FileSystem::ListFilesRecursive(util::FileSystem::PathFromUtf8(dirAbsPath)))
    {
        const std::string absPath = util::FileSystem::PathToUtf8(path);
        const std::string ext = util::StringUtils::ToLower(
            util::FileSystem::GetExtension(absPath));
        if (!IsImportableRaw(ext) && !IsTextureRaw(ext)) continue;

        if (IsExcludedByPattern(absPath)) continue;

        if (IsAlreadyImported(absPath)) {
            // インポータ版更新や原本更新で既存モデルが古くなった場合は、
            // ユーザー確認を挟まず保存済み設定のまま再インポートする。
            // WHY: targetPath のような派生データの修正を、全 FBX 手動操作へしないため。
            QueueAutomaticReimport(absPath);
            continue;
        }

        // 重複チェック
        bool found = false;
        for (const auto& p : m_pendingImports)       if (p.path == absPath) { found = true; break; }
        for (const auto& p : m_pendingConfirmImports) if (p == absPath)      { found = true; break; }
        for (const auto& p : m_pendingTextureConfirmImports) if (p == absPath) { found = true; break; }
        if (found) continue;

        // ウォッチャー経由と同じ経路へ積む → 必ず種類別 Import Settings を経由してインポート
        if (IsTextureRaw(ext))
            m_pendingTextureConfirmImports.push_back(absPath);
        else
            m_pendingConfirmImports.push_back(absPath);
    }
}

// ─── エクスプローラーからの D&D 取り込み ──────────────────────────────────────

void AssetBrowserPanel::AcceptExternalDrop(EditorContext& ctx)
{
    // ドラッグ中のライブハイライト状態を毎フレーム取り込む (ドロップ確定前のフォルダ強調に使う)。
    m_extDragActive = ctx.externalDragActive;
    m_extDragPoint  = { ctx.externalDragX, ctx.externalDragY };

    if (ctx.droppedExternalFiles.empty()) return;
    // 実コピーは OnRenderContent 末尾で確定する。ここではドロップ内容と位置を退避するだけ。
    m_externalDrop.files     = std::move(ctx.droppedExternalFiles);
    m_externalDrop.point     = { ctx.droppedExternalFilesX, ctx.droppedExternalFilesY };
    m_externalDrop.targetDir.clear();
    m_externalDrop.active    = true;
    m_externalDrop.hit       = false;
    ctx.droppedExternalFiles.clear();
}

void AssetBrowserPanel::ConsiderExternalDropTarget(
    const std::string& folderAbs, const ImVec2& mn, const ImVec2& mx)
{
    const auto contains = [&](const ImVec2& p) {
        return p.x >= mn.x && p.x < mx.x && p.y >= mn.y && p.y < mx.y;
    };

    // (1) 実ドロップ: 最初にヒットしたフォルダを取り込み先に採用する。
    if (m_externalDrop.active && !m_externalDrop.hit && contains(m_externalDrop.point)) {
        m_externalDrop.targetDir = folderAbs;
        m_externalDrop.hit       = true;
    }

    // (2) ドラッグ中: ドロップ確定前のフォルダをハイライトして落とし先を明示する (Unity 風)。
    if (m_extDragActive && contains(m_extDragPoint)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(mn, mx, ImGui::GetColorU32(ImVec4(0.35f, 0.62f, 0.95f, 0.16f)), 3.0f);
        dl->AddRect(mn, mx, ImGui::GetColorU32(ImVec4(0.40f, 0.68f, 1.0f, 0.95f)), 3.0f, 0, 2.0f);
    }
}

void AssetBrowserPanel::FinalizeExternalDrop()
{
    if (!m_externalDrop.active) return;
    // ヒットしたフォルダが無ければ現在フォルダへ取り込む。
    const std::string target = m_externalDrop.targetDir.empty()
        ? m_currentPath : m_externalDrop.targetDir;
    CopyExternalFilesInto(m_externalDrop.files, target);
    m_externalDrop = ExternalDrop{}; // クリア
}

void AssetBrowserPanel::CopyExternalFilesInto(
    const std::vector<std::string>& sources, const std::string& destDirUtf8)
{
    if (sources.empty()) return;

    namespace fs = std::filesystem;
    // 取り込み先が無効ならルートへフォールバック。
    const std::string destUtf8 =
        util::FileSystem::IsDirectory(destDirUtf8) ? destDirUtf8 : m_rootPath;
    const fs::path destDir = util::FileSystem::PathFromUtf8(destUtf8);

    int  copiedCount = 0;
    bool anyCopied = false;
    for (const std::string& src : sources) {
        if (!util::FileSystem::Exists(src)) continue;
        // 既に Assets 配下にあるものはコピーしない (自分自身への複製を防ぐ)。
        if (util::FileSystem::IsChildPathText(src, m_rootPath)) continue;

        const fs::path srcPath = util::FileSystem::PathFromUtf8(src);
        const fs::path dest    = MakeUniqueDestPath(destDir / srcPath.filename());

        const bool ok = util::FileSystem::IsDirectory(src)
            ? util::FileSystem::CopyDirectoryRecursive(srcPath, dest, /*overwrite=*/false)
            : util::FileSystem::CopyFile(srcPath, dest, /*overwrite=*/false);

        if (ok) {
            anyCopied = true;
            ++copiedCount;
            FBZZ_LOG_INFO("Imported dropped asset: %s -> %s",
                          src.c_str(),
                          util::FileSystem::PathToUtf8(dest).c_str());
        } else {
            FBZZ_LOG_WARN("Failed to import dropped asset: %s", src.c_str());
            Toast::Error("Import failed: " + util::FileSystem::GetFilename(src));
        }
    }

    // ウォッチャーが Added を拾ってインポート設定モーダルを自動表示するが、
    // 表示中フォルダのグリッドは即座に反映されるよう明示的に更新する。
    if (anyCopied) {
        const std::string where = util::FileSystem::GetFilename(destUtf8);
        Toast::Success(std::to_string(copiedCount) +
                       (copiedCount == 1 ? " asset imported" : " assets imported") +
                       (where.empty() ? "" : "  \xE2\x86\x92 " + where));
        RefreshDirectory();
    }
}

// ─── インポートバッジバー ─────────────────────────────────────────────────────

// 「! 未変換ファイル N 件 / Import All / Dismiss」の警告バーはここにあった。
// WHY 消したか: m_pendingImports に積まれるのは、自動再インポートと明示的な Import
//     メニューだけになった。どちらも積まれた次のフレームに走り出すので、バーが
//     出るのは実質「今インポート中」の一瞬だけ。人が押す必要のない Import All と、
//     自動処理を握り潰すだけの Dismiss を、警告色で常設する理由が無くなった。
//     未インポートの新規ファイルは従来どおり Import Settings の確認へ流れる。

// ─── インポートキュー処理 ─────────────────────────────────────────────────────
// WHY: OnRenderContent はウィンドウが collapsed のとき呼ばれないため
//      OnBeforeBegin (毎フレーム確実に呼ばれる) でウォッチャーとインポートを処理する。

void AssetBrowserPanel::OnBeforeBegin(EditorContext& ctx)
{
    // ── エクスプローラーからの D&D 取り込み ────────────────────────────────
    // WHY: 実コピーはドロップ位置のフォルダを判定できる OnRenderContent 末尾で行う。
    //      前フレームで解決されなかったドロップ (パネルが畳まれていた等) はここで現在フォルダへ確定する。
    if (m_externalDrop.active)
        FinalizeExternalDrop();
    AcceptExternalDrop(ctx);

    // ── ファイルシステム監視 ──────────────────────────────────────────────
    // WHY: Poll() を OnBeforeBegin に置くことで、パネルが collapsed / 非表示でも
    //      イベントを取りこぼさず、追加ファイルのインポートとツリー更新が即座に走る。
    bool needsDirectoryRefresh = false;
    const std::vector<AssetFileWatcher::FileEvent> watcherEvents = m_watcher.Poll();

    // 素材を一度に大量投入すると OS の通知バッファが溢れ、その回の変更が丸ごと捨てられる。
    // 個別イベントに追従する処理は全て空振りするので、ここだけは総取っ替えで作り直す。
    if (m_watcher.ConsumeOverflow()) {
        ResyncAfterWatcherOverflow();
        Toast::Info("Asset Browser resynced (file notifications overflowed)");
    }

    // 共通アセット索引へ同じイベントを流し、検索結果を実ファイルに追従させる。
    // WHY ここで流すか: AssetBrowser が唯一の AssetFileWatcher 所有者であり、
    //     Poll() はイベントを消費してキューを空にする。ここを通さないと
    //     索引は次のフル再構築まで古いままになる。
    AssetSearch::ApplyFileEvents(watcherEvents);

    for (const auto& ev : watcherEvents)
    {
        // WHY: 文字列連結では m_rootPath 末尾に '/' がない場合、監視パスが壊れる。
        const std::filesystem::path watcherRoot = util::FileSystem::PathFromUtf8(m_rootPath);
        const std::string absPath = util::FileSystem::PathToUtf8(
            (watcherRoot / util::FileSystem::PathFromUtf8(ev.path)).lexically_normal());
        const std::string oldAbsPath = ev.oldPath.empty()
            ? std::string{}
            : util::FileSystem::PathToUtf8(
                (watcherRoot / util::FileSystem::PathFromUtf8(ev.oldPath)).lexically_normal());

        // 中身が変わったアセットのサムネイルは作り直させる。
        // WHY: プレビューは「まだ書き込み途中」「まだインポートされていない」を失敗として
        //      抱え込む。素材を一括で入れた直後は必ずその状態を通るので、書き終わりの
        //      通知でキャッシュを捨てないと、直ったこと自体に気付けずアイコンのまま残る。
        {
            const std::string touched = util::FileSystem::NormalizePathSeparators(absPath);
            ResetAssetPreviewCache(touched);
            // "<原本>.meta" の更新は原本の見た目 (Sprite 切り抜き / インポート設定) に効く。
            if (util::StringUtils::ToLower(util::FileSystem::GetExtension(touched)) == ".meta")
                ResetAssetPreviewCache(touched.substr(0, touched.size() - 5));
            if (!oldAbsPath.empty())
                ResetAssetPreviewCache(util::FileSystem::NormalizePathSeparators(oldAbsPath));
        }

        if (ev.type == AssetFileWatcher::EventType::Added   ||
            ev.type == AssetFileWatcher::EventType::Removed ||
            ev.type == AssetFileWatcher::EventType::Renamed)
        {
            if (ev.type == AssetFileWatcher::EventType::Removed
                && util::StringUtils::ToLower(util::FileSystem::GetExtension(absPath)) != ".meta") {
                // 削除後に同じ名前で別アセットを作成しても、旧 GUID の逆引きが残らないよう
                // watcher の Removed を索引にも通知する。.meta 単体の通知は本体が残るため無視する。
                asset::AssetDatabase::OnAssetRemoved(absPath);
            }
            if (ev.type == AssetFileWatcher::EventType::Renamed && !oldAbsPath.empty()) {
                // Explorer / IDE からの移動も Asset Browser 内の移動と同じ GUID 更新経路へ
                // 通す。ここを欠くと .meta は一緒に移動しても、実行中の索引だけが旧パスを
                // 保持し、次回保存時に参照を正しく GUID 化できない。
                asset::AssetDatabase::OnAssetMoved(oldAbsPath, absPath);
            }

            // 変更が起きたディレクトリのツリーキャッシュを無効化
            InvalidateTreeCache(util::FileSystem::GetDirectory(absPath));
            if (!oldAbsPath.empty())
                InvalidateTreeCache(util::FileSystem::GetDirectory(oldAbsPath));

            // 移動元・移動先、または表示中フォルダ自体の変化ならグリッドも再スキャンする。
            if (util::FileSystem::IsChildPathText(absPath, m_currentPath) ||
                util::FileSystem::IsChildPathText(m_currentPath, absPath) ||
                (!oldAbsPath.empty() &&
                 (util::FileSystem::IsChildPathText(oldAbsPath, m_currentPath) ||
                  util::FileSystem::IsChildPathText(m_currentPath, oldAbsPath))))
                needsDirectoryRefresh = true;
        }

        if (ev.type == AssetFileWatcher::EventType::Added) {
            TryQueuePendingImport(ev.path);
            // 追加された対象に guid (.meta) を発行する。フォルダもここを通る。
            // WHY: 生成箇所 (Create メニュー / ツリー右クリック / エクスプローラー D&D /
            //      OS 側の操作) ごとに .meta 発行を書くと必ずどれかが抜ける。
            //      ウォッチャーの Added は全経路が合流する唯一の地点なので、ここ 1 箇所に集約する。
            //      ただし FBX は Import ボタンまで原本の .meta を作らない。
            const std::string addedExt = util::StringUtils::ToLower(
                util::FileSystem::GetExtension(absPath));
            if (addedExt != ".fbx")
                (void)asset::AssetDatabase::GuidFromPath(absPath);
        }

        // 追加・更新・リネームのどれで届いても、インポート済みの原本 (と その .meta) は
        // 自動再インポートの候補として拾う。実際に焼き直すかは fingerprint が決める。
        //
        // WHY 3 種すべて見るか: 「上書き保存」が必ず Modified で届くとは限らない。
        //   DCC やエクスプローラーはテンポラリへ書いてから置き換える実装が多く、その場合は
        //   Added / Renamed になる。Modified だけを見ていたので、書き出し方によっては
        //   変更が黙って無視され、結局 Reimport を手で押す運用が残っていた。
        if (ev.type == AssetFileWatcher::EventType::Added    ||
            ev.type == AssetFileWatcher::EventType::Modified ||
            ev.type == AssetFileWatcher::EventType::Renamed)
            NotifyAssetTouched(absPath);

        // .prefab の内容が変わったら、シーンに置いてあるインスタンスへ反映させる。
        // WHY: アセットを直したのに配置済みの実体が古いままだと、シーンとアセットの
        //      内容が黙って食い違う。反映の実行は EditorApp 側 (シーンを作り直すため
        //      パネル描画中に走らせられない)。ここでは対象パスを積むだけにする。
        //      自分の Apply / Prefab 保存で起きた変更は PrefabSerializer 側で弾かれる。
        if ((ev.type == AssetFileWatcher::EventType::Modified ||
             ev.type == AssetFileWatcher::EventType::Added) &&
            util::FileSystem::GetExtension(absPath) == ".prefab")
        {
            ctx.pendingPrefabReloads.push_back(absPath);
        }

        // 手書き / 外部ツール / AI が直したアセットを、実行中のキャッシュへ反映させる。
        // WHY .prefab と分けるか: プレファブはシーン内の実体を作り直す必要があり、
        //      こちらは AssetManager の中身を差し替えるだけで済む。処理の重さも
        //      失敗したときの影響範囲も違うので、同じキューに混ぜない。
        if ((ev.type == AssetFileWatcher::EventType::Modified ||
             ev.type == AssetFileWatcher::EventType::Added    ||
             ev.type == AssetFileWatcher::EventType::Renamed) &&
            IsHotReloadableAsset(absPath))
        {
            ctx.pendingAssetReloads.push_back(absPath);
        }

        // 開いているシーンがディスク上で書き換わった場合。判断 (未保存か / Play 中か)
        // は EditorApp 側でやるので、ここでは候補として渡すだけにする。
        if ((ev.type == AssetFileWatcher::EventType::Modified ||
             ev.type == AssetFileWatcher::EventType::Added    ||
             ev.type == AssetFileWatcher::EventType::Renamed) &&
            util::StringUtils::ToLower(util::FileSystem::GetExtension(absPath)) == ".scene")
        {
            ctx.pendingSceneReloads.push_back(absPath);
        }
    }

    if (needsDirectoryRefresh) {
        // 表示中フォルダ自体が移動・削除された場合は Assets ルートへ戻す。
        if (!util::FileSystem::IsDirectory(m_currentPath))
            m_currentPath = m_rootPath;
        RefreshDirectory();
    }

    // 書き込みが落ち着いた候補をインポートキューへ流す。ここから先は
    // 手動 Import と同じ経路なので、進捗はいつもの EditorTaskOverlay に出る。
    FlushScheduledReimports();

    // ── スレッド完了チェック ──────────────────────────────────────────────
    if (m_importThreadDone.load()) {
        m_importThreadDone.store(false);
        m_isImporting.store(false);
        m_importFuture = {};
        EditorTaskOverlay::End();
        asset::AssetManager::FlushFailed();
        std::vector<std::string> completedImports;
        {
            std::lock_guard<std::mutex> lock(m_importStatusMtx);
            completedImports.swap(m_completedImportPaths);
        }
        for (const std::string& path : completedImports) {
            asset::AssetManager::Unload<asset::ModelAsset>(path);
            ResetAssetPreviewCache(path);
        }
        // 成否にかかわらず「処理中」の印を外す。失敗したものを外さないと、
        // 原本を直して保存し直しても処理中と見なされ、二度と再試行されない。
        // 失敗した原本は .meta の fingerprint が更新されていないので、
        // 次に触られた時点で改めて再インポート候補になる。
        for (const std::string& path : m_inFlightImports)
            m_outdatedPaths.erase(path);
        const size_t attempted = m_inFlightImports.size();
        m_inFlightImports.clear();
        if (!completedImports.empty()) {
            Toast::Success(std::to_string(completedImports.size()) +
                           (completedImports.size() == 1 ? " model imported" : " models imported"));
        }
        // 失敗は必ず見せる。自動化した以上、黙って落ちると「保存したのに反映されない」
        // としか見えず、原因を追う手掛かりがどこにも残らない。
        if (attempted > completedImports.size()) {
            const size_t failed = attempted - completedImports.size();
            Toast::Error(std::to_string(failed) +
                         (failed == 1 ? " model failed to import" : " models failed to import"));
        }
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
    {
        std::lock_guard<std::mutex> lock(m_importStatusMtx);
        m_completedImportPaths.clear();
    }
    EditorTaskOverlay::Begin("Importing Assets");
    EditorTaskOverlay::SetProgress(0.0f);

    auto imports = std::move(m_pendingImports);
    m_pendingImports.clear(); // move 後の状態に依存しない (以降このフレームでも積まれ得る)

    // このバッチで焼く原本を控える。完了時にここを見て「処理中」の印を外す。
    m_inFlightImports.clear();
    m_inFlightImports.reserve(imports.size());
    for (const auto& imp : imports)
        m_inFlightImports.push_back(imp.path);

    m_importFuture = fbzz::TaskSystem::Submit([this, imports = std::move(imports)]() mutable {
        for (const auto& imp : imports) {
            {
                std::lock_guard<std::mutex> lock(m_importStatusMtx);
                m_importStatusStr = util::FileSystem::GetFilename(imp.path);
            }

            namespace fs = std::filesystem;
            const fs::path srcPath = util::FileSystem::PathFromUtf8(imp.path);
            const std::string outDir =
                util::FileSystem::PathToUtf8(srcPath.parent_path() / srcPath.stem());
            if (FbxImportTool::Import(imp.path, outDir, imp.path, imp.options)) {
                // 設定は実際に Import が成功した後で初めて原本の .meta に確定する。
                FbxMetaSerializer::SaveOptions(imp.path, imp.options);
                FbxMetaSerializer::SaveCacheInfo(imp.path, imp.options);
                std::lock_guard<std::mutex> lock(m_importStatusMtx);
                m_completedImportPaths.push_back(imp.path);
            }
            m_importDone.fetch_add(1);
        }
        m_importThreadDone.store(true);
    });
}

void AssetBrowserPanel::DrawImportSettingsModal(EditorContext& ctx)
{
    // Inspector からの Reimport リクエスト（優先度高）
    if (!ctx.requestOpenImportModal.empty()
        && !m_importSettings.visible
        && !m_textureImportSettings.visible) {
        // 拡張子の分類で開くウィンドウを決める。
        // WHY: 以前は「テクスチャでなければモデル」という二分岐だったため、.mat や .wav に
        //      Reimport をかけると Model Import Settings が開き、Source DCC など
        //      そのアセットに存在しない項目が並んでいた。分類外は単に無視する。
        const ImportCategory reqCategory = CategoryForExtension(util::StringUtils::ToLower(
            util::FileSystem::GetExtension(ctx.requestOpenImportModal)));
        if (IsTextureCategory(reqCategory)) {
            m_textureImportSettings.path        = ctx.requestOpenImportModal;
            m_textureImportSettings.open        = true;
            m_textureImportSettings.visible     = true;
            m_textureImportSettings.needsInit   = true;
            m_textureImportSettings.fromWatcher = false;
        } else if (reqCategory == ImportCategory::Model) {
            m_importSettings.path        = ctx.requestOpenImportModal;
            m_importSettings.options     = ctx.defaultImportOptions;
            m_importSettings.open        = true;
            m_importSettings.visible     = true;
            m_importSettings.needsInit   = true;
            m_importSettings.fromWatcher = false;
        } else {
            FBZZ_LOG_WARN("AssetBrowser: no import settings for [%s]",
                          ctx.requestOpenImportModal.c_str());
        }
        ctx.requestOpenImportModal.clear();
    }

    // ウォッチャー確認キューが溜まっていて、Model Import Settings が閉じているなら自動オープン
    if (!m_pendingConfirmImports.empty()
        && !m_importSettings.open
        && !m_importSettings.visible) {
        const std::string ext = util::StringUtils::ToLower(
            util::FileSystem::GetExtension(m_pendingConfirmImports.front()));
        m_importSettings.path        = m_pendingConfirmImports.front();
        m_importSettings.options     = ctx.defaultImportOptions;
        m_importSettings.open        = true;
        m_importSettings.visible     = true;
        m_importSettings.needsInit   = true;
        m_importSettings.fromWatcher = true;
    }

    // テクスチャ確認キューはモデルインポートと別ウィンドウでまとめて扱う。
    if (!m_pendingTextureConfirmImports.empty()
        && !m_textureImportSettings.open
        && !m_textureImportSettings.visible) {
        m_textureImportSettings.path        = m_pendingTextureConfirmImports.front();
        m_textureImportSettings.open        = true;
        m_textureImportSettings.visible     = true;
        m_textureImportSettings.needsInit   = true;
        m_textureImportSettings.fromWatcher = true;
    }

    if (m_textureImportSettings.visible) {
        if (m_textureImportSettings.open) {
            ImGui::SetNextWindowFocus();
            m_textureImportSettings.open = false;
        }

        const bool isTextureMulti = m_textureImportSettings.fromWatcher
            && m_pendingTextureConfirmImports.size() > 1;
        const float textureWindowW = isTextureMulti ? 520.0f : 420.0f;
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos({ center.x + 36.0f, center.y + 36.0f },
                                ImGuiCond_Appearing, { 0.5f, 0.5f });
        ImGui::SetNextWindowSize({ textureWindowW, 0.0f }, ImGuiCond_Appearing);

        bool textureWindowOpen = true;
        if (ImGui::Begin("Texture Import Settings", &textureWindowOpen,
                         ImGuiWindowFlags_AlwaysAutoResize)) {
            const bool initTexturePanel =
                ImGui::IsWindowAppearing() || m_textureImportSettings.needsInit;
            m_textureImportSettings.needsInit = false;
            if (initTexturePanel) {
                LoadTexMeta(m_textureImportSettings.path, m_textureImportSettings.settings);
                m_pendingTextureConfirmIncludes.assign(
                    m_pendingTextureConfirmImports.size(), true);
            }

            if (isTextureMulti) {
                const int total = static_cast<int>(m_pendingTextureConfirmImports.size());
                int checkedCount = 0;
                for (bool b : m_pendingTextureConfirmIncludes) if (b) ++checkedCount;
                ImGui::TextColored({ 0.95f, 0.75f, 0.25f, 1.0f },
                                   "%d texture(s) detected", total);
                ImGui::SameLine();
                ImGui::TextDisabled("(%d selected)", checkedCount);

                const float btnW = 38.0f;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - btnW * 2.0f - ImGui::GetStyle().ItemSpacing.x);
                if (ImGui::SmallButton("All##tex_chk"))
                    std::fill(m_pendingTextureConfirmIncludes.begin(),
                              m_pendingTextureConfirmIncludes.end(), true);
                ImGui::SameLine();
                if (ImGui::SmallButton("None##tex_chk"))
                    std::fill(m_pendingTextureConfirmIncludes.begin(),
                              m_pendingTextureConfirmIncludes.end(), false);

                const float listH = std::min(
                    static_cast<float>(total) * ImGui::GetTextLineHeightWithSpacing() + 8.0f,
                    180.0f);
                ImGui::BeginChild("##texture_confirm_list", { 0.0f, listH }, true);
                for (int i = 0; i < total; ++i) {
                    if (i >= static_cast<int>(m_pendingTextureConfirmIncludes.size()))
                        m_pendingTextureConfirmIncludes.push_back(true);
                    bool inc = m_pendingTextureConfirmIncludes[i];
                    ImGui::PushID(i);
                    if (ImGui::Checkbox("##tex_inc", &inc))
                        m_pendingTextureConfirmIncludes[i] = inc;
                    ImGui::SameLine();
                    ImGui::TextUnformatted(
                        util::FileSystem::GetFilename(m_pendingTextureConfirmImports[i]).c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", m_pendingTextureConfirmImports[i].c_str());
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::TextDisabled("Settings below apply to all checked textures.");
            } else {
                const std::string displayPath = m_textureImportSettings.fromWatcher
                    ? m_pendingTextureConfirmImports.front()
                    : m_textureImportSettings.path;
                ImGui::TextUnformatted(util::FileSystem::GetFilename(displayPath).c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", displayPath.c_str());
            }

            ImGui::Separator();
            auto& s = m_textureImportSettings.settings;

            // 拡張子で分類し、その分類で意味を持つ項目だけを描く。
            // WHY: 以前は全項目を無条件に並べていたため、.hdr に sRGB、Color テクスチャに
            //      Flip Green / Normalize Mipmaps といった無関係な項目が出ていた。
            // NOTE: 複数選択時は先頭ファイルの分類を代表として使う。混在時は
            //       いちばん制約の緩い分類ではなく先頭に合わせ、Apply 時に個別 Sanitize する。
            const std::string categoryPath = m_textureImportSettings.fromWatcher
                && !m_pendingTextureConfirmImports.empty()
                ? m_pendingTextureConfirmImports.front()
                : m_textureImportSettings.path;
            const ImportCategory category = CategoryForExtension(
                util::StringUtils::ToLower(util::FileSystem::GetExtension(categoryPath)));
            if (initTexturePanel) SanitizeTextureSettings(category, s);
            const TextureFieldMask mask = TextureFieldsFor(category, s);

            ImGui::SeparatorText("Type");
            ImGui::SetNextItemWidth(160.0f);
            if (DrawTextureTypeCombo("Type##tex_batch", category, s.type)) {
                // 型が変わると適切な既定値一式も変わるため作り直す。
                s = asset::DefaultSettingsForType(s.type);
                SanitizeTextureSettings(category, s);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset Defaults##tex_batch")) {
                s = asset::DefaultSettingsForType(s.type);
                SanitizeTextureSettings(category, s);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", ImportCategoryLabel(category));

            if (mask.srgb || mask.mipmaps || mask.flipGreen || mask.normalizeMips) {
                ImGui::SeparatorText("Encoding");
                // 表示される項目だけを 2 列へ詰める。
                // WHY: 項目を条件で消すと、固定で書いた SameLine が空振りして
                //      次の項目が思わぬ位置に流れる。描いた個数で列を決めて回避する。
                int drawnInRow = 0;
                const auto beginField = [&drawnInRow]() {
                    if (drawnInRow % 2 == 1) ImGui::SameLine(140.0f);
                    ++drawnInRow;
                };

                if (mask.srgb) {
                    beginField();
                    ImGui::Checkbox("sRGB##tex_batch", &s.srgb);
                }
                if (mask.mipmaps) {
                    beginField();
                    ImGui::Checkbox("Mipmaps##tex_batch", &s.mipmaps);
                }
                if (mask.flipGreen) {
                    beginField();
                    ImGui::Checkbox("Flip Green Channel##tex_batch", &s.flipGreen);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                        ImGui::SetTooltip(
                            "法線マップの Y 成分を反転します。\n"
                            "Blender / Maya がデフォルト出力する OpenGL 形式の場合にチェック。");
                }
                if (mask.normalizeMips) {
                    beginField();
                    ImGui::Checkbox("Normalize Mipmaps##tex_batch", &s.normalizeMipmaps);
                }
            }

            if (mask.compression) {
                ImGui::SeparatorText("Compression");
                ImGui::SetNextItemWidth(200.0f);
                DrawCompressionCombo("Format##tex_batch_comp", category, s.type, s.compression);
                static constexpr const char* kQualNames[] = { "Fast", "Normal", "High" };
                int qualIdx = static_cast<int>(s.compressionQuality);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::Combo("##tex_batch_compq", &qualIdx, kQualNames, 3))
                    s.compressionQuality = static_cast<asset::CompQuality>(qualIdx);
            }

            if (mask.maxSize || mask.sampling) {
                ImGui::SeparatorText("Sampling");
                if (mask.maxSize) {
                    int maxSize = static_cast<int>(s.maxSize);
                    ImGui::SetNextItemWidth(100.0f);
                    if (ImGui::InputInt("Max Size##tex_batch", &maxSize))
                        s.maxSize = static_cast<uint32_t>(std::max(1, maxSize));
                }
                if (mask.sampling) {
                    static constexpr const char* kWrapNames[] = { "Repeat", "Clamp", "Mirror", "Border" };
                    int wrapU = static_cast<int>(s.wrapU);
                    int wrapV = static_cast<int>(s.wrapV);
                    ImGui::SetNextItemWidth(100.0f);
                    if (ImGui::Combo("Wrap U##tex_batch", &wrapU, kWrapNames, 4)) s.wrapU = static_cast<asset::TextureWrap>(wrapU);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100.0f);
                    if (ImGui::Combo("Wrap V##tex_batch", &wrapV, kWrapNames, 4)) s.wrapV = static_cast<asset::TextureWrap>(wrapV);
                    int anisoLv = static_cast<int>(s.anisoLevel);
                    if (ImGui::SliderInt("Aniso Level##tex_batch", &anisoLv, 1, 16))
                        s.anisoLevel = static_cast<uint32_t>(anisoLv);
                }
            }

            if (mask.sprite) {
                // Sprite の矩形編集は Sprite Editor / Inspector の担当。
                // ここは「Sprite として取り込む」ことだけ確定させる。
                ImGui::SeparatorText("Sprite");
                ImGui::TextDisabled("Sprite rects can be edited in the Sprite Editor after import.");
            }

            ImGui::Separator();
            auto closeTextureWindow = [&]() {
                m_pendingTextureConfirmImports.clear();
                m_pendingTextureConfirmIncludes.clear();
                m_textureImportSettings.visible = false;
                m_textureImportSettings.open = false;
                m_textureImportSettings.fromWatcher = false;
                m_textureImportSettings.needsInit = false;
            };
            // 保存前に、その 1 枚の拡張子に照らして設定を正す。
            // WHY: 一括適用では .png と .hdr が同じチェックリストに混在しうる。
            //      画面上の値をそのまま全ファイルへ書くと、.hdr の .meta に sRGB=true の
            //      ような「その拡張子ではありえない設定」が残る。
            auto saveSanitized = [](const std::string& path, asset::TextureImportSettings settings) {
                const ImportCategory perFile = CategoryForExtension(
                    util::StringUtils::ToLower(util::FileSystem::GetExtension(path)));
                SanitizeTextureSettings(perFile, settings);
                SaveTexMeta(path, settings);
            };
            auto applyTextureSettings = [&]() {
                if (m_textureImportSettings.fromWatcher) {
                    for (int i = 0; i < static_cast<int>(m_pendingTextureConfirmImports.size()); ++i) {
                        const bool inc = i < static_cast<int>(m_pendingTextureConfirmIncludes.size())
                            && m_pendingTextureConfirmIncludes[i];
                        if (inc)
                            saveSanitized(m_pendingTextureConfirmImports[i], m_textureImportSettings.settings);
                    }
                } else {
                    saveSanitized(m_textureImportSettings.path, m_textureImportSettings.settings);
                }
                closeTextureWindow();
                RefreshDirectory();
            };

            int checkedCount = 1;
            if (m_textureImportSettings.fromWatcher) {
                checkedCount = 0;
                for (bool b : m_pendingTextureConfirmIncludes) if (b) ++checkedCount;
            }
            char applyLabel[48];
            std::snprintf(applyLabel, sizeof(applyLabel), "Apply (%d)", checkedCount);
            if (checkedCount == 0) ImGui::BeginDisabled();
            if (ImGui::Button(applyLabel, { 100.0f, 0.0f }))
                applyTextureSettings();
            if (checkedCount == 0) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", { 90.0f, 0.0f }))
                closeTextureWindow();
        }
        ImGui::End();

        if (!textureWindowOpen) {
            m_pendingTextureConfirmImports.clear();
            m_pendingTextureConfirmIncludes.clear();
            m_textureImportSettings.visible = false;
            m_textureImportSettings.open = false;
            m_textureImportSettings.fromWatcher = false;
        }
    }

    if (!m_importSettings.visible)
        return;

    auto advanceConfirmQueue = [&]() {
        if (!m_pendingConfirmImports.empty()
            && m_pendingConfirmImports.front() == m_importSettings.path) {
            m_pendingConfirmImports.erase(m_pendingConfirmImports.begin());
        }

        if (!m_pendingConfirmImports.empty()) {
            const std::string ext = util::StringUtils::ToLower(
                util::FileSystem::GetExtension(m_pendingConfirmImports.front()));
            m_importSettings.path        = m_pendingConfirmImports.front();
            m_importSettings.options     = ctx.defaultImportOptions;
            m_importSettings.open        = true;
            m_importSettings.visible     = true;
            m_importSettings.needsInit   = true;
            m_importSettings.fromWatcher = true;
        } else {
            m_importSettings.open        = false;
            m_importSettings.visible     = false;
            m_importSettings.needsInit   = false;
            m_importSettings.fromWatcher = false;
        }
    };

    if (m_importSettings.open) {
        ImGui::SetNextWindowFocus();
        m_importSettings.open = false;
    }

    const bool isMulti = m_importSettings.fromWatcher
        && m_pendingConfirmImports.size() > 1;
    const float modalW = isMulti ? 480.0f : 380.0f;
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, { 0.5f, 0.5f });
    ImGui::SetNextWindowSize({ modalW, 0.0f }, ImGuiCond_Appearing);

    bool windowOpen = true;
    if (!ImGui::Begin("Model Import Settings", &windowOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        if (!windowOpen) advanceConfirmQueue();
        return;
    }
    if (!windowOpen) {
        ImGui::End();
        advanceConfirmQueue();
        return;
    }

    const bool initPanel = ImGui::IsWindowAppearing() || m_importSettings.needsInit;
    m_importSettings.needsInit = false;

    // このウィンドウが扱えるのはモデルソースだけ。
    // WHY: Inspector の Reimport は拡張子を問わずここへ流れてくるため、モデルでない
    //      アセットに対して Source DCC / Normal Map Convention / Contents といった
    //      まったく無関係な項目が並んでいた。開く前に弾いて誤操作の余地をなくす。
    if (CategoryForExtension(util::StringUtils::ToLower(
            util::FileSystem::GetExtension(m_importSettings.path))) != ImportCategory::Model) {
        ImGui::TextUnformatted(util::FileSystem::GetFilename(m_importSettings.path).c_str());
        ImGui::Separator();
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                           "このアセットにはモデルインポート設定がありません。");
        ImGui::TextDisabled("Model Import Settings は .fbx / .obj / .gltf / .glb 専用です。");
        ImGui::Spacing();
        if (ImGui::Button("Close", { 90.0f, 0.0f }))
            advanceConfirmQueue();
        ImGui::End();
        return;
    }

    // ── 初期化（ポップアップが開くたびに実行） ─────────────────────────────
    const std::string presetsDir = GetPresetsDir(m_rootPath);
    static std::vector<ImportPreset> s_presets;
    static std::size_t               s_loadRevision = static_cast<std::size_t>(-1);
    static int                       s_presetSel = -1;
    static char                      s_presetNameBuf[64] = {};
    if (initPanel) {
        s_presets      = LoadPresetsFromDir(presetsDir);
        s_loadRevision = 0;
        s_presetSel    = -1;
        s_presetNameBuf[0] = '\0';
        m_scanResult  = {};
        if (!isMulti) {
            FbxImportOptions metaOptions = m_importSettings.options;
            if (FbxMetaSerializer::LoadOptions(m_importSettings.path, metaOptions))
                m_importSettings.options = metaOptions;
        }
        if (!isMulti) {
            // Assimp パースはレンダースレッドをブロックすると D3D11 TDR が起きるため非同期で実行
            m_scanPending = true;
            const std::string scanPath = m_importSettings.path;
            m_scanFuture = fbzz::TaskSystem::Submit([scanPath]() {
                return FbxImportTool::Scan(scanPath);
            });
        } else {
            m_scanPending = false;
        }
        // 複数ファイル: チェック状態を初期化（全選択）
        m_pendingConfirmIncludes.assign(m_pendingConfirmImports.size(), true);
    }

    // スキャン完了チェック（ポーリング: 非ブロッキング）
    if (m_scanPending && m_scanFuture.valid() &&
        m_scanFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_scanResult  = m_scanFuture.get();
        m_scanPending = false;
        if (m_importSettings.options.selectedMeshNames.empty())
            m_importSettings.options.selectedMeshNames = m_scanResult.meshNames;
        if (m_importSettings.options.selectedAnimNames.empty())
            m_importSettings.options.selectedAnimNames = m_scanResult.animNames;
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
            if (!inc) ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextFaint));
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

    // ── テクスチャ生成オプション ──────────────────────────────────────────
    ImGui::SeparatorText("Source");
    {
        static constexpr const char* kSourceDccNames[] = {
            "Auto Detect", "Maya / FBX SDK", "Blender"
        };
        int sourceDccIdx = static_cast<int>(m_importSettings.options.sourceDcc);
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo("Source DCC", &sourceDccIdx, kSourceDccNames, 3))
            m_importSettings.options.sourceDcc = static_cast<FbxSourceDcc>(sourceDccIdx);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip(
                "Auto Detect: FBX の Creator メタデータから Blender を判定します。\n"
                "Maya: Blender ルート補正を行いません。\n"
                "Blender: ルートの +90°X / scale100 補正を強制します。");
        if (!isMulti) {
            if (m_scanPending) {
                ImGui::SameLine();
                ImGui::TextDisabled("Detecting...");
            } else if (m_scanResult.valid) {
                const FbxSourceDcc resolvedDcc =
                    m_importSettings.options.sourceDcc == FbxSourceDcc::Auto
                        ? m_scanResult.detectedSourceDcc
                        : m_importSettings.options.sourceDcc;
                ImGui::SameLine();
                ImGui::TextDisabled("Detected: %s", SourceDccLabel(m_scanResult.detectedSourceDcc));
                ImGui::TextDisabled(
                    "Preview correction: %s",
                    resolvedDcc == FbxSourceDcc::Blender
                        ? "Blender axis/scale fix"
                        : "Maya / FBX SDK transform");
            }
        }
    }

    {
        static constexpr const char* kAxisNames[] = { "Auto", "Y Up", "Z Up" };
        int axisIdx = static_cast<int>(m_importSettings.options.upAxis);
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo("Source Up Axis", &axisIdx, kAxisNames, 3))
            m_importSettings.options.upAxis = static_cast<FbxUpAxis>(axisIdx);
    }
    ImGui::SetNextItemWidth(180.0f);
    ImGui::DragFloat("Unit Scale", &m_importSettings.options.unitScaleMultiplier,
                     0.01f, 0.001f, 100.0f, "%.3fx");
    ImGui::Checkbox("Generate Normals", &m_importSettings.options.generateNormals);
    ImGui::SameLine();
    ImGui::Checkbox("Generate Tangents", &m_importSettings.options.generateTangents);

    ImGui::SeparatorText("Texture Generation");
    ImGui::Checkbox("Auto-generate .meta sidecars",
                    &m_importSettings.options.generateTexDescriptors);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip(
            "インポート時にテクスチャごとに \"<画像>.meta\" サイドカーを自動生成します。\n"
            "sRGB / 圧縮 / Mipmap 等の設定は .meta から Editor で編集できます。");

    {
        static constexpr const char* kConvNames[] = {
            "DirectX (keep G)",
            "OpenGL (flip G)",
        };
        int convIdx = static_cast<int>(m_importSettings.options.normalMapConvention);
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo("Normal Map Convention", &convIdx, kConvNames, 2))
            m_importSettings.options.normalMapConvention =
                static_cast<NormalMapConvention>(convIdx);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip(
                "DirectX: G をそのまま保持（UE / Unity デフォルト）\n"
                "OpenGL:  G を反転（Blender / Maya デフォルト）");
    }

    {
        static constexpr const char* kCompNames[] = {
            "Auto", "BC1", "BC3", "BC4", "BC5", "BC6H", "BC7", "None"
        };
        int compIdx = static_cast<int>(m_importSettings.options.defaultCompression);
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo("Default Compression", &compIdx, kCompNames, 8))
            m_importSettings.options.defaultCompression =
                static_cast<asset::TextureCompression>(compIdx);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip(
                "生成する .meta のデフォルト圧縮形式。\n"
                "Auto = テクスチャタイプから自動選択。");
    }

    // ── 選択的インポート（単一ファイルのみ） ──────────────────────────────
    if (!isMulti) {
        if (m_scanPending) {
            ImGui::Spacing();
            ImGui::SeparatorText("Contents");
            ImGui::TextDisabled("Scanning...");
        } else if (m_scanResult.valid &&
                   (!m_scanResult.meshNames.empty() || !m_scanResult.animNames.empty())) {
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
            drawSelectList("Meshes", m_scanResult.meshNames,
                           m_importSettings.options.selectedMeshNames);
            if (!m_scanResult.animNames.empty()) {
                ImGui::Spacing();
                drawSelectList("Animations", m_scanResult.animNames,
                               m_importSettings.options.selectedAnimNames);
            }
        }
    } else {
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
                if (inc) {
                    m_pendingImports.push_back({ m_pendingConfirmImports[i], m_importSettings.options });
                }
            }
            m_pendingConfirmImports.clear();
            m_importSettings.fromWatcher = false;
            m_importAllRequested = true;
            m_importSettings.visible = false;
        }
        if (checkedCount == 0) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("チェックされたファイルをインポート");

        ImGui::SameLine();
        if (ImGui::Button("Import All", { 95.0f, 0.0f })) {
            for (const auto& p : m_pendingConfirmImports) {
                m_pendingImports.push_back({ p, m_importSettings.options });
            }
            m_pendingConfirmImports.clear();
            m_importSettings.fromWatcher = false;
            m_importAllRequested = true;
            m_importSettings.visible = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("全 %zu 件をインポート", m_pendingConfirmImports.size());

        ImGui::SameLine();
        if (ImGui::Button("Cancel", { 75.0f, 0.0f })) {
            m_pendingConfirmImports.clear();
            m_importSettings.fromWatcher = false;
            m_importSettings.visible = false;
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
        if (ImGui::Button("Import", { 90.0f, 0.0f })) {
            enqueueCurrentFile();
            m_importAllRequested = true;
            advanceConfirmQueue();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("インポートして次へ");

        ImGui::SameLine();
        if (ImGui::Button("Cancel", { 90.0f, 0.0f })) {
            advanceConfirmQueue();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("スキップ");
    }

    ImGui::End();
}

} // namespace fbzz::editor
